// sub_82B427D8  role: linear-ramp writer -- fills a 256-entry block of a float array with a ramp from f1 to f2 spread over r7 samples, flat at f1 for indices below zero and flat at f2 past the end of the ramp; four vectors of four per iteration on the ramp body, a 16-byte-unit fill plus memcpy on the flat ends   lifted: skate3_recomp.68.cpp:22335 (742 lines)
// STATUS: verified
// calls/session: boot 7  play 7  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B427D8.md
namespace port_82B427D8 {

// Arguments:
//   r3  out     the float array, indexed from `first`
//   r6  first   the index the block starts at; it may be negative
//   r7  length  the ramp length in samples
//   f1  start   the value at index -1 and below
//   f2  end     the value at index length-1 and above
// r4 and r5 are f1's and f2's GPR parameter slots and are never read as integers -- except by
// `subfe r3,r4,r4`, which uses r4 only as a self-cancelling operand to turn the carry bit into a
// mask. Nothing in docs/rw_audio_structs.h names this buffer, so there are no offset constants.
//
// The block always ends at index `first + 255`; the ramp body ends at
// `last = min(first + 255, length - 1)`. For index i in [0, last] the value is
// `fma(float(i + 1), step, start)` with `step = (end - start) / float(length)`, so the ramp
// reaches `end` exactly at i = length-1. Indices below zero get `start`, indices above `last` get
// `end`.

// lis r11,-32234 ; lfs f0,23056(r11): 0x82160000 + 23056. Compared against the ramp delta to pick
// the ascending or the descending form. Loaded, not assumed.
constexpr uint32_t kRampThreshold = 0x82165A10;
// lis r8,-32206 ; addi r4,r8,-17776: 0x82320000 - 17776. The vector index stride (four lanes).
constexpr uint32_t kIndexStride = 0x8231BA90;

// lvx128: sixteen bytes with the low four address bits masked off, plus the big-endian lane
// reversal RexGlue applies to every vector memory access.
inline simde__m128 LoadVectorPs(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// stvx128: the same masking and the same lane reversal on the way out. The masking matters here:
// the output pointer is never checked for alignment.
inline void StoreVectorPs(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: the double rounded to single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fsubs / fdivs: the double result rounded to single.
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }

// fmadds: a single-rounding FMA, then rounded to single. std::fma, never `a * b + c`.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

// fnmsubs: -(a*b - c), one rounding inside. Exactly the lifted expression.
inline double NmsubSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(-std::fma(a, b, -c)));
}

// extsw ; std ; lfd ; fcfid ; frsp : an int32 through a stack slot into a single. The store and
// reload are the compiler's integer-to-float-register transfer, which is this function's own
// frame; only the conversion is observable.
inline float IntToFloat(int32_t v) {
  return static_cast<float>(static_cast<double>(static_cast<int64_t>(v)));
}
inline double IntToSingle(int32_t v) { return static_cast<double>(IntToFloat(v)); }

// srawi rX,rY,2 ; addze : the compiler's signed divide by four, truncating toward zero. The carry
// term is srawi's CA, reproduced because addze consumes it.
inline int32_t DivFour(int32_t v) {
  const bool carry = (v < 0) && ((static_cast<uint32_t>(v) & 3u) != 0);
  return (v >> 2) + (carry ? 1 : 0);
}

// The inline 16-byte-unit fill, emitted twice verbatim (loc_82B428B8 and loc_82B42C40): `units`
// blocks of 16 bytes from `dst`, four stvx128 at a time while whole groups of four remain, then
// one stvx128 plus a forward memcpy that replicates it over whatever is left. Writes nothing for
// units <= 0.
inline void FillVectorUnits(PPCContext& ctx, uint8_t* base, uint32_t dst, int32_t units,
                            simde__m128 value) {
  // srawi ; addze ; rlwinm rX,rY,2,0,29 : whole groups of four units.
  const int32_t bulk =
      static_cast<int32_t>((static_cast<uint32_t>(DivFour(units)) << 2) & 0xFFFFFFFCu);
  // cmpwi cr6,rX,0 ; ble
  if (bulk > 0) {
    uint32_t p = dst + 32;                                     // addi r11,r30,32
    uint32_t passes = ((static_cast<uint32_t>(bulk) - 1u) >> 2) + 1u;
    do {
      StoreVectorPs(base, p - 32, value);                      // stvx128 v63,r11,r8   (r8 = -32)
      StoreVectorPs(base, p - 16, value);                      // stvx128 v63,r11,r7   (r7 = -16)
      StoreVectorPs(base, p + 0, value);                       // stvx128 v63,r0,r11
      StoreVectorPs(base, p + 16, value);                      // stvx128 v63,r11,r6   (r6 = 16)
      p += 64;                                                 // addi r11,r11,64
    } while (--passes != 0);                                   // bdnz
  }
  // cmpw cr6,rX,rY ; bge
  if (bulk < units) {
    const uint32_t tail = dst + ((static_cast<uint32_t>(bulk) << 4) & 0xFFFFFFF0u);  // add r4,r11,r30
    const uint32_t rest = (static_cast<uint32_t>(units - bulk) << 4) & 0xFFFFFFF0u;  // rlwinm r11
    StoreVectorPs(base, tail, value);                          // stvx128 v63,r0,r4
    // addi r9,r11,-9 ; rlwinm r5,r9,0,0,28 : rest - 16, so the copy replicates the 16 bytes just
    // stored over the remaining units. sub_82F52FB8 is a forward byte/word memcpy(r3, r4, r5).
    const uint32_t len = (rest - 9u) & 0xFFFFFFF8u;
    skate3::audio::GuestCall(ctx, base, sub_82F52FB8, tail + 16, tail, len);   // bl 0x82f52fb8
  }
}

REX_FUNC(Native) {
  const uint32_t out = ctx.r3.u32;                             // mr r30,r3
  const int32_t first = ctx.r6.s32;                            // mr r31,r6
  const int32_t length = ctx.r7.s32;
  const double start = ctx.f1.f64;                             // fmr f30,f1
  const double end = ctx.f2.f64;                               // fmr f27,f2
  // stwu r1,-240(r1) and everything at 80(r1)..140(r1) is this function's own frame: the integer
  // transfer slot, the four-lane splat scratch and the __savegprlr_26/__savefpr_27 spills. None of
  // it is reproduced and none of it is windowed.

  // addi r26,r6,255 ; addi r11,r7,-1 ; cmpw cr6,r26,r11 ; bgt ; mr r27,r26
  const int32_t block_last = static_cast<int32_t>(static_cast<uint32_t>(first) + 255u);
  const int32_t ramp_last = static_cast<int32_t>(static_cast<uint32_t>(length) - 1u);
  const int32_t last = (block_last > ramp_last) ? ramp_last : block_last;

  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by fmr f30,f1
  const double delta = SubSingle(end, start);                  // fsubs f28,f2,f1
  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by lfd f0,80(r1)
  const double length_f = IntToSingle(length);                 // fcfid f13,f0 ; frsp f29,f13
  const double step = DivSingle(delta, length_f);              // fdivs f31,f28,f29

  int32_t index = first;                                       // r31
  uint32_t cursor = out;                                       // r30
  int32_t pad = 0;                                             // r28

  // cmpwi cr6,r31,0 ; bge cr6,0x82b429e4 : the sub-zero span, flat at `start`.
  if (index < 0) {
    // neg r11,r31 ; srawi ; addze ; rlwinm r7,r8,2,0,29 ; subf r6,r7,r11 : (-index) mod 4.
    const int32_t below = static_cast<int32_t>(0u - static_cast<uint32_t>(index));  // neg r11,r31
    const int32_t below_aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(below)) << 2) & 0xFFFFFFFCu);
    const int32_t leftover = below - below_aligned;
    // subfic r11,r6,4 ; addi r10,r11,-4 ; subfic r5,r10,0 ; subfe r3,r4,r4 ; and r28,r3,r11 :
    // the carry of `0 - (-leftover)` is 1 only when leftover is zero, and subfe turns that into an
    // all-ones or all-zero mask over 4 - leftover. So pad = (4 - leftover) % 4.
    pad = (leftover == 0) ? 0 : (4 - leftover);

    // stfs f30,96(r1) .. stfs f30,108(r1) ; lvx128 v63,r0,r10 : `start` in all four lanes. The
    // four words are identical so the load's lane reversal is a no-op on them, and the scratch is
    // this function's own frame -- the native body keeps it in a register. (That assumes the guest
    // r1 is 16-byte aligned, which the lvx128 address masking relies on in the lifted form too.)
    const simde__m128 start_fill = simde_mm_set1_ps(static_cast<float>(start));

    // srawi r10,r31,2 ; addze r9,r10 ; neg r29,r9 : the 16-byte units to fill.
    const int32_t units = -DivFour(index);                     // r29
    FillVectorUnits(ctx, base, cursor, units, start_fill);

    // loc_82B428FC: rlwinm r11,r29,2,0,29 ; add. r31,r11,r31 ; rlwinm r10,r29,4,0,27 ;
    //               add r30,r10,r30
    index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                 ((static_cast<uint32_t>(units) << 2) & 0xFFFFFFFCu));
    cursor += (static_cast<uint32_t>(units) << 4) & 0xFFFFFFF0u;
    // bge 0x82b4293c : fewer than four floats left below zero, done one plus a replicating copy.
    if (index < 0) {
      ctx.fpscr.disableFlushModeUnconditional();               // emitted by stfs f30,0(r30)
      StoreSingle(base, cursor, start);                        // stfs f30,0(r30)
      const int32_t left = -index;                             // neg r29,r31
      // rlwinm r11,r29,2,0,29 ; addi r11,r11,-1 ; rlwinm r5,r11,0,0,29 : 4*left - 4 bytes.
      const uint32_t len =
          (((static_cast<uint32_t>(left) << 2) & 0xFFFFFFFCu) - 1u) & 0xFFFFFFFCu;
      skate3::audio::GuestCall(ctx, base, sub_82F52FB8, cursor + 4, cursor, len);  // bl 0x82f52fb8
      index = static_cast<int32_t>(static_cast<uint32_t>(index) + static_cast<uint32_t>(left));
      cursor += (static_cast<uint32_t>(left) << 2) & 0xFFFFFFFCu;
    }

    // loc_82B4293C: the alignment padding, `pad` ramp samples written one at a time so that the
    // vector body below starts on a four-sample boundary. Only reachable from here, which is why
    // r28 is never read when `first` is non-negative.
    ctx.fpscr.disableFlushModeUnconditional();                 // before lfs f0,23056(r11)
    const double threshold = LoadSingle(base, kRampThreshold);  // lfs f0,23056(r11)
    // fcmpu cr6,f28,f0 ; blt cr6,0x82b42998. An unordered compare leaves lt clear, so a NaN delta
    // takes the ascending form -- as `!(delta < threshold)` does.
    if (!(delta < threshold)) {
      // cmpw cr6,r31,r27 ; bgt cr6,0x82b429e4 ; addi r11,r31,1
      if (index <= last) {
        int32_t n = static_cast<int32_t>(static_cast<uint32_t>(index) + 1u);
        while (pad > 0) {                                      // cmpwi cr6,r28,0 ; ble
          ctx.fpscr.disableFlushModeUnconditional();           // emitted by lfd f0,80(r1)
          const double value = FmaddSingle(IntToSingle(n), step, start);  // fmadds f11,f12,f31,f30
          index += 1;                                          // addi r31,r31,1
          n += 1;                                              // addi r11,r11,1
          pad -= 1;                                            // addi r28,r28,-1
          StoreSingle(base, cursor, value);                    // stfs f11,0(r30)
          cursor += 4;                                         // addi r30,r30,4
          if (index > last) break;                             // ble cr6,0x82b42958
        }
      }
    } else {
      // loc_82B42998: the descending form walks in from the `end` side instead.
      if (index <= last) {
        int32_t n = static_cast<int32_t>(static_cast<uint32_t>(index) + 1u);
        while (pad > 0) {                                      // cmpwi cr6,r28,0 ; ble
          ctx.fpscr.disableFlushModeUnconditional();           // emitted by lfd f0,80(r1)
          const double back = SubSingle(length_f, IntToSingle(n));    // fsubs f11,f29,f12
          const double value = NmsubSingle(back, step, end);   // fnmsubs f10,f11,f31,f27
          index += 1;                                          // addi r31,r31,1
          n += 1;                                              // addi r11,r11,1
          pad -= 1;                                            // addi r28,r28,-1
          StoreSingle(base, cursor, value);                    // stfs f10,0(r30)
          cursor += 4;                                         // addi r30,r30,4
          if (index > last) break;                             // ble cr6,0x82b429a4
        }
      }
    }
  }

  // loc_82B429E4: the ramp body, four samples per vector.
  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by stfs f31,112(r1)
  // stfs f31,112(r1) .. 124(r1) ; lvx128 v13,r0,r7 : the step in all four lanes.
  const simde__m128 step_v = simde_mm_set1_ps(static_cast<float>(step));
  // stfs f30,128(r1) .. 140(r1) ; lvx128 v12,r0,r6 : `start` in all four lanes.
  const simde__m128 start_v = simde_mm_set1_ps(static_cast<float>(start));
  // stfs of float(index+1) at 96(r1), +2 at 100, +3 at 104, +4 at 108, read back as one vector.
  // The lane reversal puts the word at 108 in lane 0 and the word at 96 in lane 3, which is what
  // setr_ps spells out here.
  // The four `addi rN,r31,k ; extsw` pairs are 64-bit adds in the lifted form, so the index
  // arithmetic is done unsigned here rather than risking signed overflow on an absurd entry.
  const uint32_t i0 = static_cast<uint32_t>(index);
  simde__m128 idx = simde_mm_setr_ps(IntToFloat(static_cast<int32_t>(i0 + 4u)),
                                     IntToFloat(static_cast<int32_t>(i0 + 3u)),
                                     IntToFloat(static_cast<int32_t>(i0 + 2u)),
                                     IntToFloat(static_cast<int32_t>(i0 + 1u)));
  const simde__m128 stride_v = LoadVectorPs(base, kIndexStride);  // lvx128 v63,r0,r4

  // subf r11,r31,r27 ; addi r4,r11,1 ; srawi r11,r4,2 ; addze r9,r11 : groups of four samples.
  const int32_t span = static_cast<int32_t>(static_cast<uint32_t>(last) -
                                            static_cast<uint32_t>(index) + 1u);
  const int32_t groups = DivFour(span);                        // r9
  // srawi r10,r9,2 ; addze r5,r10 ; rlwinm r10,r5,2,0,29 : groups taken four at a time.
  const int32_t bulk =
      static_cast<int32_t>((static_cast<uint32_t>(DivFour(groups)) << 2) & 0xFFFFFFFCu);

  // cmpwi cr6,r10,0 ; ble cr6,0x82b42b24 (which only reloads v0, already in hand here).
  if (bulk > 0) {
    uint32_t passes = ((static_cast<uint32_t>(bulk) - 1u) >> 2) + 1u;
    do {
      // The step is the FIRST operand of every vmaddfp128 and the running index the second, as
      // lifted: with two NaN operands the winning slot is part of the semantics
      // (vmx128-exactness rule 4), so the order is copied rather than rearranged. The four
      // `vor128 v58,v12,v12` copies are just the FMA's addend register being set up.
      ctx.fpscr.enableFlushModeUnconditional();                // before the first vaddfp128
      const simde__m128 idx1 = simde_mm_add_ps(idx, stride_v);  // vaddfp128 v62,v0,v63
      const simde__m128 v0 = simde_mm_fmadd_ps(step_v, idx, start_v);   // vmaddfp128 v58,v13,v0,v58
      const simde__m128 idx2 = simde_mm_add_ps(idx1, stride_v);  // vaddfp128 v60,v62,v63
      const simde__m128 v1 = simde_mm_fmadd_ps(step_v, idx1, start_v);  // vmaddfp128 v55,v13,v62,v55
      StoreVectorPs(base, cursor + 0, v0);                     // stvx128 v58,r0,r30
      const simde__m128 idx3 = simde_mm_add_ps(idx2, stride_v);  // vaddfp128 v57,v60,v63
      const simde__m128 v2 = simde_mm_fmadd_ps(step_v, idx2, start_v);  // vmaddfp128 v53,v13,v60,v53
      StoreVectorPs(base, cursor + 16, v1);                    // stvx128 v55,r0,r11
      const simde__m128 v3 = simde_mm_fmadd_ps(step_v, idx3, start_v);  // vmaddfp128 v52,v13,v57,v52
      StoreVectorPs(base, cursor + 32, v2);                    // stvx128 v53,r0,r11
      idx = simde_mm_add_ps(idx3, stride_v);                   // vaddfp128 v0,v57,v63
      StoreVectorPs(base, cursor + 48, v3);                    // stvx128 v52,r0,r11
      cursor += 64;                                            // addi r30,r11,16
    } while (--passes != 0);                                   // bdnz 0x82b42acc
  }

  // loc_82B42B2C: cmpw cr6,r10,r9 ; bge cr6,0x82b42b50 -- the odd groups, one vector each.
  if (bulk < groups) {
    uint32_t passes = static_cast<uint32_t>(groups - bulk);    // subf r11,r10,r9 ; mtctr r11
    do {
      ctx.fpscr.enableFlushModeUnconditional();                // before vmaddfp
      const simde__m128 v = simde_mm_fmadd_ps(step_v, idx, start_v);  // vmaddfp v11,v13,v0,v12
      idx = simde_mm_add_ps(idx, stride_v);                    // vaddfp128 v0,v0,v63
      StoreVectorPs(base, cursor, v);                          // stvx128 v11,r0,r30
      cursor += 16;                                            // addi r30,r30,16
    } while (--passes != 0);                                   // bdnz 0x82b42b3c
  }

  // loc_82B42B50: rlwinm r11,r9,2,0,29 ; add r11,r11,r31 -- the samples the vectors did not cover.
  int32_t tail_index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                            ((static_cast<uint32_t>(groups) << 2) & 0xFFFFFFFCu));
  // cmpw cr6,r11,r27 ; bgt cr6,0x82b42b9c
  if (tail_index <= last) {
    // subf r9,r11,r27 ; addi r10,r11,1 ; addi r9,r9,1 ; add r11,r9,r11 ; mtctr r9
    uint32_t passes = static_cast<uint32_t>(last - tail_index) + 1u;
    int32_t n = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + 1u);
    tail_index = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + passes);
    do {
      ctx.fpscr.disableFlushModeUnconditional();               // emitted by lfd f0,96(r1)
      const double value = FmaddSingle(IntToSingle(n), step, start);  // fmadds f11,f12,f31,f30
      n += 1;                                                  // addi r10,r10,1
      StoreSingle(base, cursor, value);                        // stfs f11,0(r30)
      cursor += 4;                                             // addi r30,r30,4
    } while (--passes != 0);                                   // bdnz 0x82b42b74
  }

  // loc_82B42B9C: the rest of the 256-entry block, flat at `end`.
  // cmpw cr6,r11,r26 ; bgt cr6,0x82b42c84
  if (tail_index <= block_last) {
    // subf r10,r11,r26 ; addi r10,r10,1 : entries left in the block.
    int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(block_last) -
                                             static_cast<uint32_t>(tail_index) + 1u);
    // srawi r9,r10,2 ; addze r5,r9 : the 16-byte unit count, captured BEFORE the loop below trims
    // `remaining` -- r5 survives that loop untouched, which is what makes the two add up.
    const int32_t units = DivFour(remaining);                  // r5
    // srawi r8,r10,2 ; addze r7,r8 ; rlwinm r6,r7,2,0,29 ; subf. r4,r6,r10 ; beq 0x82b42bf4
    int32_t aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(remaining)) << 2) & 0xFFFFFFFCu);
    if (remaining != aligned) {
      // loc_82B42BC8: write single entries until the count left is a multiple of four.
      while (tail_index <= block_last) {                       // cmpw cr6,r11,r26 ; bgt
        remaining -= 1;                                        // addi r10,r10,-1
        ctx.fpscr.disableFlushModeUnconditional();             // emitted by stfs f27,0(r30)
        StoreSingle(base, cursor, end);                        // stfs f27,0(r30)
        cursor += 4;                                           // addi r30,r30,4
        tail_index += 1;                                       // addi r11,r11,1
        aligned = static_cast<int32_t>(
            (static_cast<uint32_t>(DivFour(remaining)) << 2) & 0xFFFFFFFCu);
        if (remaining == aligned) break;                       // subf. r6,r7,r10 ; bne
      }
    }
    // loc_82B42BF4: stfs f27,128(r1)/136/132/140 ; lvx128 v63,r0,r10 : `end` in all four lanes.
    ctx.fpscr.disableFlushModeUnconditional();                 // emitted by stfs f27,128(r1)
    const simde__m128 end_fill = simde_mm_set1_ps(static_cast<float>(end));
    FillVectorUnits(ctx, base, cursor, units, end_fill);
  }

  // loc_82B42C84: li r3,1 -- the function's only result.
  ctx.r3.s64 = 1;
}

// The call writes one contiguous run of the output array and nothing else outside its own frame.
// Its length is not 1024 bytes in general: `first` below zero adds a flat prefix of -first
// samples, and a `length` that has already run out makes the trailing flat fill start below the
// index the vector body left off at. So the byte count is computed by replaying the same integer
// control flow the body does -- all of it decided by the entry registers, none of it by memory.
// The 16-byte stores go through stvx128's address masking and the output pointer is never checked
// for alignment, so the run starts at `out` rounded DOWN to 16 bytes.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t out = ctx.r3.u32;
  const int32_t first = ctx.r6.s32;
  const int32_t length = ctx.r7.s32;

  spec.read(kRampThreshold, 4);   // lfs f0,23056(r11)
  spec.read(kIndexStride, 16);    // lvx128 v63,r0,r4

  const int32_t block_last = static_cast<int32_t>(static_cast<uint32_t>(first) + 255u);
  const int32_t ramp_last = static_cast<int32_t>(static_cast<uint32_t>(length) - 1u);
  const int32_t last = (block_last > ramp_last) ? ramp_last : block_last;

  int32_t index = first;
  int64_t bytes = 0;  // bytes written from `out` upward
  int32_t pad = 0;

  if (index < 0) {
    const int32_t below = static_cast<int32_t>(0u - static_cast<uint32_t>(index));  // neg r11,r31
    const int32_t below_aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(below)) << 2) & 0xFFFFFFFCu);
    const int32_t leftover = below - below_aligned;
    pad = (leftover == 0) ? 0 : (4 - leftover);

    // FillVectorUnits writes 16 bytes per unit, or nothing at all for units <= 0.
    const int32_t units = -DivFour(index);
    if (units > 0) bytes += 16ll * units;
    index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                 ((static_cast<uint32_t>(units) << 2) & 0xFFFFFFFCu));
    if (index < 0) {
      const int32_t left = -index;  // one stfs plus a replicating copy: 4 bytes per sample
      bytes += 4ll * left;
      index = 0;
    }
    // The padding loop runs while pad remains and the index is still inside the ramp, so exactly
    // min(pad, last - index + 1) samples, and only when it is entered at all. Both the ascending
    // and the descending form write the same count.
    if (index <= last && pad > 0) {
      const int32_t room = last - index + 1;
      const int32_t n = (room < pad) ? room : pad;
      bytes += 4ll * n;
      index += n;
    }
  }

  // The vector body. A negative `span` leaves both loops unentered, and the index still moves by
  // 4*groups -- which is how the trailing fill can end up longer than the block.
  const int32_t span = static_cast<int32_t>(static_cast<uint32_t>(last) -
                                            static_cast<uint32_t>(index) + 1u);
  const int32_t groups = DivFour(span);
  const int32_t bulk =
      static_cast<int32_t>((static_cast<uint32_t>(DivFour(groups)) << 2) & 0xFFFFFFFCu);
  if (bulk > 0) bytes += 16ll * bulk;               // four vectors per pass
  if (bulk < groups) bytes += 16ll * (groups - bulk);  // one vector per pass
  int32_t tail_index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                            ((static_cast<uint32_t>(groups) << 2) & 0xFFFFFFFCu));

  if (tail_index <= last) {  // the scalar ramp tail
    bytes += 4ll * (static_cast<int64_t>(last - tail_index) + 1);
    tail_index = static_cast<int32_t>(static_cast<uint32_t>(last) + 1u);
  }

  if (tail_index <= block_last) {  // the flat fill to the end of the block
    const int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(block_last) -
                                                   static_cast<uint32_t>(tail_index) + 1u);
    const int32_t units = DivFour(remaining);
    const int32_t aligned =
        static_cast<int32_t>((static_cast<uint32_t>(units) << 2) & 0xFFFFFFFCu);
    if (remaining != aligned) bytes += 4ll * (remaining - aligned);  // the one-at-a-time prefix
    if (units > 0) bytes += 16ll * units;                            // FillVectorUnits
  }

  if (bytes < 0) return false;  // unreachable for any entry state, but never declare a bad span
  const uint32_t low = out & ~0xFu;  // every stvx128 masks its address down to 16 bytes
  const int64_t total = bytes + static_cast<int64_t>(out - low);
  if (total > 0xFFFFFFFFll) return false;
  if (static_cast<uint64_t>(low) + static_cast<uint64_t>(total) > 0xFFFFFFFFull) return false;
  spec.write(low, static_cast<uint32_t>(total));
  return true;
}

}  // namespace port_82B427D8
SKATE3_PORT(82B427D8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
