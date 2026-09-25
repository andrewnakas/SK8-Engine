// sub_82B2F2C8  role: resample six band values through a blended knot curve, normalise them, then re-arm three ring reservations per queue   lifted: skate3_recomp.67.cpp:42515 (433 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 4 comparable calls in the weaker profile, 4 in the stronger, against 433 lifted lines -- 433 lines against four calls. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 8  play 8  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2F2C8.md
namespace port_82B2F2C8 {

// Constant addresses computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye: one
// misread digit in sub_82B2FE00 produced this project's first shadow divergence.
constexpr uint32_t kSegmentEdges = 0x821161A0;  // lis -32239 ; addi 24992: +0 low, +4 mid, +8 high
constexpr uint32_t kKnotX = 0x821161B0;         // lis -32239 ; addi 25008: nine knot abscissae
constexpr uint32_t kKnotRows = 0x82116218;      // kKnotX + 104: the row cursor, read at -68 and +4
                                                // and advanced 72 bytes (9 * 8) per segment
constexpr uint32_t kSegmentRate = 0x822F8600;   // lis -32208 ; addi -31232: +1120 lower, +1124 upper
constexpr uint32_t kOneSingle = 0x8231A844;     // lis -32206 ; addi -22460
constexpr uint32_t kHeadroom = 0x82063A48;      // lis -32250 ; lfs 14920
constexpr uint32_t kZeroSingle = 0x82165A10;    // lis -32234 ; lfs 23056 -- 0.0f, the same word
                                                // sub_82B1CF50 and sub_82B2DAC8 load
constexpr uint32_t kKnots = 9;                  // li r8,9
constexpr uint32_t kOutputs = 6;                // li r11,6 and li r9,6

// The rodata read by loop 1: the abscissae at kKnotX, then the three 36-byte rows the two
// segments blend between (kKnotX + 36 .. + 216).
constexpr uint32_t kKnotTableBytes = 216;

// Nothing in docs/rw_audio_structs.h names this object, so the offsets stay bare.
constexpr uint32_t kCurrentLevel = 68;    // lfs f13,68(r3)
constexpr uint32_t kTargetLevel = 360;    // lfs f0,360(r3): equal to +68 means nothing more to do
constexpr uint32_t kPendingSizes = 392;   // a stride-12 cursor read at -4, +0 and +4
constexpr uint32_t kEnvelopeBase = 480;   // addi r10,r3,480, pre-incremented 36 per store
constexpr uint32_t kEnvelopeStride = 36;
constexpr uint32_t kQueueBase = 720;      // addi r11,r3,720
constexpr uint32_t kQueueStride = 180;    // three records of 60
constexpr uint32_t kQueues = 2;           // li r10,2
constexpr uint32_t kRecords = 3;

// One reservation record, 60 bytes. Only the first two words are read; the publish writes five
// fields spanning +12 .. +36 inclusive.
constexpr uint32_t kRecStride = 60;
constexpr uint32_t kRecLimit = 0;
constexpr uint32_t kRecPos = 4;
constexpr uint32_t kRecLimitCopy = 12;
constexpr uint32_t kRecZero = 16;
constexpr uint32_t kRecSize = 20;
constexpr uint32_t kRecPosCopy = 32;
constexpr uint32_t kRecFlag = 36;         // u8
constexpr uint32_t kRecPublished = 25;    // +12 .. +36 inclusive

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// stfs / stfsu / stfsx: the double rounded back to a single and stored.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

inline double AddSingle(double a, double b) { return double(float(a + b)); }        // fadds
inline double SubSingle(double a, double b) { return double(float(a - b)); }        // fsubs
inline double MulSingle(double a, double b) { return double(float(a * b)); }        // fmuls
inline double DivSingle(double a, double b) { return double(float(a / b)); }        // fdivs
inline double FmaSingle(double a, double b, double c) {                             // fmadds
  return double(float(std::fma(a, b, c)));
}

// addi r7,r9,35 ; rlwinm r7,r7,0,0,26 ; add r10,r7,r10 -- the pending size plus a four-byte
// header, rounded up to 32, added to the write position. RexGlue's `add` is 64-bit on two
// zero-extended words and can carry into bit 32; every consumer of the sum reads only the low
// word, so the 64-bit intermediate is kept and truncated here, at the one place it is read.
inline uint32_t Needed(uint32_t size, uint32_t pos) {
  const uint32_t rounded = (size + 35u) & 0xFFFFFFE0u;
  return static_cast<uint32_t>(static_cast<uint64_t>(rounded) + static_cast<uint64_t>(pos));
}

// subfc r4,r10,r6 ; eqv r3,r10,r6 ; rlwinm r10,r3,1,31,31 ; addze r7,r10 ; clrlwi r6,r7,31 ;
// cmplwi cr6,r6,0 ; bne -- the carry plus sign-equality idiom, which works out to a SIGNED
// `limit >= needed`. Kept in its raw form so the reading follows the instructions rather than
// the inference.
inline bool Fits(uint32_t limit, uint32_t needed) {
  const uint32_t carry = limit >= needed ? 1u : 0u;          // subfc: CA = r6.u32 >= r10.u32
  const uint32_t signs_equal = (~(needed ^ limit)) >> 31;    // eqv then rlwinm of bit 31
  return ((signs_equal + carry) & 1u) == 0u;                 // addze ; clrlwi ; the beq path
}

// fcmpu on two singles, decided on the bit patterns so the window builder performs no float
// operation of its own (a signalling NaN widened to double under the guest's unmasked MXCSR
// would raise). Widening a single to double is exact, so equality is preserved: identical
// patterns are equal unless NaN, and +0.0 equals -0.0.
inline bool SingleEqual(uint32_t a, uint32_t b) {
  const bool a_nan = (a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) != 0u;
  const bool b_nan = (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0u;
  if (a_nan || b_nan) return false;
  if ((a & 0x7FFFFFFFu) == 0u && (b & 0x7FFFFFFFu) == 0u) return true;
  return a == b;
}

// Guest pointers here live around 0x82xxxxxx, so no range wraps 2^32.
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return a < b + blen && b < a + alen;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;                      // six singles, written then rewritten
  const uint32_t in_delta = ctx.r6.u32 - ctx.r4.u32;    // subf r8,r4,r6, consumed as u32
  // Two nine-entry scratch arrays in this function's own red zone, contiguous: the abscissae at
  // -80(r1) and the blended ordinates at -44(r1). Not windowed -- the function's own frame.
  const uint32_t xs = ctx.r1.u32 - 80;                  // addi r8,r1,-80
  const uint32_t ys = xs + 36;                          // addi r7,r1,-44

  // lis r11,-32239 ; addi r11,r11,24992 ; lfs f12,8(r11)
  ctx.fpscr.disableFlushModeUnconditional();
  const double high_edge = LoadSingle(base, kSegmentEdges + 8);

  double anchor = 0.0;     // f0 feeding the segment setup
  double span = 0.0;       // f13 at loc_82B2F300
  double rate = 0.0;       // f0 at loc_82B2F300
  uint32_t segment = 0;    // r11 at loc_82B2F300

  bool upper = !(ctx.f1.f64 < high_edge);               // fcmpu cr6,f1,f12 ; blt cr6,0x82b2f554
  if (upper) {
    anchor = high_edge;                                 // fmr f0,f12
  } else {
    // loc_82B2F554
    ctx.fpscr.disableFlushModeUnconditional();
    const double low_edge = LoadSingle(base, kSegmentEdges + 0);   // lfs f0,0(r11)
    const double mid_edge = LoadSingle(base, kSegmentEdges + 4);   // lfs f13,4(r11)
    anchor = low_edge;
    if (ctx.f1.f64 > low_edge) {                        // fcmpu cr6,f1,f0 ; ble cr6,0x82b2f570
      anchor = ctx.f1.f64;                              // fmr f0,f1
      upper = ctx.f1.f64 > mid_edge;                    // fcmpu cr6,f1,f13 ; bgt cr6,0x82b2f2e4
    }
    if (!upper) {
      // loc_82B2F570
      ctx.fpscr.disableFlushModeUnconditional();
      span = SubSingle(mid_edge, anchor);               // fsubs f13,f13,f0
      segment = 0;                                      // mr r11,r5
      rate = LoadSingle(base, kSegmentRate + 1120);     // lfs f0,1120(r9)
    }
  }
  if (upper) {
    // loc_82B2F2E4
    ctx.fpscr.disableFlushModeUnconditional();
    span = SubSingle(high_edge, anchor);                // fsubs f13,f12,f0
    segment = 1;                                        // li r11,1
    rate = LoadSingle(base, kSegmentRate + 1124);       // lfs f0,1124(r9)
  }

  // loc_82B2F300
  ctx.fpscr.disableFlushModeUnconditional();
  const double weight = MulSingle(span, rate);          // fmuls f0,f13,f0
  const double one = LoadSingle(base, kOneSingle);      // lfs f11,0(r10)
  // rlwinm r9,r11,3,0,28 ; add r11,r11,r9 ; rlwinm r7,r11,3,0,28 -- segment * 9 * 8.
  const uint32_t nine = segment + ((segment << 3) & 0xFFFFFFF8u);
  const uint32_t row = ((nine << 3) & 0xFFFFFFF8u) + kKnotRows;    // add r10,r7,r10
  const double rest_weight = SubSingle(one, weight);    // fsubs f13,f11,f0

  // loc_82B2F338: nine knots. The abscissae are copied straight out of the constant table; each
  // ordinate blends the row 68 bytes back with the row one word on.
  uint32_t walk = row;
  for (uint32_t k = 0; k < kKnots; k++) {               // li r8,9 ; mtctr r8 ; bdnz 0x82b2f338
    ctx.fpscr.disableFlushModeUnconditional();
    const double row_a = LoadSingle(base, walk - 68);   // lfs f10,-68(r10)
    walk += 4;                                          // lfsu f12,4(r10)
    const double row_b = LoadSingle(base, walk);
    const double scaled_b = MulSingle(row_b, rest_weight);              // fmuls f9,f12,f13
    const double knot_x = LoadSingle(base, kKnotX + 4 * k);             // lfsx f8,r11,r9
    StoreSingle(base, xs + 4 * k, knot_x);                              // stfsx f8,r11,r8
    StoreSingle(base, ys + 4 * k, FmaSingle(row_a, weight, scaled_b));  // fmadds f7 ; stfsx f7,r11,r7
  }

  // Still loop 1's basic block, so no fresh flush-mode call: the knot spacing is uniform, and
  // its reciprocal scales every lookup below.
  const double x1 = LoadSingle(base, xs + 4);           // lfs f0,-76(r1)
  const double x0 = LoadSingle(base, xs + 0);           // lfs f13,-80(r1)
  const double step = SubSingle(x1, x0);                // fsubs f12,f0,f13
  const double inv_step = DivSingle(one, step);         // fdivs f12,f11,f12
  uint32_t cursor = out;                                // mr r9,r4

  // loc_82B2F380: six inputs, each located in the abscissae and interpolated between the two
  // bracketing ordinates.
  for (uint32_t i = 0; i < kOutputs; i++) {             // li r11,6 ; mtctr r11 ; bdnz 0x82b2f380
    ctx.fpscr.disableFlushModeUnconditional();
    const double value = LoadSingle(base, in_delta + cursor);   // lfsx f13,r8,r9
    // loc_82B2F38C: walk up the abscissae until one is not below `value`. UNBOUNDED in the
    // original -- a value above every knot runs off xs[9] into ys and then out of the red zone.
    // Reproduced as written; the lifted body ran first on the same data, so it terminates.
    uint32_t index = 0;                                 // mr r11,r5
    uint32_t probe = xs;                                // addi r10,r1,-80
    double edge;
    do {
      ctx.fpscr.disableFlushModeUnconditional();
      probe += 4;                                       // lfsu f0,4(r10)
      edge = LoadSingle(base, probe);
      index++;                                          // addi r11,r11,1
    } while (value > edge);                             // fcmpu cr6,f13,f0 ; bgt cr6,0x82b2f38c
    const uint32_t slot = (index << 2) & 0xFFFFFFFCu;   // rlwinm r11,r11,2,0,29
    const double knot_x = LoadSingle(base, xs + slot);      // lfsx f0,r11,r10 (r1-80)
    const double behind = SubSingle(knot_x, value);         // fsubs f13,f0,f13
    const double y_below = LoadSingle(base, ys - 4 + slot); // lfsx f10,r11,r7 (r1-48)
    const double y_at = LoadSingle(base, ys + slot);        // lfsx f9,r11,r6  (r1-44)
    const double frac = MulSingle(behind, inv_step);        // fmuls f8,f13,f12
    const double ahead = SubSingle(one, frac);              // fsubs f7,f11,f8
    const double lower = MulSingle(y_below, frac);          // fmuls f6,f10,f8
    StoreSingle(base, cursor, FmaSingle(y_at, ahead, lower));  // fmadds f5 ; stfs f5,0(r9)
    cursor += 4;                                        // addi r9,r9,4
  }

  // Still loop 2's block. Two singles on the object decide whether anything past here runs.
  const double current = LoadSingle(base, object + kCurrentLevel);   // lfs f13,68(r3)
  const double reference = LoadSingle(base, object + kTargetLevel);  // lfs f0,360(r3)
  if (current == reference) {                           // fcmpu cr6,f13,f0 ; beq cr6,0x82b2f54c
    ctx.r3.s64 = 1;                                     // li r3,1
    return;
  }
  const double last = LoadSingle(base, out + 20);       // lfs f12,20(r4): reloads loop 2's last store
  const double headroom = LoadSingle(base, kHeadroom);  // lfs f0,14920(r11)
  double ceiling = AddSingle(last, headroom);           // fadds f0,f12,f0
  if (current > ceiling) {                              // fcmpu cr6,f13,f0 ; ble cr6,0x82b2f404
    ceiling = current;                                  // fmr f0,f13
  }
  // loc_82B2F404
  ctx.fpscr.disableFlushModeUnconditional();
  const double norm = DivSingle(one, ceiling);          // fdivs f0,f11,f0
  uint32_t src = out - 4;                               // addi r11,r4,-4
  uint32_t envelope = object + kEnvelopeBase;           // addi r10,r3,480
  const double zero = LoadSingle(base, kZeroSingle);    // lfs f13,23056(r9)
  // loc_82B2F420: clear one envelope slot per band and normalise the band in place.
  for (uint32_t i = 0; i < kOutputs; i++) {             // li r9,6 ; mtctr r9 ; bdnz 0x82b2f420
    ctx.fpscr.disableFlushModeUnconditional();
    envelope += kEnvelopeStride;                        // stfsu f13,36(r10)
    StoreSingle(base, envelope, zero);
    const double raw = LoadSingle(base, src + 4);       // lfs f12,4(r11)
    src += 4;                                           // stfsu f11,4(r11)
    StoreSingle(base, src, MulSingle(raw, norm));       // fmuls f11,f12,f0
  }

  // loc_82B2F444: two queues of three reservation records. The pending size for each record
  // comes from a stride-12 cursor; the first record of a queue reads the word BEFORE it.
  uint32_t sizes = object + kPendingSizes;              // addi r8,r3,392
  uint32_t queue = object + kQueueBase;                 // addi r11,r3,720
  for (uint32_t q = 0; q < kQueues; q++) {              // li r10,2 ; mtctr r10 ; bdnz 0x82b2f444
    {
      // Record 0. The store ORDER here differs from the other two records; kept as emitted.
      const uint32_t rec = queue;
      const uint32_t size = REX_LOAD_U32(sizes - 4);              // lwz r9,-4(r8)
      const uint32_t pos = REX_LOAD_U32(rec + kRecPos);           // lwz r10,4(r11)
      const uint32_t limit = REX_LOAD_U32(rec + kRecLimit);       // lwz r6,0(r11)
      if (Fits(limit, Needed(size, pos))) {
        const uint32_t limit_again = REX_LOAD_U32(rec + kRecLimit);  // lwz r7,0(r11)
        const uint32_t pos_again = REX_LOAD_U32(rec + kRecPos);      // lwz r6,4(r11)
        // addi r10,r9,1 is 64-bit; the stw keeps the low word.
        REX_STORE_U32(rec + kRecSize, static_cast<uint32_t>(size + 1u));  // stw r10,20(r11)
        REX_STORE_U32(rec + kRecZero, 0);                                 // stw r5,16(r11)
        REX_STORE_U8(rec + kRecFlag, 0);                                  // stb r5,36(r11)
        REX_STORE_U32(rec + kRecLimitCopy, limit_again);                  // stw r7,12(r11)
        REX_STORE_U32(rec + kRecPosCopy, pos_again);                      // stw r6,32(r11)
      }
    }
    {
      // loc_82B2F498 -- record 1.
      const uint32_t rec = queue + kRecStride;
      const uint32_t size = REX_LOAD_U32(sizes + 0);              // lwz r9,0(r8)
      const uint32_t pos = REX_LOAD_U32(rec + kRecPos);           // lwz r10,64(r11)
      const uint32_t limit = REX_LOAD_U32(rec + kRecLimit);       // lwz r6,60(r11)
      if (Fits(limit, Needed(size, pos))) {
        REX_STORE_U32(rec + kRecZero, 0);                                 // stw r5,76(r11)
        const uint32_t limit_again = REX_LOAD_U32(rec + kRecLimit);       // lwz r9,60(r11)
        REX_STORE_U32(rec + kRecLimitCopy, limit_again);                  // stw r9,72(r11)
        const uint32_t pos_again = REX_LOAD_U32(rec + kRecPos);           // lwz r7,64(r11)
        REX_STORE_U32(rec + kRecPosCopy, pos_again);                      // stw r7,92(r11)
        REX_STORE_U32(rec + kRecSize, static_cast<uint32_t>(size + 1u));  // stw r10,80(r11)
        REX_STORE_U8(rec + kRecFlag, 0);                                  // stb r5,96(r11)
      }
    }
    {
      // loc_82B2F4EC -- record 2, identical to record 1 sixty bytes on.
      const uint32_t rec = queue + 2 * kRecStride;
      const uint32_t size = REX_LOAD_U32(sizes + 4);              // lwz r9,4(r8)
      const uint32_t pos = REX_LOAD_U32(rec + kRecPos);           // lwz r10,124(r11)
      const uint32_t limit = REX_LOAD_U32(rec + kRecLimit);       // lwz r6,120(r11)
      if (Fits(limit, Needed(size, pos))) {
        REX_STORE_U32(rec + kRecZero, 0);                                 // stw r5,136(r11)
        const uint32_t limit_again = REX_LOAD_U32(rec + kRecLimit);       // lwz r9,120(r11)
        REX_STORE_U32(rec + kRecLimitCopy, limit_again);                  // stw r9,132(r11)
        const uint32_t pos_again = REX_LOAD_U32(rec + kRecPos);           // lwz r7,124(r11)
        REX_STORE_U32(rec + kRecPosCopy, pos_again);                      // stw r7,152(r11)
        REX_STORE_U32(rec + kRecSize, static_cast<uint32_t>(size + 1u));  // stw r10,140(r11)
        REX_STORE_U8(rec + kRecFlag, 0);                                  // stb r5,156(r11)
      }
    }
    sizes += 12;                                        // addi r8,r8,12
    queue += kQueueStride;                              // addi r11,r11,180
  }
  // loc_82B2F54C
  ctx.r3.s64 = 1;                                       // li r3,1
}

// The write set from entry state. Loop 1 and the two search loops touch only the red zone; the
// rest is r4's six outputs plus, when the two level singles differ, six envelope slots and the
// six reservation records. Every address derives from r3, r4 and memory this call never writes,
// so the only way it stops being enumerable is an output array aimed at one of the words a later
// branch tests -- those cases return false instead of guessing.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const uint32_t in = ctx.r6.u32;
  const uint32_t outputs = 4 * kOutputs;

  // loc_82B2F380 runs on every path through the function.
  spec.write(out, outputs);
  spec.read(in, outputs);
  spec.read(out, outputs);
  spec.read(kSegmentEdges, 12);
  spec.read(kSegmentRate + 1120, 8);
  spec.read(kOneSingle, 4);
  spec.read(kKnotX, kKnotTableBytes);
  spec.read(object + kCurrentLevel, 4);
  spec.read(object + kTargetLevel, 4);

  if (Overlaps(out, outputs, object + kCurrentLevel, 4) ||
      Overlaps(out, outputs, object + kTargetLevel, 4)) {
    return false;  // loop 2 would decide its own successor
  }
  const uint32_t current = REX_LOAD_U32(object + kCurrentLevel);
  const uint32_t target = REX_LOAD_U32(object + kTargetLevel);
  if (SingleEqual(current, target)) {
    return true;  // loc_82B2F54C: nothing past loop 2 runs
  }

  const uint32_t sizes = object + kPendingSizes - 4;
  if (Overlaps(out, outputs, sizes, 4 * kQueues * kRecords)) {
    return false;  // loop 2 would choose the sizes loop 4 reserves for
  }
  for (uint32_t q = 0; q < 2; q++) {                      // kQueues
    for (uint32_t r = 0; r < 3; r++) {                    // kRecords
      const uint32_t rec = object + kQueueBase + kQueueStride * q + kRecStride * r;
      if (Overlaps(out, outputs, rec + kRecLimit, 8)) {
        return false;  // loop 2 would choose whether this record publishes
      }
      // The union of both branches. Each publish IS predictable -- the two words it tests are
      // never written by this call -- but declaring all six costs six spans, and a record that
      // does not publish compares equal anyway.
      spec.write(rec + kRecLimitCopy, kRecPublished);
      spec.read(rec + kRecLimit, 8);
    }
  }
  spec.read(sizes, 4 * kQueues * kRecords);
  spec.read(kHeadroom, 4);
  spec.read(kZeroSingle, 4);
  // loc_82B2F420: six pre-incremented stfsu from object+480, so +516 .. +696 at stride 36.
  for (uint32_t i = 0; i < 6; i++) {                      // kOutputs
    spec.write(object + kEnvelopeBase + kEnvelopeStride * (i + 1), 4);
  }
  return true;
}

}  // namespace port_82B2F2C8
SKATE3_PORT(82B2F2C8, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnR3)
