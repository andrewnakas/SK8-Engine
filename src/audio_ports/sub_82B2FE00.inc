// sub_82B2FE00  role: clamp a rate into range, then fill a five-point ramp plus its span   lifted: skate3_recomp.67.cpp:44204 (113 lines)
// STATUS: verified
// calls/session: boot 4  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2FE00.md
namespace port_82B2FE00 {

// Six constants, each loaded at run time exactly as the original does. The addresses come from
// the lis/addi pairs; no value is assumed.
constexpr uint32_t kPool = 0x822F8600;            // lis -32208 ; addi -31232
constexpr uint32_t kUpperLimit = kPool + 468;
constexpr uint32_t kRampBase = kPool + 472;
constexpr uint32_t kRampStart = kPool + 476;
constexpr uint32_t kLowerLimit = 0x82060C50;      // lis -32250 ; lfs 3152
constexpr uint32_t kScaleA = 0x8208824C;          // lis -32247 ; lfs -32180
constexpr uint32_t kScaleB = 0x822249B4;          // lis -32222 ; lfs 18868 (0x49B4, not 0x4BB4)
constexpr uint32_t kCeiling = 0x820ED57C;         // lis -32241 ; lfs -10884
constexpr uint32_t kStepScale = 0x82099280;       // lis -32246 ; lfs -28032

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

// fmuls / fadds / fsubs: the double op rounded to single, as each lifted line does it.
double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }

REX_FUNC(Native) {
  const uint32_t rate_field = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  ctx.fpscr.disableFlushModeUnconditional();
  const double rate = LoadSingle(base, rate_field + 0);  // lfs f0,0(r3)
  double limit = LoadSingle(base, kUpperLimit);          // lfs f13,468(r11)
  // fcmpu ; bgt -- above the upper limit clamps to it. Otherwise the lower limit is loaded and
  // an unordered compare (NaN) falls into the clamp too, storing the LOWER limit.
  bool clamp = rate > limit;
  if (!clamp) {
    limit = LoadSingle(base, kLowerLimit);  // lfs f13,3152(r10)
    clamp = !(rate >= limit);               // bge, so NaN clamps
  }
  if (clamp) {
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, rate_field + 0, limit);  // stfs f13,0(r3)
  }
  ctx.fpscr.disableFlushModeUnconditional();
  // The rate is RELOADED after the possible clamp rather than reusing the register.
  const double clamped = LoadSingle(base, rate_field + 0);        // lfs f13,0(r3)
  double first = MulSingle(clamped, LoadSingle(base, kScaleA));   // fmuls f0,f13,f0
  double span = MulSingle(first, LoadSingle(base, kScaleB));      // fmuls f13,f0,f13
  const double ceiling = LoadSingle(base, kCeiling);              // lfs f12,-10884(r8)
  if (span > ceiling) {
    // Over the ceiling: the span becomes the ceiling, the ramp starts from the pool's own base,
    // and the rate field is overwritten with the pool's start value.
    span = ceiling;                                   // fmr f13,f12
    const double start = LoadSingle(base, kRampStart);  // lfs f12,476(r11)
    first = LoadSingle(base, kRampBase);                // lfs f0,472(r11)
    StoreSingle(base, rate_field + 0, start);           // stfs f12,0(r3)
  }
  ctx.fpscr.disableFlushModeUnconditional();
  const double width = SubSingle(span, first);                 // fsubs f12,f13,f0
  StoreSingle(base, out + 20, span);                           // stfs f13,20(r4)
  ctx.r3.s64 = 1;                                              // li r3,1
  StoreSingle(base, out + 0, first);                           // stfs f0,0(r4)
  const double step = MulSingle(width, LoadSingle(base, kStepScale));  // fmuls f11,f12,f13
  const double p1 = AddSingle(step, first);                    // fadds f10,f11,f0
  StoreSingle(base, out + 4, p1);
  const double p2 = AddSingle(p1, step);                       // fadds f9,f10,f11
  StoreSingle(base, out + 8, p2);
  const double p3 = AddSingle(p2, step);                       // fadds f8,f9,f11
  StoreSingle(base, out + 12, p3);
  const double p4 = AddSingle(p3, step);                       // fadds f7,f8,f11
  StoreSingle(base, out + 16, p4);
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r3.u32 + 0, 4);  // the clamp, on either path; unwritten bytes compare equal
  spec.write(ctx.r4.u32 + 0, 24);
  spec.read(ctx.r3.u32, 4);
  spec.read(kUpperLimit, 4);
  spec.read(kRampBase, 8);
  spec.read(kLowerLimit, 4);
  spec.read(kScaleA, 4);
  spec.read(kScaleB, 4);
  spec.read(kCeiling, 4);
  spec.read(kStepScale, 4);
  return true;
}

}  // namespace port_82B2FE00
SKATE3_PORT(82B2FE00, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
