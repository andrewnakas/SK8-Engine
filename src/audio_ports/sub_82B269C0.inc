// sub_82B269C0  role: place a panner at an angle and depth, via sine and cosine   lifted: skate3_recomp.67.cpp:21457 (109 lines)
// STATUS: verified
// calls/session: boot 218583  play 273708  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B269C0.md
namespace port_82B269C0 {

constexpr uint32_t kAngleScale = 0x822F8A6C;  // lis -32208 ; addi -31232 ; lfs 1132
constexpr uint32_t kZeroFloat = 0x82165A10;   // lis -32234 ; lfs 23056
constexpr uint32_t kAngleBias = 0x82060C44;   // lis -32250 ; lfs 3140
constexpr uint32_t kStoredAngle = 12;         // the angle this call settled on

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

double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r31,r3
  const double depth = ctx.f2.f64;     // fmr f30,f2
  // The original spills f29-f31 and r31 into its red zone and restores them on the way out,
  // because the three callees may clobber the volatile half of the register file. Locals do the
  // same job without touching the stack, and the harness compares f14-f31, so their entry
  // values must survive.
  const PPCRegister saved_f29 = ctx.f29;
  const PPCRegister saved_f30 = ctx.f30;
  const PPCRegister saved_f31 = ctx.f31;
  const PPCRegister saved_r31 = ctx.r31;

  ctx.fpscr.disableFlushModeUnconditional();
  const double angle = MulSingle(ctx.f1.f64, LoadSingle(base, kAngleScale));  // fmuls f31,f1,f0

  // bl 0x82f4dfb0 -- cosine. Both math callees write only their own red zone: no guest state,
  // no indirect calls, no timebase, so they replay cleanly and need no windows.
  ctx.f1.f64 = angle;
  skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);
  ctx.fpscr.disableFlushModeUnconditional();
  const double cosine = double(float(ctx.f1.f64));  // frsp f0,f1
  const double x = MulSingle(cosine, depth);        // fmuls f29,f0,f30

  ctx.f1.f64 = angle;                               // fmr f1,f31
  skate3::audio::GuestCall(ctx, base, sub_82F4DED0);  // bl 0x82f4ded0 -- sine
  ctx.fpscr.disableFlushModeUnconditional();
  const double sine = double(float(ctx.f1.f64));    // frsp f13,f1
  const double y = MulSingle(sine, depth);          // fmuls f2,f13,f30

  // bl 0x82b453d8 -- the unit-disc clamp, which stores x, y and the squared length at +0/+4/+8.
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  skate3::audio::GuestCall(ctx, base, sub_82B453D8, object);

  ctx.fpscr.disableFlushModeUnconditional();
  const double zero = LoadSingle(base, kZeroFloat);
  // fcmpu ; ble -- a positive depth keeps the angle as computed; otherwise it is biased. An
  // unordered compare (NaN depth) takes the bias path.
  if (depth > zero) {
    StoreSingle(base, object + kStoredAngle, angle);  // stfs f31,12(r31)
  } else {
    StoreSingle(base, object + kStoredAngle, AddSingle(angle, LoadSingle(base, kAngleBias)));
  }

  ctx.f29 = saved_f29;
  ctx.f30 = saved_f30;
  ctx.f31 = saved_f31;
  ctx.r31 = saved_r31;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;  // sub_82B453D8 would store through null
  // sub_82B453D8's three floats plus this function's angle word: one contiguous span.
  spec.write(object, 16);
  spec.read(kAngleScale, 4);
  spec.read(kZeroFloat, 4);
  spec.read(kAngleBias, 4);
  return true;
}

}  // namespace port_82B269C0
SKATE3_PORT(82B269C0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
