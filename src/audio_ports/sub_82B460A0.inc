// sub_82B460A0  role: fill one 8x8 mix matrix from a source's panner entries -- a stereo destination gets a two-column cosine-ish pan per source computed inline; any other destination pans each source through sub_82B454B8/sub_82B45788/sub_82B45B60 into its own row, then zeroes one whole row and one whole column and writes their crossing cell   lifted: skate3_recomp.68.cpp:31108 (286 lines)
// STATUS: verified
// calls/session: boot 146074  play 184732  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B460A0.md
namespace port_82B460A0 {

// r3 = the listener/format config object sub_82B454B8, sub_82B45788 and sub_82B45B60 all take in
// r3. Nothing in docs/rw_audio_structs.h names it; only these fields matter here.
constexpr uint32_t kCount = 56;      // s32 destination channel count: 2, 6 or 8 in the compares
constexpr uint32_t kIndex172 = 172;  // s32[4] the gain-array indices the three callees store by
constexpr uint32_t kIndexWords = 16;
constexpr uint32_t kConfigRead = 192;  // +0..+191: the speaker table, the count, the thresholds
                                       // at +60..+168 sub_82B45788 reads, and the four indices

// r4 = the panner entries sub_82B45C50 writes: 16 bytes each, {x, y, len2, gain}.
constexpr uint32_t kEntryBytes = 16;
constexpr uint32_t kEntryY = 4;      // the stereo path reads only this field
constexpr uint32_t kEntryLenSq = 8;  // handed to sub_82B45B60 in f3

// r10 = the matrix base (NOT a positional argument; `mr r27,r10` at entry). f32[8][8], row =
// source, column = destination -- the same [+444, +700) region sub_82B29BE0 declares.
constexpr uint32_t kRowBytes = 32;
constexpr uint32_t kMaxRows = 8;
constexpr uint32_t kMatrixBytes = kRowBytes * kMaxRows;  // 256

constexpr uint32_t kFrameBytes = 176;  // stwu r1,-176(r1)

// Three rodata singles. Each `lis rN,imm ; lfs fM,off(rN)` pair resolves to ((imm & 0xFFFF) << 16)
// + off; the values are the ones docs/notes for sub_82B453D8 and sub_82B454B8 already record from
// the image dump, and the body loads them through the guest map regardless.
constexpr uint32_t kHalfSingle = 0x8209975C;  // 0.5f   lis r9,-32246  ; lfs f12,-26788(r9)
constexpr uint32_t kOneSingle = 0x8231A844;   // 1.0f   lis r8,-32206  ; lfs f0,-22460(r8)
constexpr uint32_t kZeroSingle = 0x82165A10;  // 0.0f   lis r11,-32234 ; lfs f0,23056(r11)

// lfs / lfsu: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs / stfsu / stfsx: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fadds / fsubs / fmuls / fdivs / fsqrts: the double operation rounded to single, as lifted.
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double SqrtSingle(double a) { return static_cast<double>(static_cast<float>(sqrt(a))); }

// fmadds a*b+c: one fused product, rounded once to double then to single.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

// Is [a, a+alen) disjoint from [b, b+blen)? 64-bit, so a span reaching 2^32 cannot fold around.
inline bool Disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  return a + alen <= b || b + blen <= a;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;      // mr r28,r3
  const uint32_t entries = ctx.r4.u32;     // (r30 on the panning path)
  const int32_t sources = ctx.r5.s32;      // mr r26,r5
  const uint32_t matrix = ctx.r10.u32;     // mr r27,r10

  // __savegprlr_26 and __savefpr_28 spill r26-r31 and f28-f31; the tail restores them. The harness
  // compares both ranges, so the entry values have to survive -- locals do the same job.
  const PPCRegister saved_gpr[6] = {ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const PPCRegister saved_fpr[4] = {ctx.f28, ctx.f29, ctx.f30, ctx.f31};

  // stwu r1,-176(r1). Reproduced -- most ports skip their own frame -- because every callee here
  // spills below the caller's r1 (sub_82B454B8 puts f30/f31/r31 in the red zone, and the sin/cos
  // helpers sub_82F4DED0/sub_82F4DFB0 reached through sub_82B45788 spill at r1-8 .. r1-32).
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  const int32_t dest = static_cast<int32_t>(REX_LOAD_U32(object + kCount));  // lwz r11,56(r3)
  ctx.fpscr.disableFlushModeUnconditional();
  const double weight = ctx.f1.f64;  // fmr f31,f1
  const double focus = ctx.f2.f64;   // fmr f29,f2
  const double fill = ctx.f3.f64;    // fmr f28,f3
  const double gain = ctx.f4.f64;    // fmr f30,f4

  // cmpwi cr6,r11,2 ; bne cr6,0x82b4614c
  if (dest == 2) {
    // cmpwi cr6,r5,0 ; ble cr6,0x82b46210 -- a signed test, so a zero or negative count writes
    // nothing at all.
    if (sources > 0) {
      const double half = LoadSingle(base, kHalfSingle);  // lfs f12,-26788(r9)
      const double one = LoadSingle(base, kOneSingle);    // lfs f0,-22460(r8)
      uint32_t row = matrix - 28;                         // addi r11,r10,-28
      uint32_t entry = entries - 12;                      // addi r10,r4,-12
      uint32_t trips = ctx.r5.u32;                        // mtctr r5
      // loc_82B46100: two gains per source, from the entry's y alone.
      do {
        ctx.fpscr.disableFlushModeUnconditional();
        entry += kEntryBytes;                                    // lfsu f13,16(r10)
        const double y = LoadSingle(base, entry);
        const double biased = AddSingle(y, one);                  // fadds f13,f13,f0
        const double right = MulSingle(biased, half);             // fmuls f11,f13,f12
        const double left = SubSingle(one, right);                // fsubs f10,f0,f11
        double norm = MulSingle(right, right);                    // fmuls f9,f11,f11
        norm = FmaddSingle(left, left, norm);                     // fmadds f8,f10,f10,f9
        const double len = SqrtSingle(norm);                      // fsqrts f7,f8
        const double scale = DivSingle(one, len);                 // fdivs f6,f0,f7
        const double scaled = MulSingle(scale, weight);           // fmuls f5,f6,f31
        const double total = MulSingle(scaled, gain);             // fmuls f4,f5,f30
        StoreSingle(base, row + 28, MulSingle(total, right));     // fmuls f3,f4,f11 ; stfs f3,28(r11)
        const double second = MulSingle(left, total);             // fmuls f2,f10,f4
        row += kRowBytes;                                         // stfsu f2,32(r11)
        StoreSingle(base, row, second);
      } while (--trips != 0);                                     // bdnz 0x82b46100
    }
    // the stereo path's own epilogue, identical to loc_82B46210's
  } else {
    // loc_82B4614C: one row per source, minus one when the destination is 6 channels or more --
    // the row the tail below zeroes is filled by the tail instead.
    int32_t rows = sources - 1;             // addi r29,r26,-1
    if (sources < 6) rows = sources;        // bge cr6,0x82b4615c ; mr r29,r26

    // cmpwi cr6,r29,0 ; ble cr6,0x82b461b4
    if (rows > 0) {
      uint32_t entry = entries;  // mr r30,r4
      uint32_t row = matrix;     // mr r31,r27
      int32_t left = rows;
      // loc_82B4616C
      do {
        ctx.fpscr.disableFlushModeUnconditional();
        ctx.f1.f64 = focus;                                   // fmr f1,f29
        ctx.r6.u64 = row;                                     // mr r6,r31
        skate3::audio::GuestCall(ctx, base, sub_82B454B8, object, entry);  // mr r3,r28 ; mr r4,r30

        // The original reloads only r4 before the next call: sub_82B454B8 leaves r3 and r6 alone
        // (it clobbers r4, r5 and r7-r11). Set all three rather than depend on volatile registers
        // surviving a call -- same values, and it stays right if that callee is promoted.
        ctx.fpscr.disableFlushModeUnconditional();
        ctx.f1.f64 = focus;                                   // fmr f1,f29
        ctx.r6.u64 = row;
        skate3::audio::GuestCall(ctx, base, sub_82B45788, object, entry);  // mr r4,r30

        ctx.fpscr.disableFlushModeUnconditional();
        ctx.f3.f64 = LoadSingle(base, entry + kEntryLenSq);    // lfs f3,8(r30)
        ctx.f2.f64 = gain;                                    // fmr f2,f30
        ctx.f1.f64 = weight;                                  // fmr f1,f31
        ctx.r6.u64 = row;                                     // mr r6,r31
        skate3::audio::GuestCall(ctx, base, sub_82B45B60, object);         // mr r3,r28

        row += kRowBytes;                                     // addi r31,r31,32
        entry += kEntryBytes;                                 // addi r30,r30,16
      } while (--left != 0);                                  // addic. r29,r29,-1 ; bne
    }

    // loc_82B461B4: the count is RELOADED here, after every store the loop above made.
    const int32_t dest_now = static_cast<int32_t>(REX_LOAD_U32(object + kCount));  // lwz r11,56(r28)
    // cmpwi cr6,r11,6 ; blt cr6,0x82b46210
    if (dest_now >= 6) {
      const uint32_t column = (dest_now == 6) ? 5u : 7u;  // li r9,5 ; beq cr6 ; li r9,7

      // cmpwi cr6,r26,6 / cmpwi cr6,r26,8 -- the source count picks the row, or picks neither.
      if (sources == 6 || sources == 8) {
        const uint32_t which_row = (sources == 6) ? 5u : 7u;  // li r11,5 / li r11,7
        // loc_82B461D8. rlwinm r10,r11,5,0,26 and rlwinm r8,r9,2,0,29 are plain 32-bit shifts on
        // zero-extended words; `add rD,rS,r27` is a 64-bit add whose bit-32 carry only the u32
        // store addresses see, so 32-bit arithmetic is exact here.
        const uint32_t row_base = matrix + which_row * kRowBytes;  // add r7,r10,r27
        const uint32_t col_base = matrix + (column << 2);          // add r10,r8,r27
        ctx.fpscr.disableFlushModeUnconditional();
        const double zero = LoadSingle(base, kZeroSingle);         // lfs f0,23056(r11)
        uint32_t col = col_base - kRowBytes;                       // addi r10,r10,-32
        uint32_t cell = row_base - 4;                              // addi r9,r7,-4
        // loc_82B46200, eight trips: one column of every row, and one whole row, zeroed.
        for (uint32_t k = 0; k < kMaxRows; k++) {                  // mtctr r11(=8) ; bdnz
          ctx.fpscr.disableFlushModeUnconditional();
          col += kRowBytes;                                        // stfsu f0,32(r10)
          StoreSingle(base, col, zero);
          cell += 4;                                               // stfsu f0,4(r9)
          StoreSingle(base, cell, zero);
        }
        // stfsx f28,r8,r7 -- their crossing cell, overwritten with the f3 argument
        StoreSingle(base, row_base + (column << 2), fill);
      } else {
        // loc_82B46230: no row is cleared; the column is filled with the f3 argument instead.
        // addi r10,r9,-8 ; rlwinm r10,r10,2,0,29 ; add r10,r10,r27 == matrix + 4*column - 32 in
        // 32-bit arithmetic (column < 8, so the subtract wraps and the shift keeps the low word).
        uint32_t col = matrix + ((column - 8u) << 2);
        // loc_82B46244, eight trips
        for (uint32_t k = 0; k < kMaxRows; k++) {                  // mtctr r11(=8) ; bdnz
          ctx.fpscr.disableFlushModeUnconditional();
          col += kRowBytes;                                        // stfsu f28,32(r10)
          StoreSingle(base, col, fill);
        }
      }
    }
  }

  // loc_82B46210 / the two other epilogues: addi r1,r1,176 ; __restfpr_28 ; __restgprlr_26
  ctx.r1.u32 = entry_sp;
  ctx.r26 = saved_gpr[0];
  ctx.r27 = saved_gpr[1];
  ctx.r28 = saved_gpr[2];
  ctx.r29 = saved_gpr[3];
  ctx.r30 = saved_gpr[4];
  ctx.r31 = saved_gpr[5];
  ctx.f28 = saved_fpr[0];
  ctx.f29 = saved_fpr[1];
  ctx.f30 = saved_fpr[2];
  ctx.f31 = saved_fpr[3];
}

// Every byte written lands inside the 8x8 matrix at r10, on both paths:
//
//   destination == 2: columns 0 and 1 of rows 0 .. r5-1, so [matrix, matrix + 32*(r5-1) + 8).
//   otherwise:        rows 0 .. rows-1 through the three callees, each of which stores only
//                     inside [row, row+28) plus row + 4*index for the four index words -- inside
//                     the 32-byte row as long as every index is < 8 -- and then one column of all
//                     eight rows, one whole row, and their crossing cell. Declared as the whole
//                     256-byte matrix, which is a superset: the bytes no store reaches compare
//                     equal anyway.
//
// Not declared: this function's own 176-byte frame, sub_82B454B8's red-zone spills, and the frames
// of the sin/cos helpers sub_82B45788 reaches -- each is its own function's frame.
//
// Returns false when a row index would step outside the matrix, when one of the four gain indices
// is >= 8 (a callee would then store outside the row, where this window does not follow), or when
// the matrix overlaps those index words -- a callee reloads them after its own stores, so a store
// landing on them makes a later store address unknowable from entry state.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t entries = ctx.r4.u32;
  const int32_t sources = ctx.r5.s32;
  const uint32_t matrix = ctx.r10.u32;
  if (object == 0 || matrix == 0) return false;  // the original would store through null

  const int32_t dest = static_cast<int32_t>(REX_LOAD_U32(object + kCount));
  spec.read(object + kCount, 4);

  if (dest == 2) {
    if (sources <= 0) return true;  // the loop never runs: no stores, no reads past the count
    if (static_cast<uint32_t>(sources) > kMaxRows) return false;  // more rows than the matrix holds
    spec.write(matrix, static_cast<uint32_t>(sources - 1) * kRowBytes + 8);
    spec.read(entries, static_cast<uint32_t>(sources) * kEntryBytes);
    spec.read(kHalfSingle, 4);
    spec.read(kOneSingle, 4);
    return true;
  }

  const int32_t rows = (sources < 6) ? sources : sources - 1;
  if (rows > static_cast<int32_t>(kMaxRows)) return false;  // the row loop would leave the matrix
  for (uint32_t k = 0; k < 4; k++) {
    if (REX_LOAD_U32(object + kIndex172 + 4 * k) >= kMaxRows) return false;
  }
  if (!Disjoint(matrix, kMatrixBytes, object + kIndex172, kIndexWords)) return false;

  spec.write(matrix, kMatrixBytes);

  // Reads, recorded for replay only. The config head covers the speaker table sub_82B454B8 walks,
  // the count, the thresholds sub_82B45788 compares against and the four indices; the entries are
  // read 16 bytes at a time by all three callees. The gains the callees read back before storing
  // are inside the declared window, which is snapshotted anyway.
  spec.read(object, kConfigRead);
  if (rows > 0) spec.read(entries, static_cast<uint32_t>(rows) * kEntryBytes);
  spec.read(kZeroSingle, 4);
  spec.read(kOneSingle, 4);   // sub_82B454B8 / sub_82B45B60
  spec.read(kHalfSingle, 4);  // sub_82B454B8
  spec.read(0x821A0318, 4);   // 0.0005f, sub_82B454B8's snap threshold
  spec.read(0x822F8904, 4);   // sub_82B45788's angle scale
  spec.read(0x820B411C, 4);   // sub_82B45788's angle step
  return true;
}

}  // namespace port_82B460A0
SKATE3_PORT(82B460A0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
