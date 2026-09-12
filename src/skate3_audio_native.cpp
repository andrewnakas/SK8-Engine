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

}  // namespace

extern "C" REX_FUNC(sub_82B28CC0) {
  if (skate3::audio::ShadowEnabled()) {
    // Watch the player's FIFO head and tail. The write to packet+0x0C falls outside
    // this window and is not covered by the memory diff -- the harness compares one
    // region, and the packet can sit anywhere relative to the player.
    const uint32_t player = REX_LOAD_U32(ctx.r3.u32 + kRecordPlayer);
    skate3::audio::ShadowCompare(ctx, base, NativeEventSubmit, __imp__sub_82B28CC0,
                                 player + kPlayerPacketHead, 8, "EVENT_SUBMIT");
    return;
  }
  if (UseNative()) {
    NativeEventSubmit(ctx, base);
    return;
  }
  __imp__sub_82B28CC0(ctx, base);
}
