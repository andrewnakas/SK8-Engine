// sub_82B1D5D0  role: ramp an f32 value(+0) toward an s32 target(+24), re-deriving the rate when target/duration change; return round(value) (op 29 of the 0x82FD3600 table)   lifted: skate3_recomp.66.cpp:66826 (180 lines)
// STATUS: verified
// calls/session: boot 124236  play 147230  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D5D0.md
namespace port_82B1D5D0 {

// Layout read and written (nothing in docs/rw_audio_structs.h names this object):
//   +0  f32 value          current ramp position                       (written)
//   +4  f32 rate           per-step increment                          (written on re-derive)
//   +8  s32 last_target    target the rate was derived for             (written on re-derive)
//   +12 s32 last_duration  duration the rate was derived for           (written on re-derive)
//   +16 s32 duration
//   +20 s32 steps          multiplier applied to rate this call
//   +24 s32 target
constexpr uint32_t kValue = 0;
constexpr uint32_t kRate = 4;
constexpr uint32_t kLastTarget = 8;
constexpr uint32_t kLastDuration = 12;
constexpr uint32_t kDuration = 16;
constexpr uint32_t kSteps = 20;
constexpr uint32_t kTarget = 24;

// Guest singles, all loaded through the guest map by the body:
//   0x830775D8  lis r9,-31993 ; lfs f12,30168(r9)                -- a runtime global, stored by
//               sub_82B1E290 (stfs f0,30168(r7)); 0.0 in the image dump, so never assume a value
//   0x822F890C  lis r8,-32208 ; addi r7,r8,-31232 ; lfs f11,780(r7) -- 1/4096 in the image dump
//   0x82165A10  lis r9,-32234 ; lfs f12,23056(r9)                -- 0.0f
//   0x8209975C  lis r11,-32246 ; lfs f13,-26788(r11)             -- 0.5f
constexpr uint32_t kRateScaleGlobal = 0x830775D8;
constexpr uint32_t kRateUnitSingle = 0x822F890C;
constexpr uint32_t kZeroSingle = 0x82165A10;
constexpr uint32_t kHalfSingle = 0x8209975C;

// extsw ; std rN,-16(r1) ; lfd fM,-16(r1) ; fcfid ; frsp : a sign-extended word to single, held as double.
inline double WordToSingle(uint32_t word) {
  const int64_t wide = static_cast<int32_t>(word);             // extsw
  const double converted = static_cast<double>(wide);           // fcfid
  return static_cast<double>(static_cast<float>(converted));    // frsp
}

// fsubs / fadds / fmuls / fdivs / fmadds: the double operation rounded to single, as lifted.
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs: round the double to single and store its bits.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fctiwz f13,f0 ; stfd f13,-16(r1) ; lwz r3,-12(r1): as lifted, NaN -> 0x80000000, above INT_MAX
// -> INT_MAX, else CVTTSD2SI. The lwz takes the low word of the big-endian spill, zero-extended.
inline uint64_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX)) ? int64_t(INT_MAX)
                                                        : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(fixed);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                           // mr r11,r3
  const uint32_t target = REX_LOAD_U32(object + kTarget);       // lwz r3,24(r3)
  ctx.r3.u64 = target;  // r3 keeps the zero-extended target on both early returns
  ctx.fpscr.disableFlushModeUnconditional();                    // lfd f0,-16(r1)
  const double target_single = WordToSingle(target);            // extsw r10,r3 ; fcfid f12,f0 ; frsp f0,f12
  const double current = LoadSingle(base, object + kValue);     // lfs f13,0(r11)
  // fcmpu cr6,f0,f13 ; beqlr cr6   (unordered is not eq)
  if (target_single == current) return;

  // lwz r10,8(r11) ; cmpw cr6,r3,r10 ; bne cr6,0x82b1d614
  bool rederive = true;
  const int32_t last_target = static_cast<int32_t>(REX_LOAD_U32(object + kLastTarget));
  if (static_cast<int32_t>(target) == last_target) {
    const int32_t duration = static_cast<int32_t>(REX_LOAD_U32(object + kDuration));           // lwz r10,16(r11)
    const int32_t last_duration = static_cast<int32_t>(REX_LOAD_U32(object + kLastDuration));  // lwz r9,12(r11)
    // cmpw cr6,r10,r9 ; beq cr6,0x82b1d66c
    if (duration == last_duration) rederive = false;
  }

  if (rederive) {
    // loc_82B1D614
    const uint32_t duration = REX_LOAD_U32(object + kDuration);  // lwz r10,16(r11)
    REX_STORE_U32(object + kLastTarget, target);                 // stw r3,8(r11)
    REX_STORE_U32(object + kLastDuration, duration);             // stw r10,12(r11)
    // cmpwi cr6,r10,0 ; bgt cr6,0x82b1d630
    if (!(static_cast<int32_t>(duration) > 0)) {
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, object + kValue, target_single);         // stfs f0,0(r11)
      return;                                                    // blr (r3 = target)
    }
    // loc_82B1D630
    ctx.fpscr.disableFlushModeUnconditional();
    const double delta = SubSingle(target_single, current);      // fsubs f10,f0,f13
    const double frames = WordToSingle(duration);                // extsw r10,r10 ; fcfid f8,f9 ; frsp f7,f8
    const double scale = LoadSingle(base, kRateScaleGlobal);     // lfs f12,30168(r9)
    const double unit = LoadSingle(base, kRateUnitSingle);       // lfs f11,780(r7)
    const double per_frame = DivSingle(delta, frames);           // fdivs f6,f10,f7
    const double scaled = MulSingle(per_frame, scale);           // fmuls f5,f6,f12
    const double rate = MulSingle(scaled, unit);                 // fmuls f4,f5,f11
    StoreSingle(base, object + kRate, rate);                     // stfs f4,4(r11)
  }

  // loc_82B1D66C
  const uint32_t steps = REX_LOAD_U32(object + kSteps);          // lwz r10,20(r11)
  ctx.fpscr.disableFlushModeUnconditional();
  const double rate = LoadSingle(base, object + kRate);          // lfs f11,4(r11) (reloaded: may just have been stored)
  const double step_count = WordToSingle(steps);                 // extsw r8,r10 ; fcfid f8,f9 ; frsp f7,f8
  const double zero = LoadSingle(base, kZeroSingle);             // lfs f12,23056(r9)
  const bool falling = rate < zero;                              // fcmpu cr6,f11,f12 (unordered: not lt)
  const double next = FmaddSingle(step_count, rate, current);    // fmadds f6,f7,f11,f13
  StoreSingle(base, object + kValue, next);                      // stfs f6,0(r11)
  bool overshot;
  if (falling) {
    // blt cr6,0x82b1d6b4 ; loc_82B1D6B4: fcmpu cr6,f13,f0 ; bge cr6,0x82b1d6c0
    ctx.fpscr.disableFlushModeUnconditional();
    overshot = next < target_single;
  } else {
    // fcmpu cr6,f6,f0 ; ble cr6,0x82b1d6c0 ; b 0x82b1d6bc
    overshot = next > target_single;
  }
  if (overshot) {
    // loc_82B1D6BC
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, object + kValue, target_single);           // stfs f0,0(r11)
  }

  // loc_82B1D6C0
  ctx.fpscr.disableFlushModeUnconditional();
  const double value = LoadSingle(base, object + kValue);        // lfs f0,0(r11) (reloaded)
  const double half = LoadSingle(base, kHalfSingle);             // lfs f13,-26788(r11)
  // fcmpu cr6,f0,f12 ; blt cr6,0x82b1d6e8
  if (value < zero) {
    // loc_82B1D6E8
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.r3.u64 = FctiwzLowWord(SubSingle(value, half));          // fsubs f0,f0,f13 ; fctiwz ; stfd ; lwz r3,-12(r1) ; blr
  } else {
    ctx.r3.u64 = FctiwzLowWord(AddSingle(value, half));          // fadds f0,f0,f13 ; fctiwz ; stfd ; lwz r3,-12(r1) ; blr
  }
}

// Writes: value, rate, last_target, last_duration -- the object's first 16 bytes. Declared as one
// span on every path (the early return writes none of it, the duration <= 0 path skips +4); bytes
// the call leaves alone compare equal. The red-zone spills at -16(r1) are the function's own
// frame and are not windowed. Reads: duration, steps, target and the four guest singles.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.write(object + kValue, 16);
  spec.read(object + kDuration, 12);
  spec.read(kRateScaleGlobal, 4);
  spec.read(kRateUnitSingle, 4);
  spec.read(kZeroSingle, 4);
  spec.read(kHalfSingle, 4);
  return true;
}

}  // namespace port_82B1D5D0
SKATE3_PORT(82B1D5D0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
