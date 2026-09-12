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
#include <cmath>
#include <cstdint>
#include <cstring>

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

// The EVENT_PLAY command record. It shares only the player pointer at +4 with
// EVENT_SUBMIT's; the rest is three floats, so the fields are named apart.
constexpr uint32_t kRecordPlayFormat = 8;     // -> format_index, truncated to a byte
constexpr uint32_t kRecordPlayRate = 12;      // -> sample_rate, stored as float32
constexpr uint32_t kRecordPlayChannels = 16;  // -> channel_count, truncated to a byte

// The rw_player fields EVENT_PLAY writes, from docs/rw_audio_structs.h.
constexpr uint32_t kPlayerSource = 0x50;  // source object; EVENT_PLAY writes through it
constexpr uint32_t kPlayerSampleRate = 0x154;
constexpr uint32_t kPlayerChannelCount = 0x15F;
constexpr uint32_t kPlayerFormatIndex = 0x160;
// One window covering every player byte EVENT_PLAY writes: the decoder pointer at +0x150,
// the sample rate at +0x154, and the state, channel and format bytes at +0x15E..+0x160.
constexpr uint32_t kPlayerPlaySpan = 0x161 - kPlayerDecoder;

// Size EVENT_PLAY reports back to the drain, which advances by this.
constexpr uint32_t kPlayRecordSize = 20;

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

float BitsToFloat(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

/**
 * The guest's `fctidz` followed by `stfd` and `lbz +7`: truncate toward zero into a 64-bit
 * integer, spill it big-endian, then read back the least significant byte.
 *
 * The lifted body special-cases NaN and anything above LLONG_MAX before handing the rest to
 * x86 CVTTSD2SI, and those two cases disagree: saturation leaves a low byte of 0xFF, while
 * CVTTSD2SI's out-of-range "integer indefinite" leaves 0x00. Exactly 2^63 takes the second
 * path, because the lifted test is `>` and not `>=`. Reproduced branch for branch: a plain
 * cast would be undefined behaviour out of range and would disagree at that boundary.
 */
uint8_t TruncatedLowByte(float value) {
  constexpr double kTwoPow63 = 9223372036854775808.0;  // double(LLONG_MAX), exactly
  const double widened = static_cast<double>(value);
  uint64_t spilled;
  if (std::isnan(widened)) {
    spilled = 0x8000000000000000ULL;
  } else if (widened > kTwoPow63) {
    spilled = static_cast<uint64_t>(INT64_MAX);
  } else if (widened >= kTwoPow63 || widened < -kTwoPow63) {
    spilled = 0x8000000000000000ULL;
  } else {
    spilled = static_cast<uint64_t>(static_cast<int64_t>(widened));
  }
  return static_cast<uint8_t>(spilled & 0xFFu);
}

/**
 * EVENT_PLAY: publish the stream's format onto the player and mark it playing.
 *
 * Lifted from sub_82B28B78. Order is the original's: clear the decoder pointer, convert the
 * format byte, store the sample rate, mark the state playing, convert the channel count,
 * then write the zero word and the format byte through the source pointer at +0x50.
 *
 * The original then reloads the state byte it has just set to 1 and calls sub_82B29018 only
 * if that byte reads 4 or 0 -- which it cannot, unless the two stores through +0x50 overlap
 * the byte. The branch survives in the binary because the compiler could not prove the
 * pointer does not alias the field. Reproduced, reload included, rather than folded away:
 * the job is to match the original, and the harness is what decides whether it does.
 *
 * The float conversions are the ones the lifted body performs, so a NaN in the record would
 * trap on the audio worker thread (MXCSR 0x0000) in both. Deliberately not masked: masking
 * here would make this body differ from the original it is being checked against.
 */
void NativeEventPlay(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t player = REX_LOAD_U32(record + kRecordPlayer);

  // Cleared, not torn down -- EVENT_PLAY runs before a decoder exists.
  REX_STORE_U32(player + kPlayerDecoder, 0);

  REX_STORE_U8(player + kPlayerFormatIndex,
               TruncatedLowByte(BitsToFloat(REX_LOAD_U32(record + kRecordPlayFormat))));

  // The original loads the rate with lfs and stores it with stfs: a float -> double ->
  // float round trip, lossless except that it quiets a signalling NaN. Kept as a round
  // trip for exactly that reason, rather than copied as a word.
  const float rate =
      static_cast<float>(static_cast<double>(BitsToFloat(REX_LOAD_U32(record + kRecordPlayRate))));
  uint32_t rate_bits;
  std::memcpy(&rate_bits, &rate, sizeof(rate_bits));

  const uint32_t source = REX_LOAD_U32(player + kPlayerSource);
  REX_STORE_U32(player + kPlayerSampleRate, rate_bits);
  REX_STORE_U8(player + kPlayerState, 1);
  REX_STORE_U8(player + kPlayerChannelCount,
               TruncatedLowByte(BitsToFloat(REX_LOAD_U32(record + kRecordPlayChannels))));

  REX_STORE_U32(source, 0);
  REX_STORE_U8(source + 4, REX_LOAD_U8(player + kPlayerFormatIndex));

  const uint8_t state = REX_LOAD_U8(player + kPlayerState);
  if (state == 4 || state == 0) {
    ctx.lr = 0x82B28C04;
    sub_82B29018(ctx, base);
  }

  ctx.r3.u64 = kPlayRecordSize;
}

skate3::audio::ShadowStats g_event_submit_stats{"EVENT_SUBMIT"};
skate3::audio::ShadowStats g_event_stop_stats{"EVENT_STOP"};
std::atomic<uint64_t> g_event_stop_unverifiable{0};
skate3::audio::ShadowStats g_event_play_stats{"EVENT_PLAY"};
std::atomic<uint64_t> g_event_play_unverifiable{0};

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
      // Reported, not just counted: a divergence figure with an invisible skip count reads
      // as "verified" when most calls were never compared.
      const uint64_t skipped = g_event_stop_unverifiable.fetch_add(1, std::memory_order_relaxed) + 1;
      if (skipped == 1 || skipped == 16 || skipped == 256 || (skipped % 1024) == 0) {
        REXLOG_INFO("skate3-audio-shadow: EVENT_STOP not comparable on {} calls so far "
                    "(live decoder or a FIFO longer than the window list)", skipped);
      }
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

extern "C" REX_FUNC(sub_82B28B78) {
  if (skate3::audio::ShadowEnabled()) {
    const uint32_t record = ctx.r3.u32;
    const uint32_t player = REX_LOAD_U32(record + kRecordPlayer);
    const uint32_t source = REX_LOAD_U32(player + kPlayerSource);
    // sub_82B29018 reaches two indirect calls and three further functions, so if the
    // original takes that branch the harness cannot rewind what it did. The branch is
    // reachable only when the stores through +0x50 overlap the state byte at +0x15E, since
    // the function sets that byte to 1 immediately before reloading it. Predict the overlap
    // rather than asserting it never happens, and report what gets skipped -- a divergence
    // figure with an invisible skip count reads as "verified" when nothing was compared.
    const uint32_t state_ea = player + kPlayerState;
    const bool overlaps_state = state_ea >= source && state_ea <= source + 4;
    if (source == 0 || overlaps_state) {
      const uint64_t skipped =
          g_event_play_unverifiable.fetch_add(1, std::memory_order_relaxed) + 1;
      if (skipped == 1 || skipped == 16 || skipped == 256 || (skipped % 1024) == 0) {
        REXLOG_INFO("skate3-audio-shadow: EVENT_PLAY not comparable on {} calls so far "
                    "(null source, or a source pointer overlapping the state byte)", skipped);
      }
      __imp__sub_82B28B78(ctx, base);
      return;
    }
    // EVENT_PLAY fires once per stream start, so a boot that plays one frontend movie
    // yields exactly one comparison. Record which input that one call covered, rather than
    // letting "zero divergence" stand for a distribution of one unnamed point.
    static std::atomic<bool> s_logged_input{false};
    bool expected = false;
    if (s_logged_input.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      const float format = BitsToFloat(REX_LOAD_U32(record + kRecordPlayFormat));
      const float rate = BitsToFloat(REX_LOAD_U32(record + kRecordPlayRate));
      const float channels = BitsToFloat(REX_LOAD_U32(record + kRecordPlayChannels));
      REXLOG_INFO("skate3-audio-shadow: EVENT_PLAY input player={:08X} source={:08X} "
                  "format={} -> {} rate={} channels={} -> {}",
                  player, source, double(format), unsigned(TruncatedLowByte(format)),
                  double(rate), double(channels), unsigned(TruncatedLowByte(channels)));
    }
    ShadowWindow windows[2] = {{player + kPlayerDecoder, kPlayerPlaySpan}, {source, 5}};
    skate3::audio::ShadowCompare(ctx, base, NativeEventPlay, __imp__sub_82B28B78,
                                 {windows, 2}, skate3::audio::kReturnR3, g_event_play_stats);
    return;
  }
  if (UseNative()) {
    static std::atomic<uint64_t> native_runs{0};
    const uint64_t runs = native_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runs == 1 || runs == 16 || runs == 256 || runs == 1024) {
      REXLOG_INFO("skate3-audio-native: EVENT_PLAY native runs={}", runs);
    }
    NativeEventPlay(ctx, base);
    return;
  }
  __imp__sub_82B28B78(ctx, base);
}
