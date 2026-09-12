// sub_82B45788  role: add one source's angular contribution to the speaker-gain array -- reduce the source's angle, take its sine and cosine, pick one of seven sectors by comparing the reduced angle with three bounds and two reflections of them, project (sin, cos) through that sector's four coefficients, and mix-add the two normalised projections into the two gain slots the sector selects   lifted: skate3_recomp.68.cpp:29635 (608 lines)
// STATUS: verified
// calls/session: boot 219648  play 280497  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B45788.md
namespace port_82B45788 {

// r3 = the listener/format config object sub_82B454B8, sub_82B45B60 and sub_82B460A0 all take in
// r3. Nothing in docs/rw_audio_structs.h names it. What this function uses:
constexpr uint32_t kCount = 56;     // s32 channel count, compared with 6 and 8 (sub_82B454B8's +56)
constexpr uint32_t kBound0 = 60;    // f32 the first sector edge; also subtracted from the angle
constexpr uint32_t kBound1 = 64;    // f32
constexpr uint32_t kBound2 = 68;    // f32
constexpr uint32_t kCentre = 72;    // f32 scales the centre channel's share in sector 1
constexpr uint32_t kSector1 = 76;   // f32[4] per sector: {sin0, cos0, sin1, cos1}
constexpr uint32_t kSector2 = 92;
constexpr uint32_t kSector3 = 108;
constexpr uint32_t kSector45 = 124;  // sectors 4 and 5 share one coefficient block
constexpr uint32_t kSector6 = 140;
constexpr uint32_t kSector7 = 156;
constexpr uint32_t kIndex172 = 172;  // s32[4] the gain-array indices, used as 4*index
constexpr uint32_t kIndexWords = 16;

// r4 = the 12/16-byte source record sub_82B453D8 writes and sub_82B45C50 lays out: +8 its squared
// length (the value every sector normalises by), +12 the angle this function reduces.
constexpr uint32_t kRecordLength = 8;
constexpr uint32_t kRecordAngle = 12;
constexpr uint32_t kRecordBytes = 16;

// r6 = the f32 gain array. Fixed slots +4 (centre), +12, +16, +20, +24, plus 4*index.
constexpr uint32_t kGainFixedEnd = 28;  // the highest fixed slot is +24, so [gains, +28)

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)

// Two rodata singles, each computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye:
//   0x822F8904  lis -32208 ; addi -31232 ; lfs 772   -- scales the angle before reduction. The same
//               constant pool sub_82B2F2C8 reads at +1120 and sub_82B298E0 at +480.
//   0x820B411C  lis -32245 ; lfs 16668               -- the step the reduced fraction is multiplied
//               by, and (as f29) the value the two reflected bounds are subtracted from.
// probe/harness/out/image is empty on this tree, so neither value was read; the body loads both
// through the guest map, so neither needs to be known here.
constexpr uint32_t kAngleScale = 0x822F8904;
constexpr uint32_t kAngleStep = 0x820B411C;

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

// fadds / fsubs / fmuls / fdivs / fsqrts / frsp: the double operation rounded to single, as lifted.
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double SqrtSingle(double a) { return static_cast<double>(static_cast<float>(sqrt(a))); }
inline double Frsp(double a) { return static_cast<double>(static_cast<float>(a)); }

// fmadds a*b+c and fmsubs a*b-c: one fused product, rounded once to double then to single.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}
inline double FmsubSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, -c)));
}

// rlwinm rD,rS,2,0,29 on a zero-extended word: the 32-bit shift left by two. The rotate can only
// bring index bits 31..30 into the bits the mask clears, so it is just a shift.
inline uint32_t WordOffset(uint32_t index) { return index << 2; }

// [a, a+alen) and [b, b+blen) overlap, modulo 2^32.
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return (b - a) < alen || (a - b) < blen;
}

// The two projections every sector takes: coefficient +0 against sin and +4 against cos for the
// first, +8 and +12 for the second. The four loads and the two fmuls are in the lifted order; the
// two fmadds follow every load in all six blocks, so grouping them moves nothing across a store.
struct Pair {
  double first;
  double second;
};
inline Pair Project(uint8_t* base, uint32_t coeff, double sine, double cosine) {
  const double c0 = LoadSingle(base, coeff + 0);   // lfs f0,92(r31) etc.
  const double p0 = MulSingle(c0, sine);           // fmuls f12,f0,f30
  const double c1 = LoadSingle(base, coeff + 4);   // lfs f11,96(r31)
  const double c2 = LoadSingle(base, coeff + 8);   // lfs f10,100(r31)
  const double p1 = MulSingle(c2, sine);           // fmuls f9,f10,f30
  const double c3 = LoadSingle(base, coeff + 12);  // lfs f8,104(r31)
  return {FmaddSingle(c1, cosine, p0),             // fmadds f5,f11,f13,f12
          FmaddSingle(c3, cosine, p1)};            // fmadds f4,f8,f13,f9
}

// length / sqrt(first^2 + second^2): fmuls, fmadds, fsqrts, fdivs, in that order.
inline double Normalise(Pair pair, double length) {
  const double sq = MulSingle(pair.first, pair.first);                 // fmuls f3,f5,f5
  const double sum = FmaddSingle(pair.second, pair.second, sq);        // fmadds f2,f4,f4,f3
  return DivSingle(length, SqrtSingle(sum));                           // fsqrts f1,f2 ; fdivs f0,f7,f1
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r31,r3
  const uint32_t record = ctx.r4.u32;  // mr r29,r4
  const uint32_t gains = ctx.r6.u32;   // mr r30,r6

  // __savegprlr_29 and __savefpr_27 spill r29-r31 and f27-f31; the tail restores them and the
  // harness compares both ranges, so the entry values have to survive.
  const PPCRegister saved_gpr[3] = {ctx.r29, ctx.r30, ctx.r31};
  const PPCRegister saved_fpr[5] = {ctx.f27, ctx.f28, ctx.f29, ctx.f30, ctx.f31};

  // stwu r1,-160(r1). Reproduced -- most ports skip their own frame -- because the sine and cosine
  // helpers sub_82F4DED0/sub_82F4DFB0 spill f30/f31/f10 at r1-8 .. r1-32, below the caller's r1.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  ctx.fpscr.disableFlushModeUnconditional();
  const double angle = LoadSingle(base, record + kRecordAngle);  // lfs f0,12(r4)
  const double bound0 = LoadSingle(base, object + kBound0);      // lfs f13,60(r3)
  const double weight = ctx.f1.f64;                              // fmr f28,f1
  const double biased = AddSingle(angle, bound0);                // fadds f12,f0,f13
  const double scale = LoadSingle(base, kAngleScale);            // lfs f0,772(r10)
  const double scaled = MulSingle(biased, scale);                // fmuls f31,f12,f0

  // bl 0x82f4de80 -- takes f1, returns f1; the caller keeps only frsp(result) and subtracts it from
  // its own input, which is the shape of a reduction (the rounded part discarded).
  ctx.f1.f64 = scaled;                                           // fmr f1,f31
  skate3::audio::GuestCall(ctx, base, sub_82F4DE80);
  ctx.fpscr.disableFlushModeUnconditional();
  const double rounded = Frsp(ctx.f1.f64);                       // frsp f11,f1
  const double bound0_again = LoadSingle(base, object + kBound0);  // lfs f10,60(r31)
  const double step = LoadSingle(base, kAngleStep);               // lfs f29,16668(r9)
  const double fraction = SubSingle(scaled, rounded);             // fsubs f9,f31,f11
  const double reduced = FmsubSingle(fraction, step, bound0_again);  // fmsubs f31,f9,f29,f10
  // fmr f27,f31 -- f27 is written here and never read again; __restfpr_27 puts the entry value back,
  // so it is not reproduced.

  // bl 0x82f4ded0 then bl 0x82f4dfb0 -- the sine and cosine helpers (named in the note for
  // sub_82B29BE0, whose closure reaches the same two), both on the reduced angle.
  ctx.f1.f64 = reduced;
  skate3::audio::GuestCall(ctx, base, sub_82F4DED0);
  ctx.fpscr.disableFlushModeUnconditional();
  const double sine = Frsp(ctx.f1.f64);  // frsp f30,f1
  ctx.f1.f64 = reduced;                  // fmr f1,f31
  skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);
  ctx.fpscr.disableFlushModeUnconditional();
  const double edge0 = LoadSingle(base, object + kBound0);  // lfs f8,60(r31)
  const double cosine = Frsp(ctx.f1.f64);                   // frsp f13,f1

  // fcmpu cr6,f31,f8 ; bge cr6,0x82b458d4 -- an unordered compare is not lt, so a NaN angle leaves
  // sector 1 and walks the ladder below.
  if (reduced < edge0) {
    // Sector 1: the front pair, plus the centre channel when the count is 6 or more.
    const double c0 = LoadSingle(base, object + kSector1 + 0);   // lfs f0,76(r31)
    const double c2 = LoadSingle(base, object + kSector1 + 8);   // lfs f12,84(r31)
    const double p0 = MulSingle(c0, sine);                       // fmuls f11,f0,f30
    const double p1 = MulSingle(c2, sine);                       // fmuls f10,f12,f30
    const double c1 = LoadSingle(base, object + kSector1 + 4);   // lfs f9,80(r31)
    const int32_t count = static_cast<int32_t>(REX_LOAD_U32(object + kCount));  // lwz r10,56(r31)
    const double c3 = LoadSingle(base, object + kSector1 + 12);  // lfs f8,88(r31)
    double left = FmaddSingle(c1, cosine, p0);                   // fmadds f0,f9,f13,f11
    double right = FmaddSingle(c3, cosine, p1);                  // fmadds f13,f8,f13,f10
    double centre = 0.0;                                         // lfs f12,23056(r11) -- 0.0f
    // cmpwi cr6,r10,6 ; blt cr6,0x82b4586c
    if (count >= 6) {
      // fcmpu cr6,f0,f13 ; bge cr6,0x82b45854 -- a NaN takes the second arm, as `!(a < b)` does
      double share = (left < right) ? left : right;   // fmr f12,f0 / fmr f12,f13
      share = MulSingle(share, weight);               // fmuls f12,f12,f28
      const double spread = LoadSingle(base, object + kCentre);  // lfs f11,72(r31)
      left = SubSingle(left, share);                  // fsubs f0,f0,f12
      right = SubSingle(right, share);                // fsubs f13,f13,f12
      centre = MulSingle(spread, share);              // fmuls f12,f11,f12
    }

    // loc_82B4586C: the three-term normalisation -- centre is exactly 0.0f below a count of 6.
    ctx.fpscr.disableFlushModeUnconditional();
    const double right_sq = MulSingle(right, right);                    // fmuls f11,f13,f13
    const double length = LoadSingle(base, record + kRecordLength);     // lfs f10,8(r29)
    const uint32_t slot176 =
        gains + WordOffset(REX_LOAD_U32(object + kIndex172 + 4));  // lwz r11,176(r31) ; rlwinm
    const double old176 = LoadSingle(base, slot176);                    // lfsx f9,r11,r30
    const double with_centre = FmaddSingle(centre, centre, right_sq);   // fmadds f8,f12,f12,f11
    const double sum = FmaddSingle(left, left, with_centre);            // fmadds f7,f0,f0,f8
    const double norm = DivSingle(length, SqrtSingle(sum));             // fsqrts f6,f7 ; fdivs f11
    StoreSingle(base, slot176, FmaddSingle(norm, left, old176));  // fmadds f5 ; stfsx f5,r11,r30
    // lwz r10,172(r31) -- read AFTER the store above
    const uint32_t slot172 = gains + WordOffset(REX_LOAD_U32(object + kIndex172));
    const double old172 = LoadSingle(base, slot172);                    // lfsx f4,r11,r30
    StoreSingle(base, slot172, FmaddSingle(norm, right, old172));  // fmadds f3 ; stfsx f3,r11,r30
    // lwz r9,56(r31) -- reloaded again ; cmpwi cr6,r9,6 ; blt cr6,0x82b45b4c
    if (static_cast<int32_t>(REX_LOAD_U32(object + kCount)) >= 6) {
      const double old4 = LoadSingle(base, gains + 4);                  // lfs f0,4(r30)
      StoreSingle(base, gains + 4, FmaddSingle(norm, centre, old4));  // fmadds f13 ; stfs f13,4(r30)
    }
  } else {
    // loc_82B458D4
    ctx.fpscr.disableFlushModeUnconditional();
    const double edge1 = LoadSingle(base, object + kBound1);  // lfs f12,64(r31)
    // fcmpu cr6,f31,f12 ; bge cr6,0x82b45930
    if (reduced < edge1) {
      // Sector 2: the index-pair form -- the second index is read AFTER the first store, which is
      // why loc_82B45B3C is inlined at each of its three arrival points rather than shared.
      const uint32_t slot_a = gains + WordOffset(REX_LOAD_U32(object + kIndex172));  // lwz r11,172
      const Pair pair = Project(base, object + kSector2, sine, cosine);
      const double length = LoadSingle(base, record + kRecordLength);  // lfs f7,8(r29)
      const double old_a = LoadSingle(base, slot_a);                   // lfsx f6,r11,r30
      const double norm = Normalise(pair, length);
      StoreSingle(base, slot_a, FmaddSingle(pair.first, norm, old_a));  // fmadds f13 ; stfsx
      // lwz r10,180(r31) ; b 0x82b45b3c
      const uint32_t slot_b = gains + WordOffset(REX_LOAD_U32(object + kIndex172 + 8));
      const double old_b = LoadSingle(base, slot_b);                    // lfsx f12,r11,r30
      StoreSingle(base, slot_b, FmaddSingle(pair.second, norm, old_b));  // fmadds f11 ; stfsx
    } else {
      // loc_82B45930
      ctx.fpscr.disableFlushModeUnconditional();
      const double edge2 = LoadSingle(base, object + kBound2);  // lfs f0,68(r31)
      // fcmpu cr6,f31,f0 ; bge cr6 ; then lwz r11,56(r31) ; cmpwi 8 ; bne cr6 -- both must hold
      if (reduced < edge2 &&
          static_cast<int32_t>(REX_LOAD_U32(object + kCount)) == 8) {
        // Sector 3: fixed slots +12 and +20, both old values loaded before either store.
        const Pair pair = Project(base, object + kSector3, sine, cosine);
        const double length = LoadSingle(base, record + kRecordLength);  // lfs f7,8(r29)
        const double old12 = LoadSingle(base, gains + 12);               // lfs f6,12(r30)
        const double old20 = LoadSingle(base, gains + 20);               // lfs f5,20(r30)
        const double norm = Normalise(pair, length);
        StoreSingle(base, gains + 12, FmaddSingle(pair.first, norm, old12));   // stfs f12,12(r30)
        StoreSingle(base, gains + 20, FmaddSingle(pair.second, norm, old20));  // stfs f11,20(r30)
      } else {
        // loc_82B459A4: the first reflected bound, f29 - the +68 single just loaded.
        ctx.fpscr.disableFlushModeUnconditional();
        const double reflect2 = SubSingle(step, edge2);  // fsubs f0,f29,f0
        // fcmpu cr6,f31,f0 ; bge cr6 ; lwz r11,56 ; cmpwi 8 ; bne cr6
        if (reduced < reflect2 &&
            static_cast<int32_t>(REX_LOAD_U32(object + kCount)) == 8) {
          // Sector 4: fixed slots +20 and +24, the same coefficients sector 5 uses.
          const Pair pair = Project(base, object + kSector45, sine, cosine);
          const double length = LoadSingle(base, record + kRecordLength);  // lfs f7,8(r29)
          const double old20 = LoadSingle(base, gains + 20);               // lfs f6,20(r30)
          const double old24 = LoadSingle(base, gains + 24);               // lfs f5,24(r30)
          const double norm = Normalise(pair, length);
          StoreSingle(base, gains + 20, FmaddSingle(pair.first, norm, old20));   // stfs f12,20(r30)
          StoreSingle(base, gains + 24, FmaddSingle(pair.second, norm, old24));  // stfs f11,24(r30)
        } else {
          // loc_82B45A18: the second reflected bound, f29 - the +64 single.
          ctx.fpscr.disableFlushModeUnconditional();
          const double reflect1 = SubSingle(step, edge1);  // fsubs f0,f29,f12
          const bool inside = reduced < reflect1;         // fcmpu cr6,f31,f0
          const int32_t count = static_cast<int32_t>(REX_LOAD_U32(object + kCount));  // lwz r11,56
          // bge cr6,0x82b45a80 ; cmpwi cr6,r11,6 ; bgt cr6,0x82b45a80
          if (inside && count <= 6) {
            // Sector 5: the index pair (+180, +184), sector 4's coefficients.
            const uint32_t slot_a =
                gains + WordOffset(REX_LOAD_U32(object + kIndex172 + 8));  // lwz r11,180(r31)
            const Pair pair = Project(base, object + kSector45, sine, cosine);
            const double length = LoadSingle(base, record + kRecordLength);  // lfs f7,8(r29)
            const double old_a = LoadSingle(base, slot_a);                   // lfsx f6,r11,r30
            const double norm = Normalise(pair, length);
            StoreSingle(base, slot_a, FmaddSingle(pair.first, norm, old_a));  // stfsx f13
            // lwz r10,184(r31) ; b 0x82b45b3c
            const uint32_t slot_b = gains + WordOffset(REX_LOAD_U32(object + kIndex172 + 12));
            const double old_b = LoadSingle(base, slot_b);                     // lfsx f12,r11,r30
            StoreSingle(base, slot_b, FmaddSingle(pair.second, norm, old_b));  // stfsx f11
          } else if (inside && count == 8) {
            // loc_82B45A80. Reached only by the count > 6 branch above: arriving here from the
            // `bge` re-tests the SAME f0, so that arm always falls through to sector 7.
            // Sector 6: fixed slots +24 and +16, in that store order.
            const Pair pair = Project(base, object + kSector6, sine, cosine);
            const double length = LoadSingle(base, record + kRecordLength);  // lfs f7,8(r29)
            const double old16 = LoadSingle(base, gains + 16);               // lfs f6,16(r30)
            const double old24 = LoadSingle(base, gains + 24);               // lfs f5,24(r30)
            const double norm = Normalise(pair, length);
            StoreSingle(base, gains + 24, FmaddSingle(pair.first, norm, old24));   // stfs f12,24
            StoreSingle(base, gains + 16, FmaddSingle(pair.second, norm, old16));  // stfs f11,16
          } else {
            // loc_82B45AF0 -- sector 7: the index pair (+184, +176).
            const uint32_t slot_a =
                gains + WordOffset(REX_LOAD_U32(object + kIndex172 + 12));  // lwz r11,184(r31)
            const Pair pair = Project(base, object + kSector7, sine, cosine);
            const double length = LoadSingle(base, record + kRecordLength);  // lfs f7,8(r29)
            const double old_a = LoadSingle(base, slot_a);                   // lfsx f6,r11,r30
            const double norm = Normalise(pair, length);
            StoreSingle(base, slot_a, FmaddSingle(pair.first, norm, old_a));  // stfsx f13
            // lwz r10,176(r31) ; loc_82B45B3C
            const uint32_t slot_b = gains + WordOffset(REX_LOAD_U32(object + kIndex172 + 4));
            const double old_b = LoadSingle(base, slot_b);                     // lfsx f12,r11,r30
            StoreSingle(base, slot_b, FmaddSingle(pair.second, norm, old_b));  // stfsx f11
          }
        }
      }
    }
  }

  // loc_82B45B4C: addi r1,r1,160 ; __restfpr_27 ; __restgprlr_29
  ctx.r1.u32 = entry_sp;
  ctx.r29 = saved_gpr[0];
  ctx.r30 = saved_gpr[1];
  ctx.r31 = saved_gpr[2];
  ctx.f27 = saved_fpr[0];
  ctx.f28 = saved_fpr[1];
  ctx.f29 = saved_fpr[2];
  ctx.f30 = saved_fpr[3];
  ctx.f31 = saved_fpr[4];
}

// Every store is inside the gain array r6: two of the fixed slots +4/+12/+16/+20/+24, or two of the
// four `gains + 4*index` slots. Which sector runs cannot be predicted from entry state without
// redoing the reduction and both trigonometric calls, so the union of all seven is declared: one
// 28-byte span over the fixed slots, plus a word per index that falls outside it. A byte no store
// reaches compares equal anyway.
//
// Not declared: this function's own 160-byte frame and the red-zone spills of the three helpers it
// calls -- each is its own function's frame.
//
// Returns false when the declared set overlaps the four index words: sectors 2, 5 and 7 read their
// SECOND index after their first store, so a store landing there makes the second address unknowable
// from entry state -- the same exit sub_82B454B8's port takes, for the same reason.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t record = ctx.r4.u32;
  const uint32_t gains = ctx.r6.u32;

  const uint32_t index[4] = {
      REX_LOAD_U32(object + kIndex172 + 0), REX_LOAD_U32(object + kIndex172 + 4),
      REX_LOAD_U32(object + kIndex172 + 8), REX_LOAD_U32(object + kIndex172 + 12)};
  if (Overlaps(gains, kGainFixedEnd, object + kIndex172, kIndexWords)) return false;
  spec.write(gains, kGainFixedEnd);
  for (uint32_t k = 0; k < 4; k++) {
    const uint32_t offset = WordOffset(index[k]);
    if (offset + 4 <= kGainFixedEnd) continue;  // already inside the span above
    if (Overlaps(gains + offset, 4, object + kIndex172, kIndexWords)) return false;
    spec.write(gains + offset, 4);
  }

  // Reads, recorded for replay only: the record, the count, the three bounds, the centre scale, all
  // six coefficient blocks and the four indices -- [+56, +188) in one span -- and the two rodata
  // singles. The old gain values every sector reads back are inside the declared windows, which the
  // harness snapshots anyway.
  spec.read(record, kRecordBytes);
  spec.read(object + kCount, kIndex172 + kIndexWords - kCount);  // +56 .. +187
  spec.read(kAngleScale, 4);
  spec.read(kAngleStep, 4);
  return true;
}

}  // namespace port_82B45788
SKATE3_PORT(82B45788, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
