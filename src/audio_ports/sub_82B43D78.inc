// sub_82B43D78  role: build the five normalised high-shelf biquad coefficients from an angle and a gain   lifted: skate3_recomp.68.cpp:25687 (163 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 23 comparable calls in the weaker profile, 257 in the stronger, against 163 lifted lines -- thin in both profiles. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 82  play 514  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B43D78.md
namespace port_82B43D78 {

// The five words written at r3 -- the same [a1, a2, b0, b1, b2] / a0 block sub_82B43CC0 builds for
// its low-pass. With the constants read out of the dumped image (0.70710653, 1.0, 2.0, -2.0) and
// A = sqrt(f2), this is the RBJ high shelf at slope S = 1: alpha = sin(w) * 0.70710653,
//   a0 = (A+1) - (A-1)cos w + 2 sqrt(A) alpha      a1 = 2((A-1) - (A+1)cos w)
//   a2 = (A+1) - (A-1)cos w - 2 sqrt(A) alpha      b0 = A((A+1) + (A-1)cos w + 2 sqrt(A) alpha)
//   b1 = -2A((A-1) + (A+1)cos w)                    b2 = A((A+1) + (A-1)cos w - 2 sqrt(A) alpha)
constexpr uint32_t kA1 = 0;
constexpr uint32_t kA2 = 4;
constexpr uint32_t kB0 = 8;
constexpr uint32_t kB1 = 12;
constexpr uint32_t kB2 = 16;
constexpr uint32_t kCoefficientBytes = 20;

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset, confirmed against the dumped
// image (probe/harness/out/image).
constexpr uint32_t kOne = 0x8231A844;         // lis -32206 ; lfs -22460           == 1.0
constexpr uint32_t kAlphaScale = 0x822F8E50;  // lis -32208 ; addi -31232 ; lfs 2128 == 0.70710653
constexpr uint32_t kTwo = 0x82060C50;         // lis -32250 ; lfs 3152             == 2.0
constexpr uint32_t kMinusTwo = 0x82094178;    // lis -32247 ; lfs 16760            == -2.0

// stwu r1,-128(r1). Reproduced for the reason port_82B43CC0 gives: both callees spill f30/f31 and
// scratch below r1, and without the move they would land on this function's own spill slots.
constexpr uint32_t kFrameSize = 128;

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

double RoundSingle(double a) { return double(float(a)); }                // frsp
double SqrtSingle(double a) { return double(float(sqrt(a))); }           // fsqrts
double MulSingle(double a, double b) { return double(float(a * b)); }    // fmuls
double AddSingle(double a, double b) { return double(float(a + b)); }    // fadds
double SubSingle(double a, double b) { return double(float(a - b)); }    // fsubs
double DivSingle(double a, double b) { return double(float(a / b)); }    // fdivs
// fmadds d,a,c,b = a*c + b, one rounding to double then one to single, as lifted.
double MaddSingle(double a, double c, double b) { return double(float(std::fma(a, c, b))); }
// fnmsubs d,a,c,b = b - a*c, lifted as -fma(a, c, -b) so the sign of a zero result matches.
double NmsubSingle(double a, double c, double b) { return double(float(-std::fma(a, c, -b))); }

REX_FUNC(Native) {
  const uint32_t coefficients = ctx.r3.u32;  // mr r31,r3
  const double angle = ctx.f1.f64;           // fmr f30,f1
  const double gain = ctx.f2.f64;            // fmr f31,f2
  // The original spills r31 and f29-f31 into its red zone and restores them on the way out; the
  // harness compares r13-r31 and f14-f31, so the entry values have to survive.
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister saved_f29 = ctx.f29;
  const PPCRegister saved_f30 = ctx.f30;
  const PPCRegister saved_f31 = ctx.f31;

  ctx.fpscr.disableFlushModeUnconditional();  // stfd f29,-40(r1)

  // stwu r1,-128(r1): the back chain, then r1 moves.
  const uint32_t frame = ctx.r1.u32 - kFrameSize;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;

  // bl 0x82f4ded0 -- sine, on the entry f1 (never moved). Both math callees write only their own
  // frame below r1: no guest state, no indirect call, no timebase, so they need no window.
  ctx.f1.f64 = angle;
  skate3::audio::GuestCall(ctx, base, sub_82F4DED0);
  ctx.fpscr.disableFlushModeUnconditional();
  const double sine = RoundSingle(ctx.f1.f64);  // frsp f29,f1

  ctx.f1.f64 = angle;  // fmr f1,f30
  skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);  // bl 0x82f4dfb0 -- cosine
  ctx.fpscr.disableFlushModeUnconditional();

  const double a = SqrtSingle(gain);                        // fsqrts f10,f31
  const double one = LoadSingle(base, kOne);                // lfs f13,-22460(r10)
  const double alpha_scale = LoadSingle(base, kAlphaScale); // lfs f12,2128(r9)
  const double two = LoadSingle(base, kTwo);                // lfs f0,3152(r8)
  const double minus_two = LoadSingle(base, kMinusTwo);     // lfs f11,16760(r7)
  const double cosine = RoundSingle(ctx.f1.f64);            // frsp f9,f1
  const double a_plus = AddSingle(a, one);                  // fadds f8,f10,f13
  const double a_minus = SubSingle(a, one);                 // fsubs f7,f10,f13
  const double alpha = MulSingle(sine, alpha_scale);        // fmuls f6,f29,f12

  // sqrt(A) is taken four separate times (f5, f2, f4, f2); kept as four fsqrts.
  const double beta0 = MulSingle(SqrtSingle(a), alpha);     // fsqrts f5 ; fmuls f4,f5,f6
  const double a0_base = NmsubSingle(a_minus, cosine, a_plus);  // fnmsubs f3,f7,f9,f8
  const double beta1 = SqrtSingle(a);                       // fsqrts f2,f10
  const double a0 = MaddSingle(beta0, two, a0_base);        // fmadds f1,f4,f0,f3
  const double beta2 = MulSingle(beta1, alpha);             // fmuls f12,f2,f6
  const double aminus_cos = MulSingle(a_minus, cosine);      // fmuls f5,f7,f9
  const double beta3 = SqrtSingle(a);                       // fsqrts f4,f10
  const double inv_a0 = DivSingle(one, a0);                 // fdivs f3,f13,f1
  const double beta4 = SqrtSingle(a);                       // fsqrts f2,f10
  const double b0_mid = MaddSingle(beta2, two, aminus_cos);  // fmadds f1,f12,f0,f5
  const double b1_base = MaddSingle(a_plus, cosine, a_minus);   // fmadds f13,f8,f9,f7
  const double b2_base = MaddSingle(a_minus, cosine, a_plus);   // fmadds f12,f7,f9,f8
  const double beta5 = MulSingle(beta3, alpha);             // fmuls f5,f4,f6
  const double a1_base = NmsubSingle(a_plus, cosine, a_minus);  // fnmsubs f4,f8,f9,f7
  const double a2_base = NmsubSingle(a_minus, cosine, a_plus);  // fnmsubs f9,f7,f9,f8
  const double beta6 = MulSingle(beta4, alpha);             // fmuls f7,f2,f6
  const double b0_sum = AddSingle(b0_mid, a_plus);          // fadds f6,f1,f8
  const double b1_norm = MulSingle(b1_base, inv_a0);        // fmuls f2,f13,f3
  const double b2_sum = NmsubSingle(beta5, two, b2_base);   // fnmsubs f1,f5,f0,f12
  const double a1_norm = MulSingle(a1_base, inv_a0);        // fmuls f13,f4,f3
  const double a2_sum = NmsubSingle(beta6, two, a2_base);   // fnmsubs f12,f7,f0,f9
  const double b0_norm = MulSingle(b0_sum, inv_a0);         // fmuls f9,f6,f3
  const double b1_scaled = MulSingle(b1_norm, a);           // fmuls f8,f2,f10
  const double b2_norm = MulSingle(b2_sum, inv_a0);         // fmuls f7,f1,f3

  // Stores, in the original order, each right after its final multiply.
  StoreSingle(base, coefficients + kA1, MulSingle(a1_norm, two));        // fmuls f6 ; stfs f6,0(r31)
  StoreSingle(base, coefficients + kA2, MulSingle(a2_sum, inv_a0));      // fmuls f5 ; stfs f5,4(r31)
  StoreSingle(base, coefficients + kB0, MulSingle(b0_norm, a));          // fmuls f4 ; stfs f4,8(r31)
  StoreSingle(base, coefficients + kB1, MulSingle(b1_scaled, minus_two));  // fmuls f3 ; stfs f3,12(r31)
  StoreSingle(base, coefficients + kB2, MulSingle(b2_norm, a));          // fmuls f2 ; stfs f2,16(r31)

  ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,128
  ctx.r31 = saved_r31;                   // ld r31,-16(r1)
  ctx.f29 = saved_f29;                   // lfd f29,-40(r1)
  ctx.f30 = saved_f30;                   // lfd f30,-32(r1)
  ctx.f31 = saved_f31;                   // lfd f31,-24(r1)
}

// One span, from entry state: the twenty bytes at r3. The single call site (sub_82B26740, at
// 0x82B267E0) passes obj+196 and reads no register result afterwards.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t coefficients = ctx.r3.u32;
  if (coefficients == 0) return false;  // the original would store through null
  spec.write(coefficients, kCoefficientBytes);
  spec.read(kOne, 4);
  spec.read(kAlphaScale, 4);
  spec.read(kTwo, 4);
  spec.read(kMinusTwo, 4);
  // The image's sine and cosine, which the coefficient build reaches: their table, 1.0 and the NaN
  // cell. Declared for replay only (reads cost nothing against the budget); without them every
  // vector on the rebuild path is unreplayable. Values from recomp/src/audio_ports/sub_82F4DED0.inc.
  spec.read(0x82FB4E20u, 120);
  spec.read(0x82010108u, 8);
  spec.read(0x82FB58C8u, 8);
  return true;
}

}  // namespace port_82B43D78
SKATE3_PORT(82B43D78, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnNone)
