// sub_824531C8  role: four-lane sine by range reduction and an 11-term odd polynomial   lifted: skate3_recomp.7.cpp:59504 (113 lines)
// STATUS: verified
// calls/session: boot 9572672  play 9906721  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_824531C8.md
namespace port_824531C8 {

// Four 16-byte coefficient vectors, 0x82300000 minus 26544/26688/26672/26656. All are 16-byte
// aligned, so the lvx128 masking of the address is a no-op on them.
constexpr uint32_t kVectorA = 0x822F9850;
constexpr uint32_t kVectorB = 0x822F97C0;
constexpr uint32_t kVectorC = 0x822F97D0;
constexpr uint32_t kVectorD = 0x822F97E0;

// lvx128: a 16-byte load with the big-endian lane reversal RexGlue applies to every vector
// memory access.
simde__m128i LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                               simde_mm_load_si128((simde__m128i*)VectorMaskL));
}

REX_FUNC(Native) {
  // vspltw128 vD,vS,n lowers to shuffle_epi32 with a reversed immediate: word 3 is 0x00,
  // word 2 is 0x55, word 1 is 0xAA and word 0 is 0xFF. Kept as the lifted immediates.
  const simde__m128i a = LoadVector(base, kVectorA);
  const simde__m128 reduce_scale = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(a, 0x0));
  const simde__m128i b = LoadVector(base, kVectorB);
  const simde__m128 reduce_step = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(a, 0xAA));
  const simde__m128 c1 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(b, 0xAA));  // v31
  const simde__m128 c2 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(b, 0x55));
  const simde__m128i c = LoadVector(base, kVectorC);
  const simde__m128 x = simde_mm_load_ps(ctx.v1.f32);  // the argument, four lanes

  ctx.fpscr.enableFlushModeUnconditional();  // emitted before the first vector float op
  const simde__m128 scaled = simde_mm_mul_ps(x, reduce_scale);      // vmulfp128 v60,v1,v61
  const simde__m128 c3 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(b, 0x0));
  const simde__m128 c4 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c, 0xFF));
  const simde__m128i d = LoadVector(base, kVectorD);
  const simde__m128 c5 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c, 0xAA));
  const simde__m128 c6 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c, 0x55));
  const simde__m128 c7 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c, 0x0));
  const simde__m128 c8 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(d, 0xFF));
  const simde__m128 c9 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(d, 0xAA));
  const simde__m128 c10 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(d, 0x55));
  const simde__m128 c11 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(d, 0x0));

  // vrfin128 rounds to nearest; vnmsubfp is -(a*b)+c, a single-rounding negative multiply-add.
  const simde__m128 turns = simde_mm_round_ps(
      scaled, SIMDE_MM_FROUND_TO_NEAREST_INT | SIMDE_MM_FROUND_NO_EXC);  // vrfin128 v12,v60
  const simde__m128 t = simde_mm_fnmadd_ps(reduce_step, turns, x);       // vnmsubfp v0,v13,v12,v1
  const simde__m128 t2 = simde_mm_mul_ps(t, t);                          // vmulfp128 v59,v0,v0

  // Odd-power Horner: each step multiplies the running power by t^2 and folds in one
  // coefficient with a fused multiply-add. Every vmaddfp is a single-rounding FMA, and each
  // vmulfp128 stays a separate multiply -- never let a pair collapse into one FMA.
  simde__m128 power_a = simde_mm_mul_ps(t2, t);              // vmulfp128 v13,v59,v0
  simde__m128 power_b = simde_mm_mul_ps(power_a, t2);        // vmulfp128 v12,v13,v59
  simde__m128 sum = simde_mm_fmadd_ps(c1, power_a, t);       // vmaddfp v13,v31,v13,v0
  power_a = simde_mm_mul_ps(power_b, t2);                    // vmulfp128 v0,v12,v59
  sum = simde_mm_fmadd_ps(c2, power_b, sum);                 // vmaddfp v12,v2,v12,v13
  power_b = simde_mm_mul_ps(power_a, t2);                    // vmulfp128 v13,v0,v59
  sum = simde_mm_fmadd_ps(c3, power_a, sum);                 // vmaddfp v12,v3,v0,v12
  power_a = simde_mm_mul_ps(power_b, t2);                    // vmulfp128 v0,v13,v59
  sum = simde_mm_fmadd_ps(c4, power_b, sum);                 // vmaddfp v12,v4,v13,v12
  power_b = simde_mm_mul_ps(power_a, t2);                    // vmulfp128 v13,v0,v59
  sum = simde_mm_fmadd_ps(c5, power_a, sum);                 // vmaddfp v12,v5,v0,v12
  power_a = simde_mm_mul_ps(power_b, t2);                    // vmulfp128 v0,v13,v59
  sum = simde_mm_fmadd_ps(c6, power_b, sum);                 // vmaddfp v12,v6,v13,v12
  power_b = simde_mm_mul_ps(power_a, t2);                    // vmulfp128 v13,v0,v59
  sum = simde_mm_fmadd_ps(c7, power_a, sum);                 // vmaddfp v12,v7,v0,v12
  power_a = simde_mm_mul_ps(power_b, t2);                    // vmulfp128 v0,v13,v59
  sum = simde_mm_fmadd_ps(c8, power_b, sum);                 // vmaddfp v12,v8,v13,v12
  power_b = simde_mm_mul_ps(power_a, t2);                    // vmulfp128 v13,v0,v59
  const simde__m128 sum_9 = simde_mm_fmadd_ps(c9, power_a, sum);  // vmaddfp v12,v9,v0,v12
  power_a = simde_mm_mul_ps(power_b, t2);                    // vmulfp128 v0,v13,v59
  const simde__m128 sum_10 = simde_mm_fmadd_ps(c10, power_b, sum_9);  // vmaddfp v13,v10,v13,v12
  const simde__m128 result = simde_mm_fmadd_ps(c11, power_a, sum_10);  // vmaddfp v1,v11,v0,v13

  // The function writes no memory, so only its final register values are observable. It also
  // clobbers v31, which the ABI would preserve, so that value is reproduced too -- the harness
  // compares v31 unconditionally.
  simde_mm_store_ps(ctx.v1.f32, result);
  simde_mm_store_ps(ctx.v0.f32, power_a);
  simde_mm_store_ps(ctx.v12.f32, sum_9);
  simde_mm_store_ps(ctx.v13.f32, sum_10);
  simde_mm_store_ps(ctx.v59.f32, t2);
  simde_mm_store_ps(ctx.v60.f32, scaled);
  simde_mm_store_ps(ctx.v61.f32, reduce_scale);
  simde_mm_store_si128((simde__m128i*)ctx.v62.u8, d);
  simde_mm_store_si128((simde__m128i*)ctx.v63.u8, c);
  simde_mm_store_ps(ctx.v31.f32, c1);
  simde_mm_store_ps(ctx.v2.f32, c2);
  simde_mm_store_ps(ctx.v3.f32, c3);
  simde_mm_store_ps(ctx.v4.f32, c4);
  simde_mm_store_ps(ctx.v5.f32, c5);
  simde_mm_store_ps(ctx.v6.f32, c6);
  simde_mm_store_ps(ctx.v7.f32, c7);
  simde_mm_store_ps(ctx.v8.f32, c8);
  simde_mm_store_ps(ctx.v9.f32, c9);
  simde_mm_store_ps(ctx.v10.f32, c10);
  simde_mm_store_ps(ctx.v11.f32, c11);
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  // No stores at all: this is the gate-4 case. The result is named in the mask below instead,
  // which is what makes the comparison mean anything.
  spec.read(kVectorB, 16);
  spec.read(kVectorC, 16);
  spec.read(kVectorD, 16);
  spec.read(kVectorA, 16);
  return true;
}

}  // namespace port_824531C8
SKATE3_PORT(824531C8, skate3::audio::kPortVerified, skate3::audio::Vr(1))
