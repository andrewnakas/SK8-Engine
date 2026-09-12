// sub_82B1CAD8  role: evaluate a sampled curve at a cursor's position (nearest or linear), cache at +8   lifted: skate3_recomp.66.cpp:65213 (328 lines)
// STATUS: verified
// calls/session: boot 1486527  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CAD8.md
namespace port_82B1CAD8 {

// The cursor object in r3. Nothing in docs/rw_audio_structs.h names it; the names below are
// readings of how this function uses the fields, not documented facts.
constexpr uint32_t kCursorCurve = 0;    // -> curve
constexpr uint32_t kCursorLastPos = 4;  // position the cached value was computed at
constexpr uint32_t kCursorValue = 8;    // cached result
constexpr uint32_t kCursorPos = 12;     // current position

// The curve the cursor points at.
constexpr uint32_t kCurveType = 0;      // u8: 2 = s16 samples, 1 = s8 samples, anything else = s32
constexpr uint32_t kCurveCount = 2;     // u16 sample count (bounds the interpolation neighbour only)
constexpr uint32_t kCurveStart = 4;     // s32 first position
constexpr uint32_t kCurveEnd = 8;       // s32 last position
constexpr uint32_t kCurveScale = 12;    // float: (pos - start) * scale = sample index
constexpr uint32_t kCurveSamples = 16;  // sample array

// Three rodata floats the body loads by absolute address. Read from the decrypted image
// (probe/harness/out/image): 1.0f, 0.5f, 0.0f. They are loaded from the guest exactly as the
// original does, not hard-coded, so a wrong reading of the image cannot cause a divergence.
constexpr uint32_t kOneAddr = 0x8231A844;   // lis r8,-32206 ; lfs f13,-22460(r8)
constexpr uint32_t kHalfAddr = 0x8209975C;  // lis r9,-32246 ; lfs f12,-26788(r9)
constexpr uint32_t kZeroAddr = 0x82165A10;  // lis r8,-32234 ; lfs f11,23056(r8)

// lfs: a single-precision load widened to double, as the lifted form does it.
inline double LoadF32(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// fctiwz, character for character the lifted line: NaN -> 0x80000000, above INT_MAX -> INT_MAX,
// else SSE truncation (which itself yields 0x80000000 below INT_MIN). The original spills the
// result with stfd and reads the low word back with lwz; callers take uint32_t() of this.
inline int64_t Fctiwz(double value) {
  return std::isnan(value) ? int64_t(0x80000000U)
         : (value > double(INT_MAX)) ? INT_MAX
                                     : simde_mm_cvttsd_si32(simde_mm_load_sd(&value));
}

// Round half away from zero and truncate: the shared tail of both rounding sites.
//   fcmpu cr6,f0,f11 ; blt -> fsubs f0,f0,f12 ; else fadds f0,f0,f12 ; fctiwz ; stfd ; lwz
// Unordered (NaN) is not "less than", so it takes the fadds path, as cr6.lt would.
inline uint32_t RoundToWord(PPCContext& ctx, double value, double half, double zero) {
  if (value < zero) {                          // blt cr6
    ctx.fpscr.disableFlushModeUnconditional();
    value = double(float(value - half));       // fsubs f0,f0,f12
  } else {
    value = double(float(value + half));       // fadds f0,f0,f12
  }
  return uint32_t(Fctiwz(value));              // fctiwz ; stfd -16/-8(r1) ; lwz -12/-4(r1)
}

REX_FUNC(Native) {
  const uint32_t cursor = ctx.r3.u32;
  const uint32_t pos = REX_LOAD_U32(cursor + kCursorPos);       // lwz r10,12(r3)
  const uint32_t last = REX_LOAD_U32(cursor + kCursorLastPos);  // lwz r11,4(r3)
  if (int32_t(pos) == int32_t(last)) {                          // cmpw cr6,r10,r11 ; beq cr6
    ctx.r3.u64 = REX_LOAD_U32(cursor + kCursorValue);           // lwz r3,8(r3)  (loc_82B1CD20)
    return;
  }

  // The curve pointer is loaded BEFORE the store to +4; the curve's own fields after it.
  const uint32_t curve = REX_LOAD_U32(cursor + kCursorCurve);   // lwz r11,0(r3)
  REX_STORE_U32(cursor + kCursorLastPos, pos);                  // stw r10,4(r3)
  const uint32_t start = REX_LOAD_U32(curve + kCurveStart);     // lwz r9,4(r11)
  const uint32_t end = REX_LOAD_U32(curve + kCurveEnd);         // lwz r8,8(r11)
  uint32_t clamped = pos;
  if (int32_t(pos) < int32_t(start)) {                          // cmpw cr6,r10,r9 ; bge cr6
    clamped = start;                                            // mr r10,r9
  } else if (int32_t(pos) > int32_t(end)) {                     // cmpw cr6,r10,r8 ; ble cr6
    clamped = end;                                              // mr r10,r8
  }

  ctx.fpscr.disableFlushModeUnconditional();                    // lfs f0,12(r11)
  const double scale = LoadF32(base, curve + kCurveScale);
  // subf r10,r9,r10 -- a 64-bit subtract of two zero-extended words; only the low 32 bits are
  // ever used (rlwinm, extsw, or a .u32 address), so a 32-bit index is exact.
  const uint32_t index = clamped - start;
  const double one = LoadF32(base, kOneAddr);                         // lfs f13,-22460(r8)

  if (scale == one) {                                           // fcmpu cr6,f0,f13 ; bne cr6
    // Nearest sample: scale 1.0 means position units are sample units. No bounds check.
    const uint8_t type = REX_LOAD_U8(curve + kCurveType);       // lbz r9,0(r11)
    uint32_t value;
    if (type == 2) {                                            // cmplwi cr6,r9,2 ; bne cr6
      // addi r10,r10,8 ; rlwinm r9,r10,1,0,30 ; lhzx r8,r9,r11 ; extsh r7,r8
      value = uint32_t(int32_t(int16_t(REX_LOAD_U16(curve + ((index + 8) << 1)))));
    } else if (type == 1) {                                     // cmplwi cr6,r9,1 ; bne cr6
      // add r11,r10,r11 ; lbz r10,16(r11) ; extsb r9,r10
      value = uint32_t(int32_t(int8_t(REX_LOAD_U8(curve + index + kCurveSamples))));
    } else {
      // addi r10,r10,4 ; rlwinm r9,r10,2,0,29 ; lwzx r8,r9,r11
      value = REX_LOAD_U32(curve + ((index + 4) << 2));
    }
    REX_STORE_U32(cursor + kCursorValue, value);                // stw r7/r9/r8,8(r3)
    ctx.r3.u64 = value;                                         // rotlwi r3,r7/r9/r8,0
    return;                                                     // blr
  }

  // Linear interpolation between the two samples around index * scale.  (loc_82B1CB8C)
  ctx.fpscr.disableFlushModeUnconditional();                    // extsw ; std ; lfd f13,-16(r1)
  const double fpos = double(float(double(int32_t(index))));    // fcfid f10,f13 ; frsp f9,f10
  const double half = LoadF32(base, kHalfAddr);                       // lfs f12,-26788(r9)
  const double zero = LoadF32(base, kZeroAddr);                       // lfs f11,23056(r8)
  const double scaled = double(float(fpos * scale));            // fmuls f13,f9,f0
  const double biased = double(float(scaled - half));           // fsubs f0,f13,f12
  const uint32_t i0 = RoundToWord(ctx, biased, half, zero);     // ... ; lwz r9,-12(r1)

  const uint32_t count = REX_LOAD_U16(curve + kCurveCount);     // lhz r8,2(r11)
  ctx.fpscr.disableFlushModeUnconditional();                    // extsw r7,r9 ; std ; lfd f0,-16(r1)
  const double fi0 = double(float(double(int32_t(i0))));        // fcfid f10,f0 ; frsp f9,f10
  const double frac = double(float(scaled - fi0));              // fsubs f10,f13,f9
  uint32_t i1 = i0 + 1;                                         // addi r10,r9,1
  if (!(int32_t(i1) < int32_t(count))) {                        // cmpw cr6,r10,r8 ; blt cr6
    i1 = count - 1;                                             // addi r10,r8,-1
  }

  // Fetch the pair by sample type. The original converts each with std/lfd/fcfid/frsp; in every
  // branch the i0 sample ends in f0 and the i1 sample in f13 (loc_82B1CCDC / loc_82B1CCE0).
  const uint8_t type = REX_LOAD_U8(curve + kCurveType);         // lbz r8,0(r11)
  double sample0;  // f0 at loc_82B1CCE0
  double sample1;  // f13 at loc_82B1CCE0
  if (type == 2) {                                              // cmplwi cr6,r8,2 ; bne cr6
    // addi ; addi ; rlwinm ; rlwinm ; lhzx r5,r7,r11 (i0) ; lhzx r4,r6,r11 (i1) ; extsh ; extsh
    const int16_t a = int16_t(REX_LOAD_U16(curve + ((i0 + 8) << 1)));
    const int16_t b = int16_t(REX_LOAD_U16(curve + ((i1 + 8) << 1)));
    ctx.fpscr.disableFlushModeUnconditional();                  // std ; lfd f0,-16(r1)
    sample1 = double(float(double(b)));                         // fcfid f9,f13 ; frsp f13,f9
    ctx.fpscr.disableFlushModeUnconditional();                  // (loc_82B1CCDC) frsp f0,f8
    sample0 = double(float(double(a)));                         // fcfid f8,f0 ; frsp f0,f8
  } else if (type == 1) {                                       // cmplwi cr6,r8,1 ; bne cr6
    // add r10,r10,r11 ; add r9,r9,r11 ; lbz r8,16(r10) (i1) ; lbz r7,16(r9) (i0) ; extsb ; extsb
    const int8_t b = int8_t(REX_LOAD_U8(curve + i1 + kCurveSamples));
    const int8_t a = int8_t(REX_LOAD_U8(curve + i0 + kCurveSamples));
    ctx.fpscr.disableFlushModeUnconditional();                  // std ; lfd f9,-8(r1)
    sample0 = double(float(double(a)));                         // fcfid f13,f0 ; frsp f0,f13
    sample1 = double(float(double(b)));                         // fcfid f8,f9 ; frsp f13,f8
  } else {
    // addi ; addi ; rlwinm ; rlwinm ; lwzx r4,r6,r11 (i1) ; lwzx r5,r7,r11 (i0) ; extsw ; extsw
    const int32_t b = int32_t(REX_LOAD_U32(curve + ((i1 + 4) << 2)));
    const int32_t a = int32_t(REX_LOAD_U32(curve + ((i0 + 4) << 2)));
    ctx.fpscr.disableFlushModeUnconditional();                  // std ; std ; lfd f0,-8(r1)
    sample1 = double(float(double(b)));                         // fcfid f13,f0 ; frsp f13,f13
    ctx.fpscr.disableFlushModeUnconditional();                  // (loc_82B1CCDC) frsp f0,f8
    sample0 = double(float(double(a)));                         // fcfid f8,f9 ; frsp f0,f8
  }

  ctx.fpscr.disableFlushModeUnconditional();                    // (loc_82B1CCE0)
  const double delta = double(float(sample1 - sample0));        // fsubs f13,f13,f0
  const double mixed = double(float(std::fma(delta, frac, sample0)));  // fmadds f0,f13,f10,f0
  if (mixed < zero) {                                           // fcmpu cr6,f0,f11 ; blt cr6
    const uint32_t value = RoundToWord(ctx, mixed, half, zero); // fsubs ; fctiwz ; stfd ; lwz r11,-4(r1)
    REX_STORE_U32(cursor + kCursorValue, value);                // stw r11,8(r3)
    ctx.r3.u64 = REX_LOAD_U32(cursor + kCursorValue);           // lwz r3,8(r3)  (loc_82B1CD20)
  } else {
    const uint32_t value = RoundToWord(ctx, mixed, half, zero); // fadds ; fctiwz ; stfd ; lwz r11,-4(r1)
    REX_STORE_U32(cursor + kCursorValue, value);                // stw r11,8(r3)
    ctx.r3.u64 = value;                                         // rotlwi r3,r11,0
  }
}

// Writes: only the cursor's +4 (last position) and +8 (cached value), both from r3. The
// cache-hit path writes nothing; declaring the span anyway compares equal. The std/stfd to
// -16(r1)/-8(r1) are the leaf's own red-zone scratch and are not windowed.
// Reads: the cursor, the curve header, and the sample table bounded by the curve's count (the
// original does no bounds check, so an index past count reads beyond this span; not compared).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t cursor = ctx.r3.u32;
  spec.read(cursor, 16);
  spec.write(cursor + kCursorLastPos, 8);
  const uint32_t pos = REX_LOAD_U32(cursor + kCursorPos);
  const uint32_t last = REX_LOAD_U32(cursor + kCursorLastPos);
  if (int32_t(pos) == int32_t(last)) return true;
  const uint32_t curve = REX_LOAD_U32(cursor + kCursorCurve);
  spec.read(curve, kCurveSamples);
  spec.read(kOneAddr, 4);
  spec.read(kHalfAddr, 4);
  spec.read(kZeroAddr, 4);
  const uint8_t type = REX_LOAD_U8(curve + kCurveType);
  const uint32_t elem = type == 2 ? 2 : type == 1 ? 1 : 4;
  const uint32_t count = REX_LOAD_U16(curve + kCurveCount);
  constexpr uint32_t kTableReadCap = 4096;
  const uint32_t bytes = count * elem;
  spec.read(curve + kCurveSamples, bytes < kTableReadCap ? bytes : kTableReadCap);
  return true;
}

}  // namespace port_82B1CAD8
SKATE3_PORT(82B1CAD8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
