// sub_82B2DAC8  role: recompute a voice's pitch ratio and advance its fractional position   lifted: skate3_recomp.67.cpp:38898 (141 lines)
// STATUS: verified
// calls/session: boot 204194  play 306361  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2DAC8.md
namespace port_82B2DAC8 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, not read off by eye: one digit
// misread in sub_82B2FE00 cost a shadow divergence.
constexpr uint32_t kRatioScale = 0x820B4114;   // lis -32245 ; lfs 16660
constexpr uint32_t kZeroFloat = 0x82165A10;    // lis -32234 ; lfs 23056
constexpr uint32_t kRoundHalf = 0x8209975C;    // lis -32246 ; lfs -26788
constexpr uint32_t kRatioCeiling = 0x82257308; // lis -32219 ; lfs 29448
constexpr int32_t kStepLimit = 262144;         // lis r10,4

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

// fctiwz then stfd/lwz of the low word: NaN gives 0x80000000, above INT_MAX saturates, and only
// the low 32 bits of the spilled doubleword reach the register.
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(static_cast<uint64_t>(fixed) & 0xFFFFFFFFu);
}

REX_FUNC(Native) {
  const uint32_t voice = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  const uint32_t frames = ctx.r6.u32;  // the block length, also stored at +78
  const uint32_t format = REX_LOAD_U32(stream + 40);
  ctx.fpscr.disableFlushModeUnconditional();
  const double requested = LoadSingle(base, voice + 64);
  const double scale = LoadSingle(base, voice + 52);
  const double previous = LoadSingle(base, voice + 60);
  const double source_rate = LoadSingle(base, format + 12);
  const double ratio = MulSingle(DivSingle(requested, source_rate), scale);  // fdivs ; fmuls
  uint32_t step = 0;
  // fcmpu ; beq -- an unchanged ratio skips the whole recompute, including its stores.
  if (previous != ratio) {
    double scaled = MulSingle(ratio, LoadSingle(base, kRatioScale));
    const double zero = LoadSingle(base, kZeroFloat);
    const double half = LoadSingle(base, kRoundHalf);
    // blt on the compare against zero: round half away from zero, then truncate.
    if (scaled < zero) {
      ctx.fpscr.disableFlushModeUnconditional();
      scaled = SubSingle(scaled, half);
    } else {
      scaled = AddSingle(scaled, half);
    }
    step = FctiwzLowWord(scaled);
    if (static_cast<int32_t>(step) > kStepLimit) {
      // Over the limit: the stored ratio becomes a pool constant and the step is clamped.
      step = static_cast<uint32_t>(kStepLimit);
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, voice + 56, LoadSingle(base, kRatioCeiling));  // stfs f0,56(r3)
    } else {
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, voice + 56, ratio);  // stfs f12,56(r3)
    }
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, voice + 60, ratio);    // stfs f12,60(r3)
    REX_STORE_U32(voice + 68, step);         // stw r11,68(r3)
  }
  // The tail runs on both paths, and re-reads the step and the ratio from memory.
  const uint32_t stored_step = REX_LOAD_U32(voice + 68);
  ctx.fpscr.disableFlushModeUnconditional();
  const double stored_ratio = LoadSingle(base, voice + 56);
  const uint32_t fraction = REX_LOAD_U32(voice + 72);
  // mullw keeps the full 64-bit product of the sign-extended operands.
  const uint64_t advanced =
      static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(stored_step)) *
                            static_cast<int64_t>(static_cast<int32_t>(frames)));
  REX_STORE_U16(voice + 78, static_cast<uint16_t>(frames));  // sth r6,78(r3)
  const uint32_t bias = REX_LOAD_U8(voice + 80);
  const double gain = LoadSingle(base, stream + 56);
  const uint32_t credit = REX_LOAD_U8(voice + 81);
  StoreSingle(base, stream + 56, MulSingle(stored_ratio, gain));  // stfs f12,56(r4)
  const uint64_t position = advanced + fraction;
  // rlwinm r5,r6,16,16,31 takes the high half of the low word: whole frames consumed.
  const uint64_t whole = static_cast<uint32_t>(position) >> 16;
  // The remaining count, in 64 bits: a borrow here leaves the upper half set, and the sign test
  // below looks only at the LOW word, so both halves matter. Reproduced as 64-bit arithmetic.
  const uint64_t remaining = whole - bias + credit;
  const uint64_t sign = (static_cast<uint32_t>(remaining) >> 31) & 1u;
  const uint64_t mask = sign - 1u;  // addi r4,r10,-1: 0 when negative, all ones otherwise
  ctx.r3.u64 = mask & remaining;    // and r3,r4,r11 -- clamps a negative result to zero
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t voice = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  const uint32_t format = REX_LOAD_U32(stream + 40);
  if (format == 0) return false;  // the original would load through null
  // The union of both paths; a byte this call does not write compares equal anyway.
  spec.write(voice + 56, 8);   // the ratio at +56 and its previous value at +60
  spec.write(voice + 68, 4);
  spec.write(voice + 78, 2);
  spec.write(stream + 56, 4);
  spec.read(voice + 52, 32);   // 52..83: the rates, the step, the fraction, the two bytes
  spec.read(stream + 40, 4);
  spec.read(stream + 56, 4);
  spec.read(format + 12, 4);
  spec.read(kRatioScale, 4);
  spec.read(kZeroFloat, 4);
  spec.read(kRoundHalf, 4);
  spec.read(kRatioCeiling, 4);
  return true;
}

}  // namespace port_82B2DAC8
SKATE3_PORT(82B2DAC8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
