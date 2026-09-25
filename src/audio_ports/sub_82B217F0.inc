// sub_82B217F0  role: start the Dac profiler -- round the config rate, start the worker, zero the timing block   lifted: skate3_recomp.67.cpp:8933 (112 lines)
// STATUS: partial -- compared 0, skipped 1, zero divergence (session g1_splits, 150 s scripted); armed and waiting: the one call took the uncomparable path. NEVER promoted: the skipped path has a native body but no comparison. (comparable only when the run-state word at 0x830845CC is non-zero -- that path is a pure `li r3,16` with no stores, no callee and no timebase read; the zero path is gate-3 through sub_82B1F7E8's mftb and gate-1 through sub_82B218E8's bctrl, and stays uncompared)
// calls/session: boot 1  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B217F0.md
namespace port_82B217F0 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
constexpr uint32_t kRunState = 0x830845CC;  // lis r10,-31992 ; lwz r10,17868(r10)
constexpr uint32_t kProfile = 0x830BDEBC;   // lis r10,-31988 ; addi r31,r10,-8516
constexpr uint32_t kZeroFloat = 0x82165A10;  // lis r10,-32234 ; lfs f31,23056(r10)
constexpr uint32_t kRoundHalf = 0x8209975C;  // lis r11,-32246 ; lfs f13,-26788(r11)

// The config object this call takes in r3.
constexpr uint32_t kConfigTarget = 4;  // u32, the object handed to sub_82B218E8
constexpr uint32_t kConfigRate = 12;   // f32, rounded to an integer for sub_82B218E8's r4

constexpr int64_t kStatus = 16;  // li r3,16 -- returned on both paths

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

// fctiwz then stfd/lwz of the low word: NaN gives 0x80000000, above INT_MAX saturates, and only
// the low 32 bits of the spilled doubleword reach the register.
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(static_cast<uint64_t>(fixed) & 0xFFFFFFFFu);
}

REX_FUNC(Native) {
  const uint32_t config = ctx.r3.u32;  // mr r11,r3
  ctx.fpscr.disableFlushModeUnconditional();  // stfd f31,-24(r1) -- the frame spill's mode call
  const uint32_t target = REX_LOAD_U32(config + kConfigTarget);  // lwz r3,4(r3)
  const uint32_t run_state = REX_LOAD_U32(kRunState);            // lwz r10,17868(r10)
  // cmpwi ; bne -- already running: no stores, no timebase read, just the status.
  if (run_state == 0) {
    const double requested = LoadSingle(base, config + kConfigRate);  // lfs f0,12(r11)
    const double zero = LoadSingle(base, kZeroFloat);                 // lfs f31,23056(r10)
    const double half = LoadSingle(base, kRoundHalf);                 // lfs f13,-26788(r11)
    double rounded;
    // blt on the compare against zero: round half away from zero, then truncate.
    if (requested < zero) {
      ctx.fpscr.disableFlushModeUnconditional();  // loc_82B2184C is a branch target
      rounded = SubSingle(requested, half);       // fsubs f0,f0,f13
    } else {
      rounded = AddSingle(requested, half);       // fadds f0,f0,f13
    }
    // fctiwz f13,f0 ; stfd f13,80(r1) ; lwz r4,84(r1) -- the spill is this call's own frame.
    const uint32_t rate = FctiwzLowWord(rounded);
    skate3::audio::GuestCall(ctx, base, sub_82B218E8, target, rate);  // bl 0x82b218e8

    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, kProfile + 16, zero);  // stfs f31,16(r31) -- the smoothed frame time
    REX_STORE_U32(kProfile + 4, 0);          // stw r11,4(r31), r11 = 0 -- the accumulator
    StoreSingle(base, kProfile + 20, zero);  // stfs f31,20(r31) -- history slot 0
    StoreSingle(base, kProfile + 24, zero);  // stfs f31,24(r31) -- history slot 1

    skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);  // bl 0x82b1f7e8
    REX_STORE_U32(kProfile + 8, ctx.r3.u32);            // stw r3,8(r31) -- the first stamp
  }
  // loc_82B21888
  ctx.r3.s64 = kStatus;  // li r3,16
  ctx.fpscr.disableFlushModeUnconditional();  // lfd f31,-24(r1) -- the frame reload's mode call
}

// Split by path. Both gates sit on the far side of one test: the zero path reaches sub_82B218E8's
// `bctrl` (gate 1) and then stamps 0x830BDEC4 from sub_82B1F7E8's `mftb` (gate 3, and no care in
// any rewrite can make two timebase reads agree). The non-zero path -- already running -- has no
// stores, no callee and no timebase read, so it is deterministic and fully comparable. Its write
// set is empty; what it checks is that the guard is read from the right word and that the status
// really is the constant 16 on that path. The float rounding, the profiler block and the stamp
// stay unchecked, which is what the header line says.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  spec.read(kRunState, 4);
  spec.read(ctx.r3.u32 + kConfigTarget, 4);  // lwz r3,4(r3), before the branch on both paths
  // lwz r10,17868(r10) ; cmpwi cr6,r10,0 ; bne cr6,0x82b21888
  return REX_LOAD_U32(kRunState) != 0;
}

}  // namespace port_82B217F0
SKATE3_PORT(82B217F0, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
