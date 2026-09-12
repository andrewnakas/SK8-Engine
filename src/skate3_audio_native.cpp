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
    skate3_audio_buffer_size_guard, true, "Skate 3",
    "Clamp a negative buffer length to zero before rwaudio_InitBufferPair uses it.\n"
    "\n"
    "Bug 3 (docs/buffer-size-bug.md): sub_82B7F8A8 validates the two descriptor STATUS words "
    "for non-negativity but validates the two sizes only as an unsigned SUM, so one negative "
    "size masked by a positive one reaches memset(buf, 0, -12) -- 0xFFFFFFF4 as a size_t, the "
    "reported 4 GB walk. The upstream fix belongs in that caller, which the shadow harness "
    "cannot bracket, so this guards the point of damage instead. It sits before the hook "
    "dispatches, so normal calls stay bit-identical and shadow verification remains valid.");

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
constexpr uint32_t kStopRecordSize = 8;

// The command queue the producer appends to, reached from the System object in r3.
constexpr uint32_t kSystemQueue = 8;         // System -> queue object
constexpr uint32_t kQueueBuffer = 48;        // queue -> record buffer base
constexpr uint32_t kQueueWriteOffset = 204;  // queue -> write offset; published FIRST

// The params struct the producer reads its payload out of. Source stride is 8, destination
// stride 4, so these are not the same offsets as the record's.
constexpr uint32_t kParamsFirst = 4;
constexpr uint32_t kParamsSecond = 12;
constexpr uint32_t kParamsThird = 20;
constexpr uint32_t kParamsSentinel = 8;   // the fourth path writes here
constexpr uint32_t kParamsConstant = 12;  // ...and here

// The fourth path scans the player's own 20-entry table rather than appending.
constexpr uint32_t kPlayerVoiceTable = 0x54;
constexpr uint32_t kVoiceTableEntries = 20;
constexpr uint32_t kVoiceTableStride = 12;
constexpr uint32_t kVoiceDiscriminator = 9;  // entry+9, i.e. +0x5D for entry 0

// Handler addresses, derived the way the original derives them (lis then addi) so that a
// misread of the lifted body fails the build instead of shipping quietly. The expected
// values come from the three consumers, read independently of this function.
constexpr uint32_t kHandlerBase = static_cast<uint32_t>(-2102198272);
constexpr uint32_t kHandlerPlay = kHandlerBase + static_cast<uint32_t>(-29832);
constexpr uint32_t kHandlerStop = kHandlerBase + static_cast<uint32_t>(-29672);
constexpr uint32_t kHandlerSubmit = kHandlerBase + static_cast<uint32_t>(-29504);
static_assert(kHandlerPlay == 0x82B28B78u, "selector 0 must dispatch to EVENT_PLAY");
static_assert(kHandlerStop == 0x82B28C18u, "selector 1 must dispatch to EVENT_STOP");
static_assert(kHandlerSubmit == 0x82B28CC0u, "selector 2 must dispatch to EVENT_SUBMIT");

// The fourth path's two .rdata floats, and the NaN payload it marks the params with.
constexpr uint32_t kVoiceLiveConstant = static_cast<uint32_t>(-2112487424) + 23056u;
constexpr uint32_t kVoiceGoneConstant =
    static_cast<uint32_t>(-2110652416) + static_cast<uint32_t>(-22460);
static_assert(kVoiceLiveConstant == 0x82165A10u, "live constant address");
static_assert(kVoiceGoneConstant == 0x8231A844u, "gone constant address");
constexpr uint32_t kInvalidSentinel = 0x7FF7FFF1u;  // lis 32759; ori 65521

// Scheduler entry requeue (sub_82B48B28). The bucket is scheduler + (state << 5).
constexpr uint32_t kElementNode = 0;      // element -> the list node it owns
constexpr uint32_t kElementCleared = 16;  // always zeroed on the way out
constexpr uint32_t kElementState = 20;    // state byte; 3 means "nothing to do"
constexpr uint32_t kStateParked = 3;
constexpr uint32_t kBucketShift = 5;
constexpr uint32_t kBucketFrom = 16;  // head of the list the node leaves
constexpr uint32_t kBucketTo = 20;    // head of the list it joins
constexpr uint32_t kNodeNext = 0;
constexpr uint32_t kNodePrev = 4;
constexpr uint32_t kNodeLinked = 12;  // nonzero while the node is on a list

// Buffer-pair init (sub_82B7F828). The object's written span is +0..+39 inclusive.
constexpr uint32_t kPairFirst = 0;       // first buffer pointer
constexpr uint32_t kPairFirstLen = 8;    // ...and its length
constexpr uint32_t kPairSecond = 20;     // second buffer pointer
constexpr uint32_t kPairSecondLen = 28;  // ...and its length
constexpr uint32_t kPairSpan = 40;
// Half the harness budget, as margin. Measured lengths are 192-400 bytes with maxima
// (400,256), so this is ~50x observed -- but the guard exists because the maxima describe the
// calls seen, not the function's range, and an uncovered native write is never rewound.
constexpr uint32_t kPairWatchCap = 32 * 1024;

bool BufferSizeGuardEnabled() {
  static const bool on = REXCVAR_GET(skate3_audio_buffer_size_guard);
  return on;
}

std::atomic<uint64_t> g_bufpair_negative{0};

/**
 * Bug 3's guard, applied to the incoming registers before any body runs.
 *
 * A negative length here means sub_82B7F8A8's sum check let one through: it tests the two
 * descriptor status words for non-negativity and the two sizes only as an unsigned sum, so
 * sizeA = -12 with sizeB = 1000 sums to 988 and passes. The fill would then be
 * memset(buf, 0, 0xFFFFFFF4).
 *
 * Clamping before the dispatch keeps every mode consistent: the original and the native body
 * see the same inputs and still agree, so this does not invalidate the comparison, and the
 * length recorded in the object becomes 0 rather than a claim about a buffer that was never
 * zeroed. That is weaker than validating upstream -- the caller still believes it received a
 * buffer -- and it is what is reachable in a function the harness can actually check.
 */
void GuardBufferLengths(PPCContext& __restrict ctx) {
  if (!BufferSizeGuardEnabled()) {
    return;
  }
  const int32_t first = static_cast<int32_t>(ctx.r7.u32);
  const int32_t second = static_cast<int32_t>(ctx.r5.u32);
  if (first >= 0 && second >= 0) {
    return;
  }
  const uint64_t n = g_bufpair_negative.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("skate3-audio: bug 3 fired - negative buffer length ({}, {}) clamped to zero "
              "(occurrence {}); sub_82B7F8A8's sum check passed a negative size",
              first, second, n);
  if (first < 0) {
    ctx.r7.u64 = 0;
  }
  if (second < 0) {
    ctx.r5.u64 = 0;
  }
}

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

/**
 * `lfs` then `stfs`: float -> double -> float. Lossless except that it quiets a signalling
 * NaN, so it is reproduced rather than copied as a word.
 */
uint32_t RoundTripFloatBits(uint32_t bits) {
  const float narrowed = static_cast<float>(static_cast<double>(BitsToFloat(bits)));
  uint32_t out;
  std::memcpy(&out, &narrowed, sizeof(out));
  return out;
}

/**
 * The command queue producer (sub_82B28A00), a leaf: four paths, no calls at all.
 *
 * Selectors 0/1/2 append a record whose first word is the consumer's own address and whose
 * length the consumer implies -- 20 bytes for EVENT_PLAY, 8 for EVENT_STOP, 12 for
 * EVENT_SUBMIT. Those sizes and handlers match the three consumers read independently.
 *
 * The write offset at +204 is published BEFORE the handler and payload are stored. That is
 * bug 1 (`docs/command-queue.md`): a consumer seeing the advanced offset cannot know the
 * record's length, because the length is implied by a handler written afterwards, so one
 * torn append desynchronises the rest of the queue rather than corrupting one record.
 * Reproduced exactly, store order included. Fixing it here would diverge from the original
 * by construction, which is the one thing the harness cannot tell apart from a porting bug;
 * the fix lands as its own change once this body is verified.
 *
 * Any other selector does not touch the queue. It asks whether the packet at params+4 is
 * still live -- walking the submitted-packet FIFO at +0x148 through next at +0xC, the same
 * list EVENT_STOP unlinks, then the 20-entry table at +0x54 -- and writes a constant plus
 * the NaN payload 0x7FF7FFF1 into the caller's params instead. A table hit only counts as
 * live when its discriminator byte is not 2.
 */
void NativeCommandEnqueue(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t player = ctx.r3.u32;
  const uint32_t selector = ctx.r4.u32;
  const uint32_t params = ctx.r5.u32;

  if (selector <= 2) {
    const uint32_t queue = REX_LOAD_U32(player + kSystemQueue);
    const uint32_t offset = REX_LOAD_U32(queue + kQueueWriteOffset);
    const uint32_t record = REX_LOAD_U32(queue + kQueueBuffer) + offset;
    const uint32_t size =
        selector == 0 ? kPlayRecordSize : (selector == 1 ? kStopRecordSize : kSubmitRecordSize);
    const uint32_t handler =
        selector == 0 ? kHandlerPlay : (selector == 1 ? kHandlerStop : kHandlerSubmit);

    REX_STORE_U32(queue + kQueueWriteOffset, offset + size);
    REX_STORE_U32(record, handler);
    REX_STORE_U32(record + kRecordPlayer, player);
    if (selector == 0) {
      REX_STORE_U32(record + kRecordPlayFormat,
                    RoundTripFloatBits(REX_LOAD_U32(params + kParamsFirst)));
      REX_STORE_U32(record + kRecordPlayRate,
                    RoundTripFloatBits(REX_LOAD_U32(params + kParamsSecond)));
      REX_STORE_U32(record + kRecordPlayChannels,
                    RoundTripFloatBits(REX_LOAD_U32(params + kParamsThird)));
    } else if (selector == 2) {
      REX_STORE_U32(record + kRecordPacket, REX_LOAD_U32(params + kParamsFirst));
    }
    return;
  }

  const uint32_t wanted = REX_LOAD_U32(params + kParamsFirst);
  bool live = false;
  for (uint32_t node = REX_LOAD_U32(player + kPlayerPacketHead); node != 0;
       node = REX_LOAD_U32(node + kPacketNext)) {
    if (node == wanted) {
      live = true;
      break;
    }
  }
  if (!live) {
    for (uint32_t i = 0; i < kVoiceTableEntries; i++) {
      const uint32_t entry = player + kPlayerVoiceTable + i * kVoiceTableStride;
      if (REX_LOAD_U32(entry) == wanted) {
        live = REX_LOAD_U8(entry + kVoiceDiscriminator) != 2;
        break;
      }
    }
  }
  REX_STORE_U32(params + kParamsConstant,
                RoundTripFloatBits(REX_LOAD_U32(live ? kVoiceLiveConstant : kVoiceGoneConstant)));
  REX_STORE_U32(params + kParamsSentinel, kInvalidSentinel);
}

/**
 * sub_82B48B28: move a scheduler element's node onto the bucket list for its state.
 *
 * A leaf, deterministic, and every address it writes derives from state readable before the
 * call -- the neighbour pointers are read before they are overwritten. That last property is
 * what makes it comparable at all: the hook can enumerate every window up front without
 * walking anything unbounded, which is exactly what sub_82B482F8 (the caller) cannot do.
 *
 * r3 is the scheduler, r4 the element. State 3 returns immediately, writing nothing at all.
 * Otherwise the node is unlinked from the list headed at bucket+16 -- advancing that head if
 * the node is it, then patching both neighbours -- and pushed onto the list at bucket+20.
 */
void NativeSchedulerRequeue(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t scheduler = ctx.r3.u32;
  const uint32_t element = ctx.r4.u32;

  const uint32_t state = REX_LOAD_U8(element + kElementState);
  if (state == kStateParked) {
    return;  // the original returns before touching anything, +16 included
  }

  const uint32_t node = REX_LOAD_U32(element + kElementNode);
  const uint32_t bucket = scheduler + (state << kBucketShift);

  if (REX_LOAD_U8(node + kNodeLinked) != 0) {
    if (REX_LOAD_U32(bucket + kBucketFrom) == node) {
      REX_STORE_U32(bucket + kBucketFrom, REX_LOAD_U32(node + kNodeNext));
    }
    const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
    if (prev != 0) {
      REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));
    }
    const uint32_t next = REX_LOAD_U32(node + kNodeNext);
    if (next != 0) {
      REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));
    }
    // The original loads bucket+20 twice with no store in between, so one read is the same
    // value both times.
    const uint32_t head = REX_LOAD_U32(bucket + kBucketTo);
    REX_STORE_U32(node + kNodePrev, 0);
    REX_STORE_U32(node + kNodeNext, head);
    if (head != 0) {
      REX_STORE_U32(head + kNodePrev, node);
    }
    REX_STORE_U32(bucket + kBucketTo, node);
    REX_STORE_U8(node + kNodeLinked, 0);
  }
  REX_STORE_U32(element + kElementCleared, 0);
}

/**
 * sub_82B7F828: initialise a buffer pair -- record each buffer with its length, zero the four
 * bookkeeping words, and zero-fill each buffer that is present.
 *
 * Passes all three gates, measured: its only callee is sub_82F52040, a leaf confirmed to be
 * memset; the written set is the object's 40-byte span plus the two buffers, whose lengths
 * measured 192 and 196 on the first call with maxima 400 and 256; and it is deterministic.
 *
 * The fills call the guest memset on an isolated context copy rather than using a host
 * memset. A host zero-fill would write identical bytes -- zero is endian-agnostic -- but the
 * guest routine's behaviour at length 0 is not obvious from its alignment preamble, and no
 * observed call has length 0. Calling the original makes the edge cases identical by
 * construction instead of by my reading of them.
 */
void NativeBufferPairInit(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t second = ctx.r4.u32;
  const uint32_t second_len = ctx.r5.u32;
  const uint32_t first = ctx.r6.u32;
  const uint32_t first_len = ctx.r7.u32;

  REX_STORE_U32(object + kPairFirst, first);
  REX_STORE_U32(object + kPairFirstLen, first_len);
  REX_STORE_U32(object + 4, 0);
  REX_STORE_U32(object + 12, 0);
  REX_STORE_U32(object + 16, 0);
  if (first != 0) {
    PPCContext fill = ctx;
    fill.r3.u64 = first;
    fill.r4.s64 = 0;
    fill.r5.u64 = first_len;
    sub_82F52040(fill, base);
  }

  REX_STORE_U32(object + kPairSecond, second);
  REX_STORE_U32(object + kPairSecondLen, second_len);
  REX_STORE_U32(object + 24, 0);
  REX_STORE_U32(object + 32, 0);
  REX_STORE_U32(object + 36, 0);
  if (second != 0) {
    PPCContext fill = ctx;
    fill.r3.u64 = second;
    fill.r4.s64 = 0;
    fill.r5.u64 = second_len;
    sub_82F52040(fill, base);
  }

  ctx.r3.u64 = object;  // the original returns the object it initialised
}

skate3::audio::ShadowStats g_event_submit_stats{"EVENT_SUBMIT"};
skate3::audio::ShadowStats g_event_stop_stats{"EVENT_STOP"};
std::atomic<uint64_t> g_event_stop_unverifiable{0};
skate3::audio::ShadowStats g_event_play_stats{"EVENT_PLAY"};
std::atomic<uint64_t> g_event_play_unverifiable{0};
skate3::audio::ShadowStats g_command_enqueue_stats{"ENQUEUE"};
skate3::audio::ShadowStats g_requeue_stats{"REQUEUE"};
skate3::audio::ShadowStats g_bufpair_stats{"BUFPAIR"};
std::atomic<uint64_t> g_bufpair_oversize{0};

// Measurement state for sub_82B7F828, which memsets two caller-supplied buffers. Whether it
// can ever be shadow-compared depends on those lengths against kMaxWatch (64 KB across all
// windows), so the lengths get measured rather than assumed.
std::atomic<uint64_t> g_bufpair_calls{0};
std::atomic<uint32_t> g_bufpair_first_max{0};
std::atomic<uint32_t> g_bufpair_second_max{0};
std::atomic<uint64_t> g_bufpair_first_null{0};
std::atomic<uint64_t> g_bufpair_second_null{0};

void RecordMax(std::atomic<uint32_t>& slot, uint32_t value) {
  uint32_t seen = slot.load(std::memory_order_relaxed);
  while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}
// Calls per selector: 0 play, 1 stop, 2 submit, 3 the query path. A single total hides
// which paths were actually exercised, and three of the four are appends while the fourth
// mutates caller state instead -- so the split is part of the result, not a detail.
std::atomic<uint64_t> g_enqueue_selector[4]{};

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

extern "C" REX_FUNC(sub_82B28A00) {
  if (skate3::audio::ShadowEnabled()) {
    // A leaf on every path, so unlike EVENT_STOP and EVENT_PLAY there is nothing here the
    // harness cannot replay: no skip counter, and the divergence figure covers every call.
    const uint32_t player = ctx.r3.u32;
    const uint32_t selector = ctx.r4.u32;
    g_enqueue_selector[selector <= 2 ? selector : 3].fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint64_t> seen{0};
    const uint64_t n = seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 2048) == 0) {
      REXLOG_INFO("skate3-audio-shadow: ENQUEUE selectors play={} stop={} submit={} query={}",
                  g_enqueue_selector[0].load(std::memory_order_relaxed),
                  g_enqueue_selector[1].load(std::memory_order_relaxed),
                  g_enqueue_selector[2].load(std::memory_order_relaxed),
                  g_enqueue_selector[3].load(std::memory_order_relaxed));
    }
    ShadowWindow windows[2];
    size_t count = 0;
    if (selector <= 2) {
      const uint32_t queue = REX_LOAD_U32(player + kSystemQueue);
      const uint32_t offset = REX_LOAD_U32(queue + kQueueWriteOffset);
      const uint32_t size =
          selector == 0 ? kPlayRecordSize : (selector == 1 ? kStopRecordSize : kSubmitRecordSize);
      windows[count++] = {queue + kQueueWriteOffset, 4};
      windows[count++] = {REX_LOAD_U32(queue + kQueueBuffer) + offset, size};
    } else {
      // The fourth path writes the caller's params, not the queue.
      windows[count++] = {ctx.r5.u32 + kParamsSentinel, 8};
    }
    skate3::audio::ShadowCompare(ctx, base, NativeCommandEnqueue, __imp__sub_82B28A00,
                                 {windows, count}, skate3::audio::kReturnNone,
                                 g_command_enqueue_stats);
    return;
  }
  if (UseNative()) {
    static std::atomic<uint64_t> native_runs{0};
    const uint64_t runs = native_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runs == 1 || runs == 16 || runs == 256 || runs == 1024) {
      REXLOG_INFO("skate3-audio-native: ENQUEUE native runs={}", runs);
    }
    NativeCommandEnqueue(ctx, base);
    return;
  }
  __imp__sub_82B28A00(ctx, base);
}

extern "C" REX_FUNC(sub_82B48B28) {
  if (skate3::audio::ShadowEnabled()) {
    // Enumerated from pre-call state: the element's cleared word, the bucket's two heads, the
    // node's first 16 bytes (next, prev and the linked flag), and each neighbour that exists.
    // Six windows, about 40 bytes, against a 64 KB budget -- no window is ever dropped, so
    // every byte the function writes is both compared and rewound.
    const uint32_t scheduler = ctx.r3.u32;
    const uint32_t element = ctx.r4.u32;
    const uint32_t state = REX_LOAD_U8(element + kElementState);
    ShadowWindow windows[6];
    size_t count = 0;
    windows[count++] = {element + kElementCleared, 4};
    if (state != kStateParked) {
      const uint32_t node = REX_LOAD_U32(element + kElementNode);
      const uint32_t bucket = scheduler + (state << kBucketShift);
      windows[count++] = {bucket + kBucketFrom, 8};
      windows[count++] = {node + kNodeNext, 16};
      const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
      const uint32_t next = REX_LOAD_U32(node + kNodeNext);
      const uint32_t head = REX_LOAD_U32(bucket + kBucketTo);
      if (prev != 0) windows[count++] = {prev + kNodeNext, 4};
      if (next != 0) windows[count++] = {next + kNodePrev, 4};
      if (head != 0) windows[count++] = {head + kNodePrev, 4};
    }
    skate3::audio::ShadowCompare(ctx, base, NativeSchedulerRequeue, __imp__sub_82B48B28,
                                 {windows, count}, skate3::audio::kReturnNone, g_requeue_stats);
    return;
  }
  if (UseNative()) {
    static std::atomic<uint64_t> native_runs{0};
    const uint64_t runs = native_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runs == 1 || runs == 16 || runs == 256 || runs == 1024) {
      REXLOG_INFO("skate3-audio-native: REQUEUE native runs={}", runs);
    }
    NativeSchedulerRequeue(ctx, base);
    return;
  }
  __imp__sub_82B48B28(ctx, base);
}

/**
 * Measurement only -- no native body, no comparison.
 *
 * sub_82B7F828 zeroes two caller-supplied buffers: memset(r6, 0, r7) when r6 is non-null, then
 * memset(r4, 0, r5) when r4 is non-null (its one callee, sub_82F52040, is a leaf confirmed to
 * be memset). Shadow-comparing it would need windows covering both buffers, and the harness
 * drops windows past a 64 KB total -- dropping the offending window and every one after it --
 * while a write outside the windows is never rewound and so lands in the live game.
 *
 * So the question "can this be ported safely?" is a question about these two lengths, and this
 * hook answers it with numbers. It runs the original and records, nothing else.
 */
extern "C" REX_FUNC(sub_82B7F828) {
  GuardBufferLengths(ctx);  // bug 3: before anything reads the lengths, in every mode
  if (skate3::audio::ShadowEnabled()) {
    const uint32_t first_len = ctx.r7.u32;
    const uint32_t second_len = ctx.r5.u32;
    if (ctx.r6.u32 == 0) {
      g_bufpair_first_null.fetch_add(1, std::memory_order_relaxed);
    } else {
      RecordMax(g_bufpair_first_max, first_len);
    }
    if (ctx.r4.u32 == 0) {
      g_bufpair_second_null.fetch_add(1, std::memory_order_relaxed);
    } else {
      RecordMax(g_bufpair_second_max, second_len);
    }
    const uint64_t n = g_bufpair_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 16 || n == 256 || (n % 1024) == 0) {
      const uint32_t a = g_bufpair_first_max.load(std::memory_order_relaxed);
      const uint32_t b = g_bufpair_second_max.load(std::memory_order_relaxed);
      REXLOG_INFO("skate3-audio-shadow: BUFPAIR calls={} this=({},{}) max=({},{}) sum={} "
                  "budget=65536 null=({},{})",
                  n, first_len, second_len, a, b, uint64_t(a) + uint64_t(b),
                  g_bufpair_first_null.load(std::memory_order_relaxed),
                  g_bufpair_second_null.load(std::memory_order_relaxed));
    }

    // Structural guard, not a measured one: if the span plus both buffers would exceed the
    // cap, the harness would drop a window and the native body's fill would land in the live
    // game unrewound. Run the original and count it instead.
    const uint32_t watched =
        kPairSpan + (ctx.r6.u32 != 0 ? first_len : 0) + (ctx.r4.u32 != 0 ? second_len : 0);
    if (watched > kPairWatchCap) {
      const uint64_t skipped = g_bufpair_oversize.fetch_add(1, std::memory_order_relaxed) + 1;
      if (skipped == 1 || (skipped % 256) == 0) {
        REXLOG_INFO("skate3-audio-shadow: BUFPAIR not comparable on {} calls so far "
                    "(watched {} bytes over the {} cap)", skipped, watched, kPairWatchCap);
      }
      __imp__sub_82B7F828(ctx, base);
      return;
    }

    ShadowWindow windows[3];
    size_t count = 0;
    windows[count++] = {ctx.r3.u32, kPairSpan};
    if (ctx.r6.u32 != 0) windows[count++] = {ctx.r6.u32, first_len};
    if (ctx.r4.u32 != 0) windows[count++] = {ctx.r4.u32, second_len};
    skate3::audio::ShadowCompare(ctx, base, NativeBufferPairInit, __imp__sub_82B7F828,
                                 {windows, count}, skate3::audio::kReturnR3, g_bufpair_stats);
    return;
  }
  if (UseNative()) {
    static std::atomic<uint64_t> native_runs{0};
    const uint64_t runs = native_runs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (runs == 1 || runs == 16 || runs == 256) {
      REXLOG_INFO("skate3-audio-native: BUFPAIR native runs={}", runs);
    }
    NativeBufferPairInit(ctx, base);
    return;
  }
  __imp__sub_82B7F828(ctx, base);
}
