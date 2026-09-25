// sub_82B22898  role: single-sideband frequency shift over a 256-frame block -- a four-section all-pass Hilbert pair, then out = cos_branch*cos(phase) - sin_branch*sin(phase) with the phase carried in the object, optionally preceded by a band-limiting pre-filter   lifted: skate3_recomp.67.cpp:11489 (2339 lines)
// STATUS: verified
// calls/session: boot 32424  play 32228  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B22898.md
namespace port_82B22898 {

// ---- the SSB object (r3) --------------------------------------------------------------------
// docs/rw_audio_structs.h names none of this layout. +52, +56 and the stream's +28/+32/+40 and a
// descriptor's +4 are the names sub_82B27E20's port already uses for the same fields on the same
// object family; the rest describe what this function does with each slot.
constexpr uint32_t kPoolOwner = 8;           // u32 -> object whose +0 is the scratch pool
constexpr uint32_t kFrequencyInput = 52;     // f32 the shift, in Hz
constexpr uint32_t kAllpassState = 56;       // four 16-byte biquad histories at 56/72/88/104
constexpr uint32_t kBandCoefficients = 128;  // 128 bytes + one float, sub_82B42510's destination
constexpr uint32_t kPhase = 260;             // f32 the oscillator phase carried between blocks
constexpr uint32_t kPreFilterEnable = 264;   // f32 compared against 1.0f; anything else bypasses
constexpr uint32_t kCachedFrequency = 268;   // f32 the frequency the band was last designed from
constexpr uint32_t kPreFilterOffset = 272;   // u16 byte offset of the pre-filter sub-object

// ---- the stream (r4) and its buffer descriptors ---------------------------------------------
constexpr uint32_t kStreamBufferA = 28;  // descriptor read this call; written by the closing swap
constexpr uint32_t kStreamBufferB = 32;  // descriptor written this call
constexpr uint32_t kStreamFormat = 40;
constexpr uint32_t kFormatSampleRate = 12;
constexpr uint32_t kDescBase = 4;  // u32 float* of channel 0

// ---- the scratch pool: a downward bump allocator whose top word is at +32 -------------------
constexpr uint32_t kPoolTop = 32;

constexpr uint32_t kFrameBytes = 240;   // stwu r1,-240(r1)
constexpr uint32_t kBlockFrames = 256;  // li r7,256
constexpr uint32_t kRunBytes = 4 * kBlockFrames;         // one 256-single run
constexpr uint32_t kScratchBytes = 3 * kRunBytes;        // addi r27,r26,-3072
constexpr uint32_t kBlocks = kBlockFrames / 4;           // 64 four-lane groups, 0..1008 by 16
constexpr uint32_t kGroupBytes = 16;

// ---- constant-pool addresses ----------------------------------------------------------------
// Each computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye: one misread digit
// in sub_82B2FE00 cost this project's first shadow divergence. The values in the comments are
// read out of probe/harness/out/image, and are never assumed by the code -- every one is loaded
// through the guest map.
constexpr uint32_t kOne = 0x8231A844;         // lis -32206 ; lfs -22460  == 1.0f
constexpr uint32_t kTwoPi = 0x820B411C;       // lis -32245 ; lfs 16668   == 6.2831855f
constexpr uint32_t kHalf = 0x8209975C;        // lis -32246 ; lfs -26788  == 0.5f
constexpr uint32_t kLane2Scale = 0x82060C50;  // lis -32250 ; lfs 3152    == 2.0f
constexpr uint32_t kLane3Scale = 0x82063B08;  // lis -32250 ; lfs 15112   == 3.0f
constexpr uint32_t kLaneStride = 0x82257308;  // lis -32219 ; lfs 29448   == 4.0f
constexpr uint32_t kPool = 0x822F8600;        // lis -32208 ; addi -31232, sub_82B43AF8's kPool
constexpr uint32_t kFramesPerBlockF = kPool + 1096;  // lfs 1096(r5)  == 256.0f
constexpr uint32_t kTurnsPerRadian = kPool + 772;    // lfs 772(r5)   == 0.15915494f == 1/(2*pi)
constexpr uint32_t kBiquadBias = kPool + 432;        // read by sub_82B43AF8, for the read set

// The four all-pass sections sub_82B43AF8 is handed, 20 bytes each. Read from the image they are
// {a1, a2, a2, a1, 1.0} -- second-order all-passes. Sections 1 and 2 cascade into one branch of
// the Hilbert pair, 3 and 4 into the other, which is what makes this a single-sideband shift.
constexpr uint32_t kAllpass1 = 0x82FCE2B0;  // lis -32003 ; addi -7504
constexpr uint32_t kAllpass2 = 0x82FCE2C4;  // lis -32003 ; addi -7484
constexpr uint32_t kAllpass3 = 0x82FCE2D8;  // lis -32003 ; addi -7464
constexpr uint32_t kAllpass4 = 0x82FCE2EC;  // lis -32003 ; addi -7444

// The polynomial vectors sub_824531C8 and sub_82473930 load, one span, for the read set only.
constexpr uint32_t kTrigTables = 0x822F97C0;
constexpr uint32_t kTrigTableBytes = 0xA0;

// A 32-bit constant reaches a callee as the sign-extended 64-bit register the addi produced.
constexpr uint64_t SignExtend(uint32_t value) {
  return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return double(temp.f32);
}

// stfs: the double rounded back to single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// Scalar single-precision arithmetic, each rounding to single exactly once, with the operand
// order the lifted lines have: float multiply and add are not NaN-commutative and which operand's
// payload survives cannot be derived from source (docs/vmx128-exactness.md rule 4). fmadds /
// fmsubs / fnmsubs are single-rounding fused products -- never a multiply plus a separate add.
inline double MulSingle(double a, double b) { return double(float(a * b)); }
inline double AddSingle(double a, double b) { return double(float(a + b)); }
inline double DivSingle(double a, double b) { return double(float(a / b)); }
inline double FmaSingle(double a, double b, double c) {  // fmadds: a*b + c
  return double(float(std::fma(a, b, c)));
}
inline double FmsubSingle(double a, double b, double c) {  // fmsubs: a*b - c
  return double(float(std::fma(a, b, -c)));
}
inline double NmsubSingle(double a, double b, double c) {  // fnmsubs: c - a*b, fused
  return double(float(-std::fma(a, b, -c)));
}
inline double RoundToSingle(double a) { return double(float(a)); }  // frsp

// fctiwz then the stfd/lwz of the LOW word: NaN gives 0x80000000, above INT_MAX saturates, and
// only the low 32 bits of the spilled doubleword reach the register. Same helper as
// sub_82B217F0's port, the same edge cases, in the same order.
inline uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(static_cast<uint64_t>(fixed) & 0xFFFFFFFFu);
}

// lvx128: a 16-byte load with the big-endian lane reversal RexGlue applies to every vector memory
// access, and the same masking of the address down to 16 bytes.
inline simde__m128 LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// vor128 v1,vS,vS -- a bit copy of all sixteen bytes, which is how both trig callees receive their
// argument. Kept in the integer domain, as the lifted line is, so no NaN payload can be touched.
inline void SetTrigArgument(PPCContext& ctx, simde__m128 value) {
  simde_mm_store_si128((simde__m128i*)ctx.v1.u8, simde_mm_castps_si128(value));
}

// stvx128: the same reversal on the way out, the same masking.
inline void StoreVector(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

REX_FUNC(Native) {
  // __savegprlr_24 spills r24-r31 and the link register into this call's own frame and
  // __restgprlr_24 reloads them; the harness compares r13-r31, so all eight entry values have to
  // survive here. sub_82B42510 preserves only r28-r31, so without this the values it leaves in
  // r24-r27 would reach the caller. The spill slots are inside the frame and are not windowed.
  const PPCRegister saved_r24 = ctx.r24;
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  // stfd f29/f30/f31,-96/-88/-80(r1) and stvx128 v125/v126/v127 at -144/-128/-112 are this frame's
  // own spills, reloaded by the epilogue: the native body keeps all six in C++ locals and never
  // touches ctx.f29-f31 or ctx.v125-v127, which leaves them at their entry values either way.
  //
  // stwu r1,-240(r1). The frame IS reproduced -- most ports skip their own -- because five callees
  // build frames below the live r1 and sub_82B43AF8's callee reads its spill at -8(r1) back; the
  // 240 bytes have to be claimed so those land where the original put them.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);  // stwu r1,-240(r1)
  ctx.r1.u32 = entry_sp - kFrameBytes;

  const uint64_t object_reg = ctx.r3.u64;  // mr r29,r3
  const uint64_t stream_reg = ctx.r4.u64;  // mr r28,r4
  const uint32_t object = uint32_t(object_reg);
  const uint32_t stream = uint32_t(stream_reg);

  ctx.fpscr.disableFlushModeUnconditional();  // stfd f29,-96(r1) -- the prologue's mode call
  const uint32_t prefilter_offset = REX_LOAD_U16(object + kPreFilterOffset);  // lhz r11,272(r3)
  const double enable = LoadSingle(base, object + kPreFilterEnable);          // lfs f0,264(r3)
  const uint64_t prefilter_reg = uint64_t(prefilter_offset) + object_reg;     // add r31,r11,r3
  const double one = LoadSingle(base, kOne);                                  // lfs f13,-22460(r10)
  const double two_pi = LoadSingle(base, kTwoPi);                             // lfs f31,16668(r9)

  // ---- the optional band-limiting pre-filter ------------------------------------------------
  // fcmpu cr6,f0,f13 ; bne cr6,0x82b22950 -- a NaN enable compares unequal and bypasses, which is
  // what `==` on the two doubles does as well.
  if (enable == one) {
    const double cached = LoadSingle(base, object + kCachedFrequency);  // lfs f0,268(r3)
    const double frequency = LoadSingle(base, object + kFrequencyInput);  // lfs f12,52(r3)
    // fcmpu cr6,f0,f12 ; beq cr6,0x82b22940 -- a NaN on either side is unordered, so beq is not
    // taken and the band is redesigned; `!(a == b)` is true in exactly the same cases.
    if (!(cached == frequency)) {
      const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);           // lwz r11,40(r4)
      const double rate = LoadSingle(base, format + kFormatSampleRate);       // lfs f11,12(r11)
      const double half = LoadSingle(base, kHalf);                            // lfs f0,-26788(r10)
      const double inv_rate = DivSingle(one, rate);                           // fdivs f10,f13,f11
      const double nyquist = MulSingle(rate, half);                           // fmuls f9,f11,f0
      const double below = FmsubSingle(nyquist, half, frequency);             // fmsubs f8,f9,f0,f12
      const double upper = MulSingle(inv_rate, nyquist);                      // fmuls f7,f10,f9
      const double lower = MulSingle(below, inv_rate);                        // fmuls f6,f8,f10
      // The two band edges travel in f2 and f1, which GuestCall does not place: set them here,
      // exactly as the original leaves them in the volatile FPRs before the branch.
      ctx.f2.f64 = MulSingle(upper, two_pi);  // fmuls f2,f7,f31
      ctx.f1.f64 = MulSingle(lower, two_pi);  // fmuls f1,f6,f31
      // bl 0x82b42510 -- r3 and r4 as they arrived, r5 = the coefficient block at object+128.
      skate3::audio::GuestCall(ctx, base, sub_82B42510, object_reg, stream_reg,
                               object_reg + kBandCoefficients);

      ctx.fpscr.disableFlushModeUnconditional();                            // lfs f5,52(r29)
      const double designed = LoadSingle(base, object + kFrequencyInput);   // lfs f5,52(r29)
      StoreSingle(base, object + kCachedFrequency, designed);               // stfs f5,268(r29)
    }
    // loc_82B22940: bl 0x82b41d58 -- run the designed filter over the stream's buffers. It ends by
    // swapping the stream's two descriptors, so the reads below see the filtered run as the input.
    skate3::audio::GuestCall(ctx, base, sub_82B41D58, prefilter_reg, stream_reg,
                             object_reg + kBandCoefficients);
  }

  // ---- loc_82B22950: the all-pass cascade ---------------------------------------------------
  const uint32_t pool_owner = REX_LOAD_U32(object + kPoolOwner);      // lwz r11,8(r29)
  const uint32_t in_desc = REX_LOAD_U32(stream + kStreamBufferA);     // lwz r9,28(r28)
  const uint32_t out_desc = REX_LOAD_U32(stream + kStreamBufferB);    // lwz r25,32(r28)
  const uint32_t pool = REX_LOAD_U32(pool_owner);                     // lwz r8,0(r11)
  const uint64_t input_reg = REX_LOAD_U32(in_desc + kDescBase);       // lwz r24,4(r9)
  const int64_t pool_top = int64_t(REX_LOAD_U32(pool + kPoolTop));    // lwz r26,32(r8)
  // A 3 KB bump off the pool top, published before the callees run and restored at the end.
  const int64_t scratch_reg = pool_top - int64_t(kScratchBytes);      // addi r27,r26,-3072
  REX_STORE_U32(pool + kPoolTop, uint32_t(uint64_t(scratch_reg)));    // stw r27,32(r8)
  const int64_t cos_run_reg = scratch_reg + int64_t(kRunBytes);       // addi r31,r27,1024
  const int64_t sin_run_reg = cos_run_reg + int64_t(kRunBytes);       // addi r30,r31,1024
  const uint32_t scratch = uint32_t(uint64_t(scratch_reg));
  const uint32_t cos_run = uint32_t(uint64_t(cos_run_reg));
  const uint32_t sin_run = uint32_t(uint64_t(sin_run_reg));

  // Four bl 0x82b43af8. Sections 1 and 2 cascade the input into the cosine branch, 3 and 4 into
  // the sine branch; both use `scratch` as the intermediate, so section 3 overwrites what section
  // 2 consumed and the four calls cannot be reordered.
  skate3::audio::GuestCall(ctx, base, sub_82B43AF8, object_reg + kAllpassState,
                           uint64_t(scratch_reg), input_reg, SignExtend(kAllpass1),
                           uint64_t(kBlockFrames));
  skate3::audio::GuestCall(ctx, base, sub_82B43AF8, object_reg + kAllpassState + 16,
                           uint64_t(cos_run_reg), uint64_t(scratch_reg), SignExtend(kAllpass2),
                           uint64_t(kBlockFrames));
  skate3::audio::GuestCall(ctx, base, sub_82B43AF8, object_reg + kAllpassState + 32,
                           uint64_t(scratch_reg), input_reg, SignExtend(kAllpass3),
                           uint64_t(kBlockFrames));
  skate3::audio::GuestCall(ctx, base, sub_82B43AF8, object_reg + kAllpassState + 48,
                           uint64_t(sin_run_reg), uint64_t(scratch_reg), SignExtend(kAllpass4),
                           uint64_t(kBlockFrames));

  // ---- the oscillator's starting phase ------------------------------------------------------
  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);  // lwz r9,40(r28)
  ctx.fpscr.disableFlushModeUnconditional();                     // lfs f11,52(r29)
  const double frequency = LoadSingle(base, object + kFrequencyInput);  // lfs f11,52(r29)
  const double rate = LoadSingle(base, format + kFormatSampleRate);     // lfs f10,12(r9)
  const double turns_per_sample = DivSingle(frequency, rate);           // fdivs f9,f11,f10
  const double lane2_scale = LoadSingle(base, kLane2Scale);             // lfs f0,3152(r4)
  const double lane3_scale = LoadSingle(base, kLane3Scale);             // lfs f13,15112(r3)
  const double lane_stride = LoadSingle(base, kLaneStride);             // lfs f12,29448(r10)
  const double step = MulSingle(turns_per_sample, two_pi);              // fmuls f29,f9,f31
  const double phase0 = LoadSingle(base, object + kPhase);              // lfs f30,260(r29)
  // The descriptor was read before the cascade; only its base pointer is read after it, and
  // nothing the cascade writes -- the four histories and the scratch run -- can alias it.
  const uint32_t out_run = REX_LOAD_U32(out_desc + kDescBase);  // lwz r25,4(r25)

  // The four lanes are the phase at samples 0..3: the scales at kLane2Scale and kLane3Scale are
  // 2.0f and 3.0f, and sample 1 is a plain add rather than a multiply by one.
  const double phase_plus_2 = FmaSingle(step, lane2_scale, phase0);  // fmadds f8,f29,f0,f30
  const double phase_plus_3 = FmaSingle(step, lane3_scale, phase0);  // fmadds f7,f29,f13,f30
  const double phase_plus_1 = AddSingle(phase0, step);               // fadds f6,f30,f29
  const double group_step = MulSingle(step, lane_stride);            // fmuls f5,f29,f12

  // stfs f30,80(r1) ; stfs f8,88(r1) ; stfs f7,92(r1) ; stfs f6,84(r1) ; lvx128 v126,r0,r11, and
  // then stfs f5 into all four slots ; lvx128 v127,r0,r8. stfs writes big-endian and lvx128
  // reverses all sixteen bytes, so the two reversals cancel exactly: the single at +80 is host
  // lane 3 and the single at +92 is host lane 0, which is why set_ps takes them in +80, +84, +88,
  // +92 order. Both vectors are built as registers here, so the native body never writes the guest
  // stack, and the second lvx128 -- which the original issues after two calls -- sees the same
  // bytes because neither trig callee writes memory.
  const simde__m128 start = simde_mm_set_ps(float(phase0), float(phase_plus_1),
                                            float(phase_plus_2), float(phase_plus_3));
  const simde__m128 advance = simde_mm_set1_ps(float(group_step));

  // ---- 64 four-lane groups ------------------------------------------------------------------
  // Each group: out = cos_run[g]*cos(phase) - sin_run[g]*sin(phase), the phase advancing four
  // samples per group. sub_824531C8 (sine) and sub_82473930 (cosine) both take their argument in
  // v1 and leave the result there, so v1 is written before each call and read back after.
  // Every vmulfp128 is a plain multiply and must never fuse with the subtract that follows.
  //
  // Groups 14 and 41 issue the phase advance one instruction earlier and group 62 puts the new
  // phase in v127 instead of v126; both are scheduling and register allocation, and neither
  // changes a value. Group 0 addresses with r0 rather than r27, which is the same offset of zero.
  simde__m128 phase = start;
  simde__m128 argument = phase;  // vor128 v1,v126,v126
  for (uint32_t g = 0; g + 1 < kBlocks; g++) {
    const uint32_t offset = kGroupBytes * g;  // li r27,16g

    SetTrigArgument(ctx, argument);
    skate3::audio::GuestCall(ctx, base, sub_824531C8);  // bl 0x824531c8
    const simde__m128 sine = simde_mm_load_ps(ctx.v1.f32);

    const simde__m128 sin_tap = LoadVector(base, sin_run + offset);  // lvx128 v63,r30,r27
    ctx.fpscr.enableFlushModeUnconditional();
    const simde__m128 sin_part = simde_mm_mul_ps(sin_tap, sine);  // vmulfp128 v125,v63,v1

    SetTrigArgument(ctx, phase);               // vor128 v1,v126,v126
    skate3::audio::GuestCall(ctx, base, sub_82473930);  // bl 0x82473930
    const simde__m128 cosine = simde_mm_load_ps(ctx.v1.f32);

    const simde__m128 cos_tap = LoadVector(base, cos_run + offset);  // lvx128 v62,r31,r27
    ctx.fpscr.enableFlushModeUnconditional();
    const simde__m128 cos_part = simde_mm_mul_ps(cos_tap, cosine);  // vmulfp128 v61,v62,v1
    phase = simde_mm_add_ps(phase, advance);                        // vaddfp128 v126,v126,v127
    const simde__m128 shifted = simde_mm_sub_ps(cos_part, sin_part);  // vsubfp128 v60,v61,v125
    argument = phase;                                                 // vor128 v1,v126,v126
    StoreVector(base, out_run + offset, shifted);                     // stvx128 v60,r0,r25
  }

  // ---- the last group, with the phase wrap folded into it -----------------------------------
  {
    const uint32_t offset = kGroupBytes * (kBlocks - 1);  // li r27,1008

    SetTrigArgument(ctx, argument);
    skate3::audio::GuestCall(ctx, base, sub_824531C8);  // bl 0x824531c8
    const simde__m128 sine = simde_mm_load_ps(ctx.v1.f32);

    const simde__m128 sin_tap = LoadVector(base, sin_run + offset);  // lvx128 v35,r30,r27
    ctx.fpscr.enableFlushModeUnconditional();
    const simde__m128 sin_part = simde_mm_mul_ps(sin_tap, sine);  // vmulfp128 v126,v35,v1

    SetTrigArgument(ctx, phase);               // vor128 v1,v127,v127
    skate3::audio::GuestCall(ctx, base, sub_82473930);  // bl 0x82473930
    const simde__m128 cosine = simde_mm_load_ps(ctx.v1.f32);

    // The pool owner is re-read here. Nothing this call writes can change object+8 or the word it
    // points at, so this is the same pool the bump was taken from.
    const uint32_t pool_owner_again = REX_LOAD_U32(object + kPoolOwner);  // lwz r6,8(r29)
    const simde__m128 cos_tap = LoadVector(base, cos_run + offset);       // lvx128 v34,r31,r27
    ctx.fpscr.enableFlushModeUnconditional();
    const simde__m128 cos_part = simde_mm_mul_ps(cos_tap, cosine);  // vmulfp128 v33,v34,v1

    ctx.r3.s64 = 1;  // li r3,1 -- the return value, on every path

    // The scalar wrap and the last vector group interleave, and the lifted body toggles the flush
    // mode between them three times with the CONDITIONAL calls. Those stay exactly here:
    // docs/environment-linux.md records that one global choice for a body that mixes vector and
    // scalar float work is precisely what diverges.
    ctx.fpscr.disableFlushMode();                                      // lfs f0,1096(r5)
    const double frames = LoadSingle(base, kFramesPerBlockF);          // lfs f0,1096(r5)
    const double advanced = FmaSingle(step, frames, phase0);           // fmadds f4,f29,f0,f30
    const double turns_scale = LoadSingle(base, kTurnsPerRadian);      // lfs f0,772(r5)
    ctx.fpscr.enableFlushMode();                                       // vsubfp128 v32,v33,v126
    const simde__m128 shifted = simde_mm_sub_ps(cos_part, sin_part);   // vsubfp128 v32,v33,v126
    ctx.fpscr.disableFlushMode();                                      // fmuls f3,f4,f0
    const double turns = MulSingle(advanced, turns_scale);             // fmuls f3,f4,f0
    StoreVector(base, out_run + offset, shifted);                      // stvx128 v32,r25,r27

    // fctiwz f2,f3 ; stfd f2,80(r1) ; lwz r4,84(r1) ; extsw r11,r4 ; std r11,80(r1) ;
    // lfd f1,80(r1) ; fcfid f0,f1 -- an eight-byte round trip through this frame that keeps only
    // the low word of the conversion, sign-extended, then converts it back. Done in registers.
    const int32_t whole = int32_t(FctiwzLowWord(turns));
    const double whole_f = RoundToSingle(double(int64_t(whole)));      // fcfid f0,f1 ; frsp f13,f0
    const double wrapped = NmsubSingle(whole_f, two_pi, advanced);     // fnmsubs f12,f13,f31,f4
    StoreSingle(base, object + kPhase, wrapped);                       // stfs f12,260(r29)

    // The pool bump handed back, and the stream's two descriptors swapped so the run just written
    // becomes the next block's input.
    const uint32_t pool_again = REX_LOAD_U32(pool_owner_again);            // lwz r10,0(r6)
    REX_STORE_U32(pool_again + kPoolTop, uint32_t(uint64_t(pool_top)));   // stw r26,32(r10)
    const uint32_t written = REX_LOAD_U32(stream + kStreamBufferB);        // lwz r9,32(r28)
    const uint32_t consumed = REX_LOAD_U32(stream + kStreamBufferA);      // lwz r8,28(r28)
    REX_STORE_U32(stream + kStreamBufferB, consumed);                     // stw r8,32(r28)
    REX_STORE_U32(stream + kStreamBufferA, written);                      // stw r9,28(r28)

    const double frequency_now = LoadSingle(base, object + kFrequencyInput);  // lfs f11,52(r29)
    StoreSingle(base, object + kCachedFrequency, frequency_now);              // stfs f11,268(r29)
  }

  ctx.r1.u32 = entry_sp;  // addi r1,r1,240
  ctx.lr = entry_lr;      // __restgprlr_24 restores lr and r24-r31
  ctx.r24 = saved_r24;
  ctx.r25 = saved_r25;
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Every byte the call writes, from ENTRY state only.
//
// The one branch in the function is decidable here -- both of its operands are in memory before
// the call -- and the two sides have very different write sets. The bypass side is fully
// enumerable: seven spans, 4,180 bytes. The pre-filter side is not, and Windows() refuses it:
// sub_82B41D58's per-channel loops reach sub_82EDF460 (a memcpy whose dcbzl clears whole 128-byte
// lines) and sub_82B41FC8 (448 lifted lines of strided stfs runs), and neither write set has been
// read out. See probe/ports/notes/sub_82B22898.md.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  if (object == 0 || stream == 0) return false;

  // lfs f0,264(r3) ; fcmpu cr6,f0,f13 ; bne cr6,0x82b22950
  const double enable = LoadSingle(base, object + kPreFilterEnable);
  const double one = LoadSingle(base, kOne);
  if (enable == one) return false;  // the pre-filter path, not enumerable

  const uint32_t pool_owner = REX_LOAD_U32(object + kPoolOwner);   // lwz r11,8(r29)
  if (pool_owner == 0) return false;
  const uint32_t pool = REX_LOAD_U32(pool_owner);                  // lwz r8,0(r11)
  if (pool == 0) return false;
  const uint32_t pool_top = REX_LOAD_U32(pool + kPoolTop);         // lwz r26,32(r8)
  if (pool_top < kScratchBytes) return false;  // the bump would wrap the address space
  const uint32_t scratch = pool_top - kScratchBytes;               // addi r27,r26,-3072

  const uint32_t in_desc = REX_LOAD_U32(stream + kStreamBufferA);  // lwz r9,28(r28)
  const uint32_t out_desc = REX_LOAD_U32(stream + kStreamBufferB);  // lwz r25,32(r28)
  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);     // lwz r9,40(r28)
  if (in_desc == 0 || out_desc == 0 || format == 0) return false;
  const uint32_t input = REX_LOAD_U32(in_desc + kDescBase);   // lwz r24,4(r9)
  const uint32_t out_run = REX_LOAD_U32(out_desc + kDescBase);  // lwz r25,4(r25)
  if (input == 0 || out_run == 0) return false;

  // stvx128 masks every effective address down to 16 bytes and the 64 group offsets are all
  // multiples of 16, so the run written is [out & ~0xF, +1024).
  const uint32_t out_floor = out_run & ~0xFu;
  if (out_floor + kRunBytes <= out_floor) return false;
  // The input run is only read, but it is snapshotted, so a span that wraps the map is refused
  // too. `scratch` needs no such test: scratch + 3072 == pool_top by construction.
  if (input + kRunBytes <= input) return false;

  spec.write(pool + kPoolTop, 4);                 // stw r27,32(r8) ; stw r26,32(r10), same word
  spec.write(object + kAllpassState, 4 * 16);     // the four sub_82B43AF8 histories, contiguous
  spec.write(scratch, kScratchBytes);             // sub_82B43AF8's three destination runs
  spec.write(out_floor, kRunBytes);               // 64 stvx128
  spec.write(object + kPhase, 4);                 // stfs f12,260(r29)
  spec.write(stream + kStreamBufferA, 8);         // stw r8,32(r28) ; stw r9,28(r28)
  spec.write(object + kCachedFrequency, 4);       // stfs f11,268(r29)

  // Read and not written: the object's scalars, the stream's descriptors, the input run and the
  // rodata every callee loads. kHalf is not listed -- it is read only on the path refused above.
  spec.read(object + kPoolOwner, 4);
  spec.read(object + kFrequencyInput, 4);
  spec.read(object + kAllpassState, 4 * 16);
  spec.read(object + kPhase, 16);  // +260 phase, +264 enable, +268 cached, +272 offset
  spec.read(stream + kStreamBufferA, 16);  // +28, +32 and +40
  spec.read(in_desc + kDescBase, 4);
  spec.read(out_desc + kDescBase, 4);
  spec.read(format + kFormatSampleRate, 4);
  spec.read(pool + kPoolTop, 4);
  spec.read(input, kRunBytes);
  spec.read(scratch, kScratchBytes);
  spec.read(kAllpass1, 4 * 20);  // the four 20-byte sections, 0x82FCE2B0..0x82FCE2FF
  spec.read(kOne, 4);
  spec.read(kTwoPi, 4);
  spec.read(kLane2Scale, 4);
  spec.read(kLane3Scale, 4);
  spec.read(kLaneStride, 4);
  spec.read(kTurnsPerRadian, 4);
  spec.read(kFramesPerBlockF, 4);
  spec.read(kBiquadBias, 4);
  spec.read(kTrigTables, kTrigTableBytes);
  return true;
}

}  // namespace port_82B22898
SKATE3_PORT(82B22898, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
