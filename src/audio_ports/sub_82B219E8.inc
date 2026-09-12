// sub_82B219E8  role: close out a Dac update -- run the budgeted pass, publish the stats snapshot, advance the mixer clock   lifted: skate3_recomp.67.cpp:9240 (124 lines)
// STATUS: gate-3 (reaches mftb through sub_82B1F7E8)
// calls/session: boot 18684  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B219E8.md
namespace port_82B219E8 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
constexpr uint32_t kStats = 0x83077630;      // lis r9,-31993 ; addi r6,r9,30256
constexpr uint32_t kStatsOwner = 0x8307762C;  // lwz r10,30252(r8), r8 = the same lis
constexpr uint32_t kProfile = 0x830BDEBC;    // lis r4,-31988 ; addi r10,r4,-8516
// r7 = 0x822F8600, the constant pool base sub_82B2FE00 names kPool
// (lis r11,-32208 ; addi r7,r11,-31232).
constexpr uint32_t kSnapshotRate = 0x822F87CC;  // pool + 460 -- lfs f13,460(r7)

// The snapshot block the call publishes and then hands to sub_82B44858 in r4.
constexpr uint32_t kSnapClock = 0;    // f64, copied from the Dac's +8
constexpr uint32_t kSnapVoices = 8;   // u32, the Dac's voice pointer array
constexpr uint32_t kSnapRate = 12;    // f32, the pool constant above
constexpr uint32_t kSnapCount = 20;   // u16, the Dac's live voice count
constexpr uint32_t kSnapFlag = 22;    // u8, set when kStatsOwner's byte at +293 is non-zero

// The update context this call takes in r3.
constexpr uint32_t kCtxDac = 8;       // u32, the Dac object
constexpr uint32_t kCtxSink = 44;     // u32, the object passed to sub_82B44858

// The Dac object.
constexpr uint32_t kDacClock = 8;       // f64, the running mixer clock
constexpr uint32_t kDacVoices = 108;    // u32
constexpr uint32_t kDacSubmitted = 228; // u32, the sink's own +44 mirrored back
constexpr uint32_t kDacIncrement = 212; // f32, one block's worth of clock
constexpr uint32_t kDacVoiceCount = 280;  // u16

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

double LoadDouble(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u64 = REX_LOAD_U64(addr);
  return temp.f64;
}

void StoreDouble(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f64 = value;
  REX_STORE_U64(addr, temp.u64);
}

REX_FUNC(Native) {
  const uint32_t context = ctx.r3.u32;  // mr r31,r3

  skate3::audio::GuestCall(ctx, base, sub_82B3C2D0);  // bl 0x82b3c2d0

  // Each read of the Dac pointer is its own load, as lifted: the stores below could alias it.
  const uint32_t dac_for_clock = REX_LOAD_U32(context + kCtxDac);  // lwz r5,8(r31)
  ctx.fpscr.disableFlushModeUnconditional();
  const double clock = LoadDouble(base, dac_for_clock + kDacClock);  // lfd f0,8(r5)
  StoreDouble(base, kStats + kSnapClock, clock);       // stfd f0,30256(r9)

  const uint32_t dac_for_voices = REX_LOAD_U32(context + kCtxDac);  // lwz r3,8(r31)
  const uint32_t owner = REX_LOAD_U32(kStatsOwner);                 // lwz r10,30252(r8)
  const double rate = LoadSingle(base, kSnapshotRate);              // lfs f13,460(r7)
  const uint32_t voices = REX_LOAD_U32(dac_for_voices + kDacVoices);  // lwz r11,108(r3)
  StoreSingle(base, kStats + kSnapRate, rate);         // stfs f13,12(r6)
  REX_STORE_U32(kStats + kSnapVoices, voices);         // stw r11,8(r6)

  const uint32_t count =
      REX_LOAD_U16(REX_LOAD_U32(context + kCtxDac) + kDacVoiceCount);  // lwz r11,8(r31) ; lhz 280
  REX_STORE_U16(kStats + kSnapCount, uint16_t(count));  // sth r11,20(r6)

  const uint32_t owner_flag = REX_LOAD_U8(owner + 293);  // lbz r10,293(r10)
  // addic r9,r10,-1 ; subfe r11,r9,r10 -- the carry idiom for (byte != 0): 1 or 0, nothing else.
  REX_STORE_U8(kStats + kSnapFlag, uint8_t(owner_flag != 0 ? 1 : 0));  // stb r11,22(r6)

  const uint32_t sink_in = REX_LOAD_U32(context + kCtxSink);  // lwz r3,44(r31)
  // bl 0x82b44858 with r3 = the sink and r4 = the snapshot block, which mr r4,r6 left live.
  skate3::audio::GuestCall(ctx, base, sub_82B44858, sink_in, kStats);
  const uint64_t result = ctx.r3.u64;  // mr r30,r3

  const uint32_t sink = REX_LOAD_U32(context + kCtxSink);       // lwz r8,44(r31)
  const uint32_t submitted = REX_LOAD_U32(sink + 44);           // lwz r7,44(r8)
  const uint32_t dac_for_submit = REX_LOAD_U32(context + kCtxDac);  // lwz r6,8(r31)
  REX_STORE_U32(dac_for_submit + kDacSubmitted, submitted);     // stw r7,228(r6)

  const uint32_t dac = REX_LOAD_U32(context + kCtxDac);         // lwz r5,8(r31)
  ctx.fpscr.disableFlushModeUnconditional();
  const double clock_now = LoadDouble(base, dac + kDacClock);   // lfd f0,8(r5)
  const double increment = LoadSingle(base, dac + kDacIncrement);  // lfs f13,212(r5)
  // fadd, not fadds: the sum keeps double precision, and stfd stores all 64 bits.
  StoreDouble(base, dac + kDacClock, increment + clock_now);    // stfd f12,8(r5)

  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);            // bl 0x82b1f7e8
  REX_STORE_U32(kProfile + 8, ctx.r3.u32);                      // stw r11,8(r10)

  ctx.r3.u64 = result;  // mr r3,r30
}

// Never called: the macro skips the shadow branch for a gate-labelled port. The write set this
// would have declared is in the note, for whoever gains a way to stub the timebase.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B219E8
SKATE3_PORT(82B219E8, skate3::audio::kPortGate3, skate3::audio::kReturnR3)
