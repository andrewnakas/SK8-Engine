// sub_82B3D0A8  role: two-source crossfade of B and C by the per-sample coefficient D -- writes the plain blend to F and the gain-weighted blend accumulated onto A to E; 16 floats per iteration on a vector path when both inputs are 16-byte aligned, a four-wide scalar path plus a one-float tail for the rest   lifted: skate3_recomp.68.cpp:9100 (662 lines)
// STATUS: verified
// calls/session: boot 93954  play 111902  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3D0A8.md
namespace port_82B3D0A8 {

// Arguments. The PowerPC ABI gives each float argument an FPR *and* a GPR parameter slot, which
// is why r4 and r5 are never read: they are f1's and f2's slots. r3 is the element count, r6-r10
// are five float arrays and the sixth arrives on the stack at 84(r1) -- the ninth parameter slot,
// slots being eight bytes from 20(r1). Nothing in docs/rw_audio_structs.h names any of these
// buffers, so there are no offset constants: the structure is "six arrays of floats".
//
//   r3  count      r6  A (accumulator, read)   r7  B (input)     r8  C (input)
//   f1  gain_b     r9  D (coefficient)         r10 E (output)    84(r1) F (output)
//   f2  gain_c
//
// Per element i:
//   F[i] = D[i]*C[i] + (1 - D[i])*B[i]
//   E[i] = A[i] + fma((1 - D[i])*B[i], gain_b, (D[i]*C[i])*gain_c)
// with the exact association and operand order varying between the two paths and between the
// four unrolled scalar lanes. Those differences are rounding-visible, so each site is written
// the way it is lifted rather than factored into one expression; see the note.
constexpr uint32_t kNinthArgSlot = 84;

// lis r11,-32206 ; lfs f0,-22460(r11): 0x82320000 - 22460. The scalar paths' "1.0", loaded from
// the image rather than assumed, because only the vector path builds its own.
constexpr uint32_t kOneConstant = 0x8231A844;

// lvx128: sixteen bytes with the low four address bits masked off, plus the big-endian lane
// reversal RexGlue applies to every vector memory access. Only B and C are known aligned on the
// vector path, so this masking is load-bearing on A, D, E and F -- see the note.
inline simde__m128 LoadVectorPs(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// stvx128: the same masking and the same lane reversal on the way out.
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

// stfs / stfsx: the double rounded to single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fsubs / fadds / fmuls: the double result rounded to single. Never contracted with a neighbour.
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }

// fmadds: a single-rounding double-precision FMA, then rounded to single. std::fma, never
// `a * b + c`.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

REX_FUNC(Native) {
  const int32_t count = ctx.r3.s32;                                    // r3
  const uint32_t a = ctx.r6.u32;                                       // r6
  const uint32_t b = ctx.r7.u32;                                       // r7
  const uint32_t c = ctx.r8.u32;                                       // r8
  const uint32_t d = ctx.r9.u32;                                       // r9
  const uint32_t e = ctx.r10.u32;                                      // r10
  const uint32_t f = REX_LOAD_U32(ctx.r1.u32 + kNinthArgSlot);         // lwz r30,84(r1)
  const double gain_b = ctx.f1.f64;
  const double gain_c = ctx.f2.f64;
  // stw r3,20(r1) spills the count into its own parameter home slot and reloads it after the
  // vector loop; that is this function's frame, so it is kept in a local instead.

  int32_t done = 0;  // r27: elements the vector path consumed

  // clrlwi r11,r7,28 ; bne cr6 ; clrlwi r11,r8,28 ; bne cr6 : B and C both 16-byte aligned.
  // Note that A, D, E and F are NOT checked.
  if (((b & 0xFu) == 0) && ((c & 0xFu) == 0)) {
    // srawi r11,r3,4 ; addze r5,r11 : count/16 truncated toward zero. The carry term is srawi's
    // CA, reproduced because addze consumes it.
    const bool shift_carry = (count < 0) && ((static_cast<uint32_t>(count) & 0xFu) != 0);
    const int32_t blocks = (count >> 4) + (shift_carry ? 1 : 0);       // r5

    // stfs f2,-192(r1) .. stfs f2,-180(r1) ; lvx128 v63,r0,r11 and stfs f1,-176(r1) ..
    // stfs f1,-164(r1) ; lvx128 v0,r0,r10 : each gain in all four lanes. The four words are
    // identical so the load's lane reversal is a no-op on them, and the scratch below r1 is this
    // function's own frame -- the native body keeps both in registers. (That assumes the guest r1
    // is 16-byte aligned, which the lvx128 address masking relies on in the lifted form too; see
    // the note.) The FTZ disable is emitted by the first stfs, ahead of the branch below, so it
    // stays here even when the loop does not run.
    ctx.fpscr.disableFlushModeUnconditional();                         // stfs f2,-192(r1)
    const simde__m128 gain_c_v = simde_mm_set1_ps(static_cast<float>(ctx.f2.f64));  // v63
    const simde__m128 gain_b_v = simde_mm_set1_ps(static_cast<float>(ctx.f1.f64));  // v0
    // vspltisw128 v63,0 ; vupkd3d128 v62,v63,0 : every source byte is zero, so every lane of the
    // unpack is 0x3F800000. This is the vector path's 1.0, and it is built, not loaded.
    const simde__m128 one_v = simde_mm_castsi128_ps(simde_mm_set1_epi32(0x3F800000));

    // rlwinm r11,r5,2,0,29 ; cmpwi cr6,r11,0 ; ble cr6,0x82b3d2a0
    const int32_t quads = static_cast<int32_t>(static_cast<uint32_t>(blocks) << 2);
    if (quads > 0) {
      // addi r11,r11,-1 ; rlwinm r11,r11,30,2,31 ; addi r11,r11,1 ; mtctr r11 : blocks passes.
      uint32_t iterations = ((static_cast<uint32_t>(quads) - 1u) >> 2) + 1u;

      // The lifted cursors are offset from the block they address (r31 = A, r5 = D+16,
      // r11 = B+32, r4 = F+48, r10 = E, all with per-iteration index registers), so the offsets
      // below are the lifted *effective addresses*, not the lifted displacements. Every one is a
      // 32-bit wrapping add, which is what these uint32_t cursors are.
      uint32_t pa = a, pb = b, pc = c, pd = d, pe = e, pf = f;
      do {
        const simde__m128 d0 = LoadVectorPs(base, pd + 0);             // lvx128 v61,r5,r15
        const simde__m128 d1 = LoadVectorPs(base, pd + 16);            // lvx128 v60,r0,r5
        ctx.fpscr.enableFlushModeUnconditional();                      // before the first vsubfp128
        const simde__m128 inv0 = simde_mm_sub_ps(one_v, d0);           // vsubfp128 v59,v62,v61
        const simde__m128 d2 = LoadVectorPs(base, pd + 32);            // lvx128 v58,r28,r11
        const simde__m128 inv1 = simde_mm_sub_ps(one_v, d1);           // vsubfp128 v57,v62,v60
        const simde__m128 c0 = LoadVectorPs(base, pc + 0);             // lvx128 v56,r31,r17
        const simde__m128 inv2 = simde_mm_sub_ps(one_v, d2);           // vsubfp128 v55,v62,v58
        const simde__m128 d3 = LoadVectorPs(base, pd + 48);            // lvx128 v53,r5,r10
        // D is the FIRST operand of every product with C and (1-D) the first of every product
        // with B, as lifted: with two NaN operands the winning slot is part of the semantics
        // (vmx128-exactness rule 4), so the order is copied rather than rearranged.
        const simde__m128 wet0 = simde_mm_mul_ps(d0, c0);              // vmulfp128 v54,v61,v56
        const simde__m128 inv3 = simde_mm_sub_ps(one_v, d3);           // vsubfp128 v52,v62,v53
        const simde__m128 b0 = LoadVectorPs(base, pb + 0);             // lvx128 v51,r11,r14
        const simde__m128 b1 = LoadVectorPs(base, pb + 16);            // lvx128 v50,r11,r3
        const simde__m128 b2 = LoadVectorPs(base, pb + 32);            // lvx128 v49,r0,r11
        const simde__m128 c1 = LoadVectorPs(base, pc + 16);            // lvx128 v47,r5,r24
        const simde__m128 b3 = LoadVectorPs(base, pb + 48);            // lvx128 v48,r11,r9
        const simde__m128 wet1 = simde_mm_mul_ps(d1, c1);              // vmulfp128 v45,v60,v47
        const simde__m128 c2 = LoadVectorPs(base, pc + 32);            // lvx128 v46,r29,r11
        const simde__m128 c3 = LoadVectorPs(base, pc + 48);            // lvx128 v44,r4,r20
        const simde__m128 wet2 = simde_mm_mul_ps(d2, c2);              // vmulfp128 v43,v58,v46
        const simde__m128 dry0 = simde_mm_mul_ps(inv0, b0);            // vmulfp128 v13,v59,v51
        const simde__m128 dry1 = simde_mm_mul_ps(inv1, b1);            // vmulfp128 v12,v57,v50
        const simde__m128 dry2 = simde_mm_mul_ps(inv2, b2);            // vmulfp128 v11,v55,v49
        const simde__m128 wet3 = simde_mm_mul_ps(d3, c3);              // vmulfp128 v42,v53,v44
        const simde__m128 dry3 = simde_mm_mul_ps(inv3, b3);            // vmulfp128 v10,v52,v48
        const simde__m128 wg0 = simde_mm_mul_ps(wet0, gain_c_v);       // vmulfp128 v9,v54,v63
        const simde__m128 wg1 = simde_mm_mul_ps(wet1, gain_c_v);       // vmulfp128 v8,v45,v63
        const simde__m128 wg2 = simde_mm_mul_ps(wet2, gain_c_v);       // vmulfp128 v7,v43,v63
        // The F value is a separate multiply, multiply and add -- NOT an FMA. The scalar paths
        // below fuse the same expression; that difference is the original's, kept as found.
        const simde__m128 blend0 = simde_mm_add_ps(wet0, dry0);        // vaddfp128 v41,v54,v13
        const simde__m128 blend1 = simde_mm_add_ps(wet1, dry1);        // vaddfp128 v40,v45,v12
        const simde__m128 blend2 = simde_mm_add_ps(wet2, dry2);        // vaddfp128 v39,v43,v11
        const simde__m128 wg3 = simde_mm_mul_ps(wet3, gain_c_v);       // vmulfp128 v6,v42,v63
        const simde__m128 blend3 = simde_mm_add_ps(wet3, dry3);        // vaddfp128 v38,v42,v10
        const simde__m128 mix0 = simde_mm_fmadd_ps(dry0, gain_b_v, wg0);  // vmaddfp v13,v13,v0,v9
        const simde__m128 mix1 = simde_mm_fmadd_ps(dry1, gain_b_v, wg1);  // vmaddfp v12,v12,v0,v8
        const simde__m128 mix2 = simde_mm_fmadd_ps(dry2, gain_b_v, wg2);  // vmaddfp v11,v11,v0,v7
        StoreVectorPs(base, pf + 0, blend0);                           // stvx128 v41,r4,r10
        StoreVectorPs(base, pf + 16, blend1);                          // stvx128 v40,r5,r23
        StoreVectorPs(base, pf + 32, blend2);                          // stvx128 v39,r27,r11
        const simde__m128 mix3 = simde_mm_fmadd_ps(dry3, gain_b_v, wg3);  // vmaddfp v10,v10,v0,v6
        StoreVectorPs(base, pf + 48, blend3);                          // stvx128 v38,r0,r4
        // A is loaded AFTER the four F stores and each quarter after the previous E store, as
        // lifted. That ordering is what makes an A that aliases E or F behave as the original.
        const simde__m128 a0 = LoadVectorPs(base, pa + 0);             // lvx128 v37,r0,r31
        const simde__m128 out0 = simde_mm_add_ps(mix0, a0);            // vaddfp128 v36,v13,v37
        StoreVectorPs(base, pe + 0, out0);                             // stvx128 v36,r31,r16
        const simde__m128 a1 = LoadVectorPs(base, pa + 16);            // lvx128 v35,r5,r22
        const simde__m128 out1 = simde_mm_add_ps(mix1, a1);            // vaddfp128 v34,v12,v35
        StoreVectorPs(base, pe + 16, out1);                            // stvx128 v34,r5,r21
        const simde__m128 a2 = LoadVectorPs(base, pa + 32);            // lvx128 v33,r11,r26
        const simde__m128 out2 = simde_mm_add_ps(mix2, a2);            // vaddfp128 v32,v11,v33
        StoreVectorPs(base, pe + 32, out2);                            // stvx128 v32,r11,r25
        const simde__m128 a3 = LoadVectorPs(base, pa + 48);            // lvx128 v61,r4,r19
        const simde__m128 out3 = simde_mm_add_ps(mix3, a3);            // vaddfp128 v60,v10,v61
        StoreVectorPs(base, pe + 48, out3);                            // stvx128 v60,r4,r18
        pa += 64;                                                      // addi r31,r31,64
        pb += 64;                                                      // addi r11,r11,64
        pc += 64;                                                      // (C is addressed off the others)
        pd += 64;                                                      // addi r5,r5,64
        pe += 64;
        pf += 64;                                                      // addi r4,r4,64
      } while (--iterations != 0);                                     // bdnz 0x82b3d198
    }
    // loc_82B3D2A0: rlwinm r27,r5,4,0,27, reached by the ble above as well as by falling out of
    // the loop (where r5 and r3 are reloaded from the frame first).
    done = static_cast<int32_t>(static_cast<uint32_t>(blocks) << 4);
  }

  // loc_82B3D2A4. mr r15,r27 ; cmpw cr6,r27,r3 ; bge cr6,0x82b3d4f4
  int32_t tail_start = done;  // r15
  if (done >= count) return;

  ctx.fpscr.disableFlushModeUnconditional();                           // before lfs f0,-22460(r11)
  const double one = LoadSingle(base, kOneConstant);                   // lfs f0,-22460(r11)

  // subf r5,r27,r3 ; cmpwi cr6,r5,4 ; blt cr6,0x82b3d474
  if ((count - done) >= 4) {
    // subf r11,r27,r3 ; addi r11,r11,-4 ; rlwinm r11,r11,30,2,31 ; addi r11,r11,1 ; mtctr r11
    uint32_t groups = ((static_cast<uint32_t>(count - done) - 4u) >> 2) + 1u;
    // rlwinm r22,r11,2,0,29 ; add r15,r22,r27 : the index the tail loop starts from.
    tail_start = done + static_cast<int32_t>(groups * 4u);

    // The lifted cursors are r11 = &B[j+1], r5 = &D[j+2], r4 = &C[j+3] and r31 = &E[j], with the
    // other three arrays reached by a precomputed base difference. Every effective address
    // reduces, in 32-bit wrapping arithmetic, to one of these block bases plus 0, 4, 8 or 12.
    const uint32_t offset = (static_cast<uint32_t>(done) << 2) & 0xFFFFFFFCu;  // rlwinm r5,r27,2,0,29
    uint32_t pa = a + offset, pb = b + offset, pc = c + offset;
    uint32_t pd = d + offset, pe = e + offset, pf = f + offset;
    do {
      // --- element j -------------------------------------------------------------------------
      ctx.fpscr.disableFlushModeUnconditional();                       // before the first lfs
      const double d0 = LoadSingle(base, pd + 0);                      // lfs f13,-8(r5)
      const double c0 = LoadSingle(base, pc + 0);                      // lfs f12,-12(r4)
      const double inv0 = SubSingle(one, d0);                          // fsubs f11,f0,f13
      const double wet0 = MulSingle(c0, d0);                           // fmuls f10,f12,f13  (C first)
      const double b0 = LoadSingle(base, pb + 0);                      // lfs f9,-4(r11)
      const double a0 = LoadSingle(base, pa + 0);                      // lfsx f8,r31,r28
      const double dry0 = MulSingle(inv0, b0);                         // fmuls f7,f11,f9
      const double wg0 = MulSingle(wet0, gain_c);                      // fmuls f6,f10,f2
      const double mix0 = FmaddSingle(dry0, gain_b, wg0);              // fmadds f5,f7,f1,f6
      const double out0 = AddSingle(mix0, a0);                         // fadds f4,f5,f8
      StoreSingle(base, pe + 0, out0);                                 // stfs f4,0(r31)
      // The three inputs are reloaded after that store, as lifted, because it may alias them.
      const double c0b = LoadSingle(base, pc + 0);                     // lfs f3,-12(r4)
      const double b0b = LoadSingle(base, pb + 0);                     // lfs f13,-4(r11)
      const double d0b = LoadSingle(base, pd + 0);                     // lfs f12,-8(r5)
      const double inv0b = SubSingle(one, d0b);                        // fsubs f11,f0,f12
      const double wet0b = MulSingle(c0b, d0b);                        // fmuls f10,f3,f12  (C first)
      const double blend0 = FmaddSingle(inv0b, b0b, wet0b);            // fmadds f9,f11,f13,f10
      StoreSingle(base, pf + 0, blend0);                               // stfsx f9,r31,r16

      // --- element j+1 -----------------------------------------------------------------------
      const double c1 = LoadSingle(base, pc + 4);                      // lfsx f8,r26,r11
      const double b1 = LoadSingle(base, pb + 4);                      // lfs f7,0(r11)
      const double a1 = LoadSingle(base, pa + 4);                      // lfsx f6,r22,r28
      const double d1 = LoadSingle(base, pd + 4);                      // lfsx f5,r25,r11
      const double inv1 = SubSingle(one, d1);                          // fsubs f4,f0,f5
      const double wet1 = MulSingle(c1, d1);                           // fmuls f3,f8,f5   (C first)
      const double dry1 = MulSingle(inv1, b1);                         // fmuls f13,f4,f7
      const double wg1 = MulSingle(wet1, gain_c);                      // fmuls f12,f3,f2
      const double mix1 = FmaddSingle(dry1, gain_b, wg1);              // fmadds f11,f13,f1,f12
      const double out1 = AddSingle(mix1, a1);                         // fadds f10,f11,f6
      StoreSingle(base, pe + 4, out1);                                 // stfsx f10,r11,r23
      const double c1b = LoadSingle(base, pc + 4);                     // lfsx f9,r26,r11
      const double d1b = LoadSingle(base, pd + 4);                     // lfsx f7,r25,r11
      const double inv1b = SubSingle(one, d1b);                        // fsubs f6,f0,f7
      const double wet1b = MulSingle(c1b, d1b);                        // fmuls f5,f9,f7   (C first)
      const double b1b = LoadSingle(base, pb + 4);                     // lfs f8,0(r11)
      const double blend1 = FmaddSingle(inv1b, b1b, wet1b);            // fmadds f4,f6,f8,f5
      StoreSingle(base, pf + 4, blend1);                               // stfsx f4,r19,r11

      // --- element j+2 -----------------------------------------------------------------------
      // This lane associates the weighting differently: (D*gain_c)*C, not (C*D)*gain_c.
      const double b2 = LoadSingle(base, pb + 8);                      // lfs f3,4(r11)
      const double c2 = LoadSingle(base, pc + 8);                      // lfsx f9,r5,r24
      const double a2 = LoadSingle(base, pa + 8);                      // lfsx f13,r20,r28
      const double d2 = LoadSingle(base, pd + 8);                      // lfs f12,0(r5)
      const double inv2 = SubSingle(one, d2);                          // fsubs f11,f0,f12
      const double dg2 = MulSingle(d2, gain_c);                        // fmuls f10,f12,f2
      const double dry2 = MulSingle(inv2, b2);                         // fmuls f8,f11,f3
      const double wg2 = MulSingle(dg2, c2);                           // fmuls f7,f10,f9
      const double mix2 = FmaddSingle(dry2, gain_b, wg2);              // fmadds f6,f8,f1,f7
      const double out2 = AddSingle(mix2, a2);                         // fadds f5,f6,f13
      StoreSingle(base, pe + 8, out2);                                 // stfsx f5,r27,r29
      const double c2b = LoadSingle(base, pc + 8);                     // lfsx f4,r5,r24
      const double b2b = LoadSingle(base, pb + 8);                     // lfs f3,4(r11)
      const double d2b = LoadSingle(base, pd + 8);                     // lfs f13,0(r5)
      const double inv2b = SubSingle(one, d2b);                        // fsubs f12,f0,f13
      const double wet2b = MulSingle(d2b, c2b);                        // fmuls f11,f13,f4  (D first)
      const double blend2 = FmaddSingle(inv2b, b2b, wet2b);            // fmadds f10,f12,f3,f11
      StoreSingle(base, pf + 8, blend2);                               // stfsx f10,r5,r18

      // --- element j+3 -----------------------------------------------------------------------
      // And this one multiplies D by C first, then by the gain: (D*C)*gain_c.
      const double c3 = LoadSingle(base, pc + 12);                     // lfs f9,0(r4)
      const double b3 = LoadSingle(base, pb + 12);                     // lfs f8,8(r11)
      const double a3 = LoadSingle(base, pa + 12);                     // lfsx f7,r21,r28
      const double d3 = LoadSingle(base, pd + 12);                     // lfs f6,4(r5)
      const double inv3 = SubSingle(one, d3);                          // fsubs f5,f0,f6
      const double wet3 = MulSingle(d3, c3);                           // fmuls f4,f6,f9   (D first)
      const double dry3 = MulSingle(inv3, b3);                         // fmuls f3,f5,f8
      const double wg3 = MulSingle(wet3, gain_c);                      // fmuls f13,f4,f2
      const double mix3 = FmaddSingle(dry3, gain_b, wg3);              // fmadds f12,f3,f1,f13
      const double out3 = AddSingle(mix3, a3);                         // fadds f11,f12,f7
      StoreSingle(base, pe + 12, out3);                                // stfsx f11,r29,r4
      const double c3b = LoadSingle(base, pc + 12);                    // lfs f10,0(r4)
      const double b3b = LoadSingle(base, pb + 12);                    // lfs f9,8(r11)
      const double d3b = LoadSingle(base, pd + 12);                    // lfs f8,4(r5)
      const double inv3b = SubSingle(one, d3b);                        // fsubs f7,f0,f8
      const double wet3b = MulSingle(d3b, c3b);                        // fmuls f6,f8,f10  (D first)
      const double blend3 = FmaddSingle(inv3b, b3b, wet3b);            // fmadds f5,f7,f9,f6
      StoreSingle(base, pf + 12, blend3);                              // stfsx f5,r17,r4

      pa += 16;
      pb += 16;                                                        // addi r11,r11,16
      pc += 16;                                                        // addi r4,r4,16
      pd += 16;                                                        // addi r5,r5,16
      pe += 16;                                                        // addi r31,r31,16
      pf += 16;
    } while (--groups != 0);                                           // bdnz 0x82b3d330
  }

  // loc_82B3D474: the remainder, one element at a time.
  // cmpw cr6,r15,r3 ; bge cr6,0x82b3d4f4
  if (tail_start >= count) return;
  // subf r5,r15,r3 ; mtctr r5 : exactly count - tail_start passes.
  uint32_t remaining = static_cast<uint32_t>(count - tail_start);
  // rlwinm r11,r15,2,0,29 ; add r11,r11,r7 : the cursor is &B[m], the rest base differences.
  const uint32_t offset = (static_cast<uint32_t>(tail_start) << 2) & 0xFFFFFFFCu;
  uint32_t qa = a + offset, qb = b + offset, qc = c + offset;
  uint32_t qd = d + offset, qe = e + offset, qf = f + offset;
  do {
    ctx.fpscr.disableFlushModeUnconditional();                         // before the first lfsx
    const double dm = LoadSingle(base, qd);                            // lfsx f13,r9,r11
    const double cm = LoadSingle(base, qc);                            // lfsx f12,r8,r11
    const double invm = SubSingle(one, dm);                            // fsubs f11,f0,f13
    const double wetm = MulSingle(dm, cm);                             // fmuls f10,f13,f12 (D first)
    const double bm = LoadSingle(base, qb);                            // lfs f9,0(r11)
    const double am = LoadSingle(base, qa);                            // lfsx f8,r5,r6
    const double drym = MulSingle(invm, bm);                           // fmuls f7,f11,f9
    const double wgm = MulSingle(wetm, gain_c);                        // fmuls f6,f10,f2
    const double mixm = FmaddSingle(drym, gain_b, wgm);                // fmadds f5,f7,f1,f6
    const double outm = AddSingle(mixm, am);                           // fadds f4,f5,f8
    StoreSingle(base, qe, outm);                                       // stfsx f4,r11,r10
    const double cm2 = LoadSingle(base, qc);                           // lfsx f3,r8,r11
    const double bm2 = LoadSingle(base, qb);                           // lfs f13,0(r11)
    const double dm2 = LoadSingle(base, qd);                           // lfsx f12,r9,r11
    const double invm2 = SubSingle(one, dm2);                          // fsubs f11,f0,f12
    const double wetm2 = MulSingle(dm2, cm2);                          // fmuls f10,f12,f3 (D first)
    const double blendm = FmaddSingle(invm2, bm2, wetm2);              // fmadds f9,f11,f13,f10
    StoreSingle(base, qf, blendm);                                     // stfsx f9,r7,r11
    qa += 4;
    qb += 4;                                                           // addi r11,r11,4
    qc += 4;
    qd += 4;
    qe += 4;
    qf += 4;
  } while (--remaining != 0);                                          // bdnz 0x82b3d4a0
}

// The call writes E (r10) and F (the ninth argument) and nothing else outside its own frame: the
// vector path in 64-byte blocks of four stvx128 each, the scalar paths in 16-byte groups plus a
// four-byte tail. Which path runs, and how many elements each takes, is fixed by the entry
// registers alone, so the whole set is enumerable. The vector stores go through lvx128's address
// masking, so their span starts at the base rounded DOWN to 16 bytes -- E and F are never checked
// for alignment.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const int32_t count = ctx.r3.s32;
  const uint32_t a = ctx.r6.u32;
  const uint32_t b = ctx.r7.u32;
  const uint32_t c = ctx.r8.u32;
  const uint32_t d = ctx.r9.u32;
  const uint32_t e = ctx.r10.u32;
  const uint32_t f = REX_LOAD_U32(ctx.r1.u32 + kNinthArgSlot);
  spec.read(ctx.r1.u32 + kNinthArgSlot, 4);  // lwz r30,84(r1): the ninth argument
  // The 1.0 the remainder loop reads whenever the vector path leaves elements over. Declared so a
  // recorded vector carries it; a read is never rewound and costs nothing against the budget.
  spec.read(kOneConstant, 4);

  int32_t done = 0;
  if (((b & 0xFu) == 0) && ((c & 0xFu) == 0)) {
    const bool shift_carry = (count < 0) && ((static_cast<uint32_t>(count) & 0xFu) != 0);
    const int32_t blocks = (count >> 4) + (shift_carry ? 1 : 0);
    if (static_cast<int32_t>(static_cast<uint32_t>(blocks) << 2) > 0) {
      // blocks passes of 64 bytes per buffer. blocks <= 0x07FFFFFF here (count is a positive
      // int32), so the product cannot overflow, but the address sum still can.
      const uint64_t span = static_cast<uint64_t>(static_cast<uint32_t>(blocks)) * 64ull;
      if (span > 0xFFFFFFFFull) return false;
      const uint32_t len = static_cast<uint32_t>(span);
      if (static_cast<uint64_t>(e & ~0xFu) + len > 0xFFFFFFFFull) return false;
      if (static_cast<uint64_t>(f & ~0xFu) + len > 0xFFFFFFFFull) return false;
      spec.write(e & ~0xFu, len);   // stvx128 v36/v34/v32/v60, masked down to 16 bytes
      spec.write(f & ~0xFu, len);   // stvx128 v41/v40/v39/v38, likewise
      spec.read(b, len);            // lvx128 x4 per pass; B and C are the aligned pair
      spec.read(c, len);
      spec.read(d & ~0xFu, len);    // A and D are read through the same masking
      spec.read(a & ~0xFu, len);
    }
    done = static_cast<int32_t>(static_cast<uint32_t>(blocks) << 4);
  }

  // The scalar paths cover [done, count) with plain stfs/stfsx, four bytes per element. When the
  // vector path consumed everything (count a multiple of 16) or the count is zero or negative,
  // loc_82B3D2A4's `bge` returns before any of this and the set is just the vector part.
  if (done < count) {
    const uint32_t off = (static_cast<uint32_t>(done) << 2) & 0xFFFFFFFCu;
    const uint64_t bytes = static_cast<uint64_t>(static_cast<uint32_t>(count - done)) * 4ull;
    if (bytes > 0xFFFFFFFFull) return false;
    const uint32_t len = static_cast<uint32_t>(bytes);
    if (static_cast<uint64_t>(e + off) + len > 0xFFFFFFFFull) return false;
    if (static_cast<uint64_t>(f + off) + len > 0xFFFFFFFFull) return false;
    spec.write(e + off, len);  // stfs f4,0(r31) and three stfsx, or stfsx f4,r11,r10
    spec.write(f + off, len);  // four stfsx, or stfsx f9,r7,r11
    spec.read(a + off, len);
    spec.read(b + off, len);
    spec.read(c + off, len);
    spec.read(d + off, len);
  }
  return true;
}

}  // namespace port_82B3D0A8
SKATE3_PORT(82B3D0A8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
