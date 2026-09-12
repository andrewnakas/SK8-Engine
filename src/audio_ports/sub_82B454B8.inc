// sub_82B454B8  role: pan a 2D source into the speaker-gain array: distance weights over the four selected speakers (plus speaker 1 and the two 7.1 sides), scaled by sqrt(1 - lensq^2); a source at lensq == 1.0 zeroes the array   lifted: skate3_recomp.68.cpp:29225 (409 lines)
// STATUS: verified
// calls/session: boot 219648  play 280497  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B454B8.md
namespace port_82B454B8 {

// The object r3 points at -- the same one sub_82B45B60 and sub_82B460A0 take. Nothing in
// docs/rw_audio_structs.h names it. What this function uses:
//   +8*k  f32[2]  speaker k's (x,y) position, k = 0..6  (so +8/+12 is speaker 1, +40/+44
//                 speaker 5, +48/+52 speaker 6; the table ends where the count begins)
//   +56   s32     count, compared with 4, 6 and 8 (inferred: output channel count)
//   +172  s32     four speaker indices, used as 8*index into the position table above and as
//   +176  s32     4*index into the gain array r6 -- the same four fields sub_82B45B60 reads
//   +180  s32
//   +184  s32
constexpr uint32_t kSpeaker1 = 8;    // 8*1, its gain is gains+4
constexpr uint32_t kSpeaker5 = 40;   // 8*5, its gain is gains+20
constexpr uint32_t kSpeaker6 = 48;   // 8*6, its gain is gains+24
constexpr uint32_t kCount = 56;
constexpr uint32_t kIndex172 = 172;
constexpr uint32_t kIndex176 = 176;
constexpr uint32_t kIndex180 = 180;
constexpr uint32_t kIndex184 = 184;

// The 12-byte source record r4 points at -- the one sub_82B453D8 writes: a 2D position clamped
// to the unit disc and its squared length, clamped to 1.0.
constexpr uint32_t kInputX = 0;
constexpr uint32_t kInputY = 4;
constexpr uint32_t kInputLenSq = 8;

// Four rodata singles, values read from the validated image dump (probe/harness/out/image):
//   0x8231A844  1.0f              (lis r11,-32206 ; lfs f0,-22460(r11))
//   0x82165A10  0.0f              (lis r5,-32234  ; lfs f8,23056(r5))
//   0x8209975C  0.5f              (lis r31,-32246 ; lfs f12,-26788(r31))
//   0x821A0318  0.0005f 3A03126F  (lis r11,-32230 ; lfs f6,792(r11))
// The body loads them through the guest map anyway, so the values need not be trusted here.
constexpr uint32_t kOneSingle = 0x8231A844;
constexpr uint32_t kZeroSingle = 0x82165A10;
constexpr uint32_t kHalfSingle = 0x8209975C;
constexpr uint32_t kEpsSingle = 0x821A0318;

// lfs / lfsx: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs / stfsx: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fmuls / fadds / fsubs / fdivs / fsqrts: the double operation rounded to single, as lifted.
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double SqrtSingle(double a) { return static_cast<double>(static_cast<float>(sqrt(a))); }

// fmadds a*b+c and fnmsubs -(a*b-c): one fused product, rounded once to double then to single.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}
inline double NmsubSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(-std::fma(a, b, -c)));
}

// fabs: the sign bit cleared on the double representation, as the lifted line does it.
inline double AbsDouble(double value) {
  PPCRegister temp{};
  temp.f64 = value;
  temp.u64 &= ~0x8000000000000000ull;
  return temp.f64;
}

// rlwinm rD,rS,3,0,28 and rlwinm rD,rS,2,0,29 on a zero-extended word: the 32-bit shifts. The
// rotate can only bring index bits 31..29 into the bits the mask clears, so it is just a shift.
inline uint32_t PairOffset(uint32_t index) { return index << 3; }
inline uint32_t WordOffset(uint32_t index) { return index << 2; }

// [a, a+alen) and [b, b+blen) overlap, modulo 2^32.
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return (b - a) < alen || (a - b) < blen;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t input = ctx.r4.u32;
  const uint32_t gains = ctx.r6.u32;
  const double focus = ctx.f1.f64;  // f1: the caller's own weight for speaker 1

  ctx.fpscr.disableFlushModeUnconditional();
  const double lensq = LoadSingle(base, input + kInputLenSq);  // lfs f13,8(r4)
  const double one = LoadSingle(base, kOneSingle);             // lis r11,-32206 ; lfs f0,-22460(r11)

  // fcmpu cr6,f13,f0 ; bne cr6,0x82b45518 -- a NaN lensq is not eq, so it takes the main path
  if (lensq == one) {
    REX_STORE_U32(gains + 0, 0u);   // li r11,0 ; stw r11,0(r6)
    REX_STORE_U32(gains + 4, 0u);   // stw r11,4(r6)
    REX_STORE_U32(gains + 8, 0u);   // stw r11,8(r6)
    REX_STORE_U32(gains + 12, 0u);  // stw r11,12(r6)
    REX_STORE_U32(gains + 16, 0u);  // stw r11,16(r6)
    // lwz r11,56(r3) -- reloaded after those five stores ; cmpwi cr6,r11,8 ; bne cr6,0x82b45774
    if (static_cast<int32_t>(REX_LOAD_U32(object + kCount)) != 8) return;  // blr
    const double zero = LoadSingle(base, kZeroSingle);  // lis r11,-32234 ; lfs f0,23056(r11)
    StoreSingle(base, gains + 20, zero);                // stfs f0,20(r6)
    StoreSingle(base, gains + 24, zero);                // stfs f0,24(r6)
    return;                                             // lfd f30,-24(r1) ; ld r31,-8(r1) ; blr
  }

  // loc_82B45518: the four selected speakers, as (x,y) pairs at object + 8*index
  const uint32_t index172 = REX_LOAD_U32(object + kIndex172);  // lwz r11,172(r3)
  ctx.fpscr.disableFlushModeUnconditional();
  const double x = LoadSingle(base, input + kInputX);          // lfs f13,0(r4)
  const uint32_t index176 = REX_LOAD_U32(object + kIndex176);  // lwz r10,176(r3)
  const double y = LoadSingle(base, input + kInputY);          // lfs f11,4(r4)
  const uint32_t index180 = REX_LOAD_U32(object + kIndex180);  // lwz r9,180(r3)
  const uint32_t index184 = REX_LOAD_U32(object + kIndex184);  // lwz r8,184(r3)
  const int32_t count = static_cast<int32_t>(REX_LOAD_U32(object + kCount));  // lwz r7,56(r3)
  const uint32_t pair172 = object + PairOffset(index172);  // rlwinm r11,r11,3,0,28 ; add r11,r11,r3
  const uint32_t pair176 = object + PairOffset(index176);  // rlwinm r10,r10,3,0,28 ; add r10,r10,r3
  const uint32_t pair180 = object + PairOffset(index180);  // rlwinm r9,r9,3,0,28 ; add r9,r9,r3
  const uint32_t pair184 = object + PairOffset(index184);  // rlwinm r8,r8,3,0,28 ; add r8,r8,r3

  const double sx172 = LoadSingle(base, pair172 + 0);  // lfs f12,0(r11)
  const double sx176 = LoadSingle(base, pair176 + 0);  // lfs f10,0(r10)
  const double dx172 = SubSingle(sx172, x);            // fsubs f9,f12,f13
  const double sx180 = LoadSingle(base, pair180 + 0);  // lfs f8,0(r9)
  const double dx176 = SubSingle(sx176, x);            // fsubs f7,f10,f13
  const double sx184 = LoadSingle(base, pair184 + 0);  // lfs f6,0(r8)
  const double dx180 = SubSingle(sx180, x);            // fsubs f5,f8,f13
  const double dx184 = SubSingle(sx184, x);            // fsubs f4,f6,f13
  const double sy172 = LoadSingle(base, pair172 + 4);  // lfs f3,4(r11)
  const double sy176 = LoadSingle(base, pair176 + 4);  // lfs f2,4(r10)
  const double dy172 = SubSingle(sy172, y);            // fsubs f10,f3,f11
  const double sy180 = LoadSingle(base, pair180 + 4);  // lfs f8,4(r9)
  const double dy176 = SubSingle(sy176, y);            // fsubs f6,f2,f11
  const double sy184 = LoadSingle(base, pair184 + 4);  // lfs f3,4(r8)
  const double dy180 = SubSingle(sy180, y);            // fsubs f2,f8,f11
  const double dy184 = SubSingle(sy184, y);            // fsubs f3,f3,f11
  const double half = LoadSingle(base, kHalfSingle);   // lis r31,-32246 ; lfs f12,-26788(r31)
  const double zero = LoadSingle(base, kZeroSingle);   // lis r5,-32234 ; lfs f8,23056(r5)

  // weight = 1 - 0.5 * |speaker - source|, per selected speaker
  double q172 = MulSingle(dx172, dx172);  // fmuls f9,f9,f9
  double q176 = MulSingle(dx176, dx176);  // fmuls f7,f7,f7
  double q180 = MulSingle(dx180, dx180);  // fmuls f31,f5,f5
  double q184 = MulSingle(dx184, dx184);  // fmuls f30,f4,f4
  double weight5 = zero;                  // fmr f5,f8
  double weight6 = zero;                  // fmr f4,f8
  q172 = FmaddSingle(dy172, dy172, q172);  // fmadds f10,f10,f10,f9
  q176 = FmaddSingle(dy176, dy176, q176);  // fmadds f9,f6,f6,f7
  q180 = FmaddSingle(dy180, dy180, q180);  // fmadds f7,f2,f2,f31
  q184 = FmaddSingle(dy184, dy184, q184);  // fmadds f6,f3,f3,f30
  const double r172 = SqrtSingle(q172);    // fsqrts f3,f10
  const double r176 = SqrtSingle(q176);    // fsqrts f2,f9
  const double r180 = SqrtSingle(q180);    // fsqrts f7,f7
  const double r184 = SqrtSingle(q184);    // fsqrts f6,f6
  const double weight172 = NmsubSingle(r172, half, one);  // fnmsubs f10,f3,f12,f0
  const double weight176 = NmsubSingle(r176, half, one);  // fnmsubs f9,f2,f12,f0
  const double weight180 = NmsubSingle(r180, half, one);  // fnmsubs f3,f7,f12,f0
  const double weight184 = NmsubSingle(r184, half, one);  // fnmsubs f2,f6,f12,f0

  // cmpwi cr6,r7,8 (hoisted above the square roots) ; bne cr6,0x82b45634
  if (count == 8) {
    const double sx5 = LoadSingle(base, object + kSpeaker5 + 0);  // lfs f7,40(r3)
    const double sx6 = LoadSingle(base, object + kSpeaker6 + 0);  // lfs f6,48(r3)
    const double dx5 = SubSingle(sx5, x);                         // fsubs f5,f7,f13
    const double dx6 = SubSingle(sx6, x);                         // fsubs f4,f6,f13
    const double sy5 = LoadSingle(base, object + kSpeaker5 + 4);  // lfs f7,44(r3)
    const double sy6 = LoadSingle(base, object + kSpeaker6 + 4);  // lfs f6,52(r3)
    const double dy5 = SubSingle(sy5, y);                         // fsubs f7,f7,f11
    const double dy6 = SubSingle(sy6, y);                         // fsubs f6,f6,f11
    double q5 = MulSingle(dx5, dx5);                              // fmuls f5,f5,f5
    double q6 = MulSingle(dx6, dx6);                              // fmuls f4,f4,f4
    q5 = FmaddSingle(dy5, dy5, q5);                               // fmadds f7,f7,f7,f5
    q6 = FmaddSingle(dy6, dy6, q6);                               // fmadds f6,f6,f6,f4
    const double r5 = SqrtSingle(q5);                             // fsqrts f5,f7
    const double r6 = SqrtSingle(q6);                             // fsqrts f4,f6
    weight5 = NmsubSingle(r5, half, one);                         // fnmsubs f5,f5,f12,f0
    weight6 = NmsubSingle(r6, half, one);                         // fnmsubs f4,f4,f12,f0
  }

  // loc_82B45634: the front/back split, each snapped to 0 inside 0.0005 of it
  ctx.fpscr.disableFlushModeUnconditional();
  double front = AddSingle(x, one);                 // fadds f7,f13,f0
  const double eps = LoadSingle(base, kEpsSingle);  // lis r11,-32230 ; lfs f6,792(r11)
  front = MulSingle(front, half);                   // fmuls f7,f7,f12
  // fabs f31,f7 ; fcmpu cr6,f31,f6 ; bge cr6,0x82b45654 -- unordered is not lt, so NaN keeps front
  if (AbsDouble(front) < eps) front = zero;         // fmr f7,f8

  // loc_82B45654
  ctx.fpscr.disableFlushModeUnconditional();
  double back = SubSingle(one, front);              // fsubs f31,f0,f7
  // fabs f30,f31 ; fcmpu cr6,f30,f6 ; bge cr6,0x82b45668
  if (AbsDouble(back) < eps) back = zero;           // fmr f31,f8

  // loc_82B45668: normalise the front pair, over two speakers at count == 4 and over three
  // (speaker 1 weighted by the f1 argument) otherwise
  double gain172;
  double gain176;
  double center = zero;  // f8 still holds the 0.0f constant on the count == 4 path
  // cmpwi cr6,r7,4 ; bne cr6,0x82b4568c
  if (count == 4) {
    ctx.fpscr.disableFlushModeUnconditional();
    const double sq = MulSingle(weight172, weight172);         // fmuls f13,f10,f10
    const double sum = FmaddSingle(weight176, weight176, sq);  // fmadds f12,f9,f9,f13
    const double ratio = DivSingle(front, sum);                // fdivs f11,f7,f12
    const double k = SqrtSingle(ratio);                        // fsqrts f7,f11
    gain172 = MulSingle(k, weight172);                         // fmuls f13,f7,f10
    gain176 = MulSingle(k, weight176);                         // fmuls f12,f7,f9
    // b 0x82b456d0
  } else {
    // loc_82B4568C
    ctx.fpscr.disableFlushModeUnconditional();
    const double sx1 = LoadSingle(base, object + kSpeaker1 + 0);  // lfs f8,8(r3)
    const double dx1 = SubSingle(sx1, x);                         // fsubs f6,f8,f13
    const double sy1 = LoadSingle(base, object + kSpeaker1 + 4);  // lfs f13,12(r3)
    const double dy1 = SubSingle(sy1, y);                         // fsubs f11,f13,f11
    double q1 = MulSingle(dx1, dx1);                              // fmuls f8,f6,f6
    q1 = FmaddSingle(dy1, dy1, q1);                               // fmadds f6,f11,f11,f8
    const double r1 = SqrtSingle(q1);                             // fsqrts f13,f6
    const double weight1 = NmsubSingle(r1, half, one);            // fnmsubs f12,f13,f12,f0
    const double scaled1 = MulSingle(weight1, focus);             // fmuls f11,f12,f1
    double sum = MulSingle(scaled1, scaled1);                     // fmuls f8,f11,f11
    sum = FmaddSingle(weight176, weight176, sum);                 // fmadds f6,f9,f9,f8
    sum = FmaddSingle(weight172, weight172, sum);                 // fmadds f1,f10,f10,f6 (clobbers f1)
    const double ratio = DivSingle(front, sum);                   // fdivs f13,f7,f1
    const double k = SqrtSingle(ratio);                           // fsqrts f8,f13
    gain172 = MulSingle(k, weight172);                            // fmuls f13,f8,f10
    gain176 = MulSingle(k, weight176);                            // fmuls f12,f8,f9
    center = MulSingle(k, scaled1);                               // fmuls f8,f8,f11
  }

  // loc_82B456D0: every gain is scaled by sqrt(1 - lensq^2), and the back pair normalised over
  // the four remaining weights. Each index is RELOADED from the object, after the previous store.
  ctx.fpscr.disableFlushModeUnconditional();
  double spread = MulSingle(weight5, weight5);                  // fmuls f11,f5,f5
  const double lensq2 = LoadSingle(base, input + kInputLenSq);  // lfs f10,8(r4)
  const double lift = NmsubSingle(lensq2, lensq2, one);         // fnmsubs f9,f10,f10,f0
  const uint32_t out172 =
      gains + WordOffset(REX_LOAD_U32(object + kIndex172));  // lwz r11,172(r3) ; rlwinm r10,r11,2,0,29
  spread = FmaddSingle(weight6, weight6, spread);             // fmadds f7,f4,f4,f11
  const double depth = SqrtSingle(lift);                      // fsqrts f0,f9
  spread = FmaddSingle(weight184, weight184, spread);          // fmadds f6,f2,f2,f7
  const double out_a = MulSingle(depth, gain172);             // fmuls f1,f0,f13
  ctx.f1.f64 = out_a;                                         // (f1 is left as the original leaves it)
  StoreSingle(base, out172, out_a);                           // stfsx f1,r10,r6
  const uint32_t out176 =
      gains + WordOffset(REX_LOAD_U32(object + kIndex176));  // lwz r9,176(r3) ; rlwinm r8,r9,2,0,29
  spread = FmaddSingle(weight180, weight180, spread);         // fmadds f13,f3,f3,f6
  const double out_b = MulSingle(depth, gain176);             // fmuls f12,f0,f12
  StoreSingle(base, out176, out_b);                           // stfsx f12,r8,r6
  const uint32_t out180 =
      gains + WordOffset(REX_LOAD_U32(object + kIndex180));  // lwz r7,180(r3) ; rlwinm r5,r7,2,0,29
  const double ratio = DivSingle(back, spread);               // fdivs f11,f31,f13
  const double k2 = SqrtSingle(ratio);                        // fsqrts f13,f11
  const double rear180 = MulSingle(k2, weight180);            // fmuls f10,f13,f3
  const double rear184 = MulSingle(k2, weight184);            // fmuls f9,f13,f2
  const double out_c = MulSingle(rear180, depth);             // fmuls f7,f10,f0
  StoreSingle(base, out180, out_c);                           // stfsx f7,r5,r6
  const uint32_t out184 =
      gains + WordOffset(REX_LOAD_U32(object + kIndex184));  // lwz r4,184(r3) ; rlwinm r11,r4,2,0,29
  const double out_d = MulSingle(rear184, depth);             // fmuls f6,f9,f0
  StoreSingle(base, out184, out_d);                           // stfsx f6,r11,r6

  // lwz r10,56(r3) -- reloaded after the four stores ; cmpwi cr6,r10,6 ; blt cr6,0x82b45774
  if (static_cast<int32_t>(REX_LOAD_U32(object + kCount)) < 6) return;  // blr
  const double out_center = MulSingle(depth, center);  // fmuls f12,f0,f8
  StoreSingle(base, gains + 4, out_center);            // stfs f12,4(r6)
  // lwz r11,56(r3) -- reloaded again ; cmpwi cr6,r11,8 ; bne cr6,0x82b45774
  if (static_cast<int32_t>(REX_LOAD_U32(object + kCount)) != 8) return;  // blr
  const double side5 = MulSingle(k2, weight5);  // fmuls f12,f13,f5
  const double side6 = MulSingle(k2, weight6);  // fmuls f11,f13,f4
  const double out_e = MulSingle(side5, depth);  // fmuls f10,f12,f0
  StoreSingle(base, gains + 20, out_e);          // stfs f10,20(r6)
  const double out_f = MulSingle(side6, depth);  // fmuls f9,f11,f0
  StoreSingle(base, gains + 24, out_f);          // stfs f9,24(r6)
  // loc_82B45774 restores f30/f31/r31 from this frame's red zone, which is not reproduced; the
  // disableFlushModeUnconditional() there precedes only those restores, so it is not either.
}                                                  // blr

// Writes, all inside the gain array r6:
//   lensq == 1.0:  gains+0..+19, plus +20..+27 when the count reads 8
//   otherwise:     gains + 4*index for the four indices, plus gains+4 when count >= 6 and
//                  gains+20/+24 when count == 8
// Both the four indices and the count are RELOADED between the stores, so the entry values only
// predict the later addresses if no store can land on those fields:
//   - a store overlapping object+172..+187 makes a later store address unknowable: return false
//   - a store overlapping object+56..+59 only adds or removes the three fixed conditional words,
//     so those are declared unconditionally instead (a superset compares equal)
// Reads: the source record, the count, the four indices and the speaker pairs they select, the
// two extra pairs and speaker 1's pair on the paths that touch them, and the four rodata singles
// so the vector replays without the image.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t input = ctx.r4.u32;
  const uint32_t gains = ctx.r6.u32;
  const int32_t count = static_cast<int32_t>(REX_LOAD_U32(object + kCount));
  const double one = LoadSingle(base, kOneSingle);
  spec.read(object + kCount, 4);
  spec.read(input, 12);
  spec.read(kOneSingle, 4);

  // The path is predictable from entry state: the zeroing path is taken iff r4+8 reads 1.0f.
  if (LoadSingle(base, input + kInputLenSq) == one) {
    const bool count_unstable = Overlaps(gains, 20, object + kCount, 4);
    spec.write(gains, (count == 8 || count_unstable) ? 28 : 20);
    spec.read(kZeroSingle, 4);
    return true;
  }

  const uint32_t index[4] = {
      REX_LOAD_U32(object + kIndex172), REX_LOAD_U32(object + kIndex176),
      REX_LOAD_U32(object + kIndex180), REX_LOAD_U32(object + kIndex184)};
  bool count_unstable = Overlaps(gains + 4, 4, object + kCount, 4);
  for (uint32_t i = 0; i < 4; i++) {
    const uint32_t addr = gains + WordOffset(index[i]);
    if (Overlaps(addr, 4, object + kIndex172, 16)) return false;
    if (Overlaps(addr, 4, object + kCount, 4)) count_unstable = true;
    spec.write(addr, 4);
    spec.read(object + PairOffset(index[i]), 8);
  }
  if (count >= 6 || count_unstable) spec.write(gains + 4, 4);
  if (count == 8 || count_unstable) {
    spec.write(gains + 20, 4);
    spec.write(gains + 24, 4);
  }
  spec.read(object + kIndex172, 16);
  if (count == 8) spec.read(object + kSpeaker5, 16);  // speakers 5 and 6, +40..+55
  if (count != 4) spec.read(object + kSpeaker1, 8);   // speaker 1, +8..+15
  spec.read(kZeroSingle, 4);
  spec.read(kHalfSingle, 4);
  spec.read(kEpsSingle, 4);
  return true;
}

}  // namespace port_82B454B8
SKATE3_PORT(82B454B8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
