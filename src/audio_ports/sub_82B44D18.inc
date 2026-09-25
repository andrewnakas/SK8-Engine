// sub_82B44D18  role: gain-ramp mix-add -- dst[k] += src[k] * gain over 256 singles, the gain ramping from f1 by f2 per sample for the first 64 then held; the accumulating sibling of sub_82B3C098   lifted: skate3_recomp.68.cpp:27998 (402 lines)
// STATUS: verified
// calls/session: boot 34866  play 71847  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B44D18.md
namespace port_82B44D18 {

// Arguments: r3 = destination (256 singles, read AND written), r4 = source (256 singles, read),
// f1 = the gain at sample 0, f2 = the gain increment per sample. Nothing in
// docs/rw_audio_structs.h names either buffer; both are plain float runs.

// Rodata singles, each computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye:
// one misread digit in sub_82B2FE00 cost this project's first shadow divergence. No value is
// assumed here -- every one is loaded through the guest map. All four are the same cells
// sub_82B3C098 reads, in the same roles.
constexpr uint32_t kStepScale = 0x82257308;   // lis -32219 ; lfs 29448  (also sub_82B2DAC8)
constexpr uint32_t kRampSpan = 0x820ED958;    // lis -32241 ; lfs -9896
constexpr uint32_t kLane2Scale = 0x82060C50;  // lis -32250 ; lfs 3152   (also sub_82B1D3D0)
constexpr uint32_t kLane3Scale = 0x82063B08;  // lis -32250 ; lfs 15112

// Eight 16-byte group multipliers in the pool: 0x82320000 (lis -32206) minus 17776..17888 in
// steps of 16. All 16-byte aligned, so lvx128's masking of the address is a no-op on them.
// Named by the 16-byte group of the block each one offsets; kScaleStep advances the gain by a
// whole 128-byte block. The mapping is read off the register plumbing, and it agrees with
// sub_82B3C098's, which loads the same eight cells into different registers for the same roles.
constexpr uint32_t kScale1 = 0x8231BA60;     // addi -17824 -> v21
constexpr uint32_t kScale2 = 0x8231BA70;     // addi -17808 -> v20
constexpr uint32_t kScale3 = 0x8231BA80;     // addi -17792 -> v19
constexpr uint32_t kScale4 = 0x8231BA90;     // addi -17776 -> v18
constexpr uint32_t kScale5 = 0x8231BA20;     // addi -17888 -> v17
constexpr uint32_t kScale6 = 0x8231BA30;     // addi -17872 -> v16
constexpr uint32_t kScale7 = 0x8231BA40;     // addi -17856 -> v15
constexpr uint32_t kScaleStep = 0x8231BA50;  // addi -17840 -> v14

constexpr uint32_t kVectorsPerBlock = 8;
constexpr uint32_t kBlockBytes = 128;       // addi r10,r10,128 / addi r11,r11,128
constexpr uint32_t kRampBlocks = 2;         // addi r4,r4,256, the first loop's limit
constexpr uint32_t kHoldBlocks = 6;         // r30 = src + 1024, the second loop's limit
constexpr uint32_t kTotalBytes = (kRampBlocks + kHoldBlocks) * kBlockBytes;  // 1024

// The ramp loop's stvx128 order is not ascending: +0, +16, +32, +48, +96, +64, +80, +112.
// Reproduced literally -- a store order only matters when the buffers alias, and this port is
// not the place to decide that they cannot.
constexpr uint32_t kRampStoreOrder[kVectorsPerBlock] = {0, 1, 2, 3, 6, 4, 5, 7};

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// fmuls / fadds / fmadds: double arithmetic rounded back to a single. fmadds is one fused
// product with a single rounding -- never a multiply plus a separate add.
inline double Fmuls(double a, double b) { return double(float(a * b)); }
inline double Fadds(double a, double b) { return double(float(a + b)); }
inline double Fmadds(double a, double b, double c) { return double(float(std::fma(a, b, c))); }

// lvx128: a 16-byte load with the big-endian lane reversal RexGlue applies to every vector
// memory access, and the same masking of the address to 16 bytes.
inline simde__m128 LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                            simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

// stvx128: the same reversal on the way out, the same masking.
inline void StoreVector(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

REX_FUNC(Native) {
  const uint32_t dst = ctx.r3.u32;  // mr r11,r3
  const uint32_t src = ctx.r4.u32;  // mr r10,r4

  ctx.fpscr.disableFlushModeUnconditional();  // emitted at lfs f0,29448(r9)
  const double step_scale = LoadSingle(base, kStepScale);            // lfs f0,29448(r9)
  const double ramp_span = LoadSingle(base, kRampSpan);              // lfs f13,-9896(r8)
  const double step = Fmuls(ctx.f2.f64, step_scale);                 // fmuls f11,f2,f0
  const double held = Fmadds(ctx.f2.f64, ramp_span, ctx.f1.f64);     // fmadds f12,f2,f13,f1
  const double gain1 = Fadds(ctx.f1.f64, ctx.f2.f64);                // fadds f10,f1,f2
  const double lane2_scale = LoadSingle(base, kLane2Scale);          // lfs f0,3152(r7)
  const double lane3_scale = LoadSingle(base, kLane3Scale);          // lfs f13,15112(r6)
  const double gain2 = Fmadds(ctx.f2.f64, lane2_scale, ctx.f1.f64);  // fmadds f9,f2,f0,f1
  const double gain3 = Fmadds(ctx.f2.f64, lane3_scale, ctx.f1.f64);  // fmadds f8,f2,f13,f1

  // The lifted body materialises two vectors through its own stack frame: four copies of the
  // step at -112..-97 and the four starting lane gains at -96..-81, each reloaded with lvx128.
  // stfs writes big-endian and lvx128 reverses all sixteen bytes, so the two reversals cancel
  // exactly: the single at -96 is host lane 3 and the single at -84 is host lane 0. Built as
  // registers here -- the native body never writes the guest stack.
  const simde__m128 step_v = simde_mm_set1_ps(float(step));  // stfs f11 x4 ; lvx128 v13,r0,r25
  const simde__m128 start = simde_mm_set_ps(float(ctx.f1.f64), float(gain1), float(gain2),
                                            float(gain3));   // stfs f1/f10/f9/f8 ; lvx128 v0,r0,r24

  // The eight group multipliers, in the order the lifted body loads them.
  const simde__m128 scale_step = LoadVector(base, kScaleStep);  // lvx128 v14,r0,r9
  const simde__m128 scale2 = LoadVector(base, kScale2);         // lvx128 v20,r0,r9
  const simde__m128 scale7 = LoadVector(base, kScale7);         // lvx128 v15,r0,r8
  const simde__m128 scale6 = LoadVector(base, kScale6);         // lvx128 v16,r0,r7
  const simde__m128 scale5 = LoadVector(base, kScale5);         // lvx128 v17,r0,r6
  const simde__m128 scale4 = LoadVector(base, kScale4);         // lvx128 v18,r0,r29
  const simde__m128 scale3 = LoadVector(base, kScale3);         // lvx128 v19,r0,r28
  const simde__m128 scale1 = LoadVector(base, kScale1);         // lvx128 v21,r0,r27

  uint32_t src_cursor = src;  // r10
  uint32_t dst_cursor = dst;  // r11
  simde__m128 gain = start;   // v0

  // Declared outside the loop on purpose: v22..v31 are in the ABI-preserved range the harness
  // compares on every call, and the ramp loop leaves its LAST iteration's intermediates in
  // them. So these four arrays and `next` are read again after the loop. The loop always runs,
  // so none of them is read uninitialised.
  simde__m128 sample[kVectorsPerBlock]{};
  simde__m128 accum[kVectorsPerBlock]{};
  simde__m128 group_gain[kVectorsPerBlock]{};
  simde__m128 out[kVectorsPerBlock]{};
  simde__m128 next{};

  // First loop: the gain still moving. Two blocks of eight vectors, 64 singles.
  for (uint32_t block = 0; block < kRampBlocks; block++) {  // loc_82B44E14 .. bne cr6
    // lvx128 v12..v5: the source vectors, all loaded before the first store of the block.
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      sample[g] = LoadVector(base, src_cursor + 16 * g);
    }
    src_cursor += kBlockBytes;  // addi r10,r10,128

    ctx.fpscr.enableFlushModeUnconditional();  // emitted at vmaddfp v2,v21,v13,v0
    // Each vmaddfp is a single-rounding fused multiply-add; the operand order is the lifted
    // order, because a fused multiply-add is not NaN-commutative in its product
    // (docs/vmx128-exactness.md rule 4). Group 0 takes the block gain unchanged.
    group_gain[0] = gain;                                     // v0 itself, via vmaddfp v25
    group_gain[1] = simde_mm_fmadd_ps(scale1, step_v, gain);  // vmaddfp v2,v21,v13,v0
    group_gain[2] = simde_mm_fmadd_ps(scale2, step_v, gain);  // vmaddfp v1,v20,v13,v0
    group_gain[3] = simde_mm_fmadd_ps(scale3, step_v, gain);  // vmaddfp v31,v19,v13,v0
    group_gain[6] = simde_mm_fmadd_ps(scale6, step_v, gain);  // vmaddfp v28,v16,v13,v0
    group_gain[4] = simde_mm_fmadd_ps(scale4, step_v, gain);  // vmaddfp v30,v18,v13,v0
    group_gain[5] = simde_mm_fmadd_ps(scale5, step_v, gain);  // vmaddfp v29,v17,v13,v0
    group_gain[7] = simde_mm_fmadd_ps(scale7, step_v, gain);  // vmaddfp v27,v15,v13,v0
    next = simde_mm_fmadd_ps(scale_step, step_v, gain);       // vmaddfp v26,v14,v13,v0

    // lvx128 v4/v24/v23/v22/v3/v12/v0/v4: the destination read back. Every one of these loads
    // precedes every store of the block in the lifted body too, so grouping them is not a
    // reordering across a store.
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      accum[g] = LoadVector(base, dst_cursor + 16 * g);
    }

    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      // vmaddfp v25,v0,v12,v4 and the seven like it: (gain, sample, accumulator).
      out[g] = simde_mm_fmadd_ps(group_gain[g], sample[g], accum[g]);
    }
    for (uint32_t i = 0; i < kVectorsPerBlock; i++) {
      const uint32_t g = kRampStoreOrder[i];
      StoreVector(base, dst_cursor + 16 * g, out[g]);  // stvx128 v25,r0,r11 ... v12,r11,r9
    }
    dst_cursor += kBlockBytes;  // addi r11,r11,128
    gain = next;                // vor v0,v26,v26
  }

  // The ramp is over at sample 64: every remaining single takes the held gain.
  ctx.fpscr.disableFlushMode();  // emitted at stfs f12,-96(r1)
  const simde__m128 held_v = simde_mm_set1_ps(float(held));  // stfs f12 x4 ; lvx128 v0,r0,r29

  // Second loop: six blocks, 192 singles, constant gain.
  for (uint32_t block = 0; block < kHoldBlocks; block++) {  // loc_82B44EEC .. bne cr6
    simde__m128 hold_sample[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      hold_sample[g] = LoadVector(base, src_cursor + 16 * g);  // lvx128 v12/v10/v9/v5/v4/v3/v2/v1
    }
    src_cursor += kBlockBytes;  // addi r10,r10,128

    // lvx128 v13/v13/v11/v12/v13/v11/v12/v13: the destination read back. All eight loads
    // precede all eight stores in the lifted body as well.
    simde__m128 hold_accum[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      hold_accum[g] = LoadVector(base, dst_cursor + 16 * g);
    }

    ctx.fpscr.enableFlushModeUnconditional();  // emitted at vmaddfp v6,v0,v12,v13
    simde__m128 hold_out[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      hold_out[g] = simde_mm_fmadd_ps(held_v, hold_sample[g], hold_accum[g]);  // vmaddfp v6..v13
    }
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      StoreVector(base, dst_cursor + 16 * g, hold_out[g]);  // stvx128 v6,r0,r11 ... v13,r11,r9
    }
    dst_cursor += kBlockBytes;  // addi r11,r11,128
  }

  // v14..v31 are the whole ABI-preserved vector range the harness compares on every call, and
  // the original clobbers ALL EIGHTEEN without ever saving them (it uses __savegprlr_24, not
  // __savevmx_*). v14..v21 keep the group multipliers; v22..v31 keep the last ramp block's
  // intermediates, which the hold loop never touches because it only writes v0..v13.
  simde_mm_store_ps(ctx.v14.f32, scale_step);     // lvx128 v14,r0,r9
  simde_mm_store_ps(ctx.v15.f32, scale7);         // lvx128 v15,r0,r8
  simde_mm_store_ps(ctx.v16.f32, scale6);         // lvx128 v16,r0,r7
  simde_mm_store_ps(ctx.v17.f32, scale5);         // lvx128 v17,r0,r6
  simde_mm_store_ps(ctx.v18.f32, scale4);         // lvx128 v18,r0,r29
  simde_mm_store_ps(ctx.v19.f32, scale3);         // lvx128 v19,r0,r28
  simde_mm_store_ps(ctx.v20.f32, scale2);         // lvx128 v20,r0,r9
  simde_mm_store_ps(ctx.v21.f32, scale1);         // lvx128 v21,r0,r27
  simde_mm_store_ps(ctx.v22.f32, accum[3]);       // lvx128 v22,r11,r5   (dst + 48)
  simde_mm_store_ps(ctx.v23.f32, accum[2]);       // lvx128 v23,r11,r3   (dst + 32)
  simde_mm_store_ps(ctx.v24.f32, out[1]);         // vmaddfp v24,v2,v11,v24
  simde_mm_store_ps(ctx.v25.f32, out[0]);         // vmaddfp v25,v0,v12,v4
  simde_mm_store_ps(ctx.v26.f32, next);           // vmaddfp v26,v14,v13,v0
  simde_mm_store_ps(ctx.v27.f32, group_gain[7]);  // vmaddfp v27,v15,v13,v0
  simde_mm_store_ps(ctx.v28.f32, group_gain[6]);  // vmaddfp v28,v16,v13,v0
  simde_mm_store_ps(ctx.v29.f32, group_gain[5]);  // vmaddfp v29,v17,v13,v0
  simde_mm_store_ps(ctx.v30.f32, group_gain[4]);  // vmaddfp v30,v18,v13,v0
  simde_mm_store_ps(ctx.v31.f32, group_gain[3]);  // vmaddfp v31,v19,v13,v0
}

// Every byte the call writes, from ENTRY state only.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;

  // The only stores are the 64 stvx128, eight per block over eight blocks, all at a fixed
  // offset from r3: [((r3 + 128i + 16g) & ~0xF), +16) == [(r3 & ~0xF) + 128i + 16g, +16).
  // (128i and 16g are multiples of 16, so the masking factors out of the loops.) Contiguous
  // across i and g, so one 1024-byte span covers them. There is no dcbzl here, unlike
  // sub_82B3C098, because this kernel accumulates into the destination instead of replacing it.
  const uint32_t store_begin = dst & ~0xFu;
  if (store_begin + kTotalBytes < store_begin) return false;  // a dst within 1 KB of the top
  spec.write(store_begin, kTotalBytes);

  // Read but never written: the source run and the rodata cells. The destination is read too,
  // but it is already in `windows`, so the harness snapshots it for the replay either way.
  spec.read(src, kTotalBytes);
  spec.read(kStepScale, 4);
  spec.read(kRampSpan, 4);
  spec.read(kLane2Scale, 4);
  spec.read(kLane3Scale, 4);
  spec.read(kScale5, 16 * 4);  // kScale5/6/7 and kScaleStep are 0x8231BA20..0x8231BA5F
  spec.read(kScale1, 16 * 4);  // kScale1/2/3/4 are 0x8231BA60..0x8231BA9F
  return true;
}

}  // namespace port_82B44D18
SKATE3_PORT(82B44D18, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
