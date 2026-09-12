// sub_82B3C2D0  role: time the Dac update, smooth the frame time, then retire voices while the budget lasts   lifted: skate3_recomp.68.cpp:6983 (231 lines)
// STATUS: gate-3 (reaches mftb through sub_82B1F7E8)
// calls/session: boot 18684  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C2D0.md
namespace port_82B3C2D0 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
//
// The profiler block. Field names are inferred from use here and in sub_82B219E8 /
// sub_82B217F0, which touch the same cells; nothing documents them.
//   +0   u32  the Dac object this thread services (also what sub_82B3C440 reads)
//   +4   u32  accumulated ticks of work, reset to zero every pass
//   +8   u32  the last timebase stamp taken
//   +16  f32  the smoothed frame time (the two-slot history plus this pass, scaled)
//   +20  f32  history slot 0        -- also the base of the stfsx below
//   +24  f32  history slot 1
//   +28  u32  which history slot to write next: 0 or 1, toggled by the cntlzw below
constexpr uint32_t kProfile = 0x830BDEBC;      // lis r11,-31988 ; addi r31,r11,-8516
constexpr uint32_t kProfileHistory = 20;       // addi r8,r31,20 -- the stfsx base

// r30 = 0x822F8600, the same constant pool base sub_82B2FE00 names kPool
// (lis r10,-32208 ; addi r30,r10,-31232).
constexpr uint32_t kFrameScale = 0x822F87B8;   // pool + 440   -- lfs f0,440(r30)
constexpr uint32_t kBudgetScale = 0x822F8E64;  // pool + 2148  -- lfs f13,2148(r30)
constexpr uint32_t kBudgetLimit = 0x820ED57C;  // lis r10,-32241 ; lfs f13,-10884(r10)
constexpr uint32_t kCostScale = 0x820ED958;    // addi r29,r10,-10884 ; lfs f12,988(r29)
constexpr uint32_t kZeroFloat = 0x82165A10;    // lis r11,-32234 ; lfs f30,23056(r11)

// The Dac object, as this function reads it.
constexpr uint32_t kDacLoad = 224;      // f32, compared against kBudgetLimit
constexpr uint32_t kDacVoices = 108;    // u32, the voice pointer array
constexpr uint32_t kDacVoiceCount = 280;  // u16
constexpr uint32_t kDacElapsed = 240;   // u32, this pass's total tick count

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

// bl 0x82b1f7e8 -- the timebase, through its own hook, exactly as the original reaches it.
uint64_t Timebase(PPCContext& ctx, uint8_t* base) {
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  return ctx.r3.u64;
}

REX_FUNC(Native) {
  ctx.fpscr.disableFlushModeUnconditional();  // stfd f30,-56(r1) -- the frame spill's mode call
  const uint64_t entry_ticks = Timebase(ctx, base);    // mr r28,r3
  const uint64_t work_start = Timebase(ctx, base);     // the second read, used immediately below

  const uint32_t accumulated = REX_LOAD_U32(kProfile + 4);   // lwz r10,4(r31)
  ctx.fpscr.disableFlushModeUnconditional();
  const double slot0 = LoadSingle(base, kProfile + 20);      // lfs f0,20(r31)
  const uint32_t last_stamp = REX_LOAD_U32(kProfile + 8);    // lwz r11,8(r31)
  const double slot1 = LoadSingle(base, kProfile + 24);      // lfs f13,24(r31)
  const double history = AddSingle(slot0, slot1);            // fadds f13,f0,f13
  const uint32_t slot = REX_LOAD_U32(kProfile + 28);         // lwz r9,28(r31)
  // subf/add on zero-extended u32 loads: 64-bit, and a borrow here leaves the upper half set.
  const uint64_t carried = uint64_t(accumulated) - uint64_t(last_stamp);  // subf r7,r11,r10
  const double frame_scale = LoadSingle(base, kFrameScale);  // lfs f0,440(r30)
  // rlwinm r6,r9,2,0,29 -- the slot index scaled to a float array subscript.
  const uint32_t slot_byte = (uint32_t(slot) << 2) & 0xFFFFFFFCu;
  const uint64_t spent = (work_start + carried) & 0xFFFFFFFFull;  // add r11,r3,r7 ; clrldi r5,r11,32
  // std r5,80(r1) ; lfd f12,80(r1) ; fcfid f11,f12 -- the function's own frame, so the round
  // trip is elided; the doubleword is the masked value, always non-negative.
  const double spent_double = double(int64_t(spent));
  const double this_frame = double(float(spent_double));     // frsp f10,f11
  const double smoothed = MulSingle(AddSingle(history, this_frame), frame_scale);  // fadds ; fmuls

  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, kProfile + 16, smoothed);                     // stfs f0,16(r31)
  StoreSingle(base, kProfile + kProfileHistory + slot_byte, this_frame);  // stfsx f10,r6,r8
  // The slot is reloaded after the stfsx, which aliases it when the index is ever 2.
  const uint32_t slot_after = REX_LOAD_U32(kProfile + 28);        // lwz r10,28(r31)
  REX_STORE_U32(kProfile + 4, 0);                                 // stw r11,4(r31), r11 = 0
  // cntlzw then rlwinm r11,r4,27,31,31 takes bit 5 of the count: 1 only for cntlzw(0) == 32.
  const uint32_t leading = slot_after == 0 ? 32u : uint32_t(__builtin_clz(slot_after));
  REX_STORE_U32(kProfile + 28, (leading >> 5) & 1u);              // stw r11,28(r31)

  const uint64_t work_end = Timebase(ctx, base);
  ctx.fpscr.disableFlushModeUnconditional();
  const uint32_t dac = REX_LOAD_U32(kProfile + 0);                // lwz r11,0(r31)
  REX_STORE_U32(kProfile + 8, uint32_t(work_end));                // stw r3,8(r31)
  const double budget_limit = LoadSingle(base, kBudgetLimit);     // lfs f13,-10884(r10)
  const double load = LoadSingle(base, dac + kDacLoad);           // lfs f0,224(r11)
  // fcmpu ; bge -- at or over the limit, nothing is retired this pass.
  if (load < budget_limit) {
    const uint32_t voice_count = REX_LOAD_U16(dac + kDacVoiceCount);  // lhz r10,280(r11)
    const double budget_scale = LoadSingle(base, kBudgetScale);    // lfs f13,2148(r30)
    const double smoothed_now = LoadSingle(base, kProfile + 16);   // lfs f11,16(r31)
    const double target = MulSingle(load, budget_scale);           // fmuls f13,f0,f13
    // The per-voice cost scale lives in ctx.f12, not a local, and that is deliberate: the
    // original loads it here and reads it again after `bl 0x82b3c440` on the loop's FIRST
    // iteration, without reloading. f12 is volatile on this ABI, so the value the fnmsubs sees
    // is whatever the callee left. Holding it in the register file reproduces that whatever
    // the callees do; a local would silently preserve the pre-call value.
    ctx.f12.f64 = LoadSingle(base, kCostScale);                    // lfs f12,988(r29)
    double total = MulSingle(smoothed_now, ctx.f12.f64);           // fmuls f0,f11,f12
    // cmpwi ; ble -- an empty voice list skips the summation loop.
    if (int32_t(voice_count) > 0) {
      uint32_t cursor = REX_LOAD_U32(dac + kDacVoices) - 8;        // lwz r11,108(r11) ; addi -8
      for (uint32_t i = 0; i < voice_count; i++) {                 // mtctr r10 ; bdnz
        cursor += 8;                                               // lwzu r10,8(r11)
        const uint32_t voice = REX_LOAD_U32(cursor);
        ctx.fpscr.disableFlushModeUnconditional();
        const double cost = LoadSingle(base, voice + 0);           // lfs f11,0(r10)
        total = double(float(std::fma(cost, ctx.f12.f64, total)));  // fmadds f0,f11,f12,f0
      }
    }
    ctx.fpscr.disableFlushModeUnconditional();
    double budget = SubSingle(total, target);                      // fsubs f31,f0,f13
    const double zero = LoadSingle(base, kZeroFloat);              // lfs f30,23056(r11)
    // fcmpu ; ble -- no surplus, no retirement. NaN compares unordered, so it also falls out.
    if (budget > zero) {
      for (;;) {
        // bl 0x82b3c440 -- picks the next retirable voice off the Dac's array; takes no
        // arguments, so whatever the previous call left in r3 flows in, as on the machine.
        skate3::audio::GuestCall(ctx, base, sub_82B3C440);
        const uint32_t voice = ctx.r3.u32;
        if (voice == 0) break;                                     // cmplwi ; beq
        ctx.fpscr.disableFlushModeUnconditional();
        const double cost = LoadSingle(base, voice + 0);           // lfs f0,0(r3)
        // fnmsubs f31,f0,f12,f31 -- budget minus this voice's cost, single-rounded once, with
        // f12 read out of the register file for the reason given above.
        budget = double(float(-std::fma(cost, ctx.f12.f64, -budget)));
        skate3::audio::GuestCall(ctx, base, sub_82B49100, voice, 2);  // li r4,2 ; bl 0x82b49100
        ctx.fpscr.disableFlushModeUnconditional();
        if (!(budget > zero)) break;                               // fcmpu ; bgt loops
        // loc_82B3C3E0: every iteration after the first does reload it.
        ctx.fpscr.disableFlushModeUnconditional();
        ctx.f12.f64 = LoadSingle(base, kCostScale);                // lfs f12,988(r29)
      }
    }
  }

  // loc_82B3C408: fold this pass into the accumulator and stamp the whole call's cost.
  const uint64_t tail_start = Timebase(ctx, base);
  const uint32_t accum_now = REX_LOAD_U32(kProfile + 4);           // lwz r10,4(r31)
  const uint32_t stamp_now = REX_LOAD_U32(kProfile + 8);           // lwz r11,8(r31)
  // subf then add, both 64-bit on zero-extended loads, truncated only by the store.
  const uint64_t folded = tail_start + (uint64_t(accum_now) - uint64_t(stamp_now));
  REX_STORE_U32(kProfile + 4, uint32_t(folded));                   // stw r11,4(r31)

  const uint64_t exit_ticks = Timebase(ctx, base);
  const uint32_t dac_now = REX_LOAD_U32(kProfile + 0);             // lwz r11,0(r31)
  const uint64_t elapsed = exit_ticks - entry_ticks;               // subf r10,r28,r3
  REX_STORE_U32(dac_now + kDacElapsed, uint32_t(elapsed));         // stw r10,240(r11)
  ctx.fpscr.disableFlushModeUnconditional();  // lfd f30,-56(r1) -- the frame reload's mode call
  // r3 is left holding the exit timebase; both call sites ignore it.
}

// Never called: the macro skips the shadow branch for a gate-labelled port. The write set this
// would have declared is in the note, for whoever gains a way to stub the timebase.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3C2D0
SKATE3_PORT(82B3C2D0, skate3::audio::kPortGate3, skate3::audio::kReturnR3)
