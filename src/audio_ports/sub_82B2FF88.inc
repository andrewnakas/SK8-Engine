// sub_82B2FF88  role: rearm the level table for the object's layout -- one, two or three candidate singles copied from rodata to +1060.., each paired with a count at +1072.. that is the object's +1100 times a rodata rate, rounded half away from zero; the pair count goes to +1089   lifted: skate3_recomp.67.cpp:44457 (338 lines)
// STATUS: pending -- written 2026-09-13, not yet compared under the shadow harness
// calls/session: boot ?  play ?  map ?          thread: first called on load_thread (docs/audio-executed-set.txt); reached from sub_82B2F590
// note: outside the 216 -- ported because sub_82B2F590 cannot be ported to Rust without it. The three tables it fills are exactly what sub_82B2F798 reads back
namespace port_82B2FF88 {

// Every address computed as ((imm & 0xFFFF) << 16) + offset, never read by eye.
constexpr uint32_t kRateOne = 0x82116288;      // lis -32239 ; lfs 25224
constexpr uint32_t kCandidateOne = 0x821161AC; // lis -32239 ; lfs 25004
constexpr uint32_t kZeroSingle = 0x82165A10;   // lis -32234 ; lfs 23056
constexpr uint32_t kHalfSingle = 0x8209975C;   // lis -32246 ; lfs -26788
constexpr uint32_t kRateFirst = 0x822F8A50;    // lis -32208 ; addi -31232 ; lfs 1104
constexpr uint32_t kCandidateLast = 0x822F8A54; // ... lfs 1108
constexpr uint32_t kRateLast = 0x822F8A58;     // ... lfs 1112
constexpr uint32_t kCandidateFirst = 0x820BA9D0; // lis -32244 ; lfs -22064
// The object (r3). The same three tables sub_82B2F798 reads: candidates at +1060, counts at +1072,
// the number of pairs at +1089. The source value is +1100; the layout byte is sub_82B370E8's +42.
constexpr uint32_t kLayout = 42;
constexpr uint32_t kCandidates = 1060;
constexpr uint32_t kCounts = 1072;
constexpr uint32_t kPairs = 1089;
constexpr uint32_t kSource = 1100;

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}
void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}
double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return uint32_t(uint64_t(fixed) & 0xFFFFFFFFu);
}
// fcmpu against the zero cell ; blt -- below zero subtracts the half, anything else (a NaN
// included) adds it; then fctiwz and the low word of the red-zone spill at r1-16.
uint32_t RoundAway(PPCContext& ctx, double value, double zero, double half) {
  ctx.fpscr.disableFlushModeUnconditional();
  return FctiwzLowWord(value < zero ? SubSingle(value, half) : AddSingle(value, half));
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t layout = REX_LOAD_U8(object + kLayout);  // lbz r11,42(r3)
  ctx.fpscr.disableFlushModeUnconditional();
  const double source = LoadSingle(base, object + kSource);  // lfs f13,1100(r3)
  if (layout == 1) {
    // One pair: rate kRateOne, candidate kCandidateOne.
    const double scaled = MulSingle(source, LoadSingle(base, kRateOne));   // fmuls f0,f13,f0
    const double zero = LoadSingle(base, kZeroSingle);
    const double candidate = LoadSingle(base, kCandidateOne);
    REX_STORE_U8(object + kPairs, 1);                                     // stb r8,1089(r3)
    StoreSingle(base, object + kCandidates, candidate);                   // stfs f13,1060(r3)
    const double half = LoadSingle(base, kHalfSingle);
    REX_STORE_U32(object + kCounts, RoundAway(ctx, scaled, zero, half));  // stw r11,1072(r3)
    return;
  }
  const double zero = LoadSingle(base, kZeroSingle);
  const double scaled_first = MulSingle(source, LoadSingle(base, kRateFirst));  // lfs 1104 ; fmuls
  if (layout == 2 || layout == 4) {
    // loc_82B3010C -- two pairs: the first and the last.
    REX_STORE_U8(object + kPairs, 2);                                          // stb r7,1089(r3)
    StoreSingle(base, object + kCandidates, LoadSingle(base, kCandidateFirst));  // stfs 1060(r3)
    const double half = LoadSingle(base, kHalfSingle);
    const uint32_t first = RoundAway(ctx, scaled_first, zero, half);
    ctx.fpscr.disableFlushModeUnconditional();
    const double rate_last = LoadSingle(base, kRateLast);                     // lfs f0,1112(r10)
    REX_STORE_U32(object + kCounts, first);                                   // stw r11,1072(r3)
    const double scaled_last = MulSingle(source, rate_last);
    StoreSingle(base, object + kCandidates + 4, LoadSingle(base, kCandidateLast));  // stfs 1064(r3)
    REX_STORE_U32(object + kCounts + 4, RoundAway(ctx, scaled_last, zero, half));   // stw 1076(r3)
    return;
  }
  // Every other layout -- three pairs: the first, the one-pair layout's, and the last.
  REX_STORE_U8(object + kPairs, 3);                                            // stb r7,1089(r3)
  const double candidate_first = LoadSingle(base, kCandidateFirst);            // lfs f11,-22064(r8)
  const double half = LoadSingle(base, kHalfSingle);                           // lfs f10,0(r11)
  StoreSingle(base, object + kCandidates, candidate_first);                    // stfs f11,1060(r3)
  REX_STORE_U32(object + kCounts, RoundAway(ctx, scaled_first, zero, half));   // stw r11,1072(r3)
  ctx.fpscr.disableFlushModeUnconditional();
  const double scaled_middle = MulSingle(source, LoadSingle(base, kRateOne));  // lfs 25224 ; fmuls
  StoreSingle(base, object + kCandidates + 4, LoadSingle(base, kCandidateOne));  // stfs 1064(r3)
  const uint32_t middle = RoundAway(ctx, scaled_middle, zero, half);
  ctx.fpscr.disableFlushModeUnconditional();
  const double rate_last = LoadSingle(base, kRateLast);                        // lfs f0,1112(r10)
  REX_STORE_U32(object + kCounts + 4, middle);                                 // stw r11,1076(r3)
  const double scaled_last = MulSingle(source, rate_last);
  StoreSingle(base, object + kCandidates + 8, LoadSingle(base, kCandidateLast));  // stfs 1068(r3)
  REX_STORE_U32(object + kCounts + 8, RoundAway(ctx, scaled_last, zero, half));   // stw 1080(r3)
}

// A fixed write set, from r3 alone: the candidates and counts (+1060 .. +1083) and the pair byte.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;
  spec.write(object + kCandidates, kCounts + 12 - kCandidates);
  spec.write(object + kPairs, 1);
  spec.read(object + kLayout, 1);
  spec.read(object + kSource, 4);
  spec.read(kRateOne, 4);
  spec.read(kCandidateOne, 4);
  spec.read(kZeroSingle, 4);
  spec.read(kHalfSingle, 4);
  spec.read(kRateFirst, 12);  // 1104, 1108 and 1112 are contiguous
  spec.read(kCandidateFirst, 4);
  return true;
}

}  // namespace port_82B2FF88
SKATE3_PORT(82B2FF88, skate3::audio::kPortPending, skate3::audio::kReturnNone)
