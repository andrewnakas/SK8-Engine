/**
 * @file        skate3_audio_probe.cpp
 * @brief       Read-only probe of the XMA input feed, for format research
 *
 * Static reading of the decompiled audio code left one question open: what
 * exactly is handed to the hardware XMA decoder. `sub_82B4FE40` feeds each
 * context in 0x800-byte chunks through `sub_82B4FD40`, which decompiles to a
 * plain VMX128 memcpy - so whatever the source buffer holds is byte-for-byte
 * what the decoder sees. Three separate attempts to find the code that binds
 * source buffers to streams failed, because the struct offsets involved
 * (+0x24, +0x34, +0x3c, +0x44) are too common to grep for.
 *
 * Rather than keep reading, observe it. This hook logs the source address and
 * length of each feed plus the first bytes at that address, which settles two
 * things at once:
 *
 *   - whether the bytes are conventional XMA2 packets (a valid packet header
 *     has metadata == 1 in the low 3 bits of byte 2), and
 *   - whether the lengths and source strides look like file offsets or like
 *     sample-space quantities, which is the open question in
 *     docs/xma-transcode.md.
 *
 * The hook changes no behaviour: it records arguments and calls the original.
 * It is off by default and bounded, because this path runs at audio rate.
 *
 * A second hook, on `sub_828E2B48`, logs the messages game code posts to audio
 * objects: which object (by its entry in the 72-entry name table at 0x8302D4A4,
 * whose handle slots start at 0x8302EE28) and the payload words. That turns
 * "which game event reaches which bank, with what arguments" into a trace
 * (docs/audio-banks.md, "How the game addresses a player sound object").
 *
 * Under the same cvar, `sub_828E2D18` logs re-deliveries: the game rewrites a held message's payload
 * every frame (a grind's volume, cutoffs and speed) and re-delivers it to the instance's payload
 * copies, which the post hook alone never sees. Two more hooks record where a patch meets the mixer: the voice
 * device's open (`sub_824A3140`, reached through the device vtable from the voice op) with
 * the sample pointer, descriptor and parameter records it is handed, and the graph builder
 * `sub_82B48C48` with each module descriptor and the class it names.
 */

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_BOOL(skate3_audio_probe_feed, false, "Skate 3",
                    "Log the first few XMA input feeds (source, length, leading bytes).\n"
                    "\n"
                    "Research aid for the audio format work; harmless but noisy. The feed "
                    "runs at audio rate, so output is capped by "
                    "skate3_audio_probe_feed_count.");

REXCVAR_DEFINE_UINT32(skate3_audio_probe_feed_count, 24, "Skate 3",
                      "How many XMA input feeds to log before going quiet.");

namespace {

std::atomic<uint64_t> g_feeds{0};

}  // namespace

// sub_82B4FD40(dst, src, len) - the VMX128 memcpy that fills an XMA input
// buffer. r3 = destination (the context's input buffer), r4 = source, r5 = length.
extern "C" REX_FUNC(sub_82B4FD40) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t len = ctx.r5.u32;

  __imp__sub_82B4FD40(ctx, base);

  // Read the cvars exactly once. This is a general-purpose fast memcpy inside the
  // audio module, called at audio rate and possibly with a lock held; a cvar
  // lookup per call is both wasteful and a plausible way to deadlock against
  // whatever the cvar store uses internally. The counter is checked first so the
  // steady state after the cap is a single relaxed atomic load.
  static const bool enabled = REXCVAR_GET(skate3_audio_probe_feed);
  static const uint64_t cap = REXCVAR_GET(skate3_audio_probe_feed_count);
  if (!enabled || g_feeds.load(std::memory_order_relaxed) >= cap || src == 0) {
    return;
  }
  const uint64_t n = g_feeds.fetch_add(1, std::memory_order_relaxed);
  if (n >= cap) {
    return;
  }

  // First four words at the source, and the XMA2 packet fields they would mean
  // if this were conventional packet framing.
  const uint32_t w0 = REX_LOAD_U32(src);
  const uint32_t w1 = REX_LOAD_U32(src + 4);
  const uint32_t w2 = REX_LOAD_U32(src + 8);
  const uint32_t w3 = REX_LOAD_U32(src + 12);
  const uint8_t b0 = static_cast<uint8_t>(w0 >> 24);
  const uint8_t b1 = static_cast<uint8_t>(w0 >> 16);
  const uint8_t b2 = static_cast<uint8_t>(w0 >> 8);
  const uint8_t b3 = static_cast<uint8_t>(w0);
  const uint32_t frame_count = b0 >> 2;
  const uint32_t frame_offset_bits = ((b0 & 3u) << 13) | (b1 << 5) | (b2 >> 3);
  const uint32_t metadata = b2 & 7u;

  REXLOG_INFO(
      "skate3-audio-probe: feed {} dst={:08X} src={:08X} len={} ({:#x}) | "
      "[{:08X} {:08X} {:08X} {:08X}] | as-XMA2 frames={} offset_bits={} "
      "metadata={} skip={} -> {}",
      n, dst, src, len, len, w0, w1, w2, w3, frame_count, frame_offset_bits, metadata, b3,
      metadata == 1 ? "PLAUSIBLE packet" : "not a packet header");
}

REXCVAR_DEFINE_BOOL(skate3_audio_probe_messages, false, "Skate 3",
                    "Log messages posted to audio objects (object name and payload words).\n"
                    "\n"
                    "Research aid for the player-sound work. Full lines are capped by "
                    "skate3_audio_probe_messages_count; after that, per-object totals are "
                    "logged every 5000 posts.");

REXCVAR_DEFINE_UINT32(skate3_audio_probe_messages_count, 4000, "Skate 3",
                      "How many audio object messages to log in full.");

namespace {

constexpr uint32_t kObjectTable = 0x8302D4A4;  // {char* name; u16 project; u16 name_id}
constexpr uint32_t kSlotBase = 0x8302EE28;     // entry i's handle slot is kSlotBase + 8*i
constexpr uint32_t kObjects = 72;
constexpr uint32_t kPayloadWords = 28;         // the largest player message ends at +0x74

std::atomic<uint64_t> g_messages{0};
std::array<std::atomic<uint64_t>, kObjects + 1> g_per_object{};  // last bucket: not a table slot

const char* ObjectName(uint8_t* base, uint32_t index) {
  if (index >= kObjects) {
    return "(other)";
  }
  const uint32_t name = REX_LOAD_U32(kObjectTable + 8 * index);
  return name ? reinterpret_cast<const char*>(base + name) : "(null)";
}

}  // namespace

// sub_828E2B48(slot, payload, message) - post a message to an audio object's listeners.
// r3 = handle slot, r4 = payload (message + 4), r5 = message. Returns 0, or -6 / -3 for an
// empty or stale slot, after which the sender resolves the name and posts again.
extern "C" REX_FUNC(sub_828E2B48) {
  static const bool enabled = REXCVAR_GET(skate3_audio_probe_messages);
  if (!enabled) {
    __imp__sub_828E2B48(ctx, base);
    return;
  }
  static const uint64_t cap = REXCVAR_GET(skate3_audio_probe_messages_count);
  const uint32_t slot = ctx.r3.u32;
  const uint32_t payload = ctx.r4.u32;
  uint32_t index = kObjects;
  if (slot >= kSlotBase && slot < kSlotBase + 8 * kObjects && (slot - kSlotBase) % 8 == 0) {
    index = (slot - kSlotBase) / 8;
  }
  g_per_object[index].fetch_add(1, std::memory_order_relaxed);
  const uint64_t n = g_messages.fetch_add(1, std::memory_order_relaxed);

  // Read the payload before the listeners run, in case one consumes it.
  char words[kPayloadWords * 9 + 1] = {0};
  if (n < cap && payload != 0) {
    for (uint32_t i = 0; i < kPayloadWords; ++i) {
      std::snprintf(words + i * 9, 10, "%08X ", REX_LOAD_U32(payload + 4 * i));
    }
  }

  __imp__sub_828E2B48(ctx, base);

  if (n < cap) {
    // The listeners the post just called: the slot's symbol record heads a list of
    // {next, ?, fn, ctx} nodes (sub_828E2B48's first loop). Walked only after a successful post,
    // which validated the slot, and only while every pointer looks like guest heap.
    char listeners[4 * 20 + 1] = {0};
    if (ctx.r3.s32 == 0) {
      uint32_t node = REX_LOAD_U32(REX_LOAD_U32(slot));
      for (uint32_t i = 0; i < 4 && node >= 0x40000000 && node < 0xF0000000; ++i) {
        std::snprintf(listeners + i * 20, 21, "%08X/%08X ", REX_LOAD_U32(node + 8),
                      REX_LOAD_U32(node + 12));
        node = REX_LOAD_U32(node);
      }
    }
    REXLOG_INFO(
        "skate3-audio-msg: {} obj={} {} slot={:08X} payload={:08X} result={} [{}] listeners=[{}]",
        n, index, ObjectName(base, index), slot, payload, ctx.r3.s32, words, listeners);
  } else if (n % 5000 == 0) {
    std::string totals;
    for (uint32_t i = 0; i <= kObjects; ++i) {
      const uint64_t c = g_per_object[i].load(std::memory_order_relaxed);
      if (c != 0) {
        totals += ObjectName(base, i);
        totals += '=';
        totals += std::to_string(c);
        totals += ' ';
      }
    }
    REXLOG_INFO("skate3-audio-msg: totals after {}: {}", n, totals);
  }
}

namespace {

std::atomic<uint64_t> g_opens{0};
std::atomic<uint64_t> g_graphs{0};
constexpr uint64_t kSeamCap = 400;

}  // namespace

// sub_824A3140(device, sample, byte, descriptor[6], word, word, {count, records}) -- the voice
// device's open. r4 = the bank sample's EAAC stream, r5 = a descriptor byte, r6 = six shifted
// descriptor words, r7/r8 = two bank words, r9 = {u32 count, u32 records*} of 12-byte records.
extern "C" REX_FUNC(sub_824A3140) {
  static const bool enabled = REXCVAR_GET(skate3_audio_probe_messages);
  if (!enabled || g_opens.load(std::memory_order_relaxed) >= kSeamCap) {
    __imp__sub_824A3140(ctx, base);
    return;
  }
  const uint64_t n = g_opens.fetch_add(1, std::memory_order_relaxed);
  const uint32_t sample = ctx.r4.u32, byte5 = ctx.r5.u32, desc = ctx.r6.u32, w7 = ctx.r7.u32,
                 w8 = ctx.r8.u32, block = ctx.r9.u32;
  char descriptor[6 * 9 + 1] = {0};
  if (desc >= 0x40000000 && desc < 0xF0000000) {
    for (uint32_t i = 0; i < 6; ++i) {
      std::snprintf(descriptor + i * 9, 10, "%08X ", REX_LOAD_U32(desc + 4 * i));
    }
  }
  const uint32_t header0 = sample ? REX_LOAD_U32(sample) : 0;
  const uint32_t header1 = sample ? REX_LOAD_U32(sample + 4) : 0;
  std::string records;
  if (block >= 0x40000000 && block < 0xF0000000) {
    const uint32_t count = REX_LOAD_U32(block);
    const uint32_t array = REX_LOAD_U32(block + 4);
    for (uint32_t i = 0; i < count && i < 24 && array >= 0x40000000 && array < 0xF0000000; ++i) {
      char one[40];
      std::snprintf(one, sizeof one, "%u:%d/%d ", REX_LOAD_U8(array + 12 * i),
                    static_cast<int32_t>(REX_LOAD_U32(array + 12 * i + 4)),
                    static_cast<int32_t>(REX_LOAD_U32(array + 12 * i + 8)));
      records += one;
    }
  }
  __imp__sub_824A3140(ctx, base);
  REXLOG_INFO(
      "skate3-audio-open: {} sample={:08X} eaac=[{:08X} {:08X}] byte={} desc=[{}] w7={:08X} "
      "w8={:08X} records=[{}] -> voice={:08X}",
      n, sample, header0, header1, byte5, descriptor, w7, w8, records, ctx.r3.u32);
}

// sub_82B48C48(system, r4, count, descriptors) -- build a player graph from `count` 12-byte module
// descriptors; each names a module class at +4 whose vtable +4 sizes the instance. Returns the player.
extern "C" REX_FUNC(sub_82B48C48) {
  static const bool enabled = REXCVAR_GET(skate3_audio_probe_messages);
  if (!enabled || g_graphs.load(std::memory_order_relaxed) >= kSeamCap) {
    __imp__sub_82B48C48(ctx, base);
    return;
  }
  const uint64_t n = g_graphs.fetch_add(1, std::memory_order_relaxed);
  const uint32_t r4 = ctx.r4.u32, count = ctx.r5.u32, desc = ctx.r6.u32;
  std::string modules;
  for (uint32_t i = 0; i < count && i < 16 && desc >= 0x40000000 && desc < 0xF0000000; ++i) {
    const uint32_t at = desc + 12 * i;
    const uint32_t w0 = REX_LOAD_U32(at), cls = REX_LOAD_U32(at + 4), w2 = REX_LOAD_U32(at + 8);
    const uint32_t vtbl = (cls >= 0x40000000 && cls < 0xF0000000) ? REX_LOAD_U32(cls) : 0;
    char one[96];
    std::snprintf(one, sizeof one, "{%08X cls=%08X vt=%08X size=%08X %08X} ", w0, cls, vtbl,
                  (cls >= 0x40000000 && cls < 0xF0000000) ? REX_LOAD_U32(cls + 4) : 0, w2);
    modules += one;
  }
  __imp__sub_82B48C48(ctx, base);
  REXLOG_INFO("skate3-audio-graph: {} r4={:08X} count={} modules=[{}] -> player={:08X}", n, r4,
              count, modules, ctx.r3.u32);
}

namespace {

std::atomic<uint64_t> g_updates{0};
constexpr uint64_t kUpdateCap = 6000;

}  // namespace

// sub_828E2D18(node, payload) -- re-deliver a held message: call each payload callback on the post
// node's +8 list with (payload, ctx). The node's +0 is the object's symbol record, whose +4 is its name.
extern "C" REX_FUNC(sub_828E2D18) {
  static const bool enabled = REXCVAR_GET(skate3_audio_probe_messages);
  if (!enabled) {
    __imp__sub_828E2D18(ctx, base);
    return;
  }
  const uint64_t n = g_updates.fetch_add(1, std::memory_order_relaxed);
  if (n < kUpdateCap || n % 20000 == 0) {
    const uint32_t node = ctx.r3.u32, payload = ctx.r4.u32;
    const char* name = "(unknown)";
    if (node >= 0x40000000 && node < 0xF0000000) {
      const uint32_t record = REX_LOAD_U32(node);
      if (record >= 0x40000000 && record < 0xF0000000) {
        const uint32_t text = REX_LOAD_U32(record + 4);
        if (text >= 0x40000000 && text < 0xF0000000) name = reinterpret_cast<const char*>(base + text);
      }
    }
    char words[17 * 9 + 1] = {0};
    if (payload >= 0x40000000 && payload < 0xF0000000) {
      for (uint32_t i = 0; i < 17; ++i) {
        std::snprintf(words + i * 9, 10, "%08X ", REX_LOAD_U32(payload + 4 * i));
      }
    }
    REXLOG_INFO("skate3-audio-update: {} {} node={:08X} payload={:08X} [{}]", n, name, node, payload,
                words);
  }
  __imp__sub_828E2D18(ctx, base);
}
