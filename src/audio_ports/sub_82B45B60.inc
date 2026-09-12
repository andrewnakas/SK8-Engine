// sub_82B45B60  role: scale a speaker-gain array (r6) by f1*f2, power-normalised over the selected speakers when f3 < 1.0   lifted: skate3_recomp.68.cpp:30244 (151 lines)
// STATUS: verified
// calls/session: boot 219648  play 280497  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B45B60.md
namespace port_82B45B60 {

// The object r3 points at (nothing in docs/rw_audio_structs.h names it):
//   +56  s32 count      compared with 6 and 8 (inferred: output channel count, 5.1 / 7.1)
//   +172 s32 index      four gain-array indices, loaded in the order 176, 172, 180, 184
//   +176 s32 index
//   +180 s32 index
//   +184 s32 index
// r6 points at an f32 gain array: +0..+16 always scaled, +20/+24 only when count == 8.
constexpr uint32_t kCount = 56;
constexpr uint32_t kIndex172 = 172;
constexpr uint32_t kIndex176 = 176;
constexpr uint32_t kIndex180 = 180;
constexpr uint32_t kIndex184 = 184;

// 1.0f in the image dump: lis r11,-32206 ; lfs f13,-22460(r11). Loaded through the guest map.
constexpr uint32_t kOneSingle = 0x8231A844;

// fmuls / fadds / fdivs / fmadds / fsqrts: the double operation rounded to single, as lifted.
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}
inline double SqrtSingle(double a) { return static_cast<double>(static_cast<float>(std::sqrt(a))); }

// lfs / lfsx: the 32-bit word reinterpreted as a single, widened to double.
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

// rlwinm rD,rS,2,0,29 on a zero-extended word: the 32-bit shift left by 2.
inline uint32_t WordOffset(uint32_t index) { return index << 2; }

// [a, a+alen) and [b, b+blen) overlap, modulo 2^32.
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return (b - a) < alen || (a - b) < blen;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t gains = ctx.r6.u32;
  ctx.fpscr.disableFlushModeUnconditional();
  double scale = MulSingle(ctx.f1.f64, ctx.f2.f64);               // fmuls f0,f1,f2
  const double one = LoadSingle(base, kOneSingle);                // lis r11,-32206 ; lfs f13,-22460(r11)
  // fcmpu cr6,f3,f13 ; bge cr6,0x82b45bec   (unordered: not lt, so it skips the normalisation)
  if (ctx.f3.f64 < one) {
    const uint32_t index176 = REX_LOAD_U32(object + kIndex176);   // lwz r11,176(r3)
    const uint32_t index172 = REX_LOAD_U32(object + kIndex172);   // lwz r10,172(r3)
    const uint32_t index180 = REX_LOAD_U32(object + kIndex180);   // lwz r8,180(r3)
    const uint32_t index184 = REX_LOAD_U32(object + kIndex184);   // lwz r5,184(r3)
    const int32_t count = static_cast<int32_t>(REX_LOAD_U32(object + kCount));  // lwz r11,56(r3) ; cmpwi cr6,r11,6
    const double a = LoadSingle(base, WordOffset(index176) + gains);  // rlwinm r9 ; lfsx f13,r9,r6
    double sum = MulSingle(a, a);                                      // fmuls f12,f13,f13
    const double b = LoadSingle(base, WordOffset(index172) + gains);  // rlwinm r7 ; lfsx f11,r7,r6
    const double c = LoadSingle(base, WordOffset(index180) + gains);  // rlwinm r4 ; lfsx f10,r4,r6
    const double d = LoadSingle(base, WordOffset(index184) + gains);  // rlwinm r10 ; lfsx f9,r10,r6
    sum = FmaddSingle(b, b, sum);                                      // fmadds f8,f11,f11,f12
    sum = FmaddSingle(c, c, sum);                                      // fmadds f7,f10,f10,f8
    sum = FmaddSingle(d, d, sum);                                      // fmadds f13,f9,f9,f7
    // blt cr6,0x82b45be4
    if (!(count < 6)) {
      const double e = LoadSingle(base, gains + 4);                   // lfs f12,4(r6)
      sum = FmaddSingle(e, e, sum);                                    // fmadds f13,f12,f12,f13
      // cmpwi cr6,r11,8 ; bne cr6,0x82b45be4
      if (count == 8) {
        const double g = LoadSingle(base, gains + 24);                // lfs f12,24(r6)
        const double g_squared = MulSingle(g, g);                     // fmuls f11,f12,f12
        const double h = LoadSingle(base, gains + 20);                // lfs f10,20(r6)
        const double rear = FmaddSingle(h, h, g_squared);             // fmadds f9,f10,f10,f11
        sum = AddSingle(rear, sum);                                    // fadds f13,f9,f13
      }
    }
    // loc_82B45BE4
    ctx.fpscr.disableFlushModeUnconditional();
    const double norm = SqrtSingle(sum);                              // fsqrts f13,f13
    scale = DivSingle(scale, norm);                                   // fdivs f0,f0,f13
  }

  // loc_82B45BEC: all five loads precede all five stores, as lifted
  ctx.fpscr.disableFlushModeUnconditional();
  const double g0 = LoadSingle(base, gains + 0);                      // lfs f13,0(r6)
  const double g1 = LoadSingle(base, gains + 4);                      // lfs f12,4(r6)
  const double s0 = MulSingle(g0, scale);                             // fmuls f11,f13,f0
  const double g2 = LoadSingle(base, gains + 8);                      // lfs f10,8(r6)
  const double s1 = MulSingle(g1, scale);                             // fmuls f9,f12,f0
  const double g3 = LoadSingle(base, gains + 12);                     // lfs f8,12(r6)
  const double s2 = MulSingle(scale, g2);                             // fmuls f7,f0,f10
  const double g4 = LoadSingle(base, gains + 16);                     // lfs f6,16(r6)
  const double s3 = MulSingle(g3, scale);                             // fmuls f5,f8,f0
  const double s4 = MulSingle(scale, g4);                             // fmuls f4,f0,f6
  StoreSingle(base, gains + 0, s0);                                   // stfs f11,0(r6)
  StoreSingle(base, gains + 4, s1);                                   // stfs f9,4(r6)
  StoreSingle(base, gains + 8, s2);                                   // stfs f7,8(r6)
  StoreSingle(base, gains + 12, s3);                                  // stfs f5,12(r6)
  StoreSingle(base, gains + 16, s4);                                  // stfs f4,16(r6)
  // lwz r11,56(r3) (reloaded after the stores) ; cmpwi cr6,r11,8 ; bnelr cr6
  if (static_cast<int32_t>(REX_LOAD_U32(object + kCount)) != 8) return;
  const double g5 = LoadSingle(base, gains + 20);                     // lfs f13,20(r6)
  const double g6 = LoadSingle(base, gains + 24);                     // lfs f12,24(r6)
  const double s5 = MulSingle(scale, g5);                             // fmuls f11,f0,f13
  const double s6 = MulSingle(g6, scale);                             // fmuls f10,f12,f0
  StoreSingle(base, gains + 20, s5);                                  // stfs f11,20(r6)
  StoreSingle(base, gains + 24, s6);                                  // stfs f10,24(r6)
}                                                                     // blr

// Writes: gains+0..+19 always; gains+20..+27 when the +56 count reads 8. That count is reloaded
// AFTER the first five stores, so if those stores could land on object+56 the entry value does not
// predict the reload: then the two extra words are declared anyway (a superset compares equal).
// Reads: the count, the 1.0f constant, and -- only on the path that takes them, since the object
// and indices need not be valid otherwise -- the four indices and the four gains they select.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t gains = ctx.r6.u32;
  const int32_t count = static_cast<int32_t>(REX_LOAD_U32(object + kCount));
  const bool count_overwritten = Overlaps(gains, 20, object + kCount, 4);
  spec.write(gains, (count == 8 || count_overwritten) ? 28 : 20);
  spec.read(object + kCount, 4);
  spec.read(kOneSingle, 4);
  if (ctx.f3.f64 < LoadSingle(base, kOneSingle)) {
    spec.read(object + kIndex172, 16);
    spec.read(WordOffset(REX_LOAD_U32(object + kIndex176)) + gains, 4);
    spec.read(WordOffset(REX_LOAD_U32(object + kIndex172)) + gains, 4);
    spec.read(WordOffset(REX_LOAD_U32(object + kIndex180)) + gains, 4);
    spec.read(WordOffset(REX_LOAD_U32(object + kIndex184)) + gains, 4);
  }
  return true;
}

}  // namespace port_82B45B60
SKATE3_PORT(82B45B60, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
