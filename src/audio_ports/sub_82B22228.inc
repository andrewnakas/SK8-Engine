// sub_82B22228  role: clamp the span at +72 up to the floor at +52, refresh the cached rate at +68 from the global at 0x822F87CC (clearing +64 whenever it changed), then when the count at +112 is below round(span * rate) hand the subobject at +100 to sub_82B3D668   lifted: skate3_recomp.67.cpp:10458 (115 lines)
// STATUS: gate-1 (sub_82B3D668 goes to the core allocator through two bctrls one level down -- the vtable's +4 allocate at 0x82B3D6F8 and its +12 free at 0x82B3D838; the call here is `bl 0x82b3d668` at 0x82B222C4)
// calls/session: boot 184376  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B22228.md
namespace port_82B22228 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
// lis r11,-32208 -> 0x82300000 ; addi r10,r11,-31232 -> 0x822F8600 ; lfs f0,460(r10).
constexpr uint32_t kRateCell = 0x822F87CCu;
// The same two literal cells port_82B217F0 names, reached by the same lis/lfs pairs.
constexpr uint32_t kZeroFloat = 0x82165A10u;  // lis r11,-32234 ; lfs f13,23056(r11)
constexpr uint32_t kRoundHalf = 0x8209975Cu;  // lis r11,-32246 ; lfs f13,-26788(r11)

// Nothing in docs/rw_audio_structs.h names this object; plain offsets are honest.
constexpr uint32_t kFloor = 52;       // f32, a lower bound for +72
constexpr uint32_t kCounter = 64;     // u32, cleared when the cached rate is refreshed
constexpr uint32_t kCachedRate = 68;  // f32, a copy of the global at kRateCell
constexpr uint32_t kSpan = 72;        // f32, clamped up to kFloor
constexpr uint32_t kSub = 100;        // the subobject handed to sub_82B3D668
constexpr uint32_t kHave = 112;       // s32, compared SIGNED against the rounded product

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)
constexpr uint32_t kLinkSlot = 8;     // stw r12,-8(r1), i.e. frame + 88
constexpr uint32_t kSpillSlot = 80;   // stfd f13,80(r1) -- inside the 96 bytes, so not a window

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

double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }
double MulSingle(double a, double b) { return double(float(a * b)); }

// fctiwz: NaN gives 0x80000000, anything above INT_MAX saturates, and the result is what the
// following stfd spills as a doubleword.
uint64_t FctiwzDoubleword(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint64_t>(fixed);
}

REX_FUNC(Native) {
  // The frame is reproduced because sub_82B3D668 runs on it and because the fctiwz result is
  // spilled into it and read back out; both slots are inside the 96 bytes this call allocates,
  // so neither is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  // stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;

  ctx.fpscr.disableFlushModeUnconditional();
  const double floor_value = LoadSingle(base, self + kFloor);  // lfs f0,52(r3)
  const double span = LoadSingle(base, self + kSpan);          // lfs f13,72(r3)
  // fcmpu cr6,f13,f0 ; bge cr6,0x82b22248 -- an unordered compare leaves cr6.lt clear, so a NaN
  // span stores nothing.
  if (span < floor_value) {
    StoreSingle(base, self + kSpan, floor_value);  // stfs f0,72(r3)
  }

  // loc_82B22248
  ctx.fpscr.disableFlushModeUnconditional();
  const double cached_rate = LoadSingle(base, self + kCachedRate);  // lfs f13,68(r3)
  const double live_rate = LoadSingle(base, kRateCell);             // lfs f0,460(r10)
  // fcmpu cr6,f13,f0 ; beq cr6,0x82b2226c -- `!(a == b)` is `!cr6.eq`, which is also what an
  // unordered compare gives, so a NaN on either side takes the refresh path.
  if (!(cached_rate == live_rate)) {
    StoreSingle(base, self + kCachedRate, live_rate);  // stfs f0,68(r3)
    REX_STORE_U32(self + kCounter, 0);                 // stw r11,64(r3), r11 = 0
  }

  // loc_82B2226C -- both floats are RELOADED here; the store above may have changed +68.
  ctx.fpscr.disableFlushModeUnconditional();
  const double rate = LoadSingle(base, self + kCachedRate);   // lfs f13,68(r3)
  const double clamped = LoadSingle(base, self + kSpan);      // lfs f0,72(r3)
  const double product = MulSingle(clamped, rate);            // fmuls f0,f0,f13
  const double zero = LoadSingle(base, kZeroFloat);           // lfs f13,23056(r11)
  const bool negative = product < zero;                       // fcmpu cr6,f0,f13
  const double half = LoadSingle(base, kRoundHalf);           // lfs f13,-26788(r11) -- loaded
                                                              // before the branch, as lifted
  double rounded;
  // blt cr6,0x82b222a4 -- round half away from zero, then truncate.
  if (negative) {
    ctx.fpscr.disableFlushModeUnconditional();  // loc_82B222A4 is a branch target
    rounded = SubSingle(product, half);         // fsubs f0,f0,f13
  } else {
    rounded = AddSingle(product, half);         // fadds f0,f0,f13
  }
  // fctiwz f13,f0 ; stfd f13,80(r1) ; lwz r4,84(r1) -- the spill and the low-word reload both
  // land in this call's own frame, and are reproduced rather than folded.
  REX_STORE_U64(ctx.r1.u32 + kSpillSlot, FctiwzDoubleword(rounded));
  const uint32_t needed = REX_LOAD_U32(ctx.r1.u32 + kSpillSlot + 4);

  // loc_82B222B4 -- cmpw cr6,r11,r4 ; bge cr6,0x82b222c8: SIGNED, on both sides.
  const int32_t have = static_cast<int32_t>(REX_LOAD_U32(self + kHave));  // lwz r11,112(r3)
  if (have < static_cast<int32_t>(needed)) {
    // addi r3,r3,100 ; bl 0x82b3d668 -- the gate-1 call. Nothing but r3 is set up.
    skate3::audio::GuestCall(ctx, base, sub_82B3D668, self + kSub);
  }

  // loc_82B222C8 -- no `li r3,...` on either path: r3 is `self` when the call is skipped and
  // sub_82B3D668's result when it is not.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;               // addi r1,r1,96
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B22228
SKATE3_PORT(82B22228, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
