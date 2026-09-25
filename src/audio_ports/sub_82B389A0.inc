// sub_82B389A0  role: four-lane one-multiply allpass stage over an unaligned tap -- d = c - a*t stored to r8, and acc += g*(a*d + t) into r9; r10 == 0 dcbzl-clears acc first   lifted: skate3_recomp.67.cpp:64836 (280 lines)
// STATUS: verified
// calls/session: boot 50570  play 59894  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B389A0.md
namespace port_82B389A0 {

// Arguments, read off the lifted body; nothing in docs/rw_audio_structs.h names these buffers,
// so they keep letters taken from the arithmetic rather than invented struct names.
//   r3  s32  n      sample count
//   r6  ptr  c      read through lvx128, so 16-byte aligned in practice
//   r7  ptr  t      read UNALIGNED: each 16-byte group is lvlx(p) | lvrx(p+16)
//   r8  ptr  d      written with c - a*t
//   r9  ptr  acc    read and written with acc + g*(a*d + t); dcbzl-cleared first when r10 == 0
//   r10 s32  primed nonzero means acc already holds something, so only prefetch it
//   f1       a      the multiplier of the tap
//   f2       g      the gain the stage's output is accumulated with
// r4 and r5 are not read on entry: every use is preceded by a write.
//
// Per lane this is the one-multiply allpass pair `d = c - a*t ; y = a*d + t`, with y mixed into
// acc at gain g. Naming it allpass is an inference from that shape alone -- no symbol or plug-in
// metadata covers this function -- but the arithmetic itself is what the port reproduces.
//
// Address algebra: every lifted effective address reduces to one of the four bases plus a
// multiple of 16 (r22..r31 hold the constant inter-buffer deltas, and r8/r10/r11 walk by 64).
// `(A + 16m) & ~0xF == (A & ~0xF) + 16m`, so the lvx128/stvx128 masking distributes out of the
// loop and each buffer's traffic is one contiguous run.

// Purely an overflow guard on the window arithmetic. The harness's own 32 KB budget rejects
// anything past about 2,700 samples first (this function writes 12 bytes per sample).
constexpr int32_t kMaxEnumerableSamples = 0x01000000;

// lvx128: a 16-byte load with the address forced to a 16-byte boundary and the big-endian lane
// reversal RexGlue applies to every vector memory access.
inline simde__m128i LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                               simde_mm_load_si128((simde__m128i*)VectorMaskL));
}

// lvlx128: the bytes at or above the address within its 16-byte line, left-justified, the rest
// filled with 0xFF-selected zeroes by the mask row for that misalignment.
inline simde__m128i LoadVectorLeft(uint8_t* base, uint32_t address) {
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(address & ~0xFu)),
      simde_mm_load_si128((simde__m128i*)&VectorMaskL[(address & 0xF) * 16]));
}

// lvrx128: the opposite half, right-justified, and all zeroes on an aligned address -- the
// `temp & 0xF ? ... : setzero` of the lifted form, kept as the branch it is.
inline simde__m128i LoadVectorRight(uint8_t* base, uint32_t address) {
  if ((address & 0xF) == 0) return simde_mm_setzero_si128();
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(address & ~0xFu)),
      simde_mm_load_si128((simde__m128i*)&VectorMaskR[(address & 0xF) * 16]));
}

// stvx128: the same masking and lane reversal on the way out.
inline void StoreVector(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128(
      (simde__m128i*)REX_RAW_ADDR(ea),
      simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// stfs: round the double to single and store its bits.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

REX_FUNC(Native) {
  const uint32_t c_base = ctx.r6.u32;
  const uint32_t t_base = ctx.r7.u32;
  const uint32_t d_base = ctx.r8.u32;
  const uint32_t acc_base = ctx.r9.u32;

  // cmpwi cr6,r10,0 is taken on the INCOMING r10, before rlwinm r10,r3,2,0,29 overwrites it.
  const bool clear_accumulator = (ctx.r10.s32 == 0);       // bne cr6,0x82b389e0
  const uint32_t pass_bytes = (ctx.r3.u32 << 2) & 0xFFFFFFFCu;  // rlwinm r10,r3,2,0,29

  // loc_82B389C0, guarded by `cmplwi cr6,r10,0 ; beq`: clear the accumulator a cache line at a
  // time. The dcbt prefetches on r6/r7/r8 alongside it have no memory effect. loc_82B389E0 is
  // this same loop with the dcbzl omitted -- prefetch only -- and its exit value of r11 is
  // overwritten immediately below, so the r10 != 0 case has nothing to reproduce at all.
  if (clear_accumulator && pass_bytes != 0) {
    for (uint32_t off = 0; off < pass_bytes; off += 128) {  // cmplw cr6,r11,r10 ; blt (unsigned)
      const uint32_t ea = (acc_base + off) & ~127u;         // dcbzl r11,r9
      memset((void*)REX_RAW_ADDR(ea), 0, 128);
    }
  }

  // loc_82B38A10: srawi r11,r3,2 ; addze. r11,r11 -- the signed divide by 4 that truncates
  // toward zero, and the cr0 the `ble` below reads. The lifted form interleaves the addze
  // between the second and third store; the stores touch neither r11 nor CA.
  const int32_t quads = ctx.r3.s32 / 4;

  // Eight stfs into the function's own frame: four copies of a at -128(r1), four of g at
  // -112(r1), read back as two vectors below. Own frame, so never a window.
  ctx.fpscr.disableFlushModeUnconditional();
  const uint32_t coeff_slot = ctx.r1.u32 - 128;
  const uint32_t gain_slot = ctx.r1.u32 - 112;
  StoreSingle(base, coeff_slot + 0, ctx.f1.f64);   // stfs f1,-128(r1)
  StoreSingle(base, coeff_slot + 4, ctx.f1.f64);   // stfs f1,-124(r1)
  StoreSingle(base, coeff_slot + 8, ctx.f1.f64);   // stfs f1,-120(r1)
  StoreSingle(base, coeff_slot + 12, ctx.f1.f64);  // stfs f1,-116(r1)
  StoreSingle(base, gain_slot + 0, ctx.f2.f64);    // stfs f2,-112(r1)
  StoreSingle(base, gain_slot + 4, ctx.f2.f64);    // stfs f2,-108(r1)
  StoreSingle(base, gain_slot + 8, ctx.f2.f64);    // stfs f2,-104(r1)
  StoreSingle(base, gain_slot + 12, ctx.f2.f64);   // stfs f2,-100(r1)

  if (quads <= 0) return;  // ble 0x82b38b60

  // subf r3,r6,r9 -- the acc-to-c delta the loop addresses acc through, and the value the
  // function leaves in r3. Its one call site (skate3_recomp.67.cpp:67099) is a TAIL call, so r3
  // and v2 leave here as the caller's own ABI result registers; both are named in the mask and
  // reproduced rather than left as scratch. A 64-bit subtract, as lifted: r3 is compared whole.
  ctx.r3.u64 = ctx.r9.u64 - ctx.r6.u64;

  // addi r11,r11,-1 ; rlwinm r11,r11,30,2,31 ; addi r5,r11,1 ; mtctr r5
  // A logical shift right by two of quads-1, plus one: ceil(quads/4). This is a CEILING, so a
  // count whose quarter is not a multiple of four makes the loop run past n, up to the next
  // multiple of 16 samples. Reproduced, and the windows below use the same ceiling.
  const uint32_t iterations = ((static_cast<uint32_t>(quads) - 1) >> 2) + 1;

  // lvx128 v0,r0,r6 with r6 = r1-128 ; lvx128 v13,r0,r4 with r4 = r1-112
  const simde__m128 a = simde_mm_castsi128_ps(LoadVector(base, coeff_slot));
  const simde__m128 g = simde_mm_castsi128_ps(LoadVector(base, gain_slot));

  // v29, v30 and v31 are in the ABI-preserved range the harness compares on every call, and the
  // lifted body clobbers all three without saving them. Their live-out values are carried here.
  simde__m128i last_c3 = simde_mm_setzero_si128();    // v29
  simde__m128 last_y2 = simde_mm_setzero_ps();        // v30
  simde__m128i last_acc2 = simde_mm_setzero_si128();  // v31
  simde__m128i last_t3 = simde_mm_setzero_si128();    // v2, the vector ABI result register

  for (uint32_t k = 0; k < iterations; k++) {  // bdnz 0x82b38a98
    const uint32_t u = k * 64;                 // r8/r10/r11 all advance by 64 per iteration

    // The tap is read unaligned: each 16-byte group is lvlx(p) | lvrx(p+16), and the eight
    // partial loads are interleaved in the lifted order because they share address registers.
    const simde__m128i t0_left = LoadVectorLeft(base, t_base + u);  // lvlx128 v63,r5,r7
    const simde__m128i c2 = LoadVector(base, c_base + 32 + u);      // lvx128 v1,r0,r11
    const simde__m128i c0 = LoadVector(base, c_base + u);           // lvx128 v31,r11,r9
    const simde__m128i c1 = LoadVector(base, c_base + 16 + u);      // lvx128 v30,r11,r22
    const simde__m128i c3 = LoadVector(base, c_base + 48 + u);      // lvx128 v29,r11,r23

    const simde__m128i t2_left = LoadVectorLeft(base, t_base + 32 + u);    // lvlx128 v62,r6,r7
    const simde__m128i t2_right = LoadVectorRight(base, t_base + 48 + u);  // lvrx128 v61,r5,r7
    const simde__m128i t1_right = LoadVectorRight(base, t_base + 32 + u);  // lvrx128 v59,r6,r7
    const simde__m128 t2 = simde_mm_castsi128_ps(simde_mm_or_si128(t2_left, t2_right));
    // vor128 v5,v62,v61
    const simde__m128i t0_right = LoadVectorRight(base, t_base + 16 + u);  // lvrx128 v60,r4,r7
    const simde__m128i t1_left = LoadVectorLeft(base, t_base + 16 + u);    // lvlx128 v58,r4,r7
    const simde__m128 t0 = simde_mm_castsi128_ps(simde_mm_or_si128(t0_left, t0_right));
    // vor128 v4,v63,v60
    const simde__m128i t3_left = LoadVectorLeft(base, t_base + 48 + u);  // lvlx128 v57,r5,r7
    const simde__m128 t1 = simde_mm_castsi128_ps(simde_mm_or_si128(t1_left, t1_right));
    // vor128 v3,v58,v59
    const simde__m128i t3_right = LoadVectorRight(base, t_base + 64 + u);  // lvrx128 v56,r21,r7

    // vnmsubfp is -(x*y)+z, a single-rounding negative multiply-add. `a` is the FIRST factor in
    // every one of these and in the four vmaddfp below it; the accumulate pass flips the order
    // and puts y first. Per docs/vmx128-exactness.md rule 4 the operand slot is part of the
    // semantics on NaN, so every order here is copied from the lifted line, not normalised.
    ctx.fpscr.enableFlushModeUnconditional();  // emitted before this vnmsubfp
    const simde__m128 d2 = simde_mm_fnmadd_ps(a, t2, simde_mm_castsi128_ps(c2));
    // vnmsubfp v1,v0,v5,v1
    const simde__m128i t3_bits = simde_mm_or_si128(t3_left, t3_right);  // vor128 v2,v57,v56
    const simde__m128 t3 = simde_mm_castsi128_ps(t3_bits);
    const simde__m128i acc1 = LoadVector(base, acc_base + 16 + u);  // lvx128 v12,r10,r9
    const simde__m128 d0 = simde_mm_fnmadd_ps(a, t0, simde_mm_castsi128_ps(c0));
    // vnmsubfp v6,v0,v4,v31
    const simde__m128i acc2 = LoadVector(base, acc_base + 32 + u);  // lvx128 v31,r3,r11
    const simde__m128 d1 = simde_mm_fnmadd_ps(a, t1, simde_mm_castsi128_ps(c1));
    // vnmsubfp v8,v0,v3,v30
    const simde__m128i acc3 = LoadVector(base, acc_base + 48 + u);  // lvx128 v7,r0,r10
    const simde__m128i acc0 = LoadVector(base, acc_base + u);       // lvx128 v9,r10,r30
    const simde__m128 d3 = simde_mm_fnmadd_ps(a, t3, simde_mm_castsi128_ps(c3));
    // vnmsubfp v10,v0,v2,v29

    // The four y values fold each d back over its tap, and the four d stores are interleaved
    // between them in the lifted order d[2], d[0], d[1], d[3]. Every one of these is a single
    // vmaddfp: the `a*d` multiply and the `+ t` add must not become two rounded operations, and
    // the vnmsubfp/vmaddfp pair on the same lane must not fold into one.
    const simde__m128 y2 = simde_mm_fmadd_ps(a, d2, t2);  // vmaddfp v30,v0,v1,v5
    StoreVector(base, d_base + 32 + u, d2);               // stvx128 v1,r25,r11
    StoreVector(base, d_base + u, d0);                    // stvx128 v6,r0,r8
    const simde__m128 y0 = simde_mm_fmadd_ps(a, d0, t0);  // vmaddfp v5,v0,v6,v4
    const simde__m128 y1 = simde_mm_fmadd_ps(a, d1, t1);  // vmaddfp v11,v0,v8,v3
    StoreVector(base, d_base + 16 + u, d1);               // stvx128 v8,r6,r9
    const simde__m128 y3 = simde_mm_fmadd_ps(a, d3, t3);  // vmaddfp v6,v0,v10,v2
    StoreVector(base, d_base + 48 + u, d3);               // stvx128 v10,r10,r31

    const simde__m128 out2 = simde_mm_fmadd_ps(y2, g, simde_mm_castsi128_ps(acc2));
    // vmaddfp v8,v30,v13,v31
    const simde__m128 out0 = simde_mm_fmadd_ps(y0, g, simde_mm_castsi128_ps(acc0));
    // vmaddfp v10,v5,v13,v9
    const simde__m128 out1 = simde_mm_fmadd_ps(y1, g, simde_mm_castsi128_ps(acc1));
    // vmaddfp v11,v11,v13,v12
    const simde__m128 out3 = simde_mm_fmadd_ps(y3, g, simde_mm_castsi128_ps(acc3));
    // vmaddfp v12,v6,v13,v7

    StoreVector(base, acc_base + 32 + u, out2);  // stvx128 v8,r3,r11
    StoreVector(base, acc_base + u, out0);       // stvx128 v10,r10,r30
    StoreVector(base, acc_base + 16 + u, out1);  // stvx128 v11,r10,r9
    StoreVector(base, acc_base + 48 + u, out3);  // stvx128 v12,r0,r10

    last_c3 = c3;
    last_y2 = y2;
    last_acc2 = acc2;
    last_t3 = t3_bits;
  }

  // loc_82B38B60. v29 holds the last c3 load, v30 the last y2, v31 the last acc[2] load -- taken
  // before the store to that same address, so the pre-store bytes -- and v2 the last tap group.
  // The loop always runs at least once to get here, so none of these are the zero initialisers.
  // v29-v31 are in the ABI-preserved range the harness compares unconditionally; v2 is named in
  // the mask because of the tail call. r21-r31 come back through __restgprlr_21 and are not
  // touched above; the rest of what the lifted body leaves behind (v0, v1, v3-v13, v56-v63) is
  // volatile scratch no caller can read.
  simde_mm_store_si128((simde__m128i*)ctx.v29.u8, last_c3);
  simde_mm_store_ps(ctx.v30.f32, last_y2);
  simde_mm_store_si128((simde__m128i*)ctx.v31.u8, last_acc2);
  simde_mm_store_si128((simde__m128i*)ctx.v2.u8, last_t3);
}

// Three contiguous runs: the dcbzl clear of acc (128-byte lines from its 128-byte floor), and the
// main loop's 64 bytes per iteration into d and acc from their 16-byte floors. Both trip counts
// are closed forms of r3 -- ceil(4n/128) lines and ceil((n/4)/4) iterations -- and the r10 flag
// picks the clear, so the whole write set is known on entry.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const int32_t n = ctx.r3.s32;
  // A negative count makes `rlwinm r10,r3,2,0,29` an unsigned length near 2^32 and sends the
  // dcbzl loop over gigabytes: that write set is real but not enumerable, so refuse it.
  if (n < 0) return false;
  if (n == 0) return true;  // pass_bytes 0 skips the clear, quads 0 takes the `ble` out
  if (n > kMaxEnumerableSamples) return false;

  const uint32_t c_base = ctx.r6.u32;
  const uint32_t t_base = ctx.r7.u32;
  const uint32_t d_base = ctx.r8.u32;
  const uint32_t acc_base = ctx.r9.u32;

  if (ctx.r10.s32 == 0) {
    const uint32_t pass_bytes = (static_cast<uint32_t>(n) << 2) & 0xFFFFFFFCu;
    const uint32_t lines = (pass_bytes + 127) / 128;  // the loop exits at the first r11 >= r10
    spec.write(acc_base & ~127u, lines * 128);
  }

  const uint32_t quads = static_cast<uint32_t>(n / 4);
  if (quads != 0) {
    const uint32_t bytes = (((quads - 1) >> 2) + 1) * 64;  // the loop's own ceiling
    spec.write(d_base & ~0xFu, bytes);
    spec.write(acc_base & ~0xFu, bytes);
    spec.read(c_base & ~0xFu, bytes);
    spec.read(acc_base & ~0xFu, bytes);
    // The tap's last lvrx reaches the line holding t + 64 + 64*(iterations-1), one line past the
    // run, and an unaligned base makes the first lvlx start at the line below it.
    spec.read(t_base & ~0xFu, bytes + 16);
  }
  return true;
}

}  // namespace port_82B389A0
SKATE3_PORT(82B389A0, skate3::audio::kPortVerified,
            skate3::audio::Gpr(3) | skate3::audio::Vr(2))
