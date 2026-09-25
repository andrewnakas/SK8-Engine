// sub_82B399D0  role: one filter stage over `count` singles -- mixed[k] = addend[k] - f2*source[k+1], then a one-pole recursion over mixed in place (pole f1, entry state f5), while the copy bus accumulates f4*(f3*source[k] + source[k+1]); returns the last filtered single in f1   lifted: skate3_recomp.68.cpp:3 (901 lines)
// STATUS: verified
// calls/session: boot 303420  play 359364  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B399D0.md
namespace port_82B399D0 {

// Arguments. Only r3, r9, r10, f1-f5 and three outgoing stack slots are live on entry; r4-r8 are
// passed by sub_82B39FA0 and never read (r8 is reloaded from 84(r1), which holds the same value).
//
//   r3   count      singles to filter -- 0x100 in every logged call
//   r9   addend     the first input run, read only
//   r10  source     the second input run, read at k AND k+1, so it holds count+1 singles
//   f1   pole       the one-pole coefficient; f1 is also the result register
//   f2   feed       scales source[k+1] out of addend[k]
//   f3   bus_coeff  scales source[k] into the bus product
//   f4   bus_gain   scales the bus product into the copy bus
//   f5   state      the recursion's y[-1], which sub_82B39FA0 keeps at its gain block + 32
//
// The three slots are sub_82B39FA0's outgoing argument area: this kernel has no stwu of its own,
// so positive offsets off r1 are still the caller's frame (see port_82B39FA0's kStackArg*).
constexpr uint32_t kArgMixed = 84;   // lwz r8,84(r1)   -- the in/out run, descriptor + 16
constexpr uint32_t kArgCopy = 92;    // lwz r26,92(r1)  -- the accumulate bus, descriptor + 20
constexpr uint32_t kArgClear = 100;  // lwz r11,100(r1) -- zero: establish the bus as zeroes first

// A 32-byte table of byte indices 0x00..0x1F that the kernel rebuilds in .data on every call and
// then indexes by (source & 0xC) to build two vperm128 controls -- the compiler's inline stand-in
// for lvsl. Address from the immediates, never read off by eye: ((imm & 0xFFFF) << 16) + offset.
constexpr uint32_t kPermTable = ((static_cast<uint32_t>(-31987) & 0xFFFFu) << 16) + 5412;  // 0x830D1524
constexpr uint32_t kPermTableBytes = 32;

constexpr uint32_t kGroups = 4;          // 16-byte groups per block: r10/r5/r29/r28 off source
constexpr uint32_t kBlockBytes = 64;     // one loop iteration, addi r7,r7,64 and friends
constexpr uint32_t kBlockSingles = 16;   // the scalar recursion's unrolled run length

// lvx128: sixteen bytes with the low four address bits masked off, plus the big-endian lane
// reversal RexGlue applies to every vector memory access. Raw bytes, for vperm128 operands.
inline simde__m128i LoadLine(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                               simde_mm_load_si128((simde__m128i*)VectorMaskL));
}

// The same load, handed back as float lanes. Same bits.
inline simde__m128 LoadVectorPs(uint8_t* base, uint32_t address) {
  return simde_mm_castsi128_ps(LoadLine(base, address));
}

// stvx128: the same masking and the same lane reversal on the way out.
inline void StoreVectorPs(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// dcbzl r11,r26: establishes the whole 128-byte line containing the address as zeroes. A real
// write to guest memory, so it is inside the declared window and reproduced here.
inline void ZeroCacheLine(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~127u;
  memset((void*)REX_RAW_ADDR(ea), 0, 128);
}

// lfs: the word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: the double rounded back to a single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fnmsubs f,fA,fB,fC = -(A*B - C): ONE fused product with a single rounding, then rounded to
// single. Written in the lifted shape -- not std::fma(-a, b, c), which differs on NaN and on the
// sign of a zero result.
inline double Fnmsubs(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(-std::fma(a, b, -c)));
}

// Four stw into the frame followed by one lvx128: stw writes big-endian and lvx128 reverses all
// sixteen bytes, so the word stored at the LOWEST address lands in host lane 3. That lane order
// is sub_82B3C098's verified precedent, so the control is built as a register and the native body
// never writes the guest stack. `offset` is the byte position into the 32-byte vperm128 pair.
inline simde__m128i PermControl(uint8_t* base, uint32_t offset) {
  const uint32_t w0 = REX_LOAD_U32(kPermTable + offset + 0);   // lwzx r11,r4,r11
  const uint32_t w1 = REX_LOAD_U32(kPermTable + offset + 4);   // lwzx r25,r4,r27
  const uint32_t w2 = REX_LOAD_U32(kPermTable + offset + 8);   // lwzx r7,r4,r7
  const uint32_t w3 = REX_LOAD_U32(kPermTable + offset + 12);  // lwzx r6,r4,r31
  return simde_mm_set_epi32(static_cast<int32_t>(w0), static_cast<int32_t>(w1),
                            static_cast<int32_t>(w2), static_cast<int32_t>(w3));
}

// vperm128: the sixteen bytes starting `offset` into the pair (lo, hi), as float lanes.
inline simde__m128 Window(simde__m128i lo, simde__m128i hi, simde__m128i control) {
  return simde_mm_castsi128_ps(rex::ppc::simde_mm_perm_epi8_(lo, hi, control));
}

// srawi r6,r3,2 ; addze r11,r6 : count/4 truncated toward zero. srawi's carry-out is the rounding
// correction and addze folds it back in, so a negative count divides toward zero, not down.
inline int32_t QuarterToZero(int32_t count) {
  const bool carry = (count < 0) && ((count & 3) != 0);
  return (count >> 2) + (carry ? 1 : 0);
}

inline uint64_t Lower(uint64_t a, uint64_t b) { return a < b ? a : b; }
inline uint64_t Upper(uint64_t a, uint64_t b) { return a > b ? a : b; }

REX_FUNC(Native) {
  const int32_t count = ctx.r3.s32;        // r3
  const uint32_t addend = ctx.r9.u32;      // r9
  const uint32_t source = ctx.r10.u32;     // r10
  const uint32_t sp = ctx.r1.u32;          // r1, unchanged: this function has no stwu
  const double pole = ctx.f1.f64;          // f1, live through both recursions

  // The three stfd of f29-f31 at -96..-112(r1) and the __savegprlr_22 spill at -8..-88(r1) are
  // this function's own frame: not windowed, and not reproduced. This body leaves f29-f31 and
  // r22-r31 untouched, so the epilogue's __restgprlr_22 and three lfd would be no-ops.
  ctx.fpscr.disableFlushModeUnconditional();                    // emitted at stfd f29,-112(r1)
  const uint32_t clear = REX_LOAD_U32(sp + kArgClear);          // lwz r11,100(r1)
  double state = ctx.f5.f64;                                    // fmr f0,f5
  const uint32_t copy = REX_LOAD_U32(sp + kArgCopy);            // lwz r26,92(r1)
  const uint32_t span = (static_cast<uint32_t>(count) << 2) & 0xFFFFFFFCu;  // rlwinm r22,r3,2,0,29
  const uint32_t mixed = REX_LOAD_U32(sp + kArgMixed);          // lwz r8,84(r1)

  // cmpwi cr6,r11,0 ; bne cr6 : a non-zero flag takes loc_82B39A2C, which is four dcbt and a
  // counted loop of four more -- prefetch hints only, no state, so it is elided. Zero means this
  // is the first tap into the bus, and loc_82B39A0C establishes it as zeroes a line at a time.
  if (clear == 0) {
    // cmplwi cr6,r22,0 ; beq cr6 guards the loop, which then steps r11 by 128 while r11 <u r22.
    for (uint32_t off = 0; off < span; off += 128) {
      ZeroCacheLine(base, copy + off);                          // dcbzl r11,r26
    }
  }

  // loc_82B39A5C. Three splats the original materialises through its own frame -- four copies of
  // each single at -176/-144/-128(r1), each reloaded with one lvx128. The four words are
  // identical, so the load's lane reversal is a no-op on them and they are built as registers.
  ctx.fpscr.disableFlushModeUnconditional();                    // emitted at stfs f2,-176(r1)
  const simde__m128 feed = simde_mm_set1_ps(static_cast<float>(ctx.f2.f64));       // v63
  const simde__m128 bus_coeff = simde_mm_set1_ps(static_cast<float>(ctx.f3.f64));  // v13
  const simde__m128 bus_gain = simde_mm_set1_ps(static_cast<float>(ctx.f4.f64));   // v12

  // Eight lis/ori pairs written into .data in the lifted order. Byte indices 0x00..0x1F.
  REX_STORE_U32(kPermTable + 0, 0x00010203u);   // lis r11,1    ; ori 515  ; stw r11,5412(r6)
  REX_STORE_U32(kPermTable + 4, 0x04050607u);   // lis r7,1029  ; ori 1543 ; stw r7,4(r11)
  REX_STORE_U32(kPermTable + 12, 0x0C0D0E0Fu);  // lis r7,3085  ; ori 3599 ; stw r7,12(r11)
  REX_STORE_U32(kPermTable + 16, 0x10111213u);  // lis r5,4113  ; ori 4627 ; stw r5,16(r11)
  REX_STORE_U32(kPermTable + 8, 0x08090A0Bu);   // lis r6,2057  ; ori 2571 ; stw r6,8(r11)
  REX_STORE_U32(kPermTable + 24, 0x18191A1Bu);  // lis r7,6169  ; ori 6683 ; stw r7,24(r11)
  REX_STORE_U32(kPermTable + 28, 0x1C1D1E1Fu);  // lis r5,7197  ; ori 7711 ; stw r5,28(r11)
  REX_STORE_U32(kPermTable + 20, 0x14151617u);  // lis r6,5141  ; ori 5655 ; stw r6,20(r11)

  // rlwinm r4,r10,0,28,29 : the source run is only 4-byte aligned (0x...388 and 0x...3C0 both
  // appear in the census), so every read of it is an unaligned window built from two lines.
  const uint32_t sel = source & 0xCu;
  const simde__m128i shift_hi = PermControl(base, sel + 4);  // v0: the window one single further
  const simde__m128i shift_lo = PermControl(base, sel);      // v7: the window at the group

  // ---- the first block: sixteen singles of mixed, from addend and the source windows ----
  simde__m128i line[kGroups + 1];
  line[0] = LoadLine(base, source + 0);   // lvx128 v59,r0,r10
  line[1] = LoadLine(base, source + 16);  // lvx128 v62,r0,r5
  line[2] = LoadLine(base, source + 32);  // lvx128 v61,r0,r29
  line[3] = LoadLine(base, source + 48);  // lvx128 v60,r0,r28
  line[4] = LoadLine(base, source + 64);  // lvx128 v58,r0,r27

  // win_hi[g] is source[16g + 4 .. 16g + 19], win_lo[g] is source[16g .. 16g + 15].
  simde__m128 win_hi[kGroups];
  simde__m128 win_lo[kGroups];
  win_hi[0] = Window(line[0], line[1], shift_hi);  // vperm128 v2,v59,v62,v0
  win_hi[1] = Window(line[1], line[2], shift_hi);  // vperm128 v1,v62,v61,v0
  win_hi[2] = Window(line[2], line[3], shift_hi);  // vperm128 v31,v61,v60,v0
  win_hi[3] = Window(line[3], line[4], shift_hi);  // vperm128 v30,v60,v58,v0
  win_lo[0] = Window(line[0], line[1], shift_lo);  // vperm128 v11,v59,v62,v7
  win_lo[1] = Window(line[1], line[2], shift_lo);  // vperm128 v10,v62,v61,v7
  win_lo[2] = Window(line[2], line[3], shift_lo);  // vperm128 v9,v61,v60,v7
  win_lo[3] = Window(line[3], line[4], shift_lo);  // vperm128 v8,v60,v58,v7

  simde__m128 first[kGroups];
  first[0] = LoadVectorPs(base, addend + 0);   // lvx128 v28,r0,r9
  first[1] = LoadVectorPs(base, addend + 16);  // lvx128 v27,r9,r30
  first[2] = LoadVectorPs(base, addend + 32);  // lvx128 v26,r9,r31
  first[3] = LoadVectorPs(base, addend + 48);  // lvx128 v25,r9,r4

  // vnmsubfp vD,vA,vB,vC = -(A*B) + C, a single-rounding negative multiply-add. The splat is the
  // FIRST operand of every one, as lifted: with two NaN operands the winning slot is part of the
  // semantics and cannot be derived from source (docs/vmx128-exactness.md rule 4).
  ctx.fpscr.enableFlushMode();                                            // at vnmsubfp v28
  simde__m128 mix[kGroups];
  mix[0] = simde_mm_fnmadd_ps(feed, win_hi[0], first[0]);  // vnmsubfp v28,v6,v2,v28
  mix[2] = simde_mm_fnmadd_ps(feed, win_hi[2], first[2]);  // vnmsubfp v3,v6,v31,v26
  mix[1] = simde_mm_fnmadd_ps(feed, win_hi[1], first[1]);  // vnmsubfp v27,v4,v1,v27
  mix[3] = simde_mm_fnmadd_ps(feed, win_hi[3], first[3]);  // vnmsubfp v6,v5,v30,v25

  StoreVectorPs(base, mixed + 0, mix[0]);   // stvx128 v28,r0,r8
  StoreVectorPs(base, mixed + 32, mix[2]);  // stvx128 v3,r8,r31
  StoreVectorPs(base, mixed + 16, mix[1]);  // stvx128 v27,r8,r30
  StoreVectorPs(base, mixed + 48, mix[3]);  // stvx128 v6,r8,r4

  // v25-v31 are in the ABI-preserved range the harness compares unconditionally, and this kernel
  // clobbers seven of them without saving any (__savegprlr_22 covers GPRs and lr only). What
  // survives is whatever the last block through the pipeline left, so it is tracked explicitly.
  simde__m128 keep25 = first[3];  // lvx128 v25,r9,r4
  simde__m128 keep26 = first[2];  // lvx128 v26,r9,r31
  simde__m128 keep27 = mix[1];    // vnmsubfp v27,v4,v1,v27
  simde__m128 keep28 = mix[0];    // vnmsubfp v28,v6,v2,v28
  simde__m128 keep29 = mix[0];    // v29 is untouched unless the loop runs; see the store below

  // srawi/addze/addi r3,r11,-4 : blocks of four groups left after the first, then
  // addi/rlwinm/addi/mtctr : ceil(blocks/4) iterations, each 64 bytes wide. `ble cr6` skips it.
  const int32_t blocks = QuarterToZero(count) - 4;
  const uint32_t iters =
      blocks > 0 ? ((static_cast<uint32_t>(blocks - 1) >> 2) + 1u) : 0u;

  // ---- loc_82B39C4C: one block per iteration, software-pipelined one behind the windows ----
  for (uint32_t i = 0; i < iters; i++) {
    // r11 runs four bytes behind the block it filters, so `4+4k(r11)` is this run + 4k.
    const uint32_t run = mixed + kBlockBytes * i;

    // The one-pole recursion over the sixteen singles the previous block wrote. y[k] = x[k] -
    // pole*y[k-1], in place, and the four bus products are interleaved into it exactly as lifted
    // -- the flush-mode toggles bracket each vector op, and a denormal single loaded on the wrong
    // side of one would flush.
    ctx.fpscr.disableFlushModeUnconditional();  // at lfs f13,4(r11)
    double x[kBlockSingles];
    double y[kBlockSingles];
    x[0] = LoadSingle(base, run + 0);
    ctx.fpscr.enableFlushMode();                // at vmaddfp v10,v10,v13,v1
    simde__m128 prod[kGroups];
    prod[1] = simde_mm_fmadd_ps(win_lo[1], bus_coeff, win_hi[1]);
    ctx.fpscr.disableFlushMode();               // at fnmsubs f12,f0,f1,f13
    y[0] = Fnmsubs(state, pole, x[0]);
    x[1] = LoadSingle(base, run + 4);           // lfs f11,8(r11)
    x[2] = LoadSingle(base, run + 8);           // lfs f10,12(r11)
    ctx.fpscr.enableFlushMode();                // at vmaddfp v6,v11,v13,v2
    prod[0] = simde_mm_fmadd_ps(win_lo[0], bus_coeff, win_hi[0]);
    ctx.fpscr.disableFlushMode();               // at lfs f9,16(r11)
    x[3] = LoadSingle(base, run + 12);
    ctx.fpscr.enableFlushMode();                // at vmaddfp v9,v9,v13,v31
    prod[2] = simde_mm_fmadd_ps(win_lo[2], bus_coeff, win_hi[2]);
    ctx.fpscr.disableFlushMode();               // at lfs f8,20(r11)
    x[4] = LoadSingle(base, run + 16);
    ctx.fpscr.enableFlushMode();                // at vmaddfp v8,v8,v13,v30
    prod[3] = simde_mm_fmadd_ps(win_lo[3], bus_coeff, win_hi[3]);
    ctx.fpscr.disableFlushMode();               // at lfs f7,24(r11)

    // lfs f7/f6/f5/f4/f3/f2 then f13/f11/f31/f30/f29. The last five are lifted after the first
    // two stores below; hoisted here because run+44..run+60 cannot alias run+0 or run+4, and
    // every one of the sixteen singles is read before any of the sixteen is written.
    for (uint32_t k = 5; k < kBlockSingles; k++) x[k] = LoadSingle(base, run + 4u * k);

    y[1] = Fnmsubs(y[0], pole, x[1]);           // fnmsubs f0,f12,f1,f11
    StoreSingle(base, run + 4, y[1]);           // stfs f0,8(r11)  -- the second single first
    StoreSingle(base, run + 0, y[0]);           // stfs f12,4(r11)
    for (uint32_t k = 2; k < kBlockSingles; k++) {
      y[k] = Fnmsubs(y[k - 1], pole, x[k]);     // fnmsubs f10,f0,f1,f10 ...
      StoreSingle(base, run + 4u * k, y[k]);    // stfs f10,12(r11) ...
    }

    // The next block. Thirteen lvx128, every one of them before this block's first store, which
    // is what makes an overlapping mixed/copy/addend/source behave as the original does.
    const uint32_t src = source + kBlockBytes + kBlockBytes * i;  // r6 - 16
    const uint32_t bus = copy + kBlockBytes * i;                  // r7 - 32
    const uint32_t add = addend + kBlockBytes + kBlockBytes * i;  // r4 - 32
    const uint32_t out = mixed + kBlockBytes + kBlockBytes * i;   // r31 - 48

    line[1] = LoadLine(base, src + 16);              // lvx128 v62,r0,r6
    line[2] = LoadLine(base, src + 32);              // lvx128 v61,r6,r29
    win_hi[1] = Window(line[1], line[2], shift_hi);  // vperm128 v1,v62,v61,v0
    simde__m128 acc[kGroups];
    acc[1] = LoadVectorPs(base, bus + 16);           // lvx128 v11,r7,r10
    ctx.fpscr.enableFlushMode();                     // at vmaddfp v5,v10,v12,v11
    simde__m128 sum[kGroups];
    sum[1] = simde_mm_fmadd_ps(prod[1], bus_gain, acc[1]);
    line[0] = LoadLine(base, src + 0);               // lvx128 v59,r6,r25
    win_lo[1] = Window(line[1], line[2], shift_lo);  // vperm128 v57,v62,v61,v7
    line[3] = LoadLine(base, src + 48);              // lvx128 v60,r6,r28
    line[4] = LoadLine(base, src + 64);              // lvx128 v58,r6,r27
    acc[3] = LoadVectorPs(base, bus + 48);           // lvx128 v10,r7,r30
    acc[0] = LoadVectorPs(base, bus + 0);            // lvx128 v11,r7,r9
    sum[0] = simde_mm_fmadd_ps(prod[0], bus_gain, acc[0]);  // vmaddfp v6,v6,v12,v11
    acc[2] = LoadVectorPs(base, bus + 32);           // lvx128 v11,r0,r7
    sum[2] = simde_mm_fmadd_ps(prod[2], bus_gain, acc[2]);  // vmaddfp v4,v9,v12,v11
    simde__m128 next[kGroups];
    next[0] = LoadVectorPs(base, add + 0);           // lvx128 v3,r6,r24
    win_hi[0] = Window(line[0], line[1], shift_hi);  // vperm128 v2,v59,v62,v0
    next[1] = LoadVectorPs(base, add + 16);          // lvx128 v29,r4,r10
    win_hi[2] = Window(line[2], line[3], shift_hi);  // vperm128 v31,v61,v60,v0
    next[2] = LoadVectorPs(base, add + 32);          // lvx128 v28,r0,r4
    next[3] = LoadVectorPs(base, add + 48);          // lvx128 v27,r4,r30

    StoreVectorPs(base, bus + 0, sum[0]);            // stvx128 v6,r7,r9
    win_hi[3] = Window(line[3], line[4], shift_hi);  // vperm128 v30,v60,v58,v0
    sum[3] = simde_mm_fmadd_ps(prod[3], bus_gain, acc[3]);  // vmaddfp v26,v8,v12,v10
    StoreVectorPs(base, bus + 16, sum[1]);           // stvx128 v5,r7,r10
    win_lo[0] = Window(line[0], line[1], shift_lo);  // vperm128 v59,v59,v62,v7
    StoreVectorPs(base, bus + 32, sum[2]);           // stvx128 v4,r0,r7
    mix[3] = simde_mm_fnmadd_ps(feed, win_hi[3], next[3]);  // vnmsubfp v27,v6,v30,v27
    mix[0] = simde_mm_fnmadd_ps(feed, win_hi[0], next[0]);  // vnmsubfp v25,v11,v2,v3
    win_lo[2] = Window(line[2], line[3], shift_lo);  // vperm128 v56,v61,v60,v7
    mix[2] = simde_mm_fnmadd_ps(feed, win_hi[2], next[2]);  // vnmsubfp v28,v9,v31,v28
    win_lo[3] = Window(line[3], line[4], shift_lo);  // vperm128 v55,v60,v58,v7
    StoreVectorPs(base, bus + 48, sum[3]);           // stvx128 v26,r7,r30
    mix[1] = simde_mm_fnmadd_ps(feed, win_hi[1], next[1]);  // vnmsubfp v6,v4,v1,v29

    StoreVectorPs(base, out + 48, mix[3]);  // stvx128 v27,r0,r31
    StoreVectorPs(base, out + 0, mix[0]);   // stvx128 v25,r6,r5
    StoreVectorPs(base, out + 32, mix[2]);  // stvx128 v28,r4,r23
    StoreVectorPs(base, out + 16, mix[1]);  // stvx128 v6,r31,r9

    keep25 = mix[0];
    keep26 = sum[3];
    keep27 = mix[3];
    keep28 = mix[2];
    keep29 = next[1];

    // lfsu f0,64(r11) : the state for the next block is RELOADED from the single just stored at
    // run+60, not carried in a register. Reproduced as a load because a misaligned mixed would
    // let the stvx128 to out+0 above overwrite the low bytes of run+60 first.
    ctx.fpscr.disableFlushMode();
    state = LoadSingle(base, run + 4u * (kBlockSingles - 1));
  }

  // ---- loc_82B39DE8: the last block, sixteen singles at mixed + 16*blocks ----
  ctx.fpscr.enableFlushModeUnconditional();  // at vmaddfp v6,v11,v13,v2
  simde__m128 prod[kGroups];
  prod[0] = simde_mm_fmadd_ps(win_lo[0], bus_coeff, win_hi[0]);  // vmaddfp v6,v11,v13,v2
  prod[2] = simde_mm_fmadd_ps(win_lo[2], bus_coeff, win_hi[2]);  // vmaddfp v11,v9,v13,v31
  prod[1] = simde_mm_fmadd_ps(win_lo[1], bus_coeff, win_hi[1]);  // vmaddfp v7,v10,v13,v1
  prod[3] = simde_mm_fmadd_ps(win_lo[3], bus_coeff, win_hi[3]);  // vmaddfp v9,v8,v13,v30

  // rlwinm r10,r3,4,0,27 ; add r10,r10,r8, and the same 16*blocks off copy for the bus. The
  // shifts are masked and the adds are 32-bit, so the offset wraps exactly as the guest's does.
  const uint32_t tail_off = (static_cast<uint32_t>(blocks) << 4) & 0xFFFFFFF0u;
  const uint32_t last_run = mixed + tail_off;   // add r10,r10,r8
  const uint32_t last_bus = copy + tail_off;    // add r11,r11,r26

  // Sixteen lfs/lfsx, sixteen fnmsubs, sixteen stfs/stfsx, all through r8-relative indices the
  // original recomputes with rlwinm per single. The loads are hoisted: the original interleaves
  // them with the stores, but no single is ever read after it is written in this block.
  ctx.fpscr.disableFlushMode();  // at lfs f13,0(r10)
  double x[kBlockSingles];
  for (uint32_t k = 0; k < kBlockSingles; k++) x[k] = LoadSingle(base, last_run + 4u * k);
  for (uint32_t k = 0; k < kBlockSingles; k++) {
    state = Fnmsubs(state, pole, x[k]);            // fnmsubs f12,f0,f1,f13 ...
    StoreSingle(base, last_run + 4u * k, state);   // stfs f12,0(r10) / stfsx f0,r9,r8 ...
  }

  // The bus block, in the lifted load/compute/store order: +0, then +32 and +48, then +16.
  simde__m128 acc0 = LoadVectorPs(base, last_bus + 0);   // lvx128 v0,r0,r11
  ctx.fpscr.enableFlushMode();                           // at vmaddfp v10,v6,v12,v0
  const simde__m128 out0 = simde_mm_fmadd_ps(prod[0], bus_gain, acc0);
  const simde__m128 acc3 = LoadVectorPs(base, last_bus + 48);  // lvx128 v0,r0,r7
  const simde__m128 acc2 = LoadVectorPs(base, last_bus + 32);  // lvx128 v13,r0,r9
  const simde__m128 out2 = simde_mm_fmadd_ps(prod[2], bus_gain, acc2);  // vmaddfp v11,v11,v12,v13
  const simde__m128 out3 = simde_mm_fmadd_ps(prod[3], bus_gain, acc3);  // vmaddfp v13,v9,v12,v0
  StoreVectorPs(base, last_bus + 0, out0);   // stvx128 v10,r0,r11
  StoreVectorPs(base, last_bus + 32, out2);  // stvx128 v11,r0,r9
  StoreVectorPs(base, last_bus + 48, out3);  // stvx128 v13,r0,r7
  const simde__m128 acc1 = LoadVectorPs(base, last_bus + 16);  // lvx128 v0,r0,r10
  const simde__m128 out1 = simde_mm_fmadd_ps(prod[1], bus_gain, acc1);  // vmaddfp v0,v7,v12,v0
  StoreVectorPs(base, last_bus + 16, out1);  // stvx128 v0,r0,r10

  // add r5,r22,r8 ; lfs f1,-4(r5) : the result is the LAST filtered single, read back out of
  // memory. sub_82B39FA0 latches it into its gain block + 32 as the next call's entry state, and
  // it is the only register the caller reads -- hence the kReturnF1 mask.
  ctx.fpscr.disableFlushMode();
  ctx.f1.f64 = LoadSingle(base, mixed + span - 4u);

  simde_mm_store_ps(ctx.v25.f32, keep25);
  simde_mm_store_ps(ctx.v26.f32, keep26);
  simde_mm_store_ps(ctx.v27.f32, keep27);
  simde_mm_store_ps(ctx.v28.f32, keep28);
  if (iters != 0) simde_mm_store_ps(ctx.v29.f32, keep29);  // lvx128 v29,r4,r10, loop only
  simde_mm_store_ps(ctx.v30.f32, win_hi[3]);               // vperm128 v30,v60,v58,v0
  simde_mm_store_ps(ctx.v31.f32, win_hi[2]);               // vperm128 v31,v61,v60,v0
}

// Every byte the call writes, from ENTRY state only. All of it is enumerable: the two buffers and
// the flag are the caller's outgoing stack arguments at 84/92/100(r1), readable before the call,
// and every cursor in the body is a fixed offset from one of them times a loop index derived from
// r3 alone. The 56 gate-2 suspects in the census are the loop cursors and inter-buffer deltas the
// body builds (r5 = mixed - source - 16 and the like), not independent bases.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t sp = ctx.r1.u32;
  const uint32_t mixed = REX_LOAD_U32(sp + kArgMixed);
  const uint32_t copy = REX_LOAD_U32(sp + kArgCopy);
  const uint32_t clear = REX_LOAD_U32(sp + kArgClear);
  const uint32_t addend = ctx.r9.u32;
  const uint32_t source = ctx.r10.u32;
  const int32_t count = ctx.r3.s32;
  const uint32_t span = (static_cast<uint32_t>(count) << 2) & 0xFFFFFFFCu;
  const int32_t blocks = QuarterToZero(count) - 4;
  const uint32_t iters =
      blocks > 0 ? ((static_cast<uint32_t>(blocks - 1) >> 2) + 1u) : 0u;
  const int64_t tail = static_cast<int64_t>(blocks) * 16;  // may be negative for count < 16

  // The eight-word table in .data, at a fixed address, written on every path.
  spec.write(kPermTable, kPermTableBytes);
  spec.read(kPermTable, kPermTableBytes);  // read straight back through lwzx for the controls
  spec.read(sp + kArgMixed, 20);           // 84..103(r1), the three outgoing arguments

  // mixed: four stvx128 at [(mixed & ~0xF), +64) for the first block, then per iteration sixteen
  // stfs at [mixed + 64i, +64) -- unmasked -- and four stvx128 at [(mixed & ~0xF) + 64(i+1), +64),
  // then sixteen stfs at [mixed + 16*blocks, +64). One span covers the union; with the 16-byte
  // aligned mixed and count = 256 the census logs, it is exactly [mixed, mixed + 1024).
  uint64_t lo = static_cast<uint64_t>(mixed) & ~0xFull;
  uint64_t hi = lo + kBlockBytes;
  if (iters != 0) {
    lo = Lower(lo, static_cast<uint64_t>(mixed));
    hi = Upper(hi, (static_cast<uint64_t>(mixed) & ~0xFull) +
                       kBlockBytes * (static_cast<uint64_t>(iters) + 1));
    hi = Upper(hi, static_cast<uint64_t>(mixed) + kBlockBytes * static_cast<uint64_t>(iters));
  }
  const int64_t tail_mixed = static_cast<int64_t>(mixed) + tail;
  if (tail_mixed < 0) return false;  // count < 16 walks the tail block off the bottom of the map
  lo = Lower(lo, static_cast<uint64_t>(tail_mixed));
  hi = Upper(hi, static_cast<uint64_t>(tail_mixed) + kBlockBytes);
  if (hi > 0x100000000ull) return false;
  spec.write(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo));
  spec.read(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi - lo));  // recursion reads it back

  // copy: four stvx128 per iteration at [(copy & ~0xF) + 64i, +64), four more for the tail block
  // at [(copy & ~0xF) + 16*blocks, +64), and -- when the flag is zero -- the dcbzl establish,
  // which starts at the 128-byte FLOOR of copy and covers ceil(count*4 / 128) whole lines.
  const int64_t tail_copy = static_cast<int64_t>(copy) + tail;
  if (tail_copy < 0) return false;
  uint64_t clo = static_cast<uint64_t>(tail_copy) & ~0xFull;
  uint64_t chi = clo + kBlockBytes;
  if (iters != 0) {
    clo = Lower(clo, static_cast<uint64_t>(copy) & ~0xFull);
    chi = Upper(chi, (static_cast<uint64_t>(copy) & ~0xFull) +
                         kBlockBytes * static_cast<uint64_t>(iters));
  }
  if (clear == 0 && span != 0) {
    const uint64_t lines = static_cast<uint64_t>((span - 1u) / 128u) + 1u;
    const uint64_t zlo = static_cast<uint64_t>(copy) & ~127ull;
    clo = Lower(clo, zlo);
    chi = Upper(chi, zlo + 128ull * lines);
  }
  if (chi > 0x100000000ull) return false;  // a huge span: the clear would wrap the address space
  spec.write(static_cast<uint32_t>(clo), static_cast<uint32_t>(chi - clo));
  spec.read(static_cast<uint32_t>(clo), static_cast<uint32_t>(chi - clo));  // accumulated into

  // Read, never written: four lvx128 of addend per block, and five lines of source per block --
  // one past the block, because every group is also read a single further along.
  const uint64_t alo = static_cast<uint64_t>(addend) & ~0xFull;
  const uint64_t ahi = alo + kBlockBytes * (static_cast<uint64_t>(iters) + 1);
  if (ahi <= 0x100000000ull) spec.read(static_cast<uint32_t>(alo), static_cast<uint32_t>(ahi - alo));
  const uint64_t slo = static_cast<uint64_t>(source) & ~0xFull;
  const uint64_t shi = slo + kBlockBytes * (static_cast<uint64_t>(iters) + 1) + 16;
  if (shi <= 0x100000000ull) spec.read(static_cast<uint32_t>(slo), static_cast<uint32_t>(shi - slo));
  return true;
}

}  // namespace port_82B399D0
SKATE3_PORT(82B399D0, skate3::audio::kPortVerified, skate3::audio::kReturnF1)
