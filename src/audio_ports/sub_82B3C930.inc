// sub_82B3C930  role: EVENT_STOP's decoder teardown -- stop the decoder, release the voice it holds, unregister it, then release the decoder itself   lifted: skate3_recomp.68.cpp:8013 (96 lines)
// STATUS: gate-1 (four indirect calls: sub_82B3C930's own bctrl x4 -- the decoder's +12 stop hook, self vtable +4, and the core manager's release slot +12 twice)
// calls/session: boot 1926  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C930.md
namespace port_82B3C930 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
// lis r30,-31993 -> 0x83070000; +30252 -> 0x8307762C. The same cell port_82B219E8 names
// kStatsOwner: a pointer to the audio core object whose +36 holds the dispatch table.
constexpr uint32_t kCoreOwnerPtr = 0x8307762C;

constexpr uint32_t kSelfVtable = 0;      // self +0: this object's own dispatch table
constexpr uint32_t kStopHook = 12;       // self +12: a raw function pointer, 0 when idle
constexpr uint32_t kHeldVoice = 16;      // self +16: the voice to release, 0 when none
constexpr uint32_t kSelfSlotUnregister = 4;   // self vtable +4
constexpr uint32_t kOwnerTable = 36;     // core owner +36: the object holding the table
constexpr uint32_t kSlotRelease = 12;    // that table +12: the release entry

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3
  const uint64_t entry_lr = ctx.lr;  // mflr r12 / stw r12,-8(r1)

  // lwz r11,12(r3) / cmplwi cr6,r11,0 / beq cr6,0x82b3c95c
  const uint32_t stop_hook = REX_LOAD_U32(self + kStopHook);
  if (stop_hook != 0) {
    // mtctr r11 / bctrl -- a bare function pointer, called with r3 still the entry object
    // and nothing else set up.
    ctx.ctr.u64 = stop_hook;
    ctx.lr = 0x82B3C95C;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  }

  // lwz r4,16(r31) / cmplwi cr6,r4,0 / beq cr6,0x82b3c988
  const uint32_t held_voice = REX_LOAD_U32(self + kHeldVoice);
  if (held_voice != 0) {
    // lwz r11,30252(r30) / lwz r3,36(r11) / lwz r11,0(r3) / lwz r10,12(r11) / mtctr / bctrl
    // Release the held voice: r3 = the table owner, r4 = the voice, r5 = 0. The same
    // (owner+36) -> (+0) -> (+12) chain sub_82B34D60 tail-branches to.
    const uint32_t owner = REX_LOAD_U32(REX_LOAD_U32(kCoreOwnerPtr) + kOwnerTable);
    ctx.r3.u64 = owner;
    ctx.r4.u64 = held_voice;
    ctx.r5.s64 = 0;
    ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(owner + kSelfVtable) + kSlotRelease);
    ctx.lr = 0x82B3C988;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  }

  // lwz r11,0(r31) / lwz r10,4(r11) / lwz r30,30252(r30) / mtctr r10 / bctrl
  // The core-owner POINTER is loaded BEFORE this call, into the callee-saved r30, because
  // the call clobbers the volatiles. Kept in that order: the cell is read before the
  // unregister runs, not after.
  const uint32_t core_owner = REX_LOAD_U32(kCoreOwnerPtr);
  ctx.r3.u64 = self;   // mr r3,r31
  ctx.r4.s64 = 0;      // li r4,0
  ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(self + kSelfVtable) + kSelfSlotUnregister);
  ctx.lr = 0x82B3C9A4;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  // lwz r3,36(r30) / lwz r9,0(r3) / lwz r8,12(r9) / mr r4,r31 / li r5,0 / mtctr r8 / bctrl
  // Release the decoder object itself through the same release slot. Unconditional: this is
  // the call no rewind can undo.
  const uint32_t owner = REX_LOAD_U32(core_owner + kOwnerTable);
  ctx.r3.u64 = owner;
  ctx.r4.u64 = self;
  ctx.r5.s64 = 0;
  ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(owner + kSelfVtable) + kSlotRelease);
  ctx.lr = 0x82B3C9C0;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  // lwz r12,-8(r1) / mtlr r12 -- the frame restores the caller's return address, so lr is
  // net unchanged across the call however many indirect calls ran.
  ctx.lr = entry_lr;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3C930
SKATE3_PORT(82B3C930, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
