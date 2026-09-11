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

skate3::audio::ShadowStats g_event_submit_stats{"EVENT_SUBMIT"};

}  // namespace

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
