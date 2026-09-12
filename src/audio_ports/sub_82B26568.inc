// sub_82B26568  role: run the per-channel high-pass stage over a 256-frame block, or bypass it   lifted: skate3_recomp.67.cpp:20773 (287 lines)
// STATUS: verified
// calls/session: boot 171012  play 276388  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B26568.md
namespace port_82B26568 {

// The same filter object and stream as sub_82B27E20; nothing in docs/rw_audio_structs.h names
// this layout, so the offsets stay plain constants named for what this function does with them.
constexpr uint32_t kChannelCount = 42;    // u8 -- re-read from memory on every loop test
constexpr uint32_t kCutoffInput = 52;     // f32 numerator of the normalised cutoff
constexpr uint32_t kFilterState = 56;     // 16 bytes per channel {x1,x2,y1,y2}
constexpr uint32_t kCoefficients = 184;   // 5 floats, built inline here
constexpr uint32_t kCachedCutoff = 204;   // f32 -- the cutoff the coefficients were built from
// (184 - 56) / 16 == 8, so the state array holds at most eight channels.
constexpr uint32_t kMaxChannels = 8;

// The stream (r4).
constexpr uint32_t kStreamBufferA = 28;   // descriptor read this call, written on the swap
constexpr uint32_t kStreamBufferB = 32;   // descriptor written this call
constexpr uint32_t kStreamFormat = 40;
constexpr uint32_t kFormatSampleRate = 12;

// A buffer descriptor.
constexpr uint32_t kDescBase = 4;         // u32 float* of channel 0
constexpr uint32_t kDescStride = 14;      // u16 floats between channels

constexpr uint32_t kBlockFrames = 256;    // li r7,256

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye.
constexpr uint32_t kCutoffScale = 0x820B411C;    // lis -32245 ; lfs 16668
constexpr uint32_t kCutoffFloor = 0x822F8E7C;    // lis -32208 ; addi -31232 ; lfs 2172
constexpr uint32_t kCutoffCeiling = 0x822F8E80;  // lis -32208 ; addi -31232 ; lfs 2176
constexpr uint32_t kZeroFloat = 0x82165A10;      // lis -32234 ; lfs 23056
// The four coefficient constants, the same four sub_82B43CC0 uses for the low-pass twin.
constexpr uint32_t kSineGain = 0x8209975C;    // lis -32246 ; lfs -26788
constexpr uint32_t kUnit = 0x8231A844;        // lis -32206 ; lfs -22460
constexpr uint32_t kCosineGain = 0x82094178;  // lis -32247 ; lfs 16760
constexpr uint32_t kDenomGain = 0x82060C50;   // lis -32250 ; lfs 3152
constexpr uint32_t kKernelBias = 0x822F87B0;  // read by sub_82B43AF8: lfs 432(r8)

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

double DivSingle(double a, double b) { return double(float(a / b)); }
double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }

// fneg is a sign-bit flip on the doubleword, not an arithmetic negate.
double NegateBits(double value) {
  PPCRegister temp{};
  temp.f64 = value;
  temp.u64 ^= 0x8000000000000000ull;
  return temp.f64;
}

// mullw keeps the full 64-bit product of the sign-extended halves; rlwinm r,2,0,29 is a
// zero-extended 32-bit x*4; the add that follows is 64-bit on two zero-extended halves, so a
// carry into bit 32 survives into the register the callee receives.
uint64_t ChannelPointer(uint32_t buffer_base, uint32_t stride, uint64_t index) {
  const int64_t product = int64_t(int32_t(stride)) * int64_t(int32_t(uint32_t(index)));
  const uint64_t scaled = uint64_t((uint32_t(uint64_t(product)) << 2) & 0xFFFFFFFCu);
  return scaled + uint64_t(buffer_base);
}

REX_FUNC(Native) {
  const uint64_t object_reg = ctx.r3.u64;  // mr r30,r3
  const uint64_t stream_reg = ctx.r4.u64;  // mr r26,r4
  const uint32_t object = uint32_t(object_reg);
  const uint32_t stream = uint32_t(stream_reg);
  // The original spills f29-f31 and r26-r31 (__savegprlr_26) into its frame and restores them on
  // the way out, because sin/cos and the kernel may clobber the volatile file. The harness
  // compares f14-f31 and r13-r31, so the entry values have to survive here too.
  const PPCRegister saved_f29 = ctx.f29;
  const PPCRegister saved_f30 = ctx.f30;
  const PPCRegister saved_f31 = ctx.f31;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;

  ctx.fpscr.disableFlushModeUnconditional();
  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);       // lwz r10,40(r4)
  const double numerator = LoadSingle(base, object + kCutoffInput);   // lfs f0,52(r3)
  const double rate = LoadSingle(base, format + kFormatSampleRate);   // lfs f13,12(r10)
  // fdivs f12,f0,f13 ; fmuls f31,f12,f13 -- two single-rounded operations, not one FMA.
  double cutoff = MulSingle(DivSingle(numerator, rate), LoadSingle(base, kCutoffScale));
  const double floor_value = LoadSingle(base, kCutoffFloor);          // lfs f0,2172(r11)

  // bgt cr6,0x82b26608 -- a cutoff above the floor filters; at or below it (and an unordered
  // compare, i.e. a NaN cutoff) the stage is bypassed and the descriptor pair is NOT swapped, so
  // the caller keeps reading the untouched input block. This is the mirror of sub_82B27E20,
  // which bypasses at the ceiling instead.
  if (!(cutoff > floor_value)) {
    const double cached = LoadSingle(base, object + kCachedCutoff);   // lfs f13,204(r3)
    // ble cr6,0x82b26600 -- only a cached cutoff that was itself above the floor means the state
    // words still hold filter history worth clearing.
    if (cached > floor_value) {
      uint32_t channels = REX_LOAD_U8(object + kChannelCount);        // lbz r11,42(r3)
      uint64_t index = 0;                                             // li r10,0
      // beq cr6,0x82b26600
      if (channels != 0) {
        const double zero = LoadSingle(base, kZeroFloat);             // lfs f0,23056(r9)
        uint32_t cursor = object + kCutoffInput;                      // addi r11,r3,52
        do {
          ctx.fpscr.disableFlushModeUnconditional();
          StoreSingle(base, cursor + 4, zero);   // stfs f0,4(r11)
          index += 1;                            // addi r10,r10,1
          StoreSingle(base, cursor + 8, zero);   // stfs f0,8(r11)
          StoreSingle(base, cursor + 12, zero);  // stfs f0,12(r11)
          cursor += 16;                          // stfsu f0,16(r11): store at r11+16, then r11=ea
          StoreSingle(base, cursor, zero);
          channels = REX_LOAD_U8(object + kChannelCount);  // lbz r9,42(r30)
        } while (uint32_t(index) < channels);    // cmplw cr6,r10,r9 ; blt
      }
    }
    // loc_82B26600 -- then straight to loc_82B26724: no kernel, no descriptor swap.
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, object + kCachedCutoff, cutoff);  // stfs f31,204(r30)
    ctx.r3.s64 = 1;                                     // li r3,1
    ctx.f29 = saved_f29;
    ctx.f30 = saved_f30;
    ctx.f31 = saved_f31;
    ctx.r26 = saved_r26;
    ctx.r27 = saved_r27;
    ctx.r28 = saved_r28;
    ctx.r29 = saved_r29;
    ctx.r30 = saved_r30;
    ctx.r31 = saved_r31;
    return;
  }

  // loc_82B26608
  ctx.fpscr.disableFlushModeUnconditional();
  const double ceiling = LoadSingle(base, kCutoffCeiling);  // lfs f0,2176(r11)
  // ble cr6,0x82b26618 -- clamp down to the ceiling only when strictly above it.
  if (cutoff > ceiling) cutoff = ceiling;                   // fmr f31,f0

  // loc_82B26618
  ctx.fpscr.disableFlushModeUnconditional();
  const double cached = LoadSingle(base, object + kCachedCutoff);  // lfs f0,204(r30)
  // beq cr6,0x82b266ac -- an unchanged cutoff reuses the coefficients already at +184.
  if (cutoff != cached) {
    // fmr f30,f31 is dead: f30 is never read again and the epilogue reloads it, so the port
    // leaves ctx.f30 at its entry value instead.
    // fmr f1,f31 ; bl 0x82f4ded0 -- sine. GuestCall leaves f1 alone, so it is set first. Both
    // math callees write only their own frame: no guest state, no indirect calls, no timebase.
    ctx.f1.f64 = cutoff;
    skate3::audio::GuestCall(ctx, base, sub_82F4DED0);
    ctx.fpscr.disableFlushModeUnconditional();
    const double sine = double(float(ctx.f1.f64));  // frsp f29,f1

    ctx.f1.f64 = cutoff;                            // fmr f1,f31
    skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);  // bl 0x82f4dfb0 -- cosine
    ctx.fpscr.disableFlushModeUnconditional();
    const double cosine = double(float(ctx.f1.f64));  // frsp f11,f1

    const double sine_term = MulSingle(sine, LoadSingle(base, kSineGain));  // fmuls f10,f29,f0
    const double unit = LoadSingle(base, kUnit);                   // lfs f0,-22460(r10)
    // The high-pass form: (1 + cos). sub_82B43CC0, the low-pass twin, does fsubs here instead.
    const double num = AddSingle(cosine, unit);                    // fadds f9,f11,f0
    const double cos_term = MulSingle(cosine, LoadSingle(base, kCosineGain));  // fmuls f8,f11,f13
    const double denom = AddSingle(sine_term, unit);               // fadds f7,f10,f0
    const double diff = SubSingle(unit, sine_term);                // fsubs f6,f0,f10
    const double inv = DivSingle(unit, denom);                     // fdivs f5,f0,f7
    const double scaled_denom = MulSingle(denom, LoadSingle(base, kDenomGain));  // fmuls f4,f7,f12
    const double half_num = MulSingle(num, inv);                   // fmuls f3,f9,f5
    // Store order is 184, 188, 192, 200, 196, 204 -- not ascending. Kept as the original has it.
    StoreSingle(base, object + kCoefficients + 0, MulSingle(cos_term, inv));  // fmuls f2 ; stfs 184
    StoreSingle(base, object + kCoefficients + 4, MulSingle(diff, inv));      // fmuls f1 ; stfs 188
    StoreSingle(base, object + kCoefficients + 8, DivSingle(num, scaled_denom));   // fdivs f0 ; stfs 192
    StoreSingle(base, object + kCoefficients + 16, DivSingle(num, scaled_denom));  // fdivs f13 ; stfs 200
    StoreSingle(base, object + kCoefficients + 12, NegateBits(half_num));     // fneg f12 ; stfs 196
    StoreSingle(base, object + kCachedCutoff, cutoff);                        // stfs f31,204(r30)
  }

  // loc_82B266AC
  uint32_t channels = REX_LOAD_U8(object + kChannelCount);          // lbz r11,42(r30)
  uint64_t index = 0;                                               // li r31,0
  const uint32_t src_desc = REX_LOAD_U32(stream + kStreamBufferA);  // lwz r29,28(r26)
  const uint32_t dst_desc = REX_LOAD_U32(stream + kStreamBufferB);  // lwz r28,32(r26)
  // beq cr6,0x82b26714
  if (channels != 0) {
    const uint64_t coefficients = object_reg + kCoefficients;  // addi r27,r30,184
    int64_t state = int64_t(object_reg) + kFilterState;        // addi r3,r30,56
    do {
      const uint32_t src_stride = REX_LOAD_U16(src_desc + kDescStride);  // lhz r11,14(r29)
      const uint32_t dst_stride = REX_LOAD_U16(dst_desc + kDescStride);  // lhz r10,14(r28)
      const uint32_t src_base = REX_LOAD_U32(src_desc + kDescBase);      // lwz r11,4(r29)
      const uint32_t dst_base = REX_LOAD_U32(dst_desc + kDescBase);      // lwz r9,4(r28)
      const uint64_t src_ptr = ChannelPointer(src_base, src_stride, index);  // mullw;rlwinm;add
      const uint64_t dst_ptr = ChannelPointer(dst_base, dst_stride, index);
      // bl 0x82b43af8 -- the 8-way-unrolled biquad: reads 256 floats at r5, writes 256 at r4,
      // and rewrites the four state words at r3. r7 is 256, so it never takes its scalar
      // fallback (sub_82B43978, entered only when the count is zero or not a multiple of eight).
      skate3::audio::GuestCall(ctx, base, sub_82B43AF8, uint64_t(state), dst_ptr, src_ptr,
                               coefficients, uint64_t(kBlockFrames));  // li r7,256
      channels = REX_LOAD_U8(object + kChannelCount);  // lbz r4,42(r30)
      index += 1;                                      // addi r31,r31,1
      // addi r3,r3,16 -- on whatever the callee left in r3, which sub_82B43AF8 never writes, so
      // the walk depends on the callee returning its first argument untouched. Reproduced as a
      // read of ctx.r3, not as a local increment.
      state = ctx.r3.s64 + 16;
    } while (uint32_t(index) < channels);              // cmplw cr6,r31,r4 ; blt
  }

  // loc_82B26714 -- the ping-pong swap. Both loads happen before both stores, so a native
  // reordering here would write the same descriptor into both slots.
  const uint32_t new_a = REX_LOAD_U32(stream + kStreamBufferB);  // lwz r11,32(r26)
  const uint32_t new_b = REX_LOAD_U32(stream + kStreamBufferA);  // lwz r10,28(r26)
  REX_STORE_U32(stream + kStreamBufferA, new_a);                 // stw r11,28(r26)
  REX_STORE_U32(stream + kStreamBufferB, new_b);                 // stw r10,32(r26)

  // loc_82B26724
  ctx.r3.s64 = 1;  // li r3,1
  ctx.f29 = saved_f29;
  ctx.f30 = saved_f30;
  ctx.f31 = saved_f31;
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// The union of both paths, all from entry state: the channel count, the two descriptors and the
// format pointer are readable before the first store, and nothing this call writes can change
// them (every store lands in the object above +56, in the two descriptor slots, or in the output
// block; the descriptors themselves are never written).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  if (object == 0 || stream == 0) return false;
  const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);
  if (format == 0) return false;  // the original loads the sample rate through it
  const uint32_t channels = REX_LOAD_U8(object + kChannelCount);
  // A count above eight would run the state array into the coefficients: a corrupted object, not
  // a case to compare.
  if (channels > kMaxChannels) return false;

  spec.write(object + kCoefficients, 24);  // 184..203 built inline, plus the cache at 204
  spec.write(stream + kStreamBufferA, 8);  // the descriptor swap
  if (channels != 0) {
    // Both paths rewrite the same region: the bypass zeroes four words per channel at 56+16i,
    // and sub_82B43AF8 rewrites the same four.
    spec.write(object + kFilterState, 16 * channels);

    const uint32_t src_desc = REX_LOAD_U32(stream + kStreamBufferA);
    const uint32_t dst_desc = REX_LOAD_U32(stream + kStreamBufferB);
    if (src_desc == 0 || dst_desc == 0) return false;
    const uint32_t src_base = REX_LOAD_U32(src_desc + kDescBase);
    const uint32_t dst_base = REX_LOAD_U32(dst_desc + kDescBase);
    if (src_base == 0 || dst_base == 0) return false;  // the callee would store through null
    const uint32_t src_stride = REX_LOAD_U16(src_desc + kDescStride);
    const uint32_t dst_stride = REX_LOAD_U16(dst_desc + kDescStride);
    // At most kMaxChannels spans of 1 KB; a stride under 256 overlaps, which is fine.
    for (uint32_t i = 0; i < channels && i < kMaxChannels; i++) {
      spec.write(uint32_t(ChannelPointer(dst_base, dst_stride, i)), kBlockFrames * 4);
      spec.read(uint32_t(ChannelPointer(src_base, src_stride, i)), kBlockFrames * 4);
    }
    spec.read(src_desc, 16);
    spec.read(dst_desc, 16);
  }
  spec.read(object + kChannelCount, 1);
  spec.read(object + kCutoffInput, 4);
  spec.read(object + kCoefficients, 24);
  spec.read(format + kFormatSampleRate, 4);
  spec.read(kCutoffScale, 4);
  spec.read(kCutoffFloor, 8);  // 2172 and 2176 are adjacent words
  spec.read(kZeroFloat, 4);
  spec.read(kSineGain, 4);
  spec.read(kUnit, 4);
  spec.read(kCosineGain, 4);
  spec.read(kDenomGain, 4);
  spec.read(kKernelBias, 4);
  return true;
}

}  // namespace port_82B26568
SKATE3_PORT(82B26568, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
