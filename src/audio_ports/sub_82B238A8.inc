// sub_82B238A8  role: advance a voice's gain ramp, then scale all its channels by the ramp   lifted: skate3_recomp.67.cpp:14104 (420 lines)
// STATUS: verified
// calls/session: boot 49886  play 45734  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B238A8.md
namespace port_82B238A8 {

// Voice state (r3). Nothing in docs/rw_audio_structs.h names this object, so the offsets stay
// numeric; the comments are what the code does with them, not established field names.
constexpr uint32_t kChannelCount = 0x29;   // u8   channels to scale
constexpr uint32_t kLastWord = 0x30;       // u32  stamped 0x7FF7FFF1 on the way out
constexpr uint32_t kLastGain = 0x34;       // f32  copy of the block's final envelope sample
constexpr uint32_t kStartTime = 0x38;      // f64  latched into kRampTime on a start request
constexpr uint32_t kRequestSpan = 0x40;    // f32  requested ramp length, in seconds
constexpr uint32_t kRequestExtra = 0x44;   // f32  second curve parameter
constexpr uint32_t kRequestCurve = 0x48;   // u32  curve selector, narrowed to kCurve
constexpr uint32_t kRampTime = 0x50;       // f64  ramp start time
constexpr uint32_t kRampSpanCopy = 0x58;   // f32  copy of kRequestSpan
constexpr uint32_t kRampSamples = 0x5C;    // s32  ramp length in samples, at least 1
constexpr uint32_t kRampCursor = 0x60;     // s32  samples produced so far; starts negative
constexpr uint32_t kRampFrom = 0x64;       // f32  gain the ramp starts from
constexpr uint32_t kRampExtra = 0x68;      // f32  copy of kRequestExtra
constexpr uint32_t kCurrentGain = 0x6C;    // f32  gain now
constexpr uint32_t kStartPending = 0x70;   // u8   1 = a start request is waiting
constexpr uint32_t kRampState = 0x71;      // u8   0 = steady, 1 = armed, 2 = running
constexpr uint32_t kCurve = 0x72;          // u8   which of the three generators to call

// Mix descriptor (r4).
constexpr uint32_t kDescTimeBase = 0x10;   // f64  subtracted from kRampTime
constexpr uint32_t kDescChannels = 0x1C;   // u32  -> channel-buffer descriptor
constexpr uint32_t kDescMixer = 0x20;      // u32  -> object whose +4 is the envelope buffer
constexpr uint32_t kDescClock = 0x28;      // u32  -> object whose +12 is samples per second

// Channel-buffer descriptor (r4->kDescChannels), same layout sub_82B373C8 takes as its r4.
constexpr uint32_t kChanBase = 4;          // u32  float* of channel 0
constexpr uint32_t kChanStride = 14;       // u16  floats between channels

// One audio block: 256 floats of envelope, 256 floats per channel.
constexpr uint32_t kBlockFloats = 256;
constexpr uint32_t kBlockBytes = kBlockFloats * 4;

// 0x82300000 - 31232 + 256: the f64 kRampTime is compared against, meaning unknown.
constexpr uint32_t kRampTimeSentinel = 0x822F8700;
// 0x82320000 - 22460: the f32 kCurrentGain is compared against, meaning unknown.
constexpr uint32_t kGainSentinel = 0x8231A844;

// Spans: the state window, the envelope, and one per channel. 32 spans and 32 KB both cap the
// channel count at 30; the two stack scratch arrays in sub_82B373C8 suggest the real bound is 8.
constexpr uint32_t kMaxChannels = 30;

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: narrowed back to a single and stored. The round trip through float is kept rather than
// copying the word, because it is not a no-op on a signalling NaN.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fmuls: the double product rounded to single, as the lifted line does it.
inline double MulSingle(double a, double b) {
  return static_cast<double>(static_cast<float>(a * b));
}

// fctiwz then stfd/lwz of the LOW word: NaN gives 0x80000000, above INT_MAX saturates, and only
// then CVTTSD2SI (which yields 0x80000000 below INT_MIN on its own).
inline uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(fixed);
}

// lvx128: a 16-byte load with the address masked and RexGlue's big-endian lane reversal.
simde__m128 LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// stvx128: the same masking and the same reversal on the way out.
void StoreVector(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

REX_FUNC(Native) {
  const uint32_t state = ctx.r3.u32;  // mr r31,r3
  const uint32_t desc = ctx.r4.u32;

  // lwz r11,40(r4) ; lfs f12,12(r11) -- unconditional in the original, so it faults on a bad
  // clock pointer whether or not the value is used.
  ctx.fpscr.disableFlushModeUnconditional();
  const double rate = LoadSingle(base, REX_LOAD_U32(desc + kDescClock) + 12);

  // lbz r10,112(r3) ; cmplwi cr6,r10,1 ; bne cr6,0x82b23924
  if (REX_LOAD_U8(state + kStartPending) == 1) {
    // Latch a start request: copy the request fields into the ramp fields and arm the ramp.
    const double span = LoadSingle(base, state + kRequestSpan);     // lfs f0,64(r3)
    const uint32_t curve = REX_LOAD_U32(state + kRequestCurve);     // lwz r10,72(r3)
    const double samples_f = MulSingle(span, rate);                 // fmuls f13,f0,f12
    const double gain = LoadSingle(base, state + kCurrentGain);     // lfs f11,108(r3)
    const uint64_t start = REX_LOAD_U64(state + kStartTime);        // lfd f10,56(r3)
    const double extra = LoadSingle(base, state + kRequestExtra);   // lfs f9,68(r3)
    StoreSingle(base, state + kRampFrom, gain);                     // stfs f11,100(r3)
    REX_STORE_U64(state + kRampTime, start);                        // stfd f10,80(r3)
    REX_STORE_U8(state + kCurve, uint8_t(curve));                   // stb r10,114(r3)
    StoreSingle(base, state + kRampSpanCopy, span);                 // stfs f0,88(r3)
    StoreSingle(base, state + kRampExtra, extra);                   // stfs f9,104(r3)
    // fctiwz f8,f13 ; stfd f8,80(r1) ; lwz r9,84(r1) -- the low word of the conversion.
    const uint32_t samples = FctiwzLowWord(samples_f);
    REX_STORE_U32(state + kRampSamples, samples);                     // stw r9,92(r3)
    // bgt cr6,0x82b2391c ; stw r11,92(r3) -- a zero or negative length becomes one sample.
    if (int32_t(samples) <= 0) REX_STORE_U32(state + kRampSamples, 1);
    REX_STORE_U8(state + kRampState, 1);                              // stb r11,113(r31)
    REX_STORE_U8(state + kStartPending, 0);                           // stb r29,112(r31)
  }

  // lbz r11,113(r31) ; cmplwi cr6,r11,1 ; bne cr6,0x82b2399c
  if (REX_LOAD_U8(state + kRampState) == 1) {
    // Armed: place the cursor from the clock, or drop the ramp if it has already finished.
    ctx.fpscr.disableFlushModeUnconditional();
    PPCRegister when{}, sentinel{}, position{};
    when.u64 = REX_LOAD_U64(state + kRampTime);                       // lfd f13,80(r31)
    sentinel.u64 = REX_LOAD_U64(kRampTimeSentinel);                   // lfd f0,256(r10)
    position.f64 = sentinel.f64;                                      // f0 survives when equal
    // fcmpu cr6,f13,f0 ; beq cr6,0x82b23950 ; lfd f0,16(r4) ; fsub f0,f13,f0
    if (!(when.f64 == sentinel.f64)) {
      PPCRegister time_base{};
      time_base.u64 = REX_LOAD_U64(desc + kDescTimeBase);
      position.f64 = when.f64 - time_base.f64;
    }
    ctx.fpscr.disableFlushModeUnconditional();
    const double elapsed = rate * position.f64;                       // fmul f0,f12,f0
    // fctiwz f13,f0 ; stfd f13,80(r1) ; lwz r11,84(r1) ; cmpwi cr6,r11,256 ; bge -> skip
    const uint32_t raw = FctiwzLowWord(elapsed);
    if (int32_t(raw) < 256) {
      const uint32_t samples = REX_LOAD_U32(state + kRampSamples);    // lwz r10,92(r31)
      const uint32_t cursor = uint32_t(-int64_t(uint64_t(raw)));      // neg r11,r11
      const uint32_t last = samples - 1u;                             // addi r10,r10,-1
      REX_STORE_U32(state + kRampCursor, cursor);                     // stw r11,96(r31)
      // cmpw cr6,r11,r10 ; bgt cr6,0x82b23998
      if (int32_t(cursor) > int32_t(last)) {
        REX_STORE_U8(state + kRampState, 0);                          // stb r29,113(r31)
      } else {
        // cmpwi cr6,r11,0 ; ble cr6,0x82b2398c ; stw r29,96(r31)
        if (int32_t(cursor) > 0) REX_STORE_U32(state + kRampCursor, 0);
        REX_STORE_U8(state + kRampState, 2);                          // li r11,2 ; stb r11,113
      }
    }
  }

  // lwz r10,32(r4) ; lbz r11,113(r31) ; lwz r30,28(r4) ; lwz r28,4(r10)
  const uint32_t mixer = REX_LOAD_U32(desc + kDescMixer);
  const uint32_t ramp_state = REX_LOAD_U8(state + kRampState);
  const uint32_t channels = REX_LOAD_U32(desc + kDescChannels);
  const uint32_t envelope = REX_LOAD_U32(mixer + 4);

  // cmplwi cr6,r11,0 ; beq -> 0x82b23a18 ; cmplwi cr6,r11,1 ; beq -> 0x82b23a18
  if (ramp_state != 0 && ramp_state != 1) {
    // A running ramp: one of three generators writes this block's 256 envelope samples.
    const uint32_t curve = REX_LOAD_U8(state + kCurve);            // lbz r11,114(r31)
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f2.f64 = LoadSingle(base, state + kRampExtra);             // lfs f2,104(r31)
    ctx.f1.f64 = LoadSingle(base, state + kRampFrom);              // lfs f1,100(r31)
    const uint32_t samples = REX_LOAD_U32(state + kRampSamples);   // lwz r7,92(r31)
    const uint32_t cursor = REX_LOAD_U32(state + kRampCursor);     // lwz r6,96(r31)
    // r4 and r5 are forwarded from this function's own entry: the lifted body writes
    // neither, so the generator sees all 64 bits of each exactly as this call received them.
    const uint64_t arg_desc = ctx.r4.u64;
    const uint64_t r5 = ctx.r5.u64;
    if (curve == 0) {
      skate3::audio::GuestCall(ctx, base, sub_82B427D8, envelope, arg_desc, r5, cursor, samples);
    } else if (curve == 1) {  // cmplwi cr6,r11,1 ; bne cr6,0x82b239f4
      skate3::audio::GuestCall(ctx, base, sub_82B42C98, envelope, arg_desc, r5, cursor, samples);
    } else {
      skate3::audio::GuestCall(ctx, base, sub_82B43340, envelope, arg_desc, r5, cursor, samples);
    }
    // lwz r11,96(r31) ; lwz r10,92(r31) ; addi r11,r11,256 ; stw r11,96(r31)
    const uint32_t advanced = REX_LOAD_U32(state + kRampCursor) + kBlockFloats;
    const uint32_t samples_now = REX_LOAD_U32(state + kRampSamples);
    REX_STORE_U32(state + kRampCursor, advanced);
    // cmpw cr6,r11,r10 ; blt cr6,0x82b23a7c ; stb r29,113(r31)
    if (int32_t(advanced) >= int32_t(samples_now)) REX_STORE_U8(state + kRampState, 0);
  } else {
    // No ramp running: hold the current gain. Nothing to do at all if it is the sentinel.
    ctx.fpscr.disableFlushModeUnconditional();
    const double gain = LoadSingle(base, state + kCurrentGain);    // lfs f13,108(r31)
    const double sentinel = LoadSingle(base, kGainSentinel);       // lfs f0,-22460(r11)
    // fcmpu cr6,f13,f0 ; beq cr6,0x82b23b40 -- straight to the exit, no stores at all.
    if (gain == sentinel) {
      ctx.r3.s64 = 1;  // li r3,1
      return;
    }
    // Fill all 256 envelope samples with the held gain, eight per pass, reloading the gain
    // before every store exactly as the original does.
    uint32_t cursor = envelope - 4;                                   // addi r11,r28,-4
    for (int pass = 0; pass < 32; pass++) {                           // li r10,32 ; mtctr ; bdnz
      ctx.fpscr.disableFlushModeUnconditional();
      for (uint32_t slot = 4; slot <= 32; slot += 4)                  // lfs f*,108(r31) ; stfs
        StoreSingle(base, cursor + slot, LoadSingle(base, state + kCurrentGain));
      cursor += 32;                                                   // stfsu f7,32(r11)
    }
  }

  // lbz r11,41(r31) ; mr r3,r29 ; cmplwi cr6,r11,0 ; beq cr6,0x82b23b28
  uint32_t count = REX_LOAD_U8(state + kChannelCount);
  if (count != 0) {
    const uint32_t row = envelope + 48;  // addi r29,r28,48
    uint32_t channel = 0;                // r3, the loop index
    do {
      // lhz r10,14(r30) ; lwz r9,4(r30) -- both reloaded on every channel.
      const uint32_t stride = REX_LOAD_U16(channels + kChanStride);
      const uint32_t first = REX_LOAD_U32(channels + kChanBase);
      // mullw r7,r10,r3 ; rlwinm r11,r7,2,0,29 -- the offset of this channel, in bytes.
      const uint32_t index = uint32_t(int32_t(stride) * int32_t(channel));
      const uint32_t start = first + ((index << 2) & 0xFFFFFFFCu);    // add r9,r11,r9
      uint32_t dst = start + 32;                                      // addi r11,r9,32
      const uint32_t bias = envelope - start;                         // subf r9,r9,r28
      uint32_t src = row;                                             // mr r10,r29
      for (int quad = 0; quad < 16; quad++) {                         // mtctr r4 (16) ; bdnz
        // Four 16-byte products per pass, covering 64 bytes of the channel. The envelope is
        // always the first multiply operand: on a NaN lane the winning operand slot is part of
        // the semantics (docs/vmx128-exactness.md rule 4), so the order is not free.
        const simde__m128 chan0 = LoadVector(base, dst - 32);         // lvx128 v63,r11,r8
        const simde__m128 env0 = LoadVector(base, src - 48);          // lvx128 v62,r10,r7
        ctx.fpscr.enableFlushModeUnconditional();
        const simde__m128 out0 = simde_mm_mul_ps(env0, chan0);        // vmulfp128 v61,v62,v63
        StoreVector(base, dst - 32, out0);                            // stvx128 v61,r11,r6
        const simde__m128 chan1 = LoadVector(base, dst - 16);         // lvx128 v60,r11,r5
        const simde__m128 env1 = LoadVector(base, src - 32);          // lvx128 v59,r10,r8
        const simde__m128 out1 = simde_mm_mul_ps(env1, chan1);        // vmulfp128 v58,v59,v60
        StoreVector(base, dst - 16, out1);                            // stvx128 v58,r11,r5
        const simde__m128 env2 = LoadVector(base, bias + dst);        // lvx128 v56,r9,r11
        const simde__m128 chan2 = LoadVector(base, dst);              // lvx128 v57,r0,r11
        const simde__m128 out2 = simde_mm_mul_ps(env2, chan2);        // vmulfp128 v55,v56,v57
        StoreVector(base, dst, out2);                                 // stvx128 v55,r0,r11
        const simde__m128 env3 = LoadVector(base, src);               // lvx128 v54,r0,r10
        src += 64;                                                    // addi r10,r10,64
        const simde__m128 chan3 = LoadVector(base, dst + 16);         // lvx128 v53,r11,r4
        const simde__m128 out3 = simde_mm_mul_ps(env3, chan3);        // vmulfp128 v52,v54,v53
        StoreVector(base, dst + 16, out3);                            // stvx128 v52,r11,r4
        dst += 64;                                                    // addi r11,r11,64
      }
      count = REX_LOAD_U8(state + kChannelCount);  // lbz r11,41(r31) -- reloaded each pass
      channel += 1;                                // addi r3,r3,1
    } while (channel < count);                     // cmplw cr6,r3,r11 ; blt cr6,0x82b23a9c
  }

  // Remember the block's last envelope sample as the gain now, and stamp the marker word.
  ctx.fpscr.disableFlushModeUnconditional();
  const double last = LoadSingle(base, envelope + (kBlockBytes - 4));  // lfs f0,1020(r28)
  StoreSingle(base, state + kCurrentGain, last);                       // stfs f0,108(r31)
  StoreSingle(base, state + kLastGain, last);                          // stfs f0,52(r31)
  // lis r11,32759 ; ori r10,r11,65521 ; stw r10,48(r31)
  REX_STORE_U32(state + kLastWord, 0x7FF7FFF1u);
  ctx.r3.s64 = 1;  // li r3,1
}

// True when [a, a+alen) and [b, b+blen) share a byte.
bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

// The enumeration below reads three control fields that the call reloads after storing, so a
// declared span covering one of them would let the addresses change mid-call and make the list
// wrong. Checked per span rather than assumed.
bool BlockClearsControlFields(uint32_t span, uint32_t len, uint32_t state, uint32_t channels) {
  return !Overlaps(span, len, state + kChannelCount, 1) &&
         !Overlaps(span, len, channels + kChanBase, 4) &&
         !Overlaps(span, len, channels + kChanStride, 2);
}

// The write set, from entry state only. It is the SAME set on every path -- state object,
// envelope block, one block per channel -- which is why a ramp that is already running is now
// compared instead of skipped.
//
// The generator path was excluded until 2026-09-12 on the grounds that sub_82B427D8,
// sub_82B42C98 and sub_82B43340 "store through bases they derive internally". They do, but the
// bases derive from nothing except the three arguments this call passes them. Checked on the
// lifted bodies of all three with a flow-sensitive pass over every store: every store address
// outside the callee's own frame carries only entry r3 (the envelope), entry r6 (the cursor) and
// entry r7 (the sample count) -- never a loaded pointer, never a second object. Their three
// helpers cannot widen that: sub_824531C8 has no stores at all, sub_82F4DED0 writes only below
// its own r1, and sub_82F52FB8 (the 16-byte replicate) stores only through its own r3.
//
// The bound is the block, but ONLY for a cursor in range. All three open with
// `r26 = cursor + 255` (`r25` in sub_82B43340) and `r27 = min(cursor + 255, samples - 1)`, then
// write, in order: the pre-ramp fill for a negative cursor, the ramp itself clamped at r27, and
// the post-ramp fill clamped at cursor + 255. Every vector run takes `trunc(n/4)` quads with a
// scalar remainder, so no quad steps past its clamp, and sample i lands at
// envelope + 4*(i - cursor) -- so the set is [envelope, envelope + 1024), the same 256 floats
// the steady-gain fill writes. Two entry states break that, and both are checked below:
//
//   * cursor < -256. The pre-ramp fill is `-cursor` samples long and is NOT clamped to the
//     block (`r29 = -trunc(cursor/4)` units of 16 bytes, straight off the argument), so a
//     cursor of -10^6 fills 4 MB.
//   * cursor > samples - 1. Then `span = r27 - index + 1` is negative, the vector loops are
//     unentered but the index still moves by `4 * trunc(span/4)`, so the post-ramp fill starts
//     BELOW where the ramp would have and runs to cursor + 255 -- longer than the block, without
//     bound as the gap grows. (sub_82B427D8's own port documents the same quirk from the other
//     side, where it has to count those bytes.)
//
// One difference from the steady path: the generators write those floats with stvx128, whose
// effective address is masked down to 16 bytes, so a misaligned envelope would be written from
// its 16-byte floor. Hence the floor below. It is almost certainly moot (the ramp's own
// alignment prologue only makes sense on a 16-byte-aligned buffer) but it costs one span and
// makes the declaration true either way.
//
// What the enumeration still refuses: a pending or armed ramp (see the guard, and the note), a
// running ramp whose cursor is out of range, more than kMaxChannels channels, and any call where
// a span would cover one of the three control fields the call reloads after storing.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t state = ctx.r3.u32;
  const uint32_t desc = ctx.r4.u32;

  // Two ways to reach a generator, and they are windowable to different degrees. A pending start
  // always arms (state = 1) and the placement step then decides, so:
  const uint32_t pending = REX_LOAD_U8(state + kStartPending);
  const uint32_t ramp_state = REX_LOAD_U8(state + kRampState);

  // (1) A pending start, or a ramp already armed: the placement step computes the cursor in-call
  // as `-fctiwz(rate * (rampTime - timeBase))`, and its only guard is `< 256`. That guard admits
  // exactly one dangerous value: 0x80000000, which fctiwz yields for a NaN or for anything at or
  // below INT_MIN, and which `neg` leaves unchanged and the `> 0` clamp does not catch. The
  // generators would then fill from `-cursor = 2^31` samples, i.e. the whole address space.
  // Deciding that here means evaluating the guest's own float arithmetic in a hook, and host
  // float work in a hook is the SIGFPE trap in CLAUDE.md -- these products are precisely the ones
  // that can raise. So this case is refused: the LENGTH is computed by the call. It costs the
  // arming call of each ramp, not the ramp; the note has the measurement and the integer-only
  // alternative if that residue turns out to matter.
  if (pending == 1 || ramp_state == 1) return false;

  // (2) Dispatched straight from the entry state (`ramp_state` neither 0 nor 1, nothing
  // pending): the latch and the placement are both skipped, so the generator sees exactly these
  // two words and the two bounds above are decidable with no float work at all.
  if (ramp_state != 0) {
    const int32_t samples = int32_t(REX_LOAD_U32(state + kRampSamples));
    const int32_t cursor = int32_t(REX_LOAD_U32(state + kRampCursor));
    if (cursor < -int32_t(kBlockFloats)) return false;          // unclamped pre-ramp fill
    if (int64_t(cursor) > int64_t(samples) - 1) return false;    // the negative-span fill
    if (int64_t(cursor) + 255 > INT_MAX) return false;            // addi r26,r6,255 must not wrap
  }

  const uint32_t mixer = REX_LOAD_U32(desc + kDescMixer);
  const uint32_t envelope = REX_LOAD_U32(mixer + 4);
  const uint32_t channels = REX_LOAD_U32(desc + kDescChannels);
  const uint32_t stride = REX_LOAD_U16(channels + kChanStride);
  const uint32_t first = REX_LOAD_U32(channels + kChanBase);
  const uint32_t count = REX_LOAD_U8(state + kChannelCount);
  if (count > kMaxChannels) return false;  // over the span and byte budgets both

  // Everything the function stores in the state object, across all paths: 0x30 through 0x72.
  spec.write(state + kLastWord, kCurve + 1 - kLastWord);

  // All 256 envelope samples, whether a generator writes them or the steady-gain fill does.
  // The floor covers the generators' masked stvx128; the length keeps the top at
  // envelope + 1024, which is where the steady path's last stfs ends.
  const uint32_t envelope_span = envelope & ~0xFu;
  const uint32_t envelope_bytes = kBlockBytes + (envelope & 0xFu);
  if (!BlockClearsControlFields(envelope_span, envelope_bytes, state, channels)) return false;
  spec.write(envelope_span, envelope_bytes);

  // Per channel, the four stvx128 of a pass are 16 bytes apart and every pass steps 64, so the
  // sixteen passes cover exactly 1024 bytes from the channel's 16-byte-aligned start -- the
  // masking in each effective address is what makes the start aligned rather than `first`.
  for (uint32_t j = 0; j < count; j++) {
    const uint32_t index = uint32_t(int32_t(stride) * int32_t(j));
    const uint32_t span = (first + ((index << 2) & 0xFFFFFFFCu)) & ~0xFu;
    if (!BlockClearsControlFields(span, kBlockBytes, state, channels)) return false;
    spec.write(span, kBlockBytes);
  }

  spec.read(desc + kDescTimeBase, 8);
  spec.read(desc + kDescChannels, 4);
  spec.read(desc + kDescMixer, 4);
  spec.read(desc + kDescClock, 4);
  spec.read(REX_LOAD_U32(desc + kDescClock) + 12, 4);
  spec.read(mixer + 4, 4);
  spec.read(channels + kChanBase, 12);  // +4 base and +14 stride
  spec.read(state + kChannelCount, 1);
  spec.read(state + kLastWord, kCurve + 1 - kLastWord);
  spec.read(kGainSentinel, 4);
  spec.read(kRampTimeSentinel, 8);
  // The generators' own reads -- the envelope they re-read through sub_82F52FB8's replicate (a
  // write window, so its entry bytes are recorded anyway) and a handful of rodata singles
  // (0x82165A10 and neighbours) -- are deliberately not declared. `inputs` exists so a recorded
  // vector can be replayed outside the game, and a ramping call cannot be: replaying it means
  // executing the guest generator. See the note.
  return true;
}

}  // namespace port_82B238A8
SKATE3_PORT(82B238A8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
