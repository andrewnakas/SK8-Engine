// sub_82B453D8  role: store a 2D vector (f1,f2) and its squared length at r3+0/+4/+8, clamped to the unit disc: length^2 in (0.999,1) snaps +8 to 1; length^2 > 1 normalises x,y and stores 1   lifted: skate3_recomp.68.cpp:29082 (66 lines)
// STATUS: verified
// calls/session: boot 72810  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B453D8.md
namespace port_82B453D8 {

// Output record (nothing in docs/rw_audio_structs.h names it): three singles.
constexpr uint32_t kX = 0;
constexpr uint32_t kY = 4;
constexpr uint32_t kLengthSq = 8;
constexpr uint32_t kRecordBytes = 12;

// Two rodata singles, values read from the validated image dump (probe/harness/out/image):
//   0x8231A844  1.0f             (lis r11,-32206 ; lfs f13,-22460(r11))
//   0x820ED800  0.999f 3F7FBE77  (lis r11,-32241 ; lfs f12,-10240(r11))
// The body loads them through the guest map anyway, so the values need not be trusted here.
constexpr uint32_t kOneSingle = 0x8231A844;
constexpr uint32_t kSnapSingle = 0x820ED800;

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fmuls / fdivs / fsqrts: the double operation rounded to single, as the lifted line does it.
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double SqrtSingle(double a) { return static_cast<double>(static_cast<float>(sqrt(a))); }

REX_FUNC(Native) {
  const uint32_t out = ctx.r3.u32;
  const double x = ctx.f1.f64;
  const double y = ctx.f2.f64;
  ctx.fpscr.disableFlushModeUnconditional();                         // fmuls f0,f2,f2
  const double y_sq = MulSingle(y, y);
  StoreSingle(base, out + kX, x);                                    // stfs f1,0(r3)
  StoreSingle(base, out + kY, y);                                    // stfs f2,4(r3)
  const double one = LoadSingle(base, kOneSingle);                   // lfs f13,-22460(r11)
  // fmadds f0,f1,f1,f0 -- a genuine fused multiply-add, rounded once to double then to single
  const double length_sq = static_cast<double>(static_cast<float>(std::fma(x, x, y_sq)));
  StoreSingle(base, out + kLengthSq, length_sq);                     // stfs f0,8(r3)

  // fcmpu cr6,f0,f13 ; bge cr6,loc_82B45414 -- bge is "not lt", so a NaN takes the branch
  if (length_sq < one) {
    const double snap = LoadSingle(base, kSnapSingle);               // lfs f12,-10240(r11)
    // fcmpu cr6,f0,f12 ; blelr cr6
    if (!(length_sq > snap)) return;
    StoreSingle(base, out + kLengthSq, one);                         // stfs f13,8(r3)
    return;                                                          // blr
  }

  // loc_82B45414
  ctx.fpscr.disableFlushModeUnconditional();                         // fcmpu cr6,f0,f13
  // blelr cr6 -- exactly 1.0 and NaN return here, leaving the three stores above
  if (!(length_sq > one)) return;
  const double length = SqrtSingle(length_sq);                       // fsqrts f0,f0
  StoreSingle(base, out + kLengthSq, one);                           // stfs f13,8(r3)
  const double scale = DivSingle(one, length);                       // fdivs f13,f13,f0
  const double unit_x = MulSingle(scale, x);                         // fmuls f12,f13,f1
  StoreSingle(base, out + kX, unit_x);                               // stfs f12,0(r3)
  const double unit_y = MulSingle(scale, y);                         // fmuls f11,f13,f2
  StoreSingle(base, out + kY, unit_y);                               // stfs f11,4(r3)
  // blr -- f1/f2 untouched; f0/f11-f13 are scratch no caller reads (see the note)
}

// Writes: the 12-byte record at r3, every path (+0/+4 once or twice, +8 once or twice). The
// function loads nothing from r3, so the only reads are the two rodata singles.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r3.u32, kRecordBytes);
  spec.read(kOneSingle, 4);
  spec.read(kSnapSingle, 4);
  return true;
}

}  // namespace port_82B453D8
SKATE3_PORT(82B453D8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
