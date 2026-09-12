// sub_82B3C098  role: gain-ramp copy -- dst[k] = src[k] * gain over 256 singles, the gain ramping from f1 by f2 per sample for the first 64 then held, each 128-byte block dcbzl'd before it is written   lifted: skate3_recomp.68.cpp:6626 (356 lines)
// STATUS: verified
// calls/session: boot 2126778  play 3023068  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C098.md
namespace port_82B3C098 {

// Arguments: r3 = destination (256 singles, written), r4 = source (256 singles, read),
// f1 = the gain at sample 0, f2 = the gain increment per sample. Nothing in
// docs/rw_audio_structs.h names either buffer; both are plain float runs.

// Rodata singles, each computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye:
// one misread digit in sub_82B2FE00 cost this project's first shadow divergence. No value is
// assumed here -- every one is loaded through the guest map.
constexpr uint32_t kStepScale = 0x82257308;   // lis -32219 ; lfs 29448  (also sub_82B2DAC8)
constexpr uint32_t kRampSpan = 0x820ED958;    // lis -32241 ; lfs -9896
constexpr uint32_t kLane2Scale = 0x82060C50;  // lis -32250 ; lfs 3152   (also sub_82B1D3D0)
constexpr uint32_t kLane3Scale = 0x82063B08;  // lis -32250 ; lfs 15112

// Eight 16-byte group multipliers in the pool: 0x82320000 (lis -32206) minus 17776..17888 in
// steps of 16. All 16-byte aligned, so lvx128's masking of the address is a no-op on them.
// Named by the 16-byte group of the block each one offsets; kScaleStep is the whole-block
// increment. The mapping is read off the register plumbing, not off the values.
constexpr uint32_t kScale1 = 0x8231BA60;     // addi -17824 -> v4
constexpr uint32_t kScale2 = 0x8231BA70;     // addi -17808 -> v3
constexpr uint32_t kScale3 = 0x8231BA80;     // addi -17792 -> v2
constexpr uint32_t kScale4 = 0x8231BA90;     // addi -17776 -> v1
constexpr uint32_t kScale5 = 0x8231BA20;     // addi -17888 -> v31
constexpr uint32_t kScale6 = 0x8231BA30;     // addi -17872 -> v30
constexpr uint32_t kScale7 = 0x8231BA40;     // addi -17856 -> v29
constexpr uint32_t kScaleStep = 0x8231BA50;  // addi -17840 -> v28

constexpr uint32_t kVectorsPerBlock = 8;
constexpr uint32_t kBlockBytes = 128;       // addi r10,r10,128 / addi r11,r11,128
constexpr uint32_t kRampBlocks = 2;         // r31 = src + 256, the first loop's limit
constexpr uint32_t kHoldBlocks = 6;         // r30 = src + 1024, the second loop's limit
constexpr uint32_t kTotalBytes = (kRampBlocks + kHoldBlocks) * kBlockBytes;  // 1024

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

// dcbzl r0,r11: establishes the 128-byte cache line containing r11 as zeroes. A real write to
// guest memory, so it is inside the declared window and reproduced here.
inline void ZeroCacheLine(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~127u;
  memset((void*)REX_RAW_ADDR(ea), 0, 128);
}

REX_FUNC(Native) {
  const uint32_t dst = ctx.r3.u32;  // mr r11,r3
  const uint32_t src = ctx.r4.u32;  // mr r10,r4

  ctx.fpscr.disableFlushModeUnconditional();  // emitted at lfs f0,29448(r9)
  const double step_scale = LoadSingle(base, kStepScale);           // lfs f0,29448(r9)
  const double ramp_span = LoadSingle(base, kRampSpan);             // lfs f13,-9896(r8)
  const double step = Fmuls(ctx.f2.f64, step_scale);                // fmuls f11,f2,f0
  const double held = Fmadds(ctx.f2.f64, ramp_span, ctx.f1.f64);    // fmadds f12,f2,f13,f1
  const double gain1 = Fadds(ctx.f1.f64, ctx.f2.f64);               // fadds f10,f1,f2
  const double lane2_scale = LoadSingle(base, kLane2Scale);         // lfs f0,3152(r7)
  const double lane3_scale = LoadSingle(base, kLane3Scale);         // lfs f13,15112(r9)
  const double gain2 = Fmadds(ctx.f2.f64, lane2_scale, ctx.f1.f64); // fmadds f9,f2,f0,f1
  const double gain3 = Fmadds(ctx.f2.f64, lane3_scale, ctx.f1.f64); // fmadds f8,f2,f13,f1

  // The lifted body materialises two vectors through its own stack frame: four copies of the
  // step at -112..-97 and the four starting lane gains at -96..-81, each reloaded with lvx128.
  // stfs writes big-endian and lvx128 reverses all sixteen bytes, so the two reversals cancel
  // exactly: the single at -96 is host lane 3 and the single at -84 is host lane 0. Built as
  // registers here -- the native body never writes the guest stack.
  const simde__m128 step_v = simde_mm_set1_ps(float(step));  // stfs f11 x4 ; lvx128 v13,r0,r24
  const simde__m128 start = simde_mm_set_ps(float(ctx.f1.f64), float(gain1), float(gain2),
                                            float(gain3));   // stfs f1/f10/f9/f8 ; lvx128 v0,r0,r23

  // The eight group multipliers, in the order the lifted body loads them.
  const simde__m128 scale_step = LoadVector(base, kScaleStep);  // lvx128 v28,r0,r9
  const simde__m128 scale7 = LoadVector(base, kScale7);         // lvx128 v29,r0,r8
  const simde__m128 scale6 = LoadVector(base, kScale6);         // lvx128 v30,r0,r7
  const simde__m128 scale5 = LoadVector(base, kScale5);         // lvx128 v31,r0,r29
  const simde__m128 scale4 = LoadVector(base, kScale4);         // lvx128 v1,r0,r28
  const simde__m128 scale3 = LoadVector(base, kScale3);         // lvx128 v2,r0,r27
  const simde__m128 scale2 = LoadVector(base, kScale2);         // lvx128 v3,r0,r26
  const simde__m128 scale1 = LoadVector(base, kScale1);         // lvx128 v4,r0,r25

  uint32_t src_cursor = src;  // r10
  uint32_t dst_cursor = dst;  // r11
  simde__m128 gain = start;   // v0

  // First loop: the gain still moving. Two blocks of eight vectors, 64 singles.
  for (uint32_t block = 0; block < kRampBlocks; block++) {  // loc_82B3C194 .. bne cr6
    // lvx128 v63..v56. Every source vector is loaded before the first store of the block, so a
    // destination overlapping the source still sees the pre-call bytes; kept in that order.
    simde__m128 sample[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      sample[g] = LoadVector(base, src_cursor + 16 * g);
    }
    src_cursor += kBlockBytes;  // addi r10,r10,128

    ctx.fpscr.enableFlushModeUnconditional();  // emitted at vmaddfp v12,v28,v13,v0
    // Each vmaddfp is a single-rounding fused multiply-add; the operand order is the lifted
    // order, because a fused multiply-add is not NaN-commutative in its product
    // (docs/vmx128-exactness.md rule 4).
    const simde__m128 next = simde_mm_fmadd_ps(scale_step, step_v, gain);  // vmaddfp v12,v28,v13,v0
    simde__m128 group_gain[kVectorsPerBlock];
    group_gain[1] = simde_mm_fmadd_ps(scale1, step_v, gain);  // vmaddfp v11,v4,v13,v0
    group_gain[0] = gain;                                     // vor128 v55,v0,v0
    group_gain[2] = simde_mm_fmadd_ps(scale2, step_v, gain);  // vmaddfp v10,v3,v13,v0
    group_gain[3] = simde_mm_fmadd_ps(scale3, step_v, gain);  // vmaddfp v9,v2,v13,v0
    group_gain[4] = simde_mm_fmadd_ps(scale4, step_v, gain);  // vmaddfp v8,v1,v13,v0
    group_gain[5] = simde_mm_fmadd_ps(scale5, step_v, gain);  // vmaddfp v7,v31,v13,v0
    group_gain[6] = simde_mm_fmadd_ps(scale6, step_v, gain);  // vmaddfp v6,v30,v13,v0
    group_gain[7] = simde_mm_fmadd_ps(scale7, step_v, gain);  // vmaddfp v5,v29,v13,v0
    gain = next;                                              // vor v0,v12,v12

    ZeroCacheLine(base, dst_cursor);  // dcbzl r0,r11

    // vmulfp128 v54..v47: a plain multiply. It must never fuse with the vmaddfp above.
    simde__m128 out[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      out[g] = simde_mm_mul_ps(group_gain[g], sample[g]);  // vmulfp128 v54,v55,v63 ...
    }
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      StoreVector(base, dst_cursor + 16 * g, out[g]);  // stvx128 v54,r0,r11 ... v47,r11,r9
    }
    dst_cursor += kBlockBytes;  // addi r11,r11,128
  }

  // The ramp is over at sample 64: every remaining single takes the held gain.
  ctx.fpscr.disableFlushMode();  // emitted at stfs f12,-96(r1)
  const simde__m128 held_v = simde_mm_set1_ps(float(held));  // stfs f12 x4 ; lvx128 v63,r0,r31

  // Second loop: six blocks, 192 singles, constant gain.
  for (uint32_t block = 0; block < kHoldBlocks; block++) {  // loc_82B3C24C .. bne cr6
    simde__m128 sample[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      sample[g] = LoadVector(base, src_cursor + 16 * g);  // lvx128 v62..v55
    }
    src_cursor += kBlockBytes;  // addi r10,r10,128

    ZeroCacheLine(base, dst_cursor);  // dcbzl r0,r11

    ctx.fpscr.enableFlushModeUnconditional();  // emitted at vmulfp128 v46,v63,v62
    simde__m128 out[kVectorsPerBlock];
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      out[g] = simde_mm_mul_ps(held_v, sample[g]);  // vmulfp128 v46,v63,v62 ...
    }
    for (uint32_t g = 0; g < kVectorsPerBlock; g++) {
      StoreVector(base, dst_cursor + 16 * g, out[g]);  // stvx128 v46,r0,r11 ... v39,r11,r9
    }
    dst_cursor += kBlockBytes;  // addi r11,r11,128
  }

  // v28..v31 are in the ABI-preserved range the harness compares on every call, and the
  // original clobbers all four with group multipliers without ever saving them (it uses
  // __savegprlr_23, not __savevmx_*). So the four constants have to be left behind.
  simde_mm_store_ps(ctx.v28.f32, scale_step);  // lvx128 v28,r0,r9
  simde_mm_store_ps(ctx.v29.f32, scale7);      // lvx128 v29,r0,r8
  simde_mm_store_ps(ctx.v30.f32, scale6);      // lvx128 v30,r0,r7
  simde_mm_store_ps(ctx.v31.f32, scale5);      // lvx128 v31,r0,r29
}

// Every byte the call writes, from ENTRY state only.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;

  // Eight 128-byte blocks across the two loops, all of them at a fixed offset from r3. Two
  // write shapes per block i, both contiguous across i:
  //   dcbzl    [(dst + 128i) & ~127, +128)         == [(dst & ~127) + 128i, +128)
  //   stvx128  [((dst + 128i + 16g) & ~0xF), +16)  == [(dst & ~0xF) + 128i + 16g, +16)
  // (128i and 16g are multiples of the respective alignments, so the masking factors out.)
  // One span covers the union: dst & ~127 is the lower of the two starts. An aligned dst --
  // what the mixer passes in practice -- makes the two coincide and the span exactly 1024.
  const uint32_t zero_begin = dst & ~127u;
  const uint32_t store_end = (dst & ~0xFu) + kTotalBytes;
  if (store_end < zero_begin) return false;  // a dst within 1 KB of the top of the map
  spec.write(zero_begin, store_end - zero_begin);

  // Read but never written: the source run and the five rodata cells.
  spec.read(src, kTotalBytes);
  spec.read(kStepScale, 4);
  spec.read(kRampSpan, 4);
  spec.read(kLane2Scale, 4);
  spec.read(kLane3Scale, 4);
  spec.read(kScale5, 16 * 4);  // kScale5/6/7 and kScaleStep are 0x8231BA20..0x8231BA5F
  spec.read(kScale1, 16 * 4);  // kScale1/2/3/4 are 0x8231BA60..0x8231BA9F
  return true;
}

}  // namespace port_82B3C098
SKATE3_PORT(82B3C098, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
