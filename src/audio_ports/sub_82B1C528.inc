// sub_82B1C528  role: random value in a range -- when +12 is set, draw the counter and store +0 + draw % +4 into +8 and return it; otherwise hand back the cached +8 untouched   lifted: skate3_recomp.66.cpp:64370 (64 lines)
// STATUS: verified
// calls/session: boot 159493  play 203825  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C528.md
namespace port_82B1C528 {

// r3 = the object. Nothing in docs/rw_audio_structs.h names it, so the offsets stay plain.
constexpr uint32_t kBase = 0;       // u32  added to the remainder to form the result
constexpr uint32_t kRange = 4;      // u32  the modulus; a zero low word traps (twllei)
constexpr uint32_t kCurrent = 8;    // u32  the cached result, and the only word written
constexpr uint32_t kEnabled = 12;   // u32  zero: return +8 with no draw and no store
constexpr uint32_t kHeaderBytes = 16;

// lis r11,-31993 ; addi r11,r11,30192 -- 0x83070000 + 0x75F0, computed as
// ((imm & 0xFFFF) << 16) + offset, not read off. sub_82B1F360's six-word cascading counter is
// the only state the draw depends on: no timebase, no lock, no allocation, no indirect call.
constexpr uint32_t kCounter = 0x830775F0;
constexpr uint32_t kCounterBytes = 24;

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                        // mr r31,r3
  const uint32_t enabled = REX_LOAD_U32(object + kEnabled);  // lwz r11,12(r3)
  // cmpwi cr6,r11,0 ; bne cr6,0x82b1c560
  if (enabled == 0) {
    ctx.r3.u64 = REX_LOAD_U32(object + kCurrent);  // lwz r3,8(r3) -- zero-extended
    return;                                       // blr
  }

  // loc_82B1C560
  skate3::audio::GuestCall(ctx, base, sub_82B1F360);  // bl 0x82b1f360 -- no arguments
  const uint64_t draw = ctx.r3.u64;                   // the FULL 64-bit counter word

  const uint32_t range = REX_LOAD_U32(object + kRange);  // lwz r11,4(r31)
  const uint32_t start = REX_LOAD_U32(object + kBase);   // lwz r10,0(r31)
  // divwu r9,r3,r11 -- the LOW word of the draw over the range; RexGlue yields 0 on a zero
  // divisor rather than faulting, and that definition is what the harness compares against.
  const uint32_t quotient = range != 0 ? uint32_t(draw) / range : 0u;
  // twllei r11,0 -- kept because the original keeps it; ppc_trap(type 0) only logs a warning and
  // returns, so it is replayable. The lifted line's `r11.u32 < 0u` half is dead and is dropped,
  // because clang rejects a tautological unsigned compare in src/ even though generated/ is fine.
  if (static_cast<int32_t>(range) == 0) ppc_trap(ctx, base, 0);
  // mullw r8,r9,r11 ; subf r11,r8,r3 ; add r3,r11,r10 -- 64-BIT throughout. mullw is a 64-bit
  // product of two sign-extended words, the subtract carries the counter's high half with it, and
  // the final add sums two zero-extended words so a carry lands in bit 32. r3 keeps all 64 bits;
  // only the stw truncates.
  const uint64_t product =
      static_cast<uint64_t>(int64_t(int32_t(quotient)) * int64_t(int32_t(range)));
  ctx.r3.u64 = (draw - product) + uint64_t(start);
  REX_STORE_U32(object + kCurrent, uint32_t(ctx.r3.u64));  // stw r3,8(r31)
}

// One store, always to +8, plus sub_82B1F360's whole counter. The early path stores nothing and
// decides on a word read BEFORE the call, so it is predicted exactly.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  spec.read(object, kHeaderBytes);
  // lwz r11,12(r3) runs before the branch and before the call: entry state settles the path.
  if (REX_LOAD_U32(object + kEnabled) == 0) return true;

  // +0 and +4 are read AFTER sub_82B1F360 has advanced its counter. If that global overlapped the
  // header, neither the range nor the base would derive from entry state -- gate 2 for the call.
  if (uint64_t(object) < uint64_t(kCounter) + kCounterBytes &&
      uint64_t(kCounter) < uint64_t(object) + kHeaderBytes) {
    return false;
  }

  spec.write(object + kCurrent, 4);     // stw r3,8(r31)
  spec.write(kCounter, kCounterBytes);  // sub_82B1F360's six words, all six written every call
  spec.read(kCounter, kCounterBytes);
  return true;
}

}  // namespace port_82B1C528
SKATE3_PORT(82B1C528, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
