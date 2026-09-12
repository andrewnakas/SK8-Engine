// sub_82F4DE80  role: floor(double) -- f1 in, largest integral value <= f1 out in f1 (register-only leaf)   lifted: skate3_recomp.106.cpp:53797 (34 lines)
// STATUS: verified
// calls/session: boot 2412020  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82F4DE80.md
namespace port_82F4DE80 {

// Two doubles in the image's constant pool, reached by lis + displacement. Nothing in
// docs/rw_audio_structs.h names them. By the algorithm's shape they should be 1.0 (the
// step-down) and 2^52 (the "already integral" magnitude), but the body loads them rather
// than assuming: a wrong guess would not be caught by the harness if it also read them.
constexpr uint32_t kPoolStepDown = 0x82010000u + 264;    // lis r11,-32255 ; lfd f13,264(r11)
constexpr uint32_t kPoolIntegralMag = 0x82050000u + 5336;  // lis r10,-32251 ; lfd f0,5336(r10)

REX_FUNC(Native) {
  const double x = ctx.f1.f64;

  // fctidz f12,f1 -- truncate toward zero to int64. The edge handling is the lifted line's,
  // verbatim: NaN -> 0x8000000000000000; above 2^63 -> LLONG_MAX; otherwise cvttsd2si, which
  // itself yields 0x8000000000000000 for anything else out of int64 range.
  ctx.fpscr.disableFlushModeUnconditional();
  const int64_t truncated_bits =
      std::isnan(x)             ? int64_t(0x8000000000000000ULL)
      : (x > double(LLONG_MAX)) ? LLONG_MAX
                                : simde_mm_cvttsd_si64(simde_mm_load_sd(&ctx.f1.f64));

  // fabs f11,f1
  uint64_t abs_bits = ctx.f1.u64 & ~0x8000000000000000ULL;
  double abs_x;
  std::memcpy(&abs_x, &abs_bits, sizeof abs_x);

  // lfd f13,264(r11) ; lfd f0,5336(r10)
  const uint64_t step_down_bits = REX_LOAD_U64(kPoolStepDown);
  const uint64_t integral_mag_bits = REX_LOAD_U64(kPoolIntegralMag);
  double step_down, integral_mag;
  std::memcpy(&step_down, &step_down_bits, sizeof step_down);
  std::memcpy(&integral_mag, &integral_mag_bits, sizeof integral_mag);

  // fcfid f12,f12
  const double truncated = double(truncated_bits);
  // fsub f0,f0,f11 -- (2^52 - |x|): non-negative when |x| is small enough to have a fraction
  const double magnitude_margin = integral_mag - abs_x;
  // fneg f11,f11 -- (-|x|): non-negative only for +/-0 (NaN falls through fsel's else)
  uint64_t neg_abs_bits = abs_bits ^ 0x8000000000000000ULL;
  double neg_abs_x;
  std::memcpy(&neg_abs_x, &neg_abs_bits, sizeof neg_abs_x);
  // fsub f10,f1,f12 -- the fractional part, signed
  const double fraction = x - truncated;
  // fsub f13,f12,f13 -- trunc - 1
  const double stepped_down = truncated - step_down;

  // fsel f13,f10,f12,f13 -- floor: trunc when the fraction is >= 0, else trunc - 1
  const double floored = fraction >= 0.0 ? truncated : stepped_down;
  // fsel f0,f0,f13,f1 -- |x| within 2^52 takes the floor; larger magnitudes are already integral
  const double bounded = magnitude_margin >= 0.0 ? floored : x;
  // fsel f1,f11,f1,f0 -- +/-0 passes through unchanged (keeps the sign); everything else takes f0
  ctx.f1.f64 = neg_abs_x >= 0.0 ? x : bounded;
  // blr
}

// No stores. The result is f1 only; f0, f10-f13, r10, r11 are volatile scratch the ABI lets the
// caller discard, and every call site reads f1 immediately (frsp fN,f1). The two pool constants
// are recorded as inputs so the vector replays.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  spec.read(kPoolStepDown, 8);
  spec.read(kPoolIntegralMag, 8);
  return true;
}

}  // namespace port_82F4DE80
SKATE3_PORT(82F4DE80, skate3::audio::kPortVerified, skate3::audio::kReturnF1)
