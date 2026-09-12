// sub_82B2C658  role: run the per-channel peaking-EQ stage over a 256-frame block -- rebuild the RBJ peaking biquad when frequency, gain or Q moved, filter every channel through sub_82B43AF8 and swap the descriptor pair; a gain of exactly 1.0 bypasses (history zeroed on the first bypassed block, no swap)   lifted: skate3_recomp.67.cpp:35803 (386 lines)
// STATUS: verified
// calls/session: boot 61716  play 90841  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2C658.md
namespace port_82B2C658 {

// The filter object (r3) -- the same family as sub_82B26568 (high-pass) and sub_82B27E20
// (low-pass), with its own layout. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kChannelCount = 42;   // u8, re-read on every loop test
constexpr uint32_t kFrequency = 52;      // f32 numerator of the normalised centre frequency
constexpr uint32_t kGain = 60;           // f32 linear gain; exactly 1.0 bypasses
constexpr uint32_t kQuality = 68;        // f32 Q, clamped before use
constexpr uint32_t kHistory = 72;        // 16 bytes per channel {x1,x2,y1,y2}; r11 = r3+68, +4
constexpr uint32_t kBypassed = 200;      // s32: 1 while bypassed, 0 once the history is cleared
constexpr uint32_t kCoefficients = 204;  // 5 singles {a1,a2,b0,b1,b2}/a0, sub_82B43AF8's block
constexpr uint32_t kCachedWarp = 224;    // f32 the clamped angle the coefficients were built at
constexpr uint32_t kCachedGain = 228;    // f32 the gain they were built at
constexpr uint32_t kCachedQuality = 232; // f32 the (unclamped) Q they were built at
constexpr uint32_t kObjectEnd = 236;
// (200 - 72) / 16 == 8: the history array holds at most eight channels.
constexpr uint32_t kMaxChannels = 8;

// The stream (r4), the same shape sub_82B26568 names.
constexpr uint32_t kStreamBufferA = 28;  // descriptor read this call (r29), written on the swap
constexpr uint32_t kStreamBufferB = 32;  // descriptor written this call (r28)
constexpr uint32_t kStreamFormat = 40;
constexpr uint32_t kFormatSampleRate = 12;

// A buffer descriptor.
constexpr uint32_t kDescBase = 4;    // u32 float* of channel 0
constexpr uint32_t kDescStride = 14; // u16 floats between channels

constexpr uint32_t kBlockFrames = 256;  // li r7,256
constexpr uint32_t kFrameBytes = 192;   // stwu r1,-192(r1)

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset, computed not read by eye.
constexpr uint32_t kCutoffScale = 0x820B411C;    // lis -32245 ; lfs 16668
constexpr uint32_t kCutoffFloor = 0x822F8E7C;    // lis -32208 ; addi -31232 ; lfs 2172
constexpr uint32_t kCutoffCeiling = 0x822F8E80;  // lis -32208 ; addi -31232 ; lfs 2176
constexpr uint32_t kUnit = 0x8231A844;           // lis -32206 ; lfs -22460     (1.0)
constexpr uint32_t kZeroFloat = 0x82165A10;      // lis -32234 ; lfs 23056      (0.0)
constexpr uint32_t kQualityFloor = 0x82099280;   // lis -32246 ; lfs -28032
constexpr uint32_t kQualityCeiling = 0x820996EC; // lis -32246 ; lfs -26900
constexpr uint32_t kDenomGain = 0x82060C50;      // lis -32250 ; lfs 3152       (2.0)
constexpr uint32_t kCosineGain = 0x82094178;     // lis -32247 ; lfs 16760      (-2.0)
constexpr uint32_t kKernelBias = 0x822F87B0;     // read by sub_82B43AF8: lfs 432(r8)

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

// Each rounds to single once, operands in the lifted order (docs/vmx128-exactness.md rule 4).
double DivSingle(double a, double b) { return double(float(a / b)); }  // fdivs
double MulSingle(double a, double b) { return double(float(a * b)); }  // fmuls
double AddSingle(double a, double b) { return double(float(a + b)); }  // fadds
double SubSingle(double a, double b) { return double(float(a - b)); }  // fsubs
double SqrtSingle(double a) { return double(float(sqrt(a))); }         // fsqrts

// mullw keeps the full 64-bit product of the sign-extended halves; rlwinm r,2,0,29 is a
// zero-extended 32-bit x*4; the add is 64-bit on two zero-extended halves -- sub_82B26568's
// ChannelPointer, the same three instructions.
uint64_t ChannelPointer(uint32_t buffer_base, uint32_t stride, uint64_t index) {
  const int64_t product = int64_t(int32_t(stride)) * int64_t(int32_t(uint32_t(index)));
  const uint64_t scaled = uint64_t((uint32_t(uint64_t(product)) << 2) & 0xFFFFFFFCu);
  return scaled + uint64_t(buffer_base);
}

// [a, a+alen) and [b, b+blen) disjoint, 64-bit so nothing folds around 2^32.
inline bool Disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  return a + alen <= b || b + blen <= a;
}

REX_FUNC(Native) {
  const uint64_t object_reg = ctx.r3.u64;  // mr r30,r3
  const uint32_t object = uint32_t(object_reg);
  const uint32_t stream = ctx.r4.u32;      // mr r26,r4

  // __savegprlr_26 / __savefpr_26 spill r26-r31 and f26-f31; Native keeps its state in locals and
  // never writes those registers, so their entry values survive without the spill. stwu r1,-192
  // IS reproduced: sub_82B43AF8 stores at -8(r1) and spills f20-f31 below it, and sin/cos spill
  // below r1 too, so their frames land where the original put them.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);  // stwu r1,-192(r1)
  ctx.r1.u32 = entry_sp - kFrameBytes;

  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);       // lwz r10,40(r4)
  ctx.fpscr.disableFlushModeUnconditional();
  const double numerator = LoadSingle(base, object + kFrequency);     // lfs f0,52(r3)
  const double rate = LoadSingle(base, format + kFormatSampleRate);   // lfs f13,12(r10)
  const double ratio = DivSingle(numerator, rate);                    // fdivs f12,f0,f13
  // fmuls f29,f12,f13 -- two single-rounded operations, not one FMA.
  double warp = MulSingle(ratio, LoadSingle(base, kCutoffScale));     // lfs f13,16668(r9)
  // fcmpu ; blt 0x82b2c6b0 -- below the floor clamps up; otherwise fcmpu against the ceiling ;
  // ble 0x82b2c6b4 -- above it clamps down. An unordered (NaN) angle is kept as it is.
  const double warp_floor = LoadSingle(base, kCutoffFloor);           // lfs f0,2172(r11)
  if (warp < warp_floor) {
    ctx.fpscr.disableFlushModeUnconditional();
    warp = warp_floor;                                                // fmr f29,f0
  } else {
    const double warp_ceiling = LoadSingle(base, kCutoffCeiling);     // lfs f0,2176(r11)
    if (warp > warp_ceiling) {
      ctx.fpscr.disableFlushModeUnconditional();
      warp = warp_ceiling;                                            // fmr f29,f0
    }
  }

  // loc_82B2C6B4
  ctx.fpscr.disableFlushModeUnconditional();
  const double gain = LoadSingle(base, object + kGain);               // lfs f28,60(r30)
  const double unit = LoadSingle(base, kUnit);                        // lfs f31,-22460(r11)
  const int32_t bypassed = int32_t(REX_LOAD_U32(object + kBypassed)); // lwz r11,200(r30)

  // fcmpu cr6,f28,f31 ; bne 0x82b2c740 -- a NaN gain is not eq, so it filters.
  if (gain == unit) {
    // cmpwi cr6,r11,1 ; bne 0x82b2c718 -- the first bypassed block clears the history, once.
    if (bypassed == 1) {
      uint32_t channels = REX_LOAD_U8(object + kChannelCount);  // lbz r11,42(r30)
      uint64_t index = 0;                                       // li r10,0
      if (channels != 0) {                                      // cmplwi ; beq 0x82b2c710
        const double zero = LoadSingle(base, kZeroFloat);       // lfs f0,23056(r9)
        uint32_t cursor = object + kQuality;                    // addi r11,r30,68
        do {
          ctx.fpscr.disableFlushModeUnconditional();
          StoreSingle(base, cursor + 4, zero);   // stfs f0,4(r11)
          index += 1;                            // addi r10,r10,1
          StoreSingle(base, cursor + 8, zero);   // stfs f0,8(r11)
          StoreSingle(base, cursor + 12, zero);  // stfs f0,12(r11)
          cursor += 16;                          // stfsu f0,16(r11)
          StoreSingle(base, cursor, zero);
          channels = REX_LOAD_U8(object + kChannelCount);  // lbz r9,42(r30)
        } while (uint32_t(index) < channels);    // cmplw cr6,r10,r9 ; blt 0x82b2c6f0
      }
      // loc_82B2C710
      REX_STORE_U32(object + kBypassed, 0u);     // li r11,0 ; stw r11,200(r30)
    }
    // loc_82B2C718 -- publish the parameters and leave: no kernel, no descriptor swap.
    ctx.fpscr.disableFlushModeUnconditional();
    const double gain_now = LoadSingle(base, object + kGain);         // lfs f0,60(r30)
    const double quality_now = LoadSingle(base, object + kQuality);   // lfs f13,68(r30)
    StoreSingle(base, object + kCachedWarp, warp);                    // stfs f29,224(r30)
    StoreSingle(base, object + kCachedGain, gain_now);                // stfs f0,228(r30)
    StoreSingle(base, object + kCachedQuality, quality_now);          // stfs f13,232(r30)
    ctx.r3.s64 = 1;                                                   // li r3,1
    ctx.r1.u32 = entry_sp;                                            // addi r1,r1,192
    return;
  }

  // loc_82B2C740: cmpwi cr6,r11,0 ; bne 0x82b2c750 -- tested on the value loaded at entry.
  if (bypassed == 0) {
    REX_STORE_U32(object + kBypassed, 1u);  // li r11,1 ; stw r11,200(r30)
  }

  // loc_82B2C750 -- rebuild unless all three cached parameters match (three fcmpu ; bne).
  ctx.fpscr.disableFlushModeUnconditional();
  bool unchanged = warp == LoadSingle(base, object + kCachedWarp);     // lfs f0,224(r30)
  if (unchanged) unchanged = gain == LoadSingle(base, object + kCachedGain);  // lfs f0,228(r30)
  if (unchanged) {
    // lfs f0,68(r30) ; lfs f13,232(r30) ; fcmpu cr6,f0,f13 ; beq 0x82b2c834
    const double quality_now = LoadSingle(base, object + kQuality);
    unchanged = quality_now == LoadSingle(base, object + kCachedQuality);
  }
  if (!unchanged) {
    // loc_82B2C778: Q clamped into [floor, ceiling]; a NaN Q is kept.
    ctx.fpscr.disableFlushModeUnconditional();
    double quality = LoadSingle(base, object + kQuality);             // lfs f30,68(r30)
    const double quality_floor = LoadSingle(base, kQualityFloor);     // lfs f0,-28032(r11)
    if (quality < quality_floor) {                                    // fcmpu ; blt 0x82b2c79c
      ctx.fpscr.disableFlushModeUnconditional();
      quality = quality_floor;                                        // fmr f30,f0
    } else {
      const double quality_ceiling = LoadSingle(base, kQualityCeiling);  // lfs f0,-26900(r11)
      if (quality > quality_ceiling) {                                // fcmpu ; ble 0x82b2c7a0
        ctx.fpscr.disableFlushModeUnconditional();
        quality = quality_ceiling;                                    // fmr f30,f0
      }
    }

    // loc_82B2C7A0. fmr f27,f29 is dead (f27 is restored by the epilogue, never read).
    // fmr f1,f29 ; bl 0x82f4ded0 -- sine. Both math callees write only their own frame.
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f1.f64 = warp;
    skate3::audio::GuestCall(ctx, base, sub_82F4DED0);
    ctx.fpscr.disableFlushModeUnconditional();
    const double sine = double(float(ctx.f1.f64));                    // frsp f26,f1
    ctx.f1.f64 = warp;                                                // fmr f1,f29
    skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);                // bl 0x82f4dfb0 -- cosine

    ctx.fpscr.disableFlushModeUnconditional();
    const double amplitude = SqrtSingle(gain);                        // fsqrts f12,f28
    const double gain_now = LoadSingle(base, object + kGain);         // lfs f11,60(r30)
    const double quality_now = LoadSingle(base, object + kQuality);   // lfs f10,68(r30)
    StoreSingle(base, object + kCachedGain, gain_now);                // stfs f11,228(r30)
    StoreSingle(base, object + kCachedQuality, quality_now);          // stfs f10,232(r30)
    const double two = LoadSingle(base, kDenomGain);                  // lfs f0,3152(r11)
    const double minus_two = LoadSingle(base, kCosineGain);           // lfs f13,16760(r10)
    StoreSingle(base, object + kCachedWarp, warp);                    // stfs f29,224(r30)
    const double two_q = MulSingle(quality, two);                     // fmuls f9,f30,f0
    const double cosine = double(float(ctx.f1.f64));                  // frsp f8,f1
    const double alpha = DivSingle(sine, two_q);                      // fdivs f7,f26,f9
    const double cos_term = MulSingle(cosine, minus_two);             // fmuls f6,f8,f13
    const double alpha_over_a = DivSingle(alpha, amplitude);          // fdivs f5,f7,f12
    const double alpha_times_a = MulSingle(amplitude, alpha);         // fmuls f4,f12,f7
    const double a0 = AddSingle(alpha_over_a, unit);                  // fadds f3,f5,f31
    const double a2 = SubSingle(unit, alpha_over_a);                  // fsubs f2,f31,f5
    const double b0 = AddSingle(alpha_times_a, unit);                 // fadds f1,f4,f31
    const double b2 = SubSingle(unit, alpha_times_a);                 // fsubs f0,f31,f4
    const double inv = DivSingle(unit, a0);                           // fdivs f13,f31,f3
    StoreSingle(base, object + kCoefficients + 0, MulSingle(inv, cos_term));  // fmuls f12 ; stfs 204
    StoreSingle(base, object + kCoefficients + 4, MulSingle(a2, inv));        // fmuls f11 ; stfs 208
    StoreSingle(base, object + kCoefficients + 8, MulSingle(b0, inv));        // fmuls f10 ; stfs 212
    StoreSingle(base, object + kCoefficients + 12, MulSingle(inv, cos_term)); // fmuls f9 ; stfs 216
    StoreSingle(base, object + kCoefficients + 16, MulSingle(b2, inv));       // fmuls f8 ; stfs 220
  }

  // loc_82B2C834 -- every channel through the biquad, reading A and writing B.
  uint32_t channels = REX_LOAD_U8(object + kChannelCount);          // lbz r11,42(r30)
  uint64_t index = 0;                                               // li r31,0
  const uint32_t src_desc = REX_LOAD_U32(stream + kStreamBufferA);  // lwz r29,28(r26)
  const uint32_t dst_desc = REX_LOAD_U32(stream + kStreamBufferB);  // lwz r28,32(r26)
  if (channels != 0) {                                              // cmplwi ; beq 0x82b2c89c
    const uint64_t coefficients = object_reg + kCoefficients;       // addi r27,r30,204
    int64_t history = int64_t(object_reg) + kHistory;               // addi r3,r30,72
    do {
      const uint32_t src_stride = REX_LOAD_U16(src_desc + kDescStride);  // lhz r11,14(r29)
      const uint32_t dst_stride = REX_LOAD_U16(dst_desc + kDescStride);  // lhz r10,14(r28)
      const uint32_t src_base = REX_LOAD_U32(src_desc + kDescBase);      // lwz r11,4(r29)
      const uint32_t dst_base = REX_LOAD_U32(dst_desc + kDescBase);      // lwz r9,4(r28)
      const uint64_t src_ptr = ChannelPointer(src_base, src_stride, index);  // mullw;rlwinm;add r5
      const uint64_t dst_ptr = ChannelPointer(dst_base, dst_stride, index);  // mullw;rlwinm;add r4
      // bl 0x82b43af8 -- 256 frames, so never its scalar fallback sub_82B43978.
      skate3::audio::GuestCall(ctx, base, sub_82B43AF8, uint64_t(history), dst_ptr, src_ptr,
                               coefficients, uint64_t(kBlockFrames));  // li r7,256 ; mr r6,r27
      channels = REX_LOAD_U8(object + kChannelCount);  // lbz r4,42(r30)
      index += 1;                                      // addi r31,r31,1
      // addi r3,r3,16 -- on the r3 the callee left, which sub_82B43AF8 never writes. Reproduced
      // as a read of ctx.r3, as sub_82B26568 does.
      history = ctx.r3.s64 + 16;
    } while (uint32_t(index) < channels);              // cmplw cr6,r31,r4 ; blt 0x82b2c854
  }

  // loc_82B2C89C -- the ping-pong swap, both loads before both stores.
  const uint32_t new_a = REX_LOAD_U32(stream + kStreamBufferB);  // lwz r11,32(r26)
  const uint32_t new_b = REX_LOAD_U32(stream + kStreamBufferA);  // lwz r10,28(r26)
  REX_STORE_U32(stream + kStreamBufferA, new_a);                 // stw r11,28(r26)
  REX_STORE_U32(stream + kStreamBufferB, new_b);                 // stw r10,32(r26)
  ctx.r3.s64 = 1;                                                // li r3,1
  ctx.r1.u32 = entry_sp;                                         // addi r1,r1,192
}

// The object side is one span, [+72, +236): the history (both paths write only channels < 8
// there), the bypass flag, the coefficients and the three caches -- a union, and a byte a call
// leaves alone compares equal. The filtering path adds the descriptor swap and the destination
// channels, whose addresses are re-read from the destination descriptor on every iteration and
// whose count is re-read from +42: those control words must not lie under any declared write.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  if (object == 0 || stream == 0) return false;
  if (uint64_t(object) + kObjectEnd > 0x100000000ull) return false;  // no wrap: +42 < +72
  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);
  const uint32_t channels = REX_LOAD_U8(object + kChannelCount);
  // Past eight the history runs into the flag and the coefficients: a corrupted object.
  if (channels > kMaxChannels) return false;

  const uint32_t object_span = kObjectEnd - kHistory;  // 164
  spec.write(object + kHistory, object_span);
  spec.read(object + kChannelCount, 1);
  spec.read(object + kFrequency, 20);  // +52 .. +71: frequency, gain, Q
  spec.read(stream + kStreamFormat, 4);
  if (format != 0) spec.read(format + kFormatSampleRate, 4);
  spec.read(kCutoffScale, 4);
  spec.read(kCutoffFloor, 8);  // 2172 and 2176 are adjacent
  spec.read(kUnit, 4);
  spec.read(kZeroFloat, 4);
  spec.read(kQualityFloor, 4);
  spec.read(kQualityCeiling, 4);
  spec.read(kDenomGain, 4);
  spec.read(kCosineGain, 4);

  // The path is fixed at entry: +60 is loaded before any store and compared with 1.0.
  if (LoadSingle(base, object + kGain) == LoadSingle(base, kUnit)) return true;

  // Loaded once before the loop, after the stores to +200..+235.
  if (!Disjoint(stream + kStreamBufferA, 8, object + kHistory, object_span)) return false;
  spec.write(stream + kStreamBufferA, 8);  // the swap
  if (channels == 0) return true;

  const uint32_t src_desc = REX_LOAD_U32(stream + kStreamBufferA);
  const uint32_t dst_desc = REX_LOAD_U32(stream + kStreamBufferB);
  if (src_desc == 0 || dst_desc == 0) return false;
  // Re-read on every iteration: the destination base and stride (address), the count (trips).
  const uint64_t control_addr[3] = {uint64_t(dst_desc) + kDescBase, uint64_t(dst_desc) + kDescStride,
                                    uint64_t(object) + kChannelCount};
  const uint64_t control_len[3] = {4, 2, 1};
  for (uint32_t k = 0; k < 3; k++) {
    if (!Disjoint(object + kHistory, object_span, control_addr[k], control_len[k])) return false;
    if (!Disjoint(stream + kStreamBufferA, 8, control_addr[k], control_len[k])) return false;
  }
  const uint32_t src_base = REX_LOAD_U32(src_desc + kDescBase);
  const uint32_t dst_base = REX_LOAD_U32(dst_desc + kDescBase);
  const uint32_t src_stride = REX_LOAD_U16(src_desc + kDescStride);
  const uint32_t dst_stride = REX_LOAD_U16(dst_desc + kDescStride);
  // sub_82B43AF8 writes exactly [r4, r4 + 1024) and r3+0..15 at r7 = 256 (its verified port).
  for (uint32_t i = 0; i < channels && i < kMaxChannels; i++) {
    const uint32_t dst = uint32_t(ChannelPointer(dst_base, dst_stride, i));
    const uint32_t src = uint32_t(ChannelPointer(src_base, src_stride, i));
    if (dst == 0 || src == 0) return false;
    if (uint64_t(dst) + kBlockFrames * 4 > 0x100000000ull) return false;
    for (uint32_t k = 0; k < 3; k++) {
      if (!Disjoint(dst, kBlockFrames * 4, control_addr[k], control_len[k])) return false;
    }
    spec.write(dst, kBlockFrames * 4);
    spec.read(src, kBlockFrames * 4);
  }
  spec.read(src_desc, 16);
  spec.read(dst_desc, 16);
  spec.read(kKernelBias, 4);
  return true;
}

}  // namespace port_82B2C658
SKATE3_PORT(82B2C658, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
