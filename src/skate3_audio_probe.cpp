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
 */

#include <atomic>
#include <cstdint>

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
