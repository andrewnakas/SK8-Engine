// sub_82B43CC0  role: build the five normalised low-pass biquad coefficients from one angle   lifted: skate3_recomp.68.cpp:25575 (111 lines)
// STATUS: verified
// calls/session: boot 4400  play 3226  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B43CC0.md
namespace port_82B43CC0 {

// The five words written at r3, in the order sub_82B43AF8 consumes them. Reading the four
// constants out of the dumped image (0.5, 1.0, -2.0, 2.0) makes this the textbook RBJ low-pass
// at Q = 1: alpha = sin(w)/2, a0 = 1 + alpha, b0 = b2 = (1 - cos w)/2, b1 = 1 - cos w,
// a1 = -2 cos w, a2 = 1 - alpha -- every coefficient divided through by a0.
constexpr uint32_t kA1 = 0;   // -2 cos(w) / a0
constexpr uint32_t kA2 = 4;   // (1 - alpha) / a0
constexpr uint32_t kB0 = 8;   // (1 - cos w) / (2 * a0)
constexpr uint32_t kB1 = 12;  // (1 - cos w) / a0
constexpr uint32_t kB2 = 16;  // the same expression as kB0, divided a SECOND time
constexpr uint32_t kCoefficientBytes = 20;

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, never read by
// eye, then confirmed by reading the word out of the dumped image (probe/harness/out/image).
constexpr uint32_t kHalf = 0x8209975C;      // lis -32246 ; lfs -26788  == 0.5
constexpr uint32_t kOne = 0x8231A844;       // lis -32206 ; lfs -22460  == 1.0
constexpr uint32_t kMinusTwo = 0x82094178;  // lis -32247 ; lfs 16760   == -2.0
constexpr uint32_t kTwo = 0x82060C50;       // lis -32250 ; lfs 3152    == 2.0

// stwu r1,-112(r1). Reproduced, unlike most ports' frames: both callees spill f30/f31 and their
// scratch BELOW r1, so without the move their stores land where this function's own spills went
// and the native run's out-of-window footprint stops matching the original's.
constexpr uint32_t kFrameSize = 112;

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

double RoundSingle(double a) { return double(float(a)); }  // frsp
double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }
double DivSingle(double a, double b) { return double(float(a / b)); }

REX_FUNC(Native) {
  const uint32_t coefficients = ctx.r3.u32;  // mr r31,r3
  const double angle = ctx.f1.f64;           // fmr f31,f1
  // The original spills r31, f30 and f31 into its red zone and restores them on the way out; the
  // harness compares r13-r31 and f14-f31, so their entry values have to survive here too. Both
  // callees are themselves ABI-correct, so this is belt and braces -- and it is exactly right:
  // the original's exit values ARE its entry values.
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister saved_f30 = ctx.f30;
  const PPCRegister saved_f31 = ctx.f31;
  const PPCRegister entry_r1 = ctx.r1;

  ctx.fpscr.disableFlushModeUnconditional();

  // stwu r1,-112(r1): the back chain, then r1 moves. Written with the same value the original
  // writes, so the bytes outside the windows are identical after either run.
  const uint32_t frame = entry_r1.u32 - kFrameSize;
  REX_STORE_U32(frame, entry_r1.u32);
  ctx.r1.u32 = frame;

  // bl 0x82f4ded0 -- sine. Writes nothing but its own frame: no guest state, no indirect call,
  // no timebase, so it replays cleanly and needs no window.
  ctx.f1.f64 = angle;
  skate3::audio::GuestCall(ctx, base, sub_82F4DED0);
  ctx.fpscr.disableFlushModeUnconditional();
  const double sine = RoundSingle(ctx.f1.f64);  // frsp f30,f1

  // bl 0x82f4dfb0 -- cosine.
  ctx.f1.f64 = angle;                           // fmr f1,f31
  skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);
  ctx.fpscr.disableFlushModeUnconditional();
  const double cosine = RoundSingle(ctx.f1.f64);  // frsp f11,f1

  const double half = LoadSingle(base, kHalf);            // lfs f0,-26788(r11)
  const double alpha = MulSingle(sine, half);             // fmuls f10,f30,f0
  const double one = LoadSingle(base, kOne);              // lfs f0,-22460(r10)
  const double minus_two = LoadSingle(base, kMinusTwo);   // lfs f13,16760(r9)
  const double two = LoadSingle(base, kTwo);              // lfs f12,3152(r8)
  const double one_minus_cos = SubSingle(one, cosine);    // fsubs f9,f0,f11
  const double neg_two_cos = MulSingle(cosine, minus_two);  // fmuls f8,f11,f13
  const double a0 = AddSingle(alpha, one);                // fadds f7,f10,f0
  const double one_minus_alpha = SubSingle(one, alpha);   // fsubs f6,f0,f10
  const double inv_a0 = DivSingle(one, a0);               // fdivs f5,f0,f7
  const double two_a0 = MulSingle(a0, two);               // fmuls f4,f7,f12

  // Store order kept as written: each coefficient is stored immediately after it is computed.
  StoreSingle(base, coefficients + kA1, MulSingle(neg_two_cos, inv_a0));       // fmuls f3 ; stfs 0
  StoreSingle(base, coefficients + kA2, MulSingle(one_minus_alpha, inv_a0));   // fmuls f2 ; stfs 4
  StoreSingle(base, coefficients + kB0, DivSingle(one_minus_cos, two_a0));     // fdivs f1 ; stfs 8
  StoreSingle(base, coefficients + kB1, MulSingle(one_minus_cos, inv_a0));     // fmuls f0 ; stfs 12
  // fdivs f13,f9,f4 -- the SAME quotient computed a second time rather than reusing f1. Two
  // single-rounded divides of identical operands agree, but the second one is kept: a mul by
  // 0.5 or a copy of kB0 would not be the same instruction.
  StoreSingle(base, coefficients + kB2, DivSingle(one_minus_cos, two_a0));     // stfs f13,16

  ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,112
  ctx.r31 = saved_r31;
  ctx.f30 = saved_f30;
  ctx.f31 = saved_f31;
}

// One span, from entry state: the twenty bytes at r3. Both call sites pass the interior of a
// filter object (obj+184 from sub_82B27E20, obj+172 from sub_82B2D5D4) and neither reads a
// register result, so the window is the whole observable output.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t coefficients = ctx.r3.u32;
  if (coefficients == 0) return false;  // the original would store through null
  spec.write(coefficients, kCoefficientBytes);
  spec.read(kHalf, 4);
  spec.read(kOne, 4);
  spec.read(kMinusTwo, 4);
  spec.read(kTwo, 4);
  // The image's sine and cosine, as sub_82B43D78 declares them: their table, 1.0 and the NaN cell.
  // For replay only (reads cost nothing against the budget). Values from sub_82F4DED0.inc.
  spec.read(0x82FB4E20u, 120);
  spec.read(0x82010108u, 8);
  spec.read(0x82FB58C8u, 8);
  return true;
}

}  // namespace port_82B43CC0
SKATE3_PORT(82B43CC0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
