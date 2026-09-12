// sub_82B3CF58  role: n-sample scale-and-add with a parallel copy of the source -- z[i] = x[i]*gain + y[i], then w[i] = x[i] reloaded after that store; 16 floats per vector iteration when x is 16-byte aligned, scalar tail for the remainder   lifted: skate3_recomp.68.cpp:8881 (218 lines)
// STATUS: verified
// calls/session: boot 179218  play 339926  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3CF58.md
namespace port_82B3CF58 {

// Arguments, read off the lifted body; nothing in docs/rw_audio_structs.h names these buffers,
// so they keep plain letters rather than invented struct names.
//   r3  s32  n      sample count
//   r5  ptr  y      addend, read only
//   r6  ptr  x      source, read only; its low four bits select the vector or the scalar path
//   r7  ptr  z      written with x*gain + y
//   r8  ptr  w      written with x, RELOADED after the z store -- so an in-place call (z == x)
//                   copies the scaled result, not the original. Reproduced, not fixed.
//   f1       gain
// r4 is not read on entry: every use is preceded by a write.
//
// Address algebra in the vector loop: every lifted effective address reduces to one of the four
// bases plus a multiple of 16 (the loop's r9/r10/r11 walk by 64 and r21..r31 hold the constant
// inter-buffer deltas). `(A + 16m) & ~0xF == (A & ~0xF) + 16m`, so the lvx128/stvx128 address
// masking distributes out of the loop and each buffer's traffic is one contiguous run.

// Purely an overflow guard on the window arithmetic. The harness's own 32 KB budget rejects
// anything past about 4,000 samples first (this function writes 8 bytes per sample).
constexpr int32_t kMaxEnumerableSamples = 0x01000000;

// lvx128: a 16-byte load with the address forced to a 16-byte boundary and the big-endian lane
// reversal RexGlue applies to every vector memory access.
inline simde__m128i LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                               simde_mm_load_si128((simde__m128i*)VectorMaskL));
}

// stvx128: the same masking and lane reversal on the way out.
inline void StoreVector(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128(
      (simde__m128i*)REX_RAW_ADDR(ea),
      simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: round the double to single and store its bits.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fmadds: the double-precision fused multiply-add rounded to single, as lifted. Single rounding
// in the fma, then a second at the frsp -- not `float(a*b+c)`.
inline double FmaddSingle(double a, double b, double c) {
  return static_cast<double>(static_cast<float>(std::fma(a, b, c)));
}

REX_FUNC(Native) {
  const uint32_t y_base = ctx.r5.u32;
  const uint32_t x_base = ctx.r6.u32;
  const uint32_t z_base = ctx.r7.u32;
  const uint32_t w_base = ctx.r8.u32;

  uint32_t done = 0;  // li r11,0 -- samples consumed by the vector body

  // clrlwi r10,r6,28 ; cmplwi cr6,r10,0 ; bne cr6,0x82b3d05c
  if ((x_base & 0xF) == 0) {
    // srawi r11,r3,4 ; addze r20,r11 : the signed divide by 16 that truncates toward zero
    // (srawi's carry plus addze is exactly that). The lifted form interleaves the addze between
    // the second and third store below; the stores touch neither r11 nor CA.
    const int32_t blocks = ctx.r3.s32 / 16;

    // stfs f1,-128(r1) / -124 / -120 / -116 : four copies of the single-rounded gain in the
    // function's own frame, read back as one vector below. Own frame, so never a window.
    ctx.fpscr.disableFlushModeUnconditional();
    const uint32_t splat_slot = ctx.r1.u32 - 128;
    StoreSingle(base, splat_slot + 0, ctx.f1.f64);   // stfs f1,-128(r1)
    StoreSingle(base, splat_slot + 4, ctx.f1.f64);   // stfs f1,-124(r1)
    StoreSingle(base, splat_slot + 8, ctx.f1.f64);   // stfs f1,-120(r1)
    StoreSingle(base, splat_slot + 12, ctx.f1.f64);  // stfs f1,-116(r1)

    // rlwinm r11,r20,2,0,29 ; cmpwi cr6,r11,0 ; ble cr6,0x82b3d058
    const int32_t quarter_count = static_cast<int32_t>(static_cast<uint32_t>(blocks) << 2);
    if (quarter_count > 0) {
      // addi r11,r11,-1 ; rlwinm r11,r11,30,2,31 ; addi r4,r11,1 ; mtctr r4
      // A logical shift right by two of 4*blocks-1, plus one: exactly `blocks`, kept in the
      // lifted form so the derivation is visible.
      const uint32_t iterations = ((static_cast<uint32_t>(quarter_count) - 1) >> 2) + 1;

      // lvx128 v0,r0,r4 with r4 = r1-128 : the gain in all four lanes.
      const simde__m128 gain = simde_mm_castsi128_ps(LoadVector(base, splat_slot));

      for (uint32_t k = 0; k < iterations; k++) {  // bdnz 0x82b3cfe0
        const uint32_t u = k * 64;                 // r9/r10/r11 all advance by 64 per iteration

        const simde__m128 x0 = simde_mm_castsi128_ps(LoadVector(base, x_base + u));
        // lvx128 v13,r10,r24 (r10-48)
        const simde__m128 y0 = simde_mm_castsi128_ps(LoadVector(base, y_base + u));
        // lvx128 v12,r9,r25

        // vmaddfp takes the gain as its FIRST factor here. The scalar tail below passes the
        // sample first; per docs/vmx128-exactness.md rule 4 the operand slot is part of the
        // semantics on NaN, so both orders are copied from the lifted lines, not normalised.
        ctx.fpscr.enableFlushModeUnconditional();               // emitted before this vmaddfp
        const simde__m128 mix0 = simde_mm_fmadd_ps(gain, x0, y0);  // vmaddfp v13,v0,v13,v12

        const simde__m128 y1 = simde_mm_castsi128_ps(LoadVector(base, y_base + 16 + u));
        // lvx128 v10,r27,r11
        const simde__m128 y3 = simde_mm_castsi128_ps(LoadVector(base, y_base + 48 + u));
        // lvx128 v12,r30,r10
        const simde__m128 y2 = simde_mm_castsi128_ps(LoadVector(base, y_base + 32 + u));
        // lvx128 v11,r28,r23

        StoreVector(base, z_base + u, mix0);  // stvx128 v13,r11,r23

        const simde__m128 x1 = simde_mm_castsi128_ps(LoadVector(base, x_base + 16 + u));
        // lvx128 v13,r31,r11
        const simde__m128 mix1 = simde_mm_fmadd_ps(gain, x1, y1);  // vmaddfp v13,v0,v13,v10
        StoreVector(base, z_base + 16 + u, mix1);                  // stvx128 v13,r0,r11

        const simde__m128 x2 = simde_mm_castsi128_ps(LoadVector(base, x_base + 32 + u));
        // lvx128 v13,r0,r4
        const simde__m128 mix2 = simde_mm_fmadd_ps(gain, x2, y2);  // vmaddfp v13,v0,v13,v11
        StoreVector(base, z_base + 32 + u, mix2);                  // stvx128 v13,r11,r21

        const simde__m128 x3 = simde_mm_castsi128_ps(LoadVector(base, x_base + 48 + u));
        // lvx128 v13,r0,r10
        const simde__m128 mix3 = simde_mm_fmadd_ps(gain, x3, y3);  // vmaddfp v13,v0,v13,v12
        StoreVector(base, z_base + 48 + u, mix3);                  // stvx128 v13,r11,r22

        // The copy to w re-reads x AFTER the four z stores, in the lifted order 1,3,2,0. Kept
        // as loads, not as the x0..x3 already in hand: z may alias x.
        const simde__m128 copy1 = simde_mm_castsi128_ps(LoadVector(base, x_base + 16 + u));
        // lvx128 v62,r31,r11
        const simde__m128 copy3 = simde_mm_castsi128_ps(LoadVector(base, x_base + 48 + u));
        // lvx128 v60,r0,r10
        const simde__m128 copy2 = simde_mm_castsi128_ps(LoadVector(base, x_base + 32 + u));
        // lvx128 v61,r0,r4
        const simde__m128 copy0 = simde_mm_castsi128_ps(LoadVector(base, x_base + u));
        // lvx128 v63,r10,r24

        StoreVector(base, w_base + u, copy0);       // stvx128 v63,r0,r9
        StoreVector(base, w_base + 48 + u, copy3);  // stvx128 v60,r10,r29
        StoreVector(base, w_base + 16 + u, copy1);  // stvx128 v62,r26,r11
        StoreVector(base, w_base + 32 + u, copy2);  // stvx128 v61,r4,r29
      }
    }
    // loc_82B3D058: rlwinm r11,r20,4,0,27 -- 16 samples per iteration
    done = static_cast<uint32_t>(blocks) << 4;
  }

  // loc_82B3D05C: cmpw cr6,r11,r3 ; bge cr6,0x82b3d0a4 -- a signed word compare, so a negative
  // or zero count leaves through here having stored nothing outside the frame.
  if (static_cast<int32_t>(done) >= ctx.r3.s32) return;

  const uint32_t tail = ctx.r3.u32 - done;                        // subf r4,r11,r3 ; mtctr r4
  uint32_t x_ptr = ((done << 2) & 0xFFFFFFFCu) + x_base;          // rlwinm ; add r11,r11,r6
  const uint32_t z_delta = z_base - x_base;                       // subf r10,r6,r7
  const uint32_t y_delta = y_base - z_base;                       // subf r9,r7,r5
  const uint32_t w_delta = w_base - x_base;                       // subf r8,r6,r8
  for (uint32_t i = 0; i < tail; i++) {                           // bdnz 0x82b3d080
    const uint32_t z_ptr = z_delta + x_ptr;                       // add r7,r10,r11
    ctx.fpscr.disableFlushModeUnconditional();
    const double x = LoadSingle(base, x_ptr);                     // lfs f0,0(r11)
    const double y = LoadSingle(base, y_delta + z_ptr);           // lfsx f13,r9,r7
    // fmadds f12,f0,f1,f13 -- the sample is the FIRST factor here, unlike the vector path
    StoreSingle(base, z_ptr, FmaddSingle(x, ctx.f1.f64, y));      // stfsx f12,r10,r11
    const double copy = LoadSingle(base, x_ptr);                  // lfs f11,0(r11) -- reloaded
    StoreSingle(base, x_ptr + w_delta, copy);                     // stfsx f11,r11,r8
    x_ptr += 4;                                                   // addi r11,r11,4
  }
  // b 0x82f52148 -- __restgprlr_20 restores r20-r31, so the body below leaves them untouched.
  // v0/v10-v13 and v60-v63 are left as scratch: all volatile, none compared, and a caller
  // cannot read them.
}

// Two contiguous runs per written buffer: the vector body's 64*blocks bytes from the 16-byte
// floor of the base (the stvx128 address masking makes an unaligned base write low, not high),
// and the scalar tail's 4 bytes per remaining sample from the exact base. Both lengths come from
// r3 and r6's alignment alone, and the loop trip counts are closed forms of r3 -- so the write
// set is known on entry and this returns true on every path.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const int32_t n = ctx.r3.s32;
  if (n <= 0) return true;  // both paths hit the `bge` at loc_82B3D05C and store nothing
  if (n > kMaxEnumerableSamples) return false;

  const uint32_t y_base = ctx.r5.u32;
  const uint32_t x_base = ctx.r6.u32;
  const uint32_t z_base = ctx.r7.u32;
  const uint32_t w_base = ctx.r8.u32;

  const uint32_t blocks = ((x_base & 0xF) == 0) ? static_cast<uint32_t>(n / 16) : 0;
  if (blocks != 0) {
    const uint32_t bytes = blocks * 64;
    spec.write(z_base & ~0xFu, bytes);
    spec.write(w_base & ~0xFu, bytes);
    spec.read(x_base & ~0xFu, bytes);
    spec.read(y_base & ~0xFu, bytes);
  }

  const uint32_t done = blocks * 16;
  if (done < static_cast<uint32_t>(n)) {
    const uint32_t offset = done * 4;
    const uint32_t bytes = (static_cast<uint32_t>(n) - done) * 4;
    spec.write(z_base + offset, bytes);
    spec.write(w_base + offset, bytes);
    spec.read(x_base + offset, bytes);
    spec.read(y_base + offset, bytes);
  }
  return true;
}

}  // namespace port_82B3CF58
SKATE3_PORT(82B3CF58, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
