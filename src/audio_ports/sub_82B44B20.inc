// sub_82B44B20  role: dst[i] += src[i] * scale over a float array -- 32 floats per iteration on a vector path when both buffers are 128-byte aligned and the count is a multiple of 64, a scalar loop otherwise   lifted: skate3_recomp.68.cpp:27676 (321 lines)
// STATUS: verified
// calls/session: boot 3738884  play 6110212  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B44B20.md
namespace port_82B44B20 {

// Arguments, read straight off the lifted entry: r3 destination floats (read-modify-write),
// r4 source floats, r6 the float count, f1 the scale. Nothing in docs/rw_audio_structs.h names
// either buffer, so there are no offset constants -- the structure is "an array of floats".
// This is sub_82B3BED8's accumulating twin: the same two paths and the same window, with a fused
// multiply-add against the destination in place of a plain multiply.

// lvx128: sixteen bytes with the low four address bits masked off, plus the big-endian lane
// reversal RexGlue applies to every vector memory access. Returned as float lanes; same bits.
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

// stfs: the double rounded to single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fmadds: a single-rounding double-precision FMA, then rounded to single. std::fma, never
// `a * b + c`.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

REX_FUNC(Native) {
  const uint32_t dst = ctx.r3.u32;    // mr r11,r3
  const uint32_t src = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;

  // or r10,r3,r4 ; clrlwi r9,r10,25 ; bne cr6 : both buffers 128-byte aligned, and
  // clrlwi r10,r6,26 ; bne cr6 : the count a multiple of 64. Either test failing takes the
  // scalar path at loc_82B44C44.
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    // rlwinm r10,r6,2,0,27 ; add r30,r10,r4 : the source end, count*4 bytes on.
    const uint32_t span = (count << 2) & 0xFFFFFFF0u;
    const uint32_t src_end = src + span;

    // stfs f1,-48(r1) .. stfs f1,-36(r1) ; lvx128 v0,r0,r9 : the scale in all four lanes. The
    // four words are identical, so the load's lane reversal is a no-op on them and the scratch
    // at -48(r1) is this function's own frame -- the native body keeps it in a register. (That
    // assumes the guest r1 is 16-byte aligned, which the lvx128 address masking relies on in the
    // lifted form too; see the note.)
    ctx.fpscr.disableFlushModeUnconditional();               // emitted by the first stfs
    const simde__m128 scale = simde_mm_set1_ps(static_cast<float>(ctx.f1.f64));

    // The lifted cursors r10 and r11 run 32 bytes ahead of the block they address (r9 = r11-32
    // is re-derived each iteration); s and d here are the block bases, so the offsets below are
    // the lifted effective addresses, not the lifted displacements.
    uint32_t s = src;  // r10 - 32
    uint32_t d = dst;  // r11 - 32
    do {
      // Eight lvx128 off r10, then the cursor advance; the dcbt hints lift to nothing.
      const simde__m128 a0 = LoadVectorPs(base, s + 0);    // lvx128 v11,r10,r4   (r4 = -32)
      const simde__m128 a1 = LoadVectorPs(base, s + 16);   // lvx128 v10,r10,r3   (r3 = -16)
      const simde__m128 a2 = LoadVectorPs(base, s + 32);   // lvx128 v9,r0,r10
      const simde__m128 a3 = LoadVectorPs(base, s + 48);   // lvx128 v5,r10,r31   (r31 = 16)
      const simde__m128 a4 = LoadVectorPs(base, s + 64);   // lvx128 v4,r10,r5    (r5 = 32)
      const simde__m128 a5 = LoadVectorPs(base, s + 80);   // lvx128 v3,r10,r6    (r6 = 48)
      const simde__m128 a6 = LoadVectorPs(base, s + 96);   // lvx128 v2,r10,r7    (r7 = 64)
      const simde__m128 a7 = LoadVectorPs(base, s + 112);  // lvx128 v1,r10,r8    (r8 = 80)
      s += 128;                                            // addi r10,r10,128

      // The destination is loaded in pairs, interleaved with the FMAs exactly as lifted. Every
      // load of this block happens before every store of it, which is what makes an overlapping
      // src/dst behave as the original does.
      const simde__m128 b2 = LoadVectorPs(base, d + 32);   // lvx128 v12,r0,r11
      const simde__m128 b0 = LoadVectorPs(base, d + 0);    // lvx128 v13,r0,r9

      // Single-rounding FMAs. The scale is the FIRST operand of every one, as lifted: with two
      // NaN operands the winning slot is part of the semantics (vmx128-exactness rule 4), so the
      // order is copied rather than rearranged.
      ctx.fpscr.enableFlushModeUnconditional();            // before the first vmaddfp
      const simde__m128 out2 = simde_mm_fmadd_ps(scale, a2, b2);  // vmaddfp v6,v0,v9,v12
      const simde__m128 out0 = simde_mm_fmadd_ps(scale, a0, b0);  // vmaddfp v7,v0,v11,v13
      const simde__m128 b1 = LoadVectorPs(base, d + 16);   // lvx128 v12,r11,r3
      const simde__m128 b3 = LoadVectorPs(base, d + 48);   // lvx128 v13,r11,r31
      const simde__m128 out1 = simde_mm_fmadd_ps(scale, a1, b1);  // vmaddfp v8,v0,v10,v12
      const simde__m128 out3 = simde_mm_fmadd_ps(scale, a3, b3);  // vmaddfp v9,v0,v5,v13
      const simde__m128 b4 = LoadVectorPs(base, d + 64);   // lvx128 v12,r11,r5
      const simde__m128 b5 = LoadVectorPs(base, d + 80);   // lvx128 v13,r11,r6
      const simde__m128 out4 = simde_mm_fmadd_ps(scale, a4, b4);  // vmaddfp v10,v0,v4,v12
      const simde__m128 out5 = simde_mm_fmadd_ps(scale, a5, b5);  // vmaddfp v11,v0,v3,v13
      const simde__m128 b6 = LoadVectorPs(base, d + 96);   // lvx128 v12,r11,r7
      const simde__m128 b7 = LoadVectorPs(base, d + 112);  // lvx128 v13,r11,r8
      const simde__m128 out6 = simde_mm_fmadd_ps(scale, a6, b6);  // vmaddfp v12,v0,v2,v12
      const simde__m128 out7 = simde_mm_fmadd_ps(scale, a7, b7);  // vmaddfp v13,v0,v1,v13

      // The store order is the lifted one: +32, +0, +16, +48, +64, +80, +96, +112.
      StoreVectorPs(base, d + 32, out2);                     // stvx128 v6,r0,r11
      StoreVectorPs(base, d + 0, out0);                      // stvx128 v7,r0,r9
      StoreVectorPs(base, d + 16, out1);                     // stvx128 v8,r11,r3
      StoreVectorPs(base, d + 48, out3);                     // stvx128 v9,r11,r31
      StoreVectorPs(base, d + 64, out4);                     // stvx128 v10,r11,r5
      StoreVectorPs(base, d + 80, out5);                     // stvx128 v11,r11,r6
      StoreVectorPs(base, d + 96, out6);                     // stvx128 v12,r11,r7
      StoreVectorPs(base, d + 112, out7);                    // stvx128 v13,r11,r8
      d += 128;                                            // addi r11,r11,128
      // cmplw cr6,r4,r30 ; bne cr6,0x82b44b94, where r4 is r10-32, i.e. s. A span that is zero
      // or not a multiple of 128 never terminates; the original hangs there too, so it is
      // reproduced, not guarded.
    } while (s != src_end);
    return;
  }

  // loc_82B44C44, the scalar path: any alignment, any count.
  // rlwinm r9,r6,2,0,29 ; add r9,r9,r11 : the destination end, count*4 bytes on.
  const uint32_t dst_end = dst + ((count << 2) & 0xFFFFFFFCu);
  // cmplw cr6,r11,r9 ; bge cr6,0x82b44d14
  if (dst >= dst_end) return;

  uint32_t s = src;  // r10
  uint32_t d = dst;  // r11

  // subf r8,r11,r9 ; addi r8,r8,3 ; srawi r7,r8,2 ; addze r6,r7 : the float count. len is a
  // positive multiple of four here, so this is len/4; the carry term is srawi's CA, reproduced
  // because addze consumes it.
  const uint32_t len = dst_end - d;
  const int32_t biased = static_cast<int32_t>(len + 3u);
  const bool shift_carry = (biased < 0) && ((static_cast<uint32_t>(biased) & 3u) != 0);
  const int32_t floats = (biased >> 2) + (shift_carry ? 1 : 0);

  // cmpwi cr6,r6,4 ; blt cr6,0x82b44cdc
  if (floats >= 4) {
    // addi r6,r7,-13 ; rlwinm r6,r6,28,4,31 ; addi r6,r6,1 ; mtctr r6 : len/16 groups of four.
    // r7 tracks r8+r11 and equals r10 at the top of every iteration, so `4(r7)` is just src[1];
    // it is written that way here.
    uint32_t groups = ((len - 13u) >> 4) + 1u;
    do {
      ctx.fpscr.disableFlushModeUnconditional();            // before the first lfs
      const double c0 = LoadSingle(base, d + 0);            // lfs f0,0(r11)
      const double c1 = LoadSingle(base, d + 4);            // lfs f13,4(r11)
      const double c2 = LoadSingle(base, d + 8);            // lfs f12,8(r11)
      const double c3 = LoadSingle(base, d + 12);           // lfs f11,12(r11)
      const double a0 = LoadSingle(base, s + 0);            // lfs f10,0(r10)
      const double a1 = LoadSingle(base, s + 4);            // lfs f9,4(r7)
      const double r0 = FmaddSingle(a0, ctx.f1.f64, c0);    // fmadds f8,f10,f1,f0
      const double a2 = LoadSingle(base, s + 8);            // lfs f7,8(r10)
      const double r1 = FmaddSingle(a1, ctx.f1.f64, c1);    // fmadds f6,f9,f1,f13
      const double a3 = LoadSingle(base, s + 12);           // lfs f5,12(r10)
      const double r2 = FmaddSingle(a2, ctx.f1.f64, c2);    // fmadds f4,f7,f1,f12
      const double r3 = FmaddSingle(a3, ctx.f1.f64, c3);    // fmadds f3,f5,f1,f11
      StoreSingle(base, d + 0, r0);                         // stfs f8,0(r11)
      StoreSingle(base, d + 4, r1);                         // stfs f6,4(r11)
      s += 16;                                              // addi r10,r10,16
      StoreSingle(base, d + 8, r2);                         // stfs f4,8(r11)
      StoreSingle(base, d + 12, r3);                        // stfs f3,12(r11)
      d += 16;                                              // addi r11,r11,16
    } while (--groups != 0);                                // bdnz 0x82b44c8c
  }

  // loc_82B44CDC: the remainder, one float at a time.
  // cmplw cr6,r11,r9 ; bge cr6,0x82b44d14
  if (d >= dst_end) return;
  // subf r9,r11,r9 ; addi r9,r9,-1 ; rlwinm r9,r9,30,2,31 ; addi r9,r9,1 ; mtctr r9
  uint32_t tail = ((dst_end - d - 1u) >> 2) + 1u;
  uint32_t dp = d - 4;                                      // addi r11,r11,-4
  uint32_t sp = s - 4;                                      // addi r10,r10,-4
  do {
    ctx.fpscr.disableFlushModeUnconditional();              // before the lfs
    const double c = LoadSingle(base, dp + 4);              // lfs f13,4(r11), no update
    sp += 4;                                                // lfsu f0,4(r10) updates first
    const double a = LoadSingle(base, sp);
    const double r = FmaddSingle(a, ctx.f1.f64, c);         // fmadds f12,f0,f1,f13
    dp += 4;                                                // stfsu f12,4(r11) updates first
    StoreSingle(base, dp, r);
  } while (--tail != 0);                                    // bdnz 0x82b44d00
}

// Every byte the call writes is [r3, r3 + count*4): the vector loop covers it in 128-byte blocks
// of eight stvx128, the scalar loop in 16-byte groups plus a 4-byte tail. Identical to
// sub_82B3BED8's window; only the fast-path end-pointer arithmetic differs (rlwinm here, srawi
// plus rlwinm there), so it is computed each way as the original does.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;

  uint32_t len;
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    // The vector path. rlwinm r10,r6,2,0,27 : the byte span the loop walks.
    len = (count << 2) & 0xFFFFFFF0u;
    // The loop exits only on (r10 - 32) == r4 + len after whole 128-byte steps. A zero span, or
    // one that is not a multiple of 128 (count negative, or count*4 overflowing), never
    // terminates, so the write set is unbounded. count == 0 with both pointers aligned is that
    // case.
    if (len == 0 || (len & 127u) != 0) return false;
  } else {
    // loc_82B44C44. rlwinm r9,r6,2,0,29 ; add r9,r9,r11 ; cmplw ; bge -> immediate return.
    len = (count << 2) & 0xFFFFFFFCu;
    if (dst + len <= dst) len = 0;  // dst >= dst_end: the call writes nothing at all
  }
  if (static_cast<uint64_t>(dst) + len > 0xFFFFFFFFull) return false;  // would wrap the address space

  spec.write(dst, len);  // stvx128 x8, or stfs x4 / stfsu, over [dst, dst+len)
  spec.read(src, len);   // lvx128 x8, or lfs x4 / lfsu, over [src, src+len)
  spec.read(dst, len);   // the accumulator is read before it is written; declared for replay
  return true;
}

}  // namespace port_82B44B20
SKATE3_PORT(82B44B20, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
