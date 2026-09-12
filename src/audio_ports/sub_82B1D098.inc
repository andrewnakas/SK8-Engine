// sub_82B1D098  role: round(f32[+0] * s32[+4] * s32[+8]) to s32, half away from zero (register-only)   lifted: skate3_recomp.66.cpp:66046 (72 lines)
// STATUS: verified
// calls/session: boot 694465  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D098.md
namespace port_82B1D098 {

// Two single-precision constants in the guest image, loaded at run time exactly as the
// original does. 0x82165A10 is the 0.0f cell docs/command-queue.md names; 0x8209975C is the
// rounding offset (presumably 0.5f; the port never assumes its value).
constexpr uint32_t kZeroF32Cell = 0x82160000u + 23056u;       // lis r9,-32234 ; lfs f13,23056(r9)
constexpr uint32_t kRoundOffsetF32Cell = 0x820A0000u - 26788u; // lis r11,-32246 ; lfs f13,-26788(r11)

inline double LoadF32AsF64(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// fctiwz, as lifted: NaN -> 0x80000000, above INT_MAX -> INT_MAX, else CVTTSD2SI (whose own
// out-of-range result is 0x80000000). The original spills the 64-bit result with stfd and
// reads back the LOW word with `lwz r3,-12(r1)`, so only the low 32 bits reach r3,
// zero-extended.
inline uint64_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX)) ? int64_t(INT_MAX)
                                                        : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return uint64_t(uint32_t(uint64_t(fixed) & 0xFFFFFFFFu));
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;

  const int32_t second = int32_t(REX_LOAD_U32(object + 4));  // lwz r11,4(r3)
  ctx.fpscr.disableFlushModeUnconditional();                  // (at lfs f0,0(r3))
  double value = LoadF32AsF64(base, object + 0);                    // lfs f0,0(r3)
  const int32_t third = int32_t(REX_LOAD_U32(object + 8));   // lwz r10,8(r3)

  // extsw r8,r11 ; std r8,-16(r1) ; lfd f12,-16(r1) ; fcfid f9,f12  -- the red-zone spill is a
  // bit copy into an FPR; the value converted is the sign-extended 32-bit field. Likewise r7.
  const double second_f64 = double(int64_t(second));         // fcfid f9,f12
  const double third_f64 = double(int64_t(third));           // fcfid f10,f11
  const double zero = LoadF32AsF64(base, kZeroF32Cell);            // lfs f13,23056(r9)
  const double second_f32 = double(float(second_f64));       // frsp f7,f9
  const double third_f32 = double(float(third_f64));         // frsp f8,f10
  const double product = double(float(third_f32 * second_f32));  // fmuls f6,f8,f7
  value = double(float(product * value));                    // fmuls f0,f6,f0
  ctx.cr6.compare(value, zero);                              // fcmpu cr6,f0,f13
  const double offset = LoadF32AsF64(base, kRoundOffsetF32Cell);   // lfs f13,-26788(r11)
  if (ctx.cr6.lt) {                                          // blt cr6,0x82b1d100
    ctx.fpscr.disableFlushModeUnconditional();               // (at fsubs)
    value = double(float(value - offset));                   // fsubs f0,f0,f13
  } else {
    value = double(float(value + offset));                   // fadds f0,f0,f13
  }
  // fctiwz f13,f0 ; stfd f13,-16(r1) ; lwz r3,-12(r1) ; blr
  ctx.r3.u64 = FctiwzLowWord(value);
}

// No stores outside the function's own red zone (-16(r1), spilled and reloaded twice, then the
// fctiwz result once); the result lives in r3 only.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 12);            // {f32 +0, s32 +4, s32 +8}
  spec.read(kZeroF32Cell, 4);           // the 0.0f compare constant
  spec.read(kRoundOffsetF32Cell, 4);    // the +/- rounding offset
  return true;
}

}  // namespace port_82B1D098
SKATE3_PORT(82B1D098, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
