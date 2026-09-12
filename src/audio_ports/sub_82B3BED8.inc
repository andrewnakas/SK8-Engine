// sub_82B3BED8  role: dst[i] = src[i] * scale over a float array -- 32 floats per iteration on a vector path when both buffers are 128-byte aligned and the count is a multiple of 64, a scalar loop otherwise   lifted: skate3_recomp.68.cpp:6345 (280 lines)
// STATUS: verified
// calls/session: boot 2843558  play 4411988  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3BED8.md
namespace port_82B3BED8 {

// Arguments, read straight off the lifted entry: r3 destination floats, r4 source floats,
// r6 the float count, f1 the scale. Nothing in docs/rw_audio_structs.h names this buffer, so
// there are no offset constants to use -- the only structure here is "an array of floats".

// lvx128: sixteen bytes with the low four address bits masked off, plus the big-endian lane
// reversal RexGlue applies to every vector memory access. Returned as float lanes because every
// consumer here is float; the bits are the same either way.
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

// fmuls: the double product rounded to single. Never an FMA -- there is no fmadd in this
// function, and nothing adjacent for a compiler to contract with.
inline double MulSingle(double a, double b) {
  return static_cast<double>(static_cast<float>(a * b));
}

REX_FUNC(Native) {
  const uint32_t dst = ctx.r3.u32;    // mr r11,r3
  const uint32_t src = ctx.r4.u32;    // mr r10,r4
  const uint32_t count = ctx.r6.u32;

  // or r9,r3,r4 ; clrlwi r8,r9,25 ; bne cr6 : both buffers 128-byte aligned, and
  // clrlwi r9,r6,26 ; bne cr6 : the count a multiple of 64. Either test failing takes the
  // scalar path at loc_82B3BFD4.
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    // srawi r5,r6,2 ; rlwinm r5,r5,4,0,27 ; add r5,r5,r4 : the source end, count*4 bytes on.
    const uint32_t span = (static_cast<uint32_t>(static_cast<int32_t>(count) >> 2) << 4) &
                          0xFFFFFFF0u;
    const uint32_t src_end = src + span;

    // stfs f1,-32(r1) .. stfs f1,-20(r1) ; lvx128 v63,r0,r30 : the scale in all four lanes.
    // The four words are identical, so the load's lane reversal is a no-op on them and the
    // scratch at -32(r1) is this function's own frame -- the native body keeps it in a register.
    // (This assumes the guest r1 is 16-byte aligned, which the lvx128 address masking relies on
    // in the lifted form too; see the note.)
    ctx.fpscr.disableFlushModeUnconditional();               // emitted by the first stfs
    const simde__m128 scale = simde_mm_set1_ps(static_cast<float>(ctx.f1.f64));

    uint32_t s = src;  // r10
    uint32_t d = dst;  // r11
    do {
      // Eight lvx128 off r10, then the cursor advance; the dcbt hints lift to nothing.
      const simde__m128 a0 = LoadVectorPs(base, s + 0);    // lvx128 v62,r0,r10
      const simde__m128 a1 = LoadVectorPs(base, s + 16);   // lvx128 v61,r10,r9   (r9 = 16)
      const simde__m128 a2 = LoadVectorPs(base, s + 32);   // lvx128 v60,r10,r8   (r8 = 32)
      const simde__m128 a3 = LoadVectorPs(base, s + 48);   // lvx128 v59,r10,r7   (r7 = 48)
      const simde__m128 a4 = LoadVectorPs(base, s + 64);   // lvx128 v58,r10,r6   (r6 = 64)
      const simde__m128 a5 = LoadVectorPs(base, s + 80);   // lvx128 v57,r10,r31  (r31 = 80)
      const simde__m128 a6 = LoadVectorPs(base, s + 96);   // lvx128 v56,r10,r3   (r3 = 96)
      const simde__m128 a7 = LoadVectorPs(base, s + 112);  // lvx128 v55,r10,r4   (r4 = 112)
      s += 128;                                            // addi r10,r10,128

      // Eight separate multiplies. The scale is the FIRST operand of every one, as lifted:
      // with two NaN operands the winning slot is part of the semantics (vmx128-exactness
      // rule 4), so the order is copied rather than rearranged.
      ctx.fpscr.enableFlushModeUnconditional();            // before the first vmulfp128
      const simde__m128 p0 = simde_mm_mul_ps(scale, a0);   // vmulfp128 v62,v63,v62
      const simde__m128 p1 = simde_mm_mul_ps(scale, a1);   // vmulfp128 v61,v63,v61
      const simde__m128 p2 = simde_mm_mul_ps(scale, a2);   // vmulfp128 v60,v63,v60
      const simde__m128 p3 = simde_mm_mul_ps(scale, a3);   // vmulfp128 v59,v63,v59
      const simde__m128 p4 = simde_mm_mul_ps(scale, a4);   // vmulfp128 v58,v63,v58
      const simde__m128 p5 = simde_mm_mul_ps(scale, a5);   // vmulfp128 v57,v63,v57
      const simde__m128 p6 = simde_mm_mul_ps(scale, a6);   // vmulfp128 v56,v63,v56
      const simde__m128 p7 = simde_mm_mul_ps(scale, a7);   // vmulfp128 v55,v63,v55

      // dcbzl r0,r11: zeroes the 128-byte line at d & ~127. d is 128-byte aligned on this path,
      // so every byte of it is overwritten by the eight stores below -- kept anyway, in place,
      // because it is a store the original makes.
      memset((void*)REX_RAW_ADDR(d & ~127u), 0, 128);

      // The store order is the lifted one: +16, +32, +48, +64, then +0, then +80, +96, +112.
      StoreVectorPs(base, d + 16, p1);                     // stvx128 v61,r11,r9
      StoreVectorPs(base, d + 32, p2);                     // stvx128 v60,r11,r8
      StoreVectorPs(base, d + 48, p3);                     // stvx128 v59,r11,r7
      StoreVectorPs(base, d + 64, p4);                     // stvx128 v58,r11,r6
      StoreVectorPs(base, d + 0, p0);                      // stvx128 v62,r0,r11
      StoreVectorPs(base, d + 80, p5);                      // stvx128 v57,r11,r31
      StoreVectorPs(base, d + 96, p6);                      // stvx128 v56,r11,r3
      StoreVectorPs(base, d + 112, p7);                     // stvx128 v55,r11,r4
      d += 128;                                             // addi r11,r11,128
      // cmplw cr6,r10,r5 ; bne cr6,0x82b3bf48. A span that is zero or not a multiple of 128
      // never terminates; the original hangs there too, so it is reproduced, not guarded.
    } while (s != src_end);
    return;
  }

  // loc_82B3BFD4, the scalar path: any alignment, any count.
  // rlwinm r9,r6,2,0,29 ; add r9,r9,r11 : the destination end, count*4 bytes on.
  const uint32_t dst_end = dst + ((count << 2) & 0xFFFFFFFCu);
  // cmplw cr6,r11,r9 ; bge cr6,0x82b3c08c
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

  // cmpwi cr6,r6,4 ; blt cr6,0x82b3c058
  if (floats >= 4) {
    // addi r6,r7,-13 ; rlwinm r6,r6,28,4,31 ; addi r6,r6,1 ; mtctr r6 : len/16 groups of four.
    // r7 tracks r8+r11 and is equal to r10 at the top of every iteration, so `4(r7)` is just
    // src[1]; it is written that way here.
    uint32_t groups = ((len - 13u) >> 4) + 1u;
    do {
      ctx.fpscr.disableFlushModeUnconditional();            // before the first lfs
      const double a0 = LoadSingle(base, s + 0);            // lfs f0,0(r10)
      const double a1 = LoadSingle(base, s + 4);            // lfs f13,4(r7)
      const double a2 = LoadSingle(base, s + 8);            // lfs f11,8(r10)
      const double a3 = LoadSingle(base, s + 12);           // lfs f9,12(r10)
      // Four fmuls, the scale second as lifted.
      const double p0 = MulSingle(a0, ctx.f1.f64);          // fmuls f12,f0,f1
      const double p1 = MulSingle(a1, ctx.f1.f64);          // fmuls f10,f13,f1
      const double p2 = MulSingle(a2, ctx.f1.f64);          // fmuls f8,f11,f1
      const double p3 = MulSingle(a3, ctx.f1.f64);          // fmuls f7,f9,f1
      // Every load above happens before every store below, which is what makes an overlapping
      // src/dst behave as the original does.
      StoreSingle(base, d + 0, p0);                         // stfs f12,0(r11)
      StoreSingle(base, d + 4, p1);                         // stfs f10,4(r11)
      s += 16;                                              // addi r10,r10,16
      StoreSingle(base, d + 8, p2);                         // stfs f8,8(r11)
      StoreSingle(base, d + 12, p3);                        // stfs f7,12(r11)
      d += 16;                                              // addi r11,r11,16
    } while (--groups != 0);                                // bdnz 0x82b3c018
  }

  // loc_82B3C058: the remainder, one float at a time.
  // cmplw cr6,r11,r9 ; bge cr6,0x82b3c08c
  if (d >= dst_end) return;
  // subf r8,r11,r9 ; addi r8,r8,-1 ; rlwinm r10,r8,30,2,31 ; addi r10,r10,1 ; mtctr r10
  uint32_t tail = ((dst_end - d - 1u) >> 2) + 1u;
  uint32_t sp = s - 4;                                      // addi r11,r10,-4
  uint32_t dp = d - 4;                                      // addi r9,r11,-4
  do {
    ctx.fpscr.disableFlushModeUnconditional();              // before the lfsu
    sp += 4;                                                // lfsu f0,4(r11) updates first
    const double a = LoadSingle(base, sp);
    const double p = MulSingle(a, ctx.f1.f64);              // fmuls f0,f0,f1
    dp += 4;                                                // stfsu f0,4(r9) updates first
    StoreSingle(base, dp, p);
  } while (--tail != 0);                                    // bdnz 0x82b3c07c
}

// Every byte the call writes is [r3, r3 + count*4): the vector loop covers it in 128-byte
// blocks (dcbzl plus eight stvx128), the scalar loop in 16-byte groups plus a 4-byte tail.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;

  uint32_t len;
  if ((((dst | src) & 0x7Fu) == 0) && ((count & 0x3Fu) == 0)) {
    // The vector path. srawi r5,r6,2 ; rlwinm r5,r5,4,0,27 : the byte span the loop walks.
    len = (static_cast<uint32_t>(static_cast<int32_t>(count) >> 2) << 4) & 0xFFFFFFF0u;
    // The loop exits only on r10 == r4 + len after whole 128-byte steps. A zero span, or one
    // that is not a multiple of 128 (count negative, or count*4 overflowing), never terminates,
    // so the write set is unbounded. count == 0 with both pointers aligned is that case.
    if (len == 0 || (len & 127u) != 0) return false;
  } else {
    // loc_82B3BFD4. rlwinm r9,r6,2,0,29 ; add r9,r9,r11 ; cmplw ; bge -> immediate return.
    len = (count << 2) & 0xFFFFFFFCu;
    if (dst + len <= dst) len = 0;  // dst >= dst_end: the call writes nothing at all
  }
  if (static_cast<uint64_t>(dst) + len > 0xFFFFFFFFull) return false;  // would wrap the address space

  spec.write(dst, len);  // dcbzl + stvx128 x8, or stfs x4 / stfsu, over [dst, dst+len)
  spec.read(src, len);   // lvx128 x8, or lfs x4 / lfsu, over [src, src+len)
  return true;
}

}  // namespace port_82B3BED8
SKATE3_PORT(82B3BED8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
