// sub_82B43340  role: sine-curve ramp writer -- fills a 256-entry block of a float array with start + (end-start)*sin(k*(i+1)/length), flat at start below index zero and flat at end past the ramp; four vectors per pass through the four-lane sine kernel sub_824531C8, the scalar tail through sub_82F4DED0, a 16-byte-unit fill plus memcpy on the flat ends   lifted: skate3_recomp.68.cpp:24085 (973 lines)
// STATUS: pending -- written 2026-09-14, not yet compared under the shadow harness
// calls/session: boot ?  play ?  map ?          thread: reached from sub_82B238A8 on curve 2
// note: the third of sub_82B238A8's generators, beside sub_82B427D8 and sub_82B42C98. Same arguments,
//   same lead-in, same flat fills and the same window arithmetic as sub_82B42C98 -- copied from it, and
//   marked where -- with the square root replaced by a sine of the scaled index
namespace port_82B43340 {
// Arguments, as the siblings: r3 out, r6 first, r7 length, f1 start, f2 end.
//
// The phase step is `k / (float(length) * c)` with both constants loaded; index i in [0, last] gets
// `fma(sin(step*(i+1)), delta, start)`, or descending `end - sin(step*(length-(i+1)))*delta`. The
// vector body computes the sine of `(-step) * (length - idx)` there instead, which is the same value
// by the sine's oddness only when the kernel is exactly odd -- it is the lifted form either way.
//
// As in both siblings, the alignment padding after the flat lead-in is LINEAR in the phase step:
// `fma(float(n), step, start)`, with no sine at all. Reproduced as found.
constexpr uint32_t kRampThreshold = 0x82165A10;     // lis r8,-32234 ; lfs f26,23056(r8)
constexpr uint32_t kAngleNumerator = 0x82060C44;    // lis r10,-32250 ; lfs f13,3140(r10)
constexpr uint32_t kAngleLengthScale = 0x82060C50;  // addi r9,r10,3140 ; lfs f0,12(r9)
constexpr uint32_t kIndexStride = 0x8231BA90;       // lis r10,-32206 ; addi r9,r10,-17776
constexpr uint32_t kDescendingScale = 0x8216DEE0;   // lis r8,-32233 ; lfs f0,-8480(r8)
constexpr uint32_t kFrameBytes = 368;               // stwu r1,-368(r1)
// ---- helpers COPIED from sub_82B42C98.inc (compiled after this file, so not nameable) ----
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

// ---- end of copy ----

// The four-lane sine: v1 in, v1 out, through the verified kernel's own hook on the live context.
inline simde__m128 Sine4(PPCContext& ctx, uint8_t* base, simde__m128 x) {
  simde_mm_store_ps(ctx.v1.f32, x);
  skate3::audio::GuestCall(ctx, base, sub_824531C8);  // bl 0x824531c8
  return simde_mm_load_ps(ctx.v1.f32);
}

REX_FUNC(Native) {
  const uint32_t out = ctx.r3.u32;    // mr r31,r3
  const int32_t first = ctx.r6.s32;   // mr r30,r6
  const int32_t length = ctx.r7.s32;
  const double start = ctx.f1.f64;    // fmr f28,f1
  const double end = ctx.f2.f64;      // fmr f27,f2
  // __savegprlr_25, __savefpr_26 and __savevmx_123 spill r25-r31, f26-f31 and v123-v127; the body
  // keeps locals and never writes those registers, so their entry values stand. stwu r1,-368(r1) is
  // REPRODUCED because both sine callees run below it and the scalar one spills under its r1.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const int32_t block_last = static_cast<int32_t>(static_cast<uint32_t>(first) + 255u);  // addi r25,r6,255
  const int32_t ramp_last = static_cast<int32_t>(static_cast<uint32_t>(length) - 1u);
  const int32_t last = (block_last > ramp_last) ? ramp_last : block_last;  // r26
  ctx.fpscr.disableFlushModeUnconditional();
  const double delta = SubSingle(end, start);                        // fsubs f29,f2,f1
  ctx.fpscr.disableFlushModeUnconditional();
  const double length_f = IntToSingle(length);                       // fcfid f13,f0 ; frsp f30,f13
  const double length_scale = LoadSingle(base, kAngleLengthScale);   // lfs f0,12(r9)
  const double numerator = LoadSingle(base, kAngleNumerator);        // lfs f13,3140(r10)
  const double threshold = LoadSingle(base, kRampThreshold);         // lfs f26,23056(r8)
  const double scaled_length = MulSingle(length_f, length_scale);    // fmuls f12,f30,f0
  const double step = DivSingle(numerator, scaled_length);           // fdivs f31,f13,f12
  int32_t index = first;
  uint32_t cursor = out;
  int32_t pad = 0;
  if (index < 0) {
    const int32_t below = static_cast<int32_t>(0u - static_cast<uint32_t>(index));
    const int32_t below_aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(below)) << 2) & 0xFFFFFFFCu);
    const int32_t leftover = below - below_aligned;
    pad = (leftover == 0) ? 0 : (4 - leftover);
    const simde__m128 start_fill = simde_mm_set1_ps(static_cast<float>(start));  // 112..124 ; lvx128 v63
    const int32_t units = -DivFour(index);
    FillVectorUnits(ctx, base, cursor, units, start_fill);
    index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                 ((static_cast<uint32_t>(units) << 2) & 0xFFFFFFFCu));
    cursor += (static_cast<uint32_t>(units) << 4) & 0xFFFFFFF0u;
    if (index < 0) {
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, cursor, start);                                // stfs f28,0(r31)
      const int32_t left = static_cast<int32_t>(0u - static_cast<uint32_t>(index));
      const uint32_t len = (((static_cast<uint32_t>(left) << 2) & 0xFFFFFFFCu) - 1u) & 0xFFFFFFFCu;
      skate3::audio::GuestCall(ctx, base, sub_82F52FB8, cursor + 4, cursor, len);  // bl 0x82f52fb8
      index = static_cast<int32_t>(static_cast<uint32_t>(index) + static_cast<uint32_t>(left));
      cursor += (static_cast<uint32_t>(left) << 2) & 0xFFFFFFFCu;
    }
    // loc_82B434C8: the padding, linear in the phase step.
    ctx.fpscr.disableFlushModeUnconditional();
    if (!(delta < threshold)) {
      if (index <= last) {
        int32_t n = static_cast<int32_t>(static_cast<uint32_t>(index) + 1u);
        while (pad > 0) {
          ctx.fpscr.disableFlushModeUnconditional();
          const double value = FmaddSingle(IntToSingle(n), step, start);  // fmadds f11,f12,f31,f28
          index += 1;
          n += 1;
          pad -= 1;
          StoreSingle(base, cursor, value);
          cursor += 4;
          if (index > last) break;
        }
      }
    } else {
      if (index <= last) {
        int32_t n = static_cast<int32_t>(static_cast<uint32_t>(index) + 1u);
        while (pad > 0) {
          ctx.fpscr.disableFlushModeUnconditional();
          const double back = SubSingle(length_f, IntToSingle(n));        // fsubs f11,f30,f12
          const double value = NmsubSingle(back, step, end);             // fnmsubs f10,f11,f31,f27
          index += 1;
          n += 1;
          pad -= 1;
          StoreSingle(base, cursor, value);
          cursor += 4;
          if (index > last) break;
        }
      }
    }
  }
  // loc_82B43568: the splats the lifted form parks in the frame, held as registers here -- the phase
  // step at 112, the delta at 144, `start` at 96 and the index vector at 80.
  ctx.fpscr.disableFlushModeUnconditional();
  const simde__m128 step_v = simde_mm_set1_ps(static_cast<float>(step));
  const simde__m128 delta_v = simde_mm_set1_ps(static_cast<float>(delta));
  const simde__m128 start_v = simde_mm_set1_ps(static_cast<float>(start));
  const uint32_t i0 = static_cast<uint32_t>(index);
  const simde__m128 idx = simde_mm_setr_ps(IntToFloat(static_cast<int32_t>(i0 + 4u)),
                                           IntToFloat(static_cast<int32_t>(i0 + 3u)),
                                           IntToFloat(static_cast<int32_t>(i0 + 2u)),
                                           IntToFloat(static_cast<int32_t>(i0 + 1u)));
  const int32_t span = static_cast<int32_t>(static_cast<uint32_t>(last) -
                                            static_cast<uint32_t>(index) + 1u);
  const int32_t groups = DivFour(span);                                // r27
  const int32_t bulk =
      static_cast<int32_t>((static_cast<uint32_t>(DivFour(groups)) << 2) & 0xFFFFFFFCu);  // r28
  const simde__m128 stride_up = LoadVectorPs(base, kIndexStride);     // lvx128 v123,r0,r9
  simde__m128 x;       // v127
  simde__m128 stride;  // v123
  simde__m128 angle;   // v126
  simde__m128 base_v;  // v124
  if (!(delta < threshold)) {  // fcmpu cr6,f29,f26 ; bge cr6,0x82b436dc
    x = idx;
    stride = stride_up;
    angle = step_v;
    base_v = start_v;
  } else {
    // The descending setup: 96..108 all `end`, 112..124 all `step * scale`, 160..172 all 0xC0800000.
    const double scale = LoadSingle(base, kDescendingScale);          // lfs f0,-8480(r8)
    angle = simde_mm_set1_ps(static_cast<float>(MulSingle(step, scale)));  // fmuls f0,f31,f0
    base_v = simde_mm_set1_ps(static_cast<float>(end));
    const simde__m128 length_v = simde_mm_set1_ps(static_cast<float>(length_f));  // 128..140
    ctx.fpscr.enableFlushMode();
    x = simde_mm_sub_ps(length_v, idx);                               // vsubfp128 v127,v63,v62
    stride = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0xC0800000)));  // lis r11,-16256
  }
  // One vector: the sine of the scaled argument, the argument stepped, and the curve value stored.
  // Operand orders are the lifted ones: angle first in the multiply, the running argument first in
  // the add, and the delta, then the sine, then the base in the multiply-add.
  auto one_vector = [&]() {
    ctx.fpscr.enableFlushModeUnconditional();
    const simde__m128 arg = simde_mm_mul_ps(angle, x);                // vmulfp128 v1,v126,v127
    const simde__m128 s = Sine4(ctx, base, arg);                      // bl 0x824531c8
    ctx.fpscr.enableFlushModeUnconditional();
    x = simde_mm_add_ps(x, stride);                                   // vaddfp128 v127,v127,v123
    const simde__m128 a = simde_mm_fmadd_ps(delta_v, s, base_v);      // vmaddfp v0,v125,v0,v124
    StoreVectorPs(base, cursor, a);                                   // stvx128 v0,r0,r31
    cursor += 16;
  };
  if (bulk > 0) {
    uint32_t passes = ((static_cast<uint32_t>(bulk) - 1u) >> 2) + 1u;
    do {
      one_vector();
      one_vector();
      one_vector();
      one_vector();
    } while (--passes != 0);
  }
  if (bulk < groups) {
    uint32_t passes = static_cast<uint32_t>(groups - bulk);
    do {
      one_vector();
    } while (--passes != 0);
  }
  int32_t tail_index = static_cast<int32_t>(static_cast<uint32_t>(index) +
                                            ((static_cast<uint32_t>(groups) << 2) & 0xFFFFFFFCu));
  ctx.fpscr.disableFlushModeUnconditional();
  const bool ascending = !(delta < threshold);
  if (tail_index <= last) {
    uint32_t passes = static_cast<uint32_t>(last - tail_index) + 1u;
    int32_t n = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + 1u);
    tail_index = static_cast<int32_t>(static_cast<uint32_t>(tail_index) + passes);
    do {
      ctx.fpscr.disableFlushModeUnconditional();
      const double arg = ascending
                             ? MulSingle(IntToSingle(n), step)                         // fmuls f1,f12,f31
                             : MulSingle(SubSingle(length_f, IntToSingle(n)), step);  // fsubs ; fmuls
      ctx.f1.f64 = arg;
      skate3::audio::GuestCall(ctx, base, sub_82F4DED0);              // bl 0x82f4ded0
      ctx.fpscr.disableFlushModeUnconditional();
      const double s = static_cast<double>(static_cast<float>(ctx.f1.f64));  // frsp
      const double value = ascending ? FmaddSingle(s, delta, start)          // fmadds f10,f11,f29,f28
                                     : NmsubSingle(s, delta, end);           // fnmsubs f9,f10,f29,f27
      n += 1;
      StoreSingle(base, cursor, value);
      cursor += 4;
    } while (--passes != 0);
  }
  // loc_82B43870: the flat fill to the end of the block, as the siblings.
  if (tail_index <= block_last) {
    int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(block_last) -
                                             static_cast<uint32_t>(tail_index) + 1u);
    const int32_t units = DivFour(remaining);
    int32_t aligned =
        static_cast<int32_t>((static_cast<uint32_t>(DivFour(remaining)) << 2) & 0xFFFFFFFCu);
    if (remaining != aligned) {
      while (tail_index <= block_last) {
        remaining -= 1;
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, cursor, end);                               // stfs f27,0(r31)
        cursor += 4;
        tail_index += 1;
        aligned = static_cast<int32_t>(
            (static_cast<uint32_t>(DivFour(remaining)) << 2) & 0xFFFFFFFCu);
        if (remaining == aligned) break;
      }
    }
    ctx.fpscr.disableFlushModeUnconditional();
    FillVectorUnits(ctx, base, cursor, units, simde_mm_set1_ps(static_cast<float>(end)));
  }
  ctx.r3.s64 = 1;          // li r3,1
  ctx.r1.u32 = entry_sp;   // addi r1,r1,368
}

// The write run is the square-root writer's: the same integer control flow decides it, so its window
// builder is copied from sub_82B42C98.inc, with this function's extra reads added.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t out = ctx.r3.u32;
  const int32_t first = ctx.r6.s32;
  const int32_t length = ctx.r7.s32;

  spec.read(kRampThreshold, 4);      // lfs f26,23056(r10)
  spec.read(kIndexStride, 16);       // lvx128 v62,r0,r8
  spec.read(kDescendingScale, 4);    // lfs f0,-8480(r8), descending form only
  spec.read(kAngleNumerator, 4);     // lfs f13,3140(r10)
  spec.read(kAngleLengthScale, 4);   // lfs f0,12(r9)
  // For replay only: the pools of the two sine callees, as their own window builders declare them --
  // sub_824531C8's four constant vectors and sub_82F4DED0's table, 1.0 and NaN cells.
  spec.read(0x822F97C0u, 48);
  spec.read(0x822F9850u, 16);
  spec.read(0x82FB4E20u, 120);
  spec.read(0x82010108u, 8);
  spec.read(0x82FB58C8u, 8);

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

}  // namespace port_82B43340
SKATE3_PORT(82B43340, skate3::audio::kPortPending, skate3::audio::kReturnR3)
