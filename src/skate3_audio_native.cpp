/**
 * @file        skate3_audio_native.cpp
 * @brief       Native replacements for rw::audio functions, gated on proven equivalence
 *
 * Each function here has three modes, selected by cvar:
 *
 *   default              run the original recompiled body; native code is not used
 *   skate3_audio_shadow  run both, diff them, keep the original's result
 *   skate3_audio_native  run the native body for real
 *
 * The default is the original on purpose. A native replacement earns its way to
 * `skate3_audio_native` by surviving a play session under `skate3_audio_shadow` with zero
 * divergence -- not by looking correct.
 *
 * Offsets referenced here are documented in docs/rw_audio_structs.h, which asserts them.
 */
#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/skate3_init.h"
#include "skate3_audio_shadow.h"

REXCVAR_DEFINE_BOOL(
    skate3_audio_native, false, "Skate 3",
    "Use the native audio implementations instead of the recompiled originals.\n"
    "\n"
    "Only turn this on for functions verified under skate3_audio_shadow. With both off "
    "the game runs entirely on the original recompiled code.");

namespace {

using skate3::audio::ShadowWindow;

// rw::audio::core::Player, from docs/rw_audio_structs.h
constexpr uint32_t kPlayerPacketHead = 0x148;
constexpr uint32_t kPlayerPacketTail = 0x14C;
constexpr uint32_t kPacketNext = 0x0C;

// rw_player fields EVENT_STOP touches, from docs/rw_audio_structs.h and the lifted body.
constexpr uint32_t kPlayerTable = 0x48;      // 20 entries, stride 12, from +0x54
constexpr uint32_t kPlayerDecoder = 0x150;   // active decoder; 0 when idle
constexpr uint32_t kPlayerTornDown = 0x158;  // 0xFF on teardown - then cleared by the wipe
constexpr uint32_t kPlayerState = 0x15E;     // 1 = playing, 4 = stopped
constexpr uint32_t kPlayerStopWipe = 0x14;   // bytes cleared from +0x150
// One window covering every byte EVENT_STOP writes in the player: the table at +0x54, the
// FIFO head and tail at +0x148, the play state from +0x150, and the three bytes at +0x170.
constexpr uint32_t kPlayerStopSpan = 0x175 - kPlayerTable;

// The command record EVENT_SUBMIT is dispatched with.
constexpr uint32_t kRecordPlayer = 4;
constexpr uint32_t kRecordPacket = 8;

// Size EVENT_SUBMIT reports back to the drain, which advances by this.
constexpr uint32_t kSubmitRecordSize = 0x0C;

bool UseNative() {
  static const bool on = REXCVAR_GET(skate3_audio_native);
  return on;
}

/**
 * EVENT_SUBMIT: append a packet to the player's FIFO.
 *
 * Decompiled from sub_82B28CC0. The original writes the tail's next pointer, or both
 * head and tail when the list is empty, then clears the new packet's next pointer and
 * returns its own record size. Reproduced in the same order, because the drain reads
 * the return value to advance and a torn ordering here is what bug 1 is about.
 */
void NativeEventSubmit(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t player = REX_LOAD_U32(record + kRecordPlayer);
  const uint32_t packet = REX_LOAD_U32(record + kRecordPacket);

  if (REX_LOAD_U32(player + kPlayerPacketHead) == 0) {
    REX_STORE_U32(player + kPlayerPacketHead, packet);
  } else {
    REX_STORE_U32(REX_LOAD_U32(player + kPlayerPacketTail) + kPacketNext, packet);
  }
  REX_STORE_U32(player + kPlayerPacketTail, packet);
  REX_STORE_U32(packet + kPacketNext, 0);

  ctx.r3.u64 = kSubmitRecordSize;
}

/**
 * EVENT_STOP: stop playback and drop every submitted packet.
 *
 * Decompiled from sub_82B28C18, cross-checked against docs/command-queue.md. Order is the
 * original's: tear the decoder down if one is live, clear the play state, mark the state
 * byte stopped, unlink the whole FIFO, clear the 20-entry table, then the bytes at +0x170.
 *
 * The 0xFF written to +0x158 is immediately overwritten by the 20-byte wipe that follows
 * it. Reproduced anyway - the job is to match the original, not to tidy it.
 */
void NativeEventStop(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t player = REX_LOAD_U32(record + kRecordPlayer);

  const uint32_t decoder = REX_LOAD_U32(player + kPlayerDecoder);
  if (decoder != 0) {
    PPCContext teardown = ctx;
    teardown.r3.u64 = decoder;
    __imp__sub_82B3C930(teardown, base);
    REX_STORE_U32(player + kPlayerDecoder, 0);
    REX_STORE_U32(player + kPlayerTornDown, 255);
  }
  for (uint32_t i = 0; i < kPlayerStopWipe; i++) {
    REX_STORE_U8(player + kPlayerDecoder + i, 0);
  }
  REX_STORE_U8(player + kPlayerState, 4);

  // Unlink every submitted packet, clearing each one's next pointer as it goes.
  for (;;) {
    const uint32_t packet = REX_LOAD_U32(player + kPlayerPacketHead);
    if (packet == 0) {
      break;
    }
    const uint32_t next = REX_LOAD_U32(packet + kPacketNext);
    REX_STORE_U32(player + kPlayerPacketHead, next);
    if (next == 0) {
      REX_STORE_U32(player + kPlayerPacketTail, 0);
    }
    REX_STORE_U32(packet + kPacketNext, 0);
  }

  // Each table entry: the discriminator byte at +0x09, then the entry word itself. The
  // original walks with stwu from +0x48, so the first word it clears is the one at +0x54.
  uint32_t entry = player + kPlayerTable;
  for (uint32_t i = 0; i < 20; i++) {
    REX_STORE_U8(entry + 21, 0);
    entry += 12;
    REX_STORE_U32(entry, 0);
  }

  REX_STORE_U8(player + 0x173, 0);
  REX_STORE_U8(player + 0x174, 0);
  REX_STORE_U8(player + 0x172, 16);

  ctx.r3.u64 = 8;  // EVENT_STOP's own record size
}

skate3::audio::ShadowStats g_event_submit_stats{"EVENT_SUBMIT"};
skate3::audio::ShadowStats g_event_stop_stats{"EVENT_STOP"};
std::atomic<uint64_t> g_event_stop_unverifiable{0};

}  // namespace

extern "C" REX_FUNC(sub_82B28C18) {
  if (skate3::audio::ShadowEnabled()) {
    const uint32_t record = ctx.r3.u32;
    const uint32_t player = REX_LOAD_U32(record + kRecordPlayer);
    // A live decoder means the original calls sub_82B3C930, which releases the voice
    // through four indirect calls. The harness runs the native body a second time against
    // rewound memory, so those calls would land on an already-released object - and the
    // release is not memory this harness can rewind. Not comparable: run the original and
    // count it, rather than pretending the path was checked.
    ShadowWindow windows[16];
    size_t count = 0;
    windows[count++] = {player + kPlayerTable, kPlayerStopSpan};
    uint32_t packet = REX_LOAD_U32(player + kPlayerPacketHead);
    while (packet != 0 && count < 16) {
      windows[count++] = {packet + kPacketNext, 4};
      packet = REX_LOAD_U32(packet + kPacketNext);
    }
    if (REX_LOAD_U32(player + kPlayerDecoder) != 0 || packet != 0) {
      g_event_stop_unverifiable.fetch_add(1, std::memory_order_relaxed);
      __imp__sub_82B28C18(ctx, base);
      return;
    }
    skate3::audio::ShadowCompare(ctx, base, NativeEventStop, __imp__sub_82B28C18,
                                 {windows, count}, skate3::audio::kReturnR3,
                                 g_event_stop_stats);
    return;
  }
  if (UseNative()) {
    NativeEventStop(ctx, base);
    return;
  }
  __imp__sub_82B28C18(ctx, base);
}

extern "C" REX_FUNC(sub_82B28CC0) {
  if (skate3::audio::ShadowEnabled()) {
    // Every byte the function writes: the FIFO head and tail, the new packet's next
    // pointer, and -- when the list already had a packet -- the old tail's next pointer.
    const uint32_t record = ctx.r3.u32;
    const uint32_t player = REX_LOAD_U32(record + kRecordPlayer);
    const uint32_t packet = REX_LOAD_U32(record + kRecordPacket);
    const uint32_t head = REX_LOAD_U32(player + kPlayerPacketHead);
    const uint32_t tail = REX_LOAD_U32(player + kPlayerPacketTail);
    ShadowWindow windows[3] = {{player + kPlayerPacketHead, 8}, {packet + kPacketNext, 4}};
    size_t count = 2;
    if (head != 0) {
      windows[count++] = {tail + kPacketNext, 4};
    }
    skate3::audio::ShadowCompare(ctx, base, NativeEventSubmit, __imp__sub_82B28CC0,
                                 {windows, count}, skate3::audio::kReturnR3,
                                 g_event_submit_stats);
    return;
  }
  if (UseNative()) {
    // Milestone counts, so a promoted session shows the native body actually engaged.
    static std::atomic<uint64_t> native_runs{0};
    const uint64_t runs = native_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runs == 1 || runs == 16 || runs == 256 || runs == 1024) {
      REXLOG_INFO("skate3-audio-native: EVENT_SUBMIT native runs={}", runs);
    }
    NativeEventSubmit(ctx, base);
    return;
  }
  __imp__sub_82B28CC0(ctx, base);
}
