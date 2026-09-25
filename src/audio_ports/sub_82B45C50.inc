// sub_82B45C50  role: lay out a source's panner entries around a centre point -- the centre is (cos, sin) of one scaled angle times f2, and each entry is placed at the centre plus (cos, sin) of the centre angle offset by one of three spreads, times f3; sub_82B453D8 clamps each into the unit disc and sub_82F52318 turns the clamped direction back into the angle stored at +12. Which entries are written depends only on the count: 1 -> sub_82B269C0, 2 -> {0,1}, 4 -> {0..3}, 6 -> {0..4}, 8 -> {0..6}, and 3/5/7 write nothing   lifted: skate3_recomp.68.cpp:30396 (711 lines)
// STATUS: verified
// calls/session: boot 146074  play 184732  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B45C50.md
namespace port_82B45C50 {

// r3 = the panner entries, 16 bytes each: {x, y, lengthsq, angle}. sub_82B453D8 writes the first
// three, this function writes the fourth. r4 = the channel count. f1 = the centre's angle, f2 = its
// distance, f3 = each entry's radius, f4 = an angle added to the centre's before every spread,
// f5/f6/f7 = the three spreads.
constexpr uint32_t kEntryBytes = 16;
constexpr uint32_t kEntryAngle = 12;
constexpr uint32_t kMaxEntries = 7;  // count 8 writes entries 0..6; nothing writes entry 7

// Four rodata singles, each computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye:
//   0x822F8E48  lis -32208 ; addi -31232 ; lfs 2120  -- scales the two input angles. The same pool
//               sub_82B45788 reads at +772 and sub_82B298E0 at +480.
//   0x822F8E4C  the next word in that pool           -- scales all three spreads
//   0x82256FD8  lis -32219 ; lfs 28632               -- the value f5 is compared against on the
//               two-entry path, which selects a mirrored pair over a spread pair
//   0x822538BC  lis -32219 ; lfs 14524               -- added to the angle on that same path
// probe/harness/out/image is empty on this tree, so no value was read; the body loads all four
// through the guest map, so none needs to be known here.
constexpr uint32_t kAngleScale = 0x822F8E48;
constexpr uint32_t kSpreadScale = 0x822F8E4C;
constexpr uint32_t kMirrorMarker = 0x82256FD8;
constexpr uint32_t kMirrorBias = 0x822538BC;

// The rodata the callees read, copied from their own ported Windows() so a recorded vector replays:
// sub_82B453D8.inc (1.0f and 0.999f) and sub_82B269C0.inc (an angle scale, 0.0f and a bias). Both
// are lower addresses in other aggregators, so they are copied rather than referenced; if either
// port's constant set changes, this list has to change with it.
constexpr uint32_t kClampOne = 0x8231A844;
constexpr uint32_t kClampSnap = 0x820ED800;
constexpr uint32_t kPlaceScale = 0x822F8A6C;
constexpr uint32_t kPlaceZero = 0x82165A10;
constexpr uint32_t kPlaceBias = 0x82060C44;
// sub_82F52318's own table: lis -32005 ; addi 20584, read at +8 .. +176 (and once indexed).
constexpr uint32_t kAtanTable = 0x82FB5068;
constexpr uint32_t kAtanTableBytes = 192;

constexpr uint32_t kFrameBytes = 224;  // stwu r1,-224(r1)

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

// fadds / fsubs / fmuls / frsp: the double operation rounded to single, as the lifted line does it.
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double Frsp(double a) { return static_cast<double>(static_cast<float>(a)); }

// rlwinm rD,rS,4,0,27 on a zero-extended word: the 32-bit shift left by four, so index * 16.
inline uint32_t EntryOffset(uint32_t index) { return index << 4; }

// The tail every placement shares: read the clamped (x, y) back out of the entry, ask sub_82F52318
// for the angle of that direction (f1 = y, f2 = x) and store it at +12.
inline void StoreAngle(PPCContext& ctx, uint8_t* base, uint32_t entry) {
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.f2.f64 = LoadSingle(base, entry + 0);            // lfs f2,0(rN)
  ctx.f1.f64 = LoadSingle(base, entry + 4);            // lfs f1,4(rN)
  skate3::audio::GuestCall(ctx, base, sub_82F52318);   // bl 0x82f52318
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, entry + kEntryAngle, Frsp(ctx.f1.f64));  // frsp fN,f1 ; stfs fN,12(rN)
}

// One placement: cosine and sine of `angle`, each scaled by `radius`, offset by the centre, clamped
// by sub_82B453D8, then StoreAngle. The two scaled components are returned because the mirrored pair
// on the two-entry path reuses them negated instead of taking a second pair of sines.
struct Placed {
  double dx;
  double dy;
};
inline Placed Place(PPCContext& ctx, uint8_t* base, uint32_t entry, double angle, double radius,
                    double centre_x, double centre_y) {
  ctx.f1.f64 = angle;                                 // fmr f1,fN
  skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);  // bl 0x82f4dfb0 -- cosine
  ctx.fpscr.disableFlushModeUnconditional();
  const double cosine = Frsp(ctx.f1.f64);             // frsp f0,f1
  ctx.f1.f64 = angle;                                 // fmr f1,fN, before the fmuls below
  const double dx = MulSingle(cosine, radius);        // fmuls fN,f0,f31
  skate3::audio::GuestCall(ctx, base, sub_82F4DED0);  // bl 0x82f4ded0 -- sine
  ctx.fpscr.disableFlushModeUnconditional();
  const double sine = Frsp(ctx.f1.f64);               // frsp f13,f1
  const double x = AddSingle(dx, centre_x);           // fadds f1,fN,f30
  const double dy = MulSingle(sine, radius);          // fmuls f12,f13,f31
  const double y = AddSingle(dy, centre_y);           // fadds f2,f12,f29
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  skate3::audio::GuestCall(ctx, base, sub_82B453D8, entry);  // bl 0x82b453d8
  StoreAngle(ctx, base, entry);
  return {dx, dy};
}

REX_FUNC(Native) {
  const uint32_t entries = ctx.r3.u32;  // mr r31,r3
  const int32_t count = ctx.r4.s32;     // mr r26,r4

  // __savegprlr_26 and __savefpr_21 spill r26-r31 and f21-f31; the tail restores them and the
  // harness compares both ranges, so the entry values have to survive.
  const PPCRegister saved_gpr[6] = {ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const PPCRegister saved_fpr[11] = {ctx.f21, ctx.f22, ctx.f23, ctx.f24, ctx.f25, ctx.f26,
                                     ctx.f27, ctx.f28, ctx.f29, ctx.f30, ctx.f31};

  // stwu r1,-224(r1). Reproduced -- most ports skip their own frame -- because sub_82F52318 stores
  // both of its arguments into the caller's parameter save area at r1+16 and r1+24 and reads them
  // back as integers, and the sine and cosine helpers spill below the live r1.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  ctx.fpscr.disableFlushModeUnconditional();
  const double distance = ctx.f2.f64;  // fmr f29,f2
  const double radius = ctx.f3.f64;    // fmr f31,f3
  const double spread5 = ctx.f5.f64;   // fmr f24,f5
  const double spread6 = ctx.f6.f64;   // fmr f23,f6
  const double spread7 = ctx.f7.f64;   // fmr f25,f7

  // cmpwi cr6,r4,1 ; bne cr6,0x82b45c9c -- one channel is a tail call with the arguments untouched
  if (count == 1) {
    skate3::audio::GuestCall(ctx, base, sub_82B269C0, entries);  // bl 0x82b269c0
  } else {
    // loc_82B45C9C
    uint32_t index_b = 1;  // li r30,1
    uint32_t index_c = 2;  // li r29,2
    uint32_t index_d = 3;  // li r28,3
    const double scale = LoadSingle(base, kAngleScale);  // lfs f0,2120(r27)
    const double angle_a = MulSingle(ctx.f1.f64, scale);  // fmuls f28,f1,f0
    const double angle_b = MulSingle(ctx.f4.f64, scale);  // fmuls f27,f4,f0
    // fmr f30,f28 -- dead, overwritten by the centre's x below

    // The centre: (cos, sin) of angle_a, each times f2.
    ctx.f1.f64 = angle_a;                               // fmr f1,f28
    skate3::audio::GuestCall(ctx, base, sub_82F4DFB0);  // bl 0x82f4dfb0
    ctx.fpscr.disableFlushModeUnconditional();
    const double centre_cos = Frsp(ctx.f1.f64);           // frsp f0,f1
    ctx.f1.f64 = angle_a;                                 // fmr f1,f28
    const double centre_x = MulSingle(centre_cos, distance);  // fmuls f30,f0,f29
    skate3::audio::GuestCall(ctx, base, sub_82F4DED0);    // bl 0x82f4ded0
    ctx.fpscr.disableFlushModeUnconditional();
    const double centre_sin = Frsp(ctx.f1.f64);               // frsp f13,f1
    const double centre_y = MulSingle(centre_sin, distance);  // fmuls f29,f13,f29

    // addi r11,r26,-2 ; cmplwi cr6,r11,6 ; bgt cr6,0x82b4608c -- an UNSIGNED compare, so counts
    // below 2 (including 0 and every negative) leave here too.
    const uint32_t selector = static_cast<uint32_t>(count) - 2;
    if (selector <= 6) {
      const double spread_scale = LoadSingle(base, kSpreadScale);  // lfs f26,2124(r27)
      // mtctr r11 ; cmpwi cr6,r11,0 ; six bdzf 4*cr6+eq ; bne cr6 -- a jump table in branch form:
      // 0 falls through to the pair below, 2 enters loc_82B45F48, 4 enters loc_82B45EF4, 6 enters
      // loc_82B45E5C, and 1, 3 and 5 (counts 3, 5 and 7) leave without a single store.
      if (selector == 0) {
        // The two-entry layout, 0x82B45D08.
        const double marker = LoadSingle(base, kMirrorMarker);  // lfs f0,28632(r11)
        // fcmpu cr6,f24,f0 ; bne cr6,0x82b45dac
        if (spread5 == marker) {
          const double sum = AddSingle(angle_a, angle_b);   // fadds f13,f28,f27
          const double bias = LoadSingle(base, kMirrorBias);  // lfs f0,14524(r11)
          const double angle = AddSingle(sum, bias);        // fadds f28,f13,f0
          const Placed first = Place(ctx, base, entries, angle, radius, centre_x, centre_y);
          // The mirror: no second pair of trigonometric calls -- the two scaled components are
          // reused negated, which is also why f31 (the radius) is dead after the first placement.
          ctx.fpscr.disableFlushModeUnconditional();
          const uint32_t second = entries + kEntryBytes;      // addi r31,r31,16
          ctx.f2.f64 = SubSingle(centre_y, first.dy);         // fsubs f2,f29,f31
          ctx.f1.f64 = SubSingle(centre_x, first.dx);         // fsubs f1,f30,f28
          skate3::audio::GuestCall(ctx, base, sub_82B453D8, second);  // bl 0x82b453d8
          StoreAngle(ctx, base, second);
        } else {
          // loc_82B45DAC: a spread pair instead, with the spread constant reloaded.
          const double reload = LoadSingle(base, kSpreadScale);  // lfs f0,2124(r27)
          const double sum = AddSingle(angle_a, angle_b);        // fadds f28,f28,f27
          const double offset = MulSingle(spread5, reload);      // fmuls f27,f24,f0
          Place(ctx, base, entries, AddSingle(sum, offset), radius, centre_x, centre_y);
          Place(ctx, base, entries + kEntryBytes, SubSingle(sum, offset), radius, centre_x,
                centre_y);
        }
      } else if (selector == 2 || selector == 4 || selector == 6) {
        // The three remaining cases fall through one into the next: 8 into 6 into 4. Selectors 1, 3
        // and 5 -- counts 3, 5 and 7 -- reach loc_82B4608C with no store at all.
        if (selector == 6) {
          // loc_82B45E5C: entries 5 and 6, at the third spread.
          const double offset7 = MulSingle(spread7, spread_scale);  // fmuls f25,f25,f26
          const double sum = AddSingle(angle_a, angle_b);           // fadds f22,f28,f27
          Place(ctx, base, entries + EntryOffset(5), AddSingle(sum, offset7), radius, centre_x,
                centre_y);  // fadds f21,f22,f25 ; addi r3,r31,80
          Place(ctx, base, entries + EntryOffset(6), SubSingle(sum, offset7), radius, centre_x,
                centre_y);  // fsubs f25,f22,f25 ; addi r3,r31,96
        }
        if (selector == 6 || selector == 4) {
          // loc_82B45EF4: entry 1, straight down the centre angle, and the three indices shift up.
          const double sum = AddSingle(angle_a, angle_b);  // fadds f25,f28,f27
          Place(ctx, base, entries + EntryOffset(1), sum, radius, centre_x, centre_y);
          index_b = 2;  // li r30,2
          index_c = 3;  // li r29,3
          index_d = 4;  // li r28,4
        }
        // loc_82B45F48: four entries at the first two spreads. Reached by counts 4, 6 and 8.
        const double sum = AddSingle(angle_a, angle_b);            // fadds f28,f28,f27
        const double offset5 = MulSingle(spread5, spread_scale);   // fmuls f25,f24,f26
        const double offset6 = MulSingle(spread6, spread_scale);   // fmuls f27,f23,f26
        Place(ctx, base, entries, AddSingle(sum, offset5), radius, centre_x,
              centre_y);  // fadds f26,f28,f25 ; mr r3,r31
        Place(ctx, base, entries + EntryOffset(index_b), SubSingle(sum, offset5), radius, centre_x,
              centre_y);  // fsubs f26,f28,f25 ; rlwinm r11,r30,4 ; add r30,r11,r31
        Place(ctx, base, entries + EntryOffset(index_c), AddSingle(sum, offset6), radius, centre_x,
              centre_y);  // fadds f26,f28,f27 ; rlwinm r11,r29,4 ; add r30,r11,r31
        Place(ctx, base, entries + EntryOffset(index_d), SubSingle(sum, offset6), radius, centre_x,
              centre_y);  // fsubs f28,f28,f27 ; rlwinm r11,r28,4 ; add r31,r11,r31
      }
    }
  }

  // loc_82B4608C: addi r1,r1,224 ; __restfpr_21 ; __restgprlr_26
  ctx.r1.u32 = entry_sp;
  ctx.r26 = saved_gpr[0];
  ctx.r27 = saved_gpr[1];
  ctx.r28 = saved_gpr[2];
  ctx.r29 = saved_gpr[3];
  ctx.r30 = saved_gpr[4];
  ctx.r31 = saved_gpr[5];
  ctx.f21 = saved_fpr[0];
  ctx.f22 = saved_fpr[1];
  ctx.f23 = saved_fpr[2];
  ctx.f24 = saved_fpr[3];
  ctx.f25 = saved_fpr[4];
  ctx.f26 = saved_fpr[5];
  ctx.f27 = saved_fpr[6];
  ctx.f28 = saved_fpr[7];
  ctx.f29 = saved_fpr[8];
  ctx.f30 = saved_fpr[9];
  ctx.f31 = saved_fpr[10];
}

// The write set is a prefix of the entry array, and which prefix depends only on r4 -- an entry
// register -- so it is exact, one span, and never in doubt:
//
//   count 1        entry 0, through sub_82B269C0, whose port declares exactly [r3, r3+16)
//   count 2        entries 0 and 1
//   count 4        entries 0..3
//   count 6        entry 1 from loc_82B45EF4, then 0, 2, 3, 4 from loc_82B45F48
//   count 8        entries 5 and 6 from loc_82B45E5C, then the six above
//   anything else  nothing at all: counts 3, 5, 7 fall through the branch table, and the unsigned
//                  `count - 2 >u 6` test also drops 0 and every negative count
//
// Per entry it is sub_82B453D8's three singles at +0/+4/+8 and this function's angle at +12, so the
// full 16 bytes. Not declared: this function's own 224-byte frame, which is where sub_82F52318 puts
// its two arguments (r1+16, r1+24) and where the sine and cosine helpers spill.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t entries = ctx.r3.u32;
  const int32_t count = ctx.r4.s32;
  if (entries == 0) return false;  // sub_82B453D8 would store through null

  uint32_t written = 0;
  if (count == 1) {
    written = 1;
  } else if (static_cast<uint32_t>(count) - 2 <= 6) {
    switch (count) {
      case 2: written = 2; break;
      case 4: written = 4; break;
      case 6: written = 5; break;
      case 8: written = kMaxEntries; break;
      default: written = 0; break;  // 3, 5, 7
    }
  }
  spec.write(entries, written * kEntryBytes);

  // Reads recorded for replay: the four rodata singles this function loads, the ones its two ported
  // callees load, and sub_82F52318's table. The clamped (x, y) each placement reads back are inside
  // the declared window, which the harness snapshots anyway.
  spec.read(kAngleScale, 8);  // kAngleScale and kSpreadScale are adjacent
  spec.read(kMirrorMarker, 4);
  spec.read(kMirrorBias, 4);
  spec.read(kClampOne, 4);
  spec.read(kClampSnap, 4);
  if (count == 1) {
    spec.read(kPlaceScale, 4);
    spec.read(kPlaceZero, 4);
    spec.read(kPlaceBias, 4);
  }
  spec.read(kAtanTable, kAtanTableBytes);
  // The image's sine and cosine pools, reached through the callees -- for replay only.
  spec.read(0x82FB4E20u, 120);
  spec.read(0x82010108u, 8);
  spec.read(0x82FB58C8u, 8);
  return true;
}

}  // namespace port_82B45C50
SKATE3_PORT(82B45C50, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
