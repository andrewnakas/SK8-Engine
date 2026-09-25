// sub_82B42C98  role: square-root-curve ramp writer -- fills a 256-entry block of a float array with start + (end-start)*sqrt(i+1)/sqrt(length), flat at start below index zero and flat at end past the end of the ramp; four vectors of four per pass, each square root from vrsqrtefp128 plus one Newton step and a NaN-mask fixup, a 16-byte-unit fill plus memcpy on the flat ends   lifted: skate3_recomp.68.cpp:23078 (1006 lines)
// STATUS: verified
// calls/session: boot 62842  play 47931  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B42C98.md
namespace port_82B42C98 {

// Arguments -- the same five as the sibling generator sub_82B427D8, which the shared caller
// sub_82B238A8 dispatches to on state[0x72]:
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
// `last = min(first + 255, length - 1)`. With `step = (end - start) / sqrt(float(length))`, index i
// in [0, last] gets `fma(sqrt(i + 1), step, start)` -- reaching `end` exactly at i = length-1 --
// or, when the ramp descends, `end - sqrt(length - (i + 1)) * step`. Indices below zero get
// `start`, indices above `last` get `end`.
//
// EXCEPT in the alignment-padding loop, which is linear: it computes `fma(float(n), step, start)`
// with no fsqrts at all (lifted 23882 and 23928, against the square roots at 24413 and 24460).
// Reproduced as found; see the note.

// lis r10,-32234 ; lfs f26,23056(r10): 0x82160000 + 23056. Compared against the ramp delta to pick
// the ascending or the descending form. Loaded, not assumed. Same word the sibling loads.
constexpr uint32_t kRampThreshold = 0x82165A10;
// lis r10,-32206 ; addi r8,r10,-17776: 0x82320000 - 17776. The vector index stride (four lanes).
constexpr uint32_t kIndexStride = 0x8231BA90;
// lis r6,-32233 ; lfs f0,-8480(r6): 0x82170000 - 8480. The descending form's coefficient sign;
// `fnmsubs` in the scalar tail implies -1.0f, but it is loaded, never assumed.
constexpr uint32_t kDescendingScale = 0x8216DEE0;

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

// fsubs / fdivs / fmuls / fsqrts: the double result rounded to single. Each stays one operation.
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double SqrtSingle(double a) { return static_cast<double>(static_cast<float>(sqrt(a))); }

// fmadds: a single-rounding FMA, then rounded to single. std::fma, never `a * b + c`.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

// fnmsubs: -(a*b - c), one rounding inside. Exactly the lifted expression.
inline double NmsubSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(-std::fma(a, b, -c)));
}

// extsw ; std ; lfd ; fcfid : an int32 through a stack slot into a double. The store and reload are
// the compiler's integer-to-float-register transfer, which is this function's own frame; only the
// conversion is observable. The scalar ramp tail takes the square root of THIS, with no frsp.
inline double IntToDouble(int32_t v) { return static_cast<double>(static_cast<int64_t>(v)); }

// ...and the same with a following frsp, which is what every other site uses.
inline float IntToFloat(int32_t v) { return static_cast<float>(IntToDouble(v)); }
inline double IntToSingle(int32_t v) { return static_cast<double>(IntToFloat(v)); }

// srawi rX,rY,2 ; addze : the compiler's signed divide by four, truncating toward zero. The carry
// term is srawi's CA, reproduced because addze consumes it.
inline int32_t DivFour(int32_t v) {
  const bool carry = (v < 0) && ((static_cast<uint32_t>(v) & 3u) != 0);
  return (v >> 2) + (carry ? 1 : 0);
}

// vspltisw128 v61,1 ; vcsxwfp128 v0,v61,1 : the signed word 1 converted to float and scaled by
// 2^-1, i.e. 0.5 in all four lanes. Kept as the lifted expression rather than folded to a literal.
inline simde__m128 HalfVector() {
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(int(0x1))),
                         simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))));
}

// The square root, emitted five times with identical shape -- four software-pipelined groups in the
// main loop (lifted 23679..23815) and once in the residual loop (23831..23857):
//
//   e    = vrsqrtefp128(x)                the table-driven reciprocal-square-root estimate
//   c    = vnmsubfp(x*half, e*e, half)    one Newton step's correction term
//   r    = vmaddfp(e, c, e)               the refined estimate
//   mask = vcmpeqfp(c,c) XOR vcmpeqfp(e,e)
//   out  = vsel(x*r, x, mask)             mask ? x : x*r
//
// The mask is set exactly in the lanes where one of e and c is a NaN and the other is not, which is
// x == +0 (e = +inf, c = 0.5 - NaN) and x == +inf (e = +0, c = NaN): the two inputs the estimate
// cannot refine, and the two that are their own square root, so selecting x is the fixup. Nothing
// is algebraically simplified -- every vmulfp128 stays a separate multiply and both FMAs stay
// single-rounding -- and the operand order of every one is the lifted order, per rule 4 of
// docs/vmx128-exactness.md.
struct SqrtParts {
  simde__m128 value;    // vsel vD,x*r,x,mask
  simde__m128 refined;  // vmaddfp vD,e,c,e
  simde__m128 mask;     // vxor128 vD,vcmpeqfp(c,c),vcmpeqfp(e,e)
};

inline SqrtParts SqrtVector(simde__m128 x, simde__m128 half) {
  const simde__m128 e = rex::ppc::simde_mm_vrsqrtefp_ps(x);        // vrsqrtefp128 v13,v63
  const simde__m128 scaled = simde_mm_mul_ps(x, half);             // vmulfp128 v10,v63,v0
  const simde__m128 square = simde_mm_mul_ps(e, e);                // vmulfp128 v7,v13,v13
  const simde__m128 mask_e = simde_mm_cmpeq_ps(e, e);              // vcmpeqfp128 v58,v13,v13
  const simde__m128 c = simde_mm_fnmadd_ps(scaled, square, half);  // vnmsubfp v1,v10,v7,v0
  const simde__m128 refined = simde_mm_fmadd_ps(e, c, e);          // vmaddfp v31,v13,v1,v13
  const simde__m128 mask_c = simde_mm_cmpeq_ps(c, c);              // vcmpeqfp128 v55,v1,v1
  const simde__m128 product = simde_mm_mul_ps(x, refined);         // vmulfp128 v12,v63,v31
  const simde__m128i mask = simde_mm_xor_si128(simde_mm_castps_si128(mask_c),
                                               simde_mm_castps_si128(mask_e));  // vxor128 v31,v55,v58
  // vsel vD,vA,vB,vC == (~vC & vA) | (vC & vB): the set lanes take x, the clear ones take x*r.
  const simde__m128 value = simde_mm_castsi128_ps(
      simde_mm_or_si128(simde_mm_andnot_si128(mask, simde_mm_castps_si128(product)),
                        simde_mm_and_si128(mask, simde_mm_castps_si128(x))));  // vsel v2,v12,v4,v31
  return {value, refined, simde_mm_castsi128_ps(mask)};
}

// The inline 16-byte-unit fill, emitted twice verbatim (loc_82B42D84 and loc_82B432E8): `units`
// blocks of 16 bytes from `dst`, four stvx128 at a time while whole groups of four remain, then one
// stvx128 plus a forward memcpy that replicates it over whatever is left. Writes nothing for
// units <= 0. Byte for byte the same shape as the sibling's.
inline void FillVectorUnits(PPCContext& ctx, uint8_t* base, uint32_t dst, int32_t units,
                            simde__m128 value) {
  // srawi ; addze ; rlwinm rX,rY,2,0,29 : whole groups of four units.
  const int32_t bulk =
      static_cast<int32_t>((static_cast<uint32_t>(DivFour(units)) << 2) & 0xFFFFFFFCu);
  // cmpwi cr6,rX,0 ; ble
  if (bulk > 0) {
    uint32_t p = dst + 32;                                     // addi r11,r31,32
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
    const uint32_t tail = dst + ((static_cast<uint32_t>(bulk) << 4) & 0xFFFFFFF0u);  // add r4,r11,r31
    const uint32_t rest = (static_cast<uint32_t>(units - bulk) << 4) & 0xFFFFFFF0u;  // rlwinm r11
    StoreVectorPs(base, tail, value);                          // stvx128 v63,r0,r4
    // addi r9,r11,-9 ; rlwinm r5,r9,0,0,28 : rest - 16, so the copy replicates the 16 bytes just
    // stored over the remaining units. sub_82F52FB8 is a forward byte/word memcpy(r3, r4, r5) that
    // touches only r0 and r3-r10 -- no vector register, no non-volatile GPR.
    const uint32_t len = (rest - 9u) & 0xFFFFFFF8u;
    skate3::audio::GuestCall(ctx, base, sub_82F52FB8, tail + 16, tail, len);   // bl 0x82f52fb8
  }
}

REX_FUNC(Native) {
  const uint32_t out = ctx.r3.u32;                             // mr r31,r3
  const int32_t first = ctx.r6.s32;                            // mr r30,r6
  const int32_t length = ctx.r7.s32;
  const double start = ctx.f1.f64;                             // fmr f29,f1
  const double end = ctx.f2.f64;                               // fmr f28,f2
  // stwu r1,-272(r1) and everything at 80(r1)..159(r1) is this function's own frame: the integer
  // transfer slot, five four-lane splat scratch areas and the __savegprlr_26/__savefpr_26 spills.
  // None of it is reproduced and none of it is windowed. The splats are built as registers, which
  // is exact because the four words of each are identical (so the lvx128 lane reversal is a no-op)
  // except the index vector at 96(r1), whose reversal is spelled out below -- and because every
  // slot used is a multiple of 16 from r1, which the lifted lvx128 address masking relies on too.

  // addi r26,r6,255 ; addi r11,r7,-1 ; cmpw cr6,r26,r11 ; bgt ; mr r27,r26
  const int32_t block_last = static_cast<int32_t>(static_cast<uint32_t>(first) + 255u);
  const int32_t ramp_last = static_cast<int32_t>(static_cast<uint32_t>(length) - 1u);
  const int32_t last = (block_last > ramp_last) ? ramp_last : block_last;

  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by fmr f29,f1
  const double delta = SubSingle(end, start);                   // fsubs f27,f2,f1
  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by lfd f0,80(r1)
  const double length_f = IntToSingle(length);                  // fcfid f13,f0 ; frsp f30,f13
  const double threshold = LoadSingle(base, kRampThreshold);    // lfs f26,23056(r10)
  const double sqrt_length = SqrtSingle(length_f);              // fsqrts f12,f30
  const double step = DivSingle(delta, sqrt_length);            // fdivs f31,f27,f12

  int32_t index = first;                                       // r30
  uint32_t cursor = out;                                       // r31
  int32_t pad = 0;                                             // r28

  // cmpwi cr6,r30,0 ; bge cr6,0x82b42ea8 : the sub-zero span, flat at `start`.
  if (index < 0) {
    // neg r11,r30 ; srawi ; addze ; rlwinm r7,r8,2,0,29 ; subf r6,r7,r11 : (-index) mod 4.
    const int32_t below = static_cast<int32_t>(0u - static_cast<uint32_t>(index));  // neg r11,r30
    const int32_t below_aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(below)) << 2) & 0xFFFFFFFCu);
    const int32_t leftover = below - below_aligned;
    // subfic r11,r6,4 ; addi r10,r11,-4 ; subfic r5,r10,0 ; subfe r3,r4,r4 ; and r28,r3,r11 :
    // the carry of `0 - (-leftover)` is 1 only when leftover is zero, and subfe turns that into an
    // all-ones or all-zero mask over 4 - leftover. So pad = (4 - leftover) % 4.
    pad = (leftover == 0) ? 0 : (4 - leftover);

    // stfs f29,128(r1) .. stfs f29,140(r1) ; lvx128 v63,r0,r10 : `start` in all four lanes.
    const simde__m128 start_fill = simde_mm_set1_ps(static_cast<float>(start));

    // srawi r10,r30,2 ; addze r9,r10 ; neg r29,r9 : the 16-byte units to fill.
    const int32_t units = -DivFour(index);                     // r29
    FillVectorUnits(ctx, base, cursor, units, start_fill);

    // loc_82B42DC8: rlwinm r11,r29,2,0,29 ; add. r30,r11,r30 ; rlwinm r10,r29,4,0,27 ;
    //               add r31,r10,r31
    index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                 ((static_cast<uint32_t>(units) << 2) & 0xFFFFFFFCu));
    cursor += (static_cast<uint32_t>(units) << 4) & 0xFFFFFFF0u;
    // bge 0x82b42e08 : fewer than four floats left below zero, done one plus a replicating copy.
    if (index < 0) {
      ctx.fpscr.disableFlushModeUnconditional();               // emitted by stfs f29,0(r31)
      StoreSingle(base, cursor, start);                        // stfs f29,0(r31)
      const int32_t left = static_cast<int32_t>(0u - static_cast<uint32_t>(index));  // neg r29,r30
      // rlwinm r11,r29,2,0,29 ; addi r11,r11,-1 ; rlwinm r5,r11,0,0,29 : 4*left - 4 bytes.
      const uint32_t len =
          (((static_cast<uint32_t>(left) << 2) & 0xFFFFFFFCu) - 1u) & 0xFFFFFFFCu;
      skate3::audio::GuestCall(ctx, base, sub_82F52FB8, cursor + 4, cursor, len);  // bl 0x82f52fb8
      index = static_cast<int32_t>(static_cast<uint32_t>(index) + static_cast<uint32_t>(left));
      cursor += (static_cast<uint32_t>(left) << 2) & 0xFFFFFFFCu;
    }

    // loc_82B42E08: the alignment padding, `pad` samples written one at a time so that the vector
    // body below starts on a four-sample boundary. Only reachable from here, which is why r28 is
    // never read when `first` is non-negative.
    //
    // These `pad` samples are LINEAR -- `fma(float(n), step, start)`, with no fsqrts anywhere in
    // either form, against the `fsqrts` the scalar ramp tail below does have. Kept as found.
    ctx.fpscr.disableFlushModeUnconditional();                 // emitted by fcmpu cr6,f27,f26
    // fcmpu cr6,f27,f26 ; blt cr6,0x82b42e5c. An unordered compare leaves lt clear, so a NaN delta
    // takes the ascending form -- as `!(delta < threshold)` does.
    if (!(delta < threshold)) {
      // cmpw cr6,r30,r27 ; bgt cr6,0x82b42ea8 ; addi r11,r30,1
      if (index <= last) {
        int32_t n = static_cast<int32_t>(static_cast<uint32_t>(index) + 1u);
        while (pad > 0) {                                      // cmpwi cr6,r28,0 ; ble
          ctx.fpscr.disableFlushModeUnconditional();           // emitted by lfd f0,80(r1)
          const double value = FmaddSingle(IntToSingle(n), step, start);  // fmadds f11,f12,f31,f29
          index += 1;                                          // addi r30,r30,1
          n += 1;                                              // addi r11,r11,1
          pad -= 1;                                            // addi r28,r28,-1
          StoreSingle(base, cursor, value);                    // stfs f11,0(r31)
          cursor += 4;                                         // addi r31,r31,4
          if (index > last) break;                             // ble cr6,0x82b42e1c
        }
      }
    } else {
      // loc_82B42E5C: the descending form walks in from the `end` side instead. Also linear.
      if (index <= last) {
        int32_t n = static_cast<int32_t>(static_cast<uint32_t>(index) + 1u);
        while (pad > 0) {                                      // cmpwi cr6,r28,0 ; ble
          ctx.fpscr.disableFlushModeUnconditional();           // emitted by lfd f0,80(r1)
          const double back = SubSingle(length_f, IntToSingle(n));  // fsubs f11,f30,f12
          const double value = NmsubSingle(back, step, end);   // fnmsubs f10,f11,f31,f28
          index += 1;                                          // addi r30,r30,1
          n += 1;                                              // addi r11,r11,1
          pad -= 1;                                            // addi r28,r28,-1
          StoreSingle(base, cursor, value);                    // stfs f10,0(r31)
          cursor += 4;                                         // addi r31,r31,4
          if (index > last) break;                             // ble cr6,0x82b42e68
        }
      }
    }
  }

  // loc_82B42EA8: the vector body's setup. Five splats and one loaded constant vector, all of them
  // parked in the frame by the lifted form and held in registers here.
  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by stfs f31,112(r1)
  // stfs f31,112(r1) .. 124(r1) : the step in all four lanes.
  const simde__m128 step_v = simde_mm_set1_ps(static_cast<float>(step));
  // stfs f29,128(r1) .. 140(r1) : `start` in all four lanes.
  const simde__m128 start_v = simde_mm_set1_ps(static_cast<float>(start));
  // stfs of float(index+1) at 96(r1), +2 at 100, +3 at 104, +4 at 108, read back as one vector.
  // The lane reversal puts the word at 108 in lane 0 and the word at 96 in lane 3, which is what
  // setr_ps spells out here. The four `addi rN,r30,k ; extsw` pairs are 64-bit adds in the lifted
  // form, so the index arithmetic is done unsigned rather than risking signed overflow.
  const uint32_t i0 = static_cast<uint32_t>(index);
  const simde__m128 idx = simde_mm_setr_ps(IntToFloat(static_cast<int32_t>(i0 + 4u)),
                                           IntToFloat(static_cast<int32_t>(i0 + 3u)),
                                           IntToFloat(static_cast<int32_t>(i0 + 2u)),
                                           IntToFloat(static_cast<int32_t>(i0 + 1u)));
  // subf r11,r30,r27 ; addi r3,r11,1 ; srawi r11,r3,2 ; addze r9,r11 : groups of four samples.
  const int32_t span = static_cast<int32_t>(static_cast<uint32_t>(last) -
                                            static_cast<uint32_t>(index) + 1u);
  const int32_t groups = DivFour(span);                        // r9
  // srawi r6,r9,2 ; addze r5,r6 ; rlwinm r10,r5,2,0,29 : groups taken four at a time.
  const int32_t bulk =
      static_cast<int32_t>((static_cast<uint32_t>(DivFour(groups)) << 2) & 0xFFFFFFFCu);
  // lvx128 v62,r0,r8 : the +4.0 index stride. stvx128 v62,r0,r7 parks it at 144(r1), inside the
  // frame, where only the descending path reads it back -- and only after overwriting it.
  const simde__m128 stride_up = LoadVectorPs(base, kIndexStride);

  simde__m128 x;                                               // v63, the square root's argument
  simde__m128 stride;                                          // v62, added per group of four
  simde__m128 coef;                                            // v9,  from 112(r1)
  simde__m128 base_v;                                          // v8,  from 128(r1)

  // fcmpu cr6,f27,f26 ; bge cr6,0x82b4300c
  if (!(delta < threshold)) {
    // loc_82B4300C: the ascending form. lvx128 v63,r0,r11 reloads the index vector from 96(r1) and
    // v62 still holds the stride the setup loaded.
    x = idx;
    stride = stride_up;
    coef = step_v;                                             // lvx128 v9,r0,r11  (112(r1))
    base_v = start_v;                                          // lvx128 v8,r0,r8   (128(r1))
  } else {
    // The descending form rewrites three of the frame splats in place, then reloads them: 128..140
    // all become `end` (stfs f28,128(r1) and three lwz/stw hops off it), 112..124 all become
    // `step * scale` (stfs f0,112(r1) and the same hops), and 144..156 become the four words of
    // `lis r11,-16256`. Those lwz/stw chains only move words around inside the frame, so the
    // native form is three splats.
    const double scale = LoadSingle(base, kDescendingScale);   // lfs f0,-8480(r6)
    coef = simde_mm_set1_ps(static_cast<float>(MulSingle(step, scale)));  // fmuls f0,f31,f0
    base_v = simde_mm_set1_ps(static_cast<float>(end));
    // stfs f30,80(r1) .. 92(r1) ; lvx128 v63,r0,r11 : length_f in all four lanes.
    const simde__m128 length_v = simde_mm_set1_ps(static_cast<float>(length_f));
    ctx.fpscr.enableFlushMode();                               // emitted by vsubfp128 v63,v63,v62
    x = simde_mm_sub_ps(length_v, idx);                        // vsubfp128 v63,v63,v62
    // lis r11,-16256 : the word 0xC0800000, stored over the index stride at 144(r1).
    stride = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0xC0800000)));  // lvx128 v62,r0,r28
  }

  // v29, v30 and v31 are in the ABI-preserved range the harness compares on every call, and the
  // main loop clobbers all three without the function ever saving them (__savegprlr_26 and
  // __savefpr_26 only, no __savevmx_*). So whatever the LAST iteration left in them has to be left
  // behind -- and left behind right after the loop, not at the exit, so that the trailing fill's
  // sub_82F52FB8 disturbs them exactly as it does under the original (it does not: it touches only
  // r0 and r3-r10).
  bool main_ran = false;
  simde__m128 keep29 = simde_mm_setzero_ps();
  simde__m128 keep30 = simde_mm_setzero_ps();
  simde__m128 keep31 = simde_mm_setzero_ps();

  // loc_82B43014: cmpwi cr6,r10,0 ; ble cr6,0x82b43148 -- four groups of four per pass.
  if (bulk > 0) {
    ctx.fpscr.enableFlushModeUnconditional();                  // emitted by vcsxwfp128 v0,v61,1
    const simde__m128 half = HalfVector();
    // addi r11,r10,-1 ; rlwinm r11,r11,30,2,31 ; addi r11,r11,1 ; mtctr r11
    uint32_t passes = ((static_cast<uint32_t>(bulk) - 1u) >> 2) + 1u;
    do {
      ctx.fpscr.enableFlushModeUnconditional();                // emitted by vaddfp128 v60,v63,v62
      // The four arguments are stepped first, then four independent square roots are interleaved.
      // Each `vaddfp128` keeps the running value as its first operand, as lifted.
      const simde__m128 x0 = x;                                // vor128 v4,v63,v63
      const simde__m128 x1 = simde_mm_add_ps(x0, stride);       // vaddfp128 v60,v63,v62
      const simde__m128 x2 = simde_mm_add_ps(x1, stride);       // vaddfp128 v59,v60,v62
      const simde__m128 x3 = simde_mm_add_ps(x2, stride);       // vaddfp128 v57,v59,v62
      x = simde_mm_add_ps(x3, stride);                          // vaddfp128 v63,v57,v62
      const SqrtParts s0 = SqrtVector(x0, half);
      const SqrtParts s1 = SqrtVector(x1, half);
      const SqrtParts s2 = SqrtVector(x2, half);
      const SqrtParts s3 = SqrtVector(x3, half);
      // The four `vor128 v47,v8,v8` copies are the FMAs' addend register being set up fresh every
      // pass, so nothing accumulates across passes. The coefficient is the FIRST operand of every
      // vmaddfp128 and the square root the second, as lifted: with two NaN operands the winning
      // slot is part of the semantics (vmx128-exactness rule 4), so the order is copied.
      const simde__m128 a0 = simde_mm_fmadd_ps(coef, s0.value, base_v);  // vmaddfp128 v47,v9,v2,v47
      const simde__m128 a1 = simde_mm_fmadd_ps(coef, s1.value, base_v);  // vmaddfp128 v45,v9,v10,v45
      const simde__m128 a2 = simde_mm_fmadd_ps(coef, s2.value, base_v);  // vmaddfp128 v43,v9,v11,v43
      const simde__m128 a3 = simde_mm_fmadd_ps(coef, s3.value, base_v);  // vmaddfp128 v42,v9,v12,v42
      StoreVectorPs(base, cursor + 0, a0);                     // stvx128 v47,r0,r31
      StoreVectorPs(base, cursor + 16, a1);                    // stvx128 v45,r0,r11
      StoreVectorPs(base, cursor + 32, a2);                    // stvx128 v43,r0,r11
      StoreVectorPs(base, cursor + 48, a3);                    // stvx128 v42,r0,r11
      cursor += 64;                                            // addi r31,r11,16
      keep31 = s0.mask;                                        // vxor128 v31,v55,v58
      keep29 = s1.refined;                                     // vmaddfp v29,v12,v30,v12
      keep30 = s2.refined;                                     // vmaddfp v30,v11,v1,v11
      main_ran = true;
    } while (--passes != 0);                                   // bdnz 0x82b43044
  }
  if (main_ran) {
    simde_mm_store_ps(ctx.v29.f32, keep29);
    simde_mm_store_ps(ctx.v30.f32, keep30);
    simde_mm_store_ps(ctx.v31.f32, keep31);
  }

  // loc_82B43148: cmpw cr6,r10,r9 ; bge cr6,0x82b431a0 -- the odd groups, one vector each. Nothing
  // in here touches v29-v31.
  if (bulk < groups) {
    ctx.fpscr.enableFlushModeUnconditional();                  // emitted by vcsxwfp128 v7,v41,1
    const simde__m128 half = HalfVector();
    uint32_t passes = static_cast<uint32_t>(groups - bulk);    // subf r11,r10,r9 ; mtctr r11
    do {
      ctx.fpscr.enableFlushModeUnconditional();                // emitted by vrsqrtefp128 v0,v63
      const SqrtParts s = SqrtVector(x, half);
      x = simde_mm_add_ps(x, stride);                          // vaddfp128 v63,v63,v62
      const simde__m128 a = simde_mm_fmadd_ps(coef, s.value, base_v);  // vmaddfp v0,v9,v12,v8
      StoreVectorPs(base, cursor, a);                          // stvx128 v0,r0,r31
      cursor += 16;                                            // addi r31,r31,16
    } while (--passes != 0);                                   // bdnz 0x82b43160
  }

  // loc_82B431A0: rlwinm r11,r9,2,0,29 ; add r11,r11,r30 -- the samples the vectors did not cover.
  int32_t tail_index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                            ((static_cast<uint32_t>(groups) << 2) & 0xFFFFFFFCu));
  ctx.fpscr.disableFlushModeUnconditional();                   // emitted by fcmpu cr6,f27,f26
  // fcmpu cr6,f27,f26 ; blt cr6,0x82b431f8
  if (!(delta < threshold)) {
    // cmpw cr6,r11,r27 ; bgt cr6,0x82b43244
    if (tail_index <= last) {
      // subf r9,r11,r27 ; addi r10,r11,1 ; addi r9,r9,1 ; add r11,r9,r11 ; mtctr r9
      uint32_t passes = static_cast<uint32_t>(last - tail_index) + 1u;
      int32_t n = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + 1u);
      tail_index = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + passes);
      do {
        ctx.fpscr.disableFlushModeUnconditional();             // emitted by lfd f0,96(r1)
        // fcfid f13,f0 ; fsqrts f12,f13 : the root of the DOUBLE, with no frsp in between.
        const double root = SqrtSingle(IntToDouble(n));        // fsqrts f12,f13
        const double value = FmaddSingle(root, step, start);   // fmadds f11,f12,f31,f29
        n += 1;                                                // addi r10,r10,1
        StoreSingle(base, cursor, value);                      // stfs f11,0(r31)
        cursor += 4;                                           // addi r31,r31,4
      } while (--passes != 0);                                 // bdnz 0x82b431cc
    }
  } else {
    // loc_82B431F8: the descending form, which does round to single before subtracting.
    if (tail_index <= last) {
      uint32_t passes = static_cast<uint32_t>(last - tail_index) + 1u;
      int32_t n = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + 1u);
      tail_index = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + passes);
      do {
        ctx.fpscr.disableFlushModeUnconditional();             // emitted by lfd f0,96(r1)
        const double back = SubSingle(length_f, IntToSingle(n));  // frsp f12,f13 ; fsubs f11,f30,f12
        const double root = SqrtSingle(back);                  // fsqrts f10,f11
        const double value = NmsubSingle(root, step, end);     // fnmsubs f9,f10,f31,f28
        n += 1;                                                // addi r10,r10,1
        StoreSingle(base, cursor, value);                      // stfs f9,0(r31)
        cursor += 4;                                           // addi r31,r31,4
      } while (--passes != 0);                                 // bdnz 0x82b43214
    }
  }

  // loc_82B43244: the rest of the 256-entry block, flat at `end` in both directions.
  // cmpw cr6,r11,r26 ; bgt cr6,0x82b4332c
  if (tail_index <= block_last) {
    // subf r10,r11,r26 ; addi r10,r10,1 : entries left in the block.
    int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(block_last) -
                                             static_cast<uint32_t>(tail_index) + 1u);
    // srawi r9,r10,2 ; addze r5,r9 : the 16-byte unit count, captured BEFORE the loop below trims
    // `remaining` -- r5 survives that loop untouched, which is what makes the two add up.
    const int32_t units = DivFour(remaining);                  // r5
    // srawi r8,r10,2 ; addze r7,r8 ; rlwinm r6,r7,2,0,29 ; subf. r4,r6,r10 ; beq 0x82b4329c
    int32_t aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(remaining)) << 2) & 0xFFFFFFFCu);
    if (remaining != aligned) {
      // loc_82B43270: write single entries until the count left is a multiple of four.
      while (tail_index <= block_last) {                       // cmpw cr6,r11,r26 ; bgt
        remaining -= 1;                                        // addi r10,r10,-1
        ctx.fpscr.disableFlushModeUnconditional();             // emitted by stfs f28,0(r31)
        StoreSingle(base, cursor, end);                        // stfs f28,0(r31)
        cursor += 4;                                           // addi r31,r31,4
        tail_index += 1;                                       // addi r11,r11,1
        aligned = static_cast<int32_t>(
            (static_cast<uint32_t>(DivFour(remaining)) << 2) & 0xFFFFFFFCu);
        if (remaining == aligned) break;                       // subf. r6,r7,r10 ; bne
      }
    }
    // loc_82B4329C: stfs f28,132(r1)/136/128/140 ; lvx128 v63,r0,r10 : `end` in all four lanes.
    ctx.fpscr.disableFlushModeUnconditional();                 // emitted by stfs f28,132(r1)
    const simde__m128 end_fill = simde_mm_set1_ps(static_cast<float>(end));
    FillVectorUnits(ctx, base, cursor, units, end_fill);
  }

  // loc_82B4332C: li r3,1 -- the function's only result. f26-f31 and r26-r31 come back from the
  // frame at their entry values, which is what leaving them alone here gives.
  ctx.r3.s64 = 1;
}

// The call writes one contiguous run of the output array and nothing else outside its own frame.
// Checked against the census provenance rather than taken from the caller's note: of the 68 stores,
// the 41 through r1 are the frame (the integer transfer slot at 80/96, the five splat areas at
// 80..159, and the three lwz/stw hop chains the descending setup uses to copy words around inside
// it), and every one of the other 27 addresses r31, which is entry r3 plus an offset built only
// from entry r6 and entry r7. No store address carries a loaded value.
//
// Its length is not 1024 bytes in general. Two entry states make the run longer than the block, the
// same two the sibling sub_82B427D8 documents: `first < -256` makes the sub-zero fill `-first`
// samples with no clamp, and `first > length - 1` makes `span` negative, so both vector loops are
// unentered while the index still advances by 4*trunc(span/4) and the trailing flat fill starts
// BELOW where the ramp left off and runs to `first + 255`. For -256 <= first <= length-1 the total
// is exactly 1024 bytes plus `out & 0xF`, i.e. the one envelope span the caller expects.
//
// Either way the count is decided entirely by the entry registers -- none of it by memory -- so it
// is computed by replaying the same integer control flow the body does. The 16-byte stores go
// through stvx128's address masking and the output pointer is never checked for alignment, so the
// run starts at `out` rounded DOWN to 16 bytes; a pathological length is left to the harness's own
// 32 KB budget check rather than refused here.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t out = ctx.r3.u32;
  const int32_t first = ctx.r6.s32;
  const int32_t length = ctx.r7.s32;

  spec.read(kRampThreshold, 4);      // lfs f26,23056(r10)
  spec.read(kIndexStride, 16);       // lvx128 v62,r0,r8
  spec.read(kDescendingScale, 4);    // lfs f0,-8480(r6), descending form only

  const int32_t block_last = static_cast<int32_t>(static_cast<uint32_t>(first) + 255u);
  const int32_t ramp_last = static_cast<int32_t>(static_cast<uint32_t>(length) - 1u);
  const int32_t last = (block_last > ramp_last) ? ramp_last : block_last;

  int32_t index = first;
  int64_t bytes = 0;  // bytes written from `out` upward
  int32_t pad = 0;

  if (index < 0) {
    const int32_t below = static_cast<int32_t>(0u - static_cast<uint32_t>(index));
    const int32_t below_aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(below)) << 2) & 0xFFFFFFFCu);
    const int32_t leftover = below - below_aligned;
    pad = (leftover == 0) ? 0 : (4 - leftover);

    // FillVectorUnits writes 16 bytes per unit, or nothing at all for units <= 0: the bulk loop
    // covers 16*bulk and the tail stvx128 plus its replicating memcpy the remaining 16*(units-bulk).
    const int32_t units = -DivFour(index);
    if (units > 0) bytes += 16ll * units;
    index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                 ((static_cast<uint32_t>(units) << 2) & 0xFFFFFFFCu));
    if (index < 0) {
      const int32_t left = static_cast<int32_t>(0u - static_cast<uint32_t>(index));
      bytes += 4ll * left;  // one stfs plus a replicating copy: 4 bytes per sample
      index = 0;
    }
    // The padding loop runs while pad remains and the index is still inside the ramp, so exactly
    // min(pad, last - index + 1) samples, and only when it is entered at all. The ascending and the
    // descending form write the same count.
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
  if (bulk > 0) bytes += 16ll * bulk;                  // four vectors per pass
  if (bulk < groups) bytes += 16ll * (groups - bulk);  // one vector per pass
  int32_t tail_index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                            ((static_cast<uint32_t>(groups) << 2) & 0xFFFFFFFCu));

  if (tail_index <= last) {  // the scalar ramp tail, one sample per pass
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

}  // namespace port_82B42C98
SKATE3_PORT(82B42C98, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
