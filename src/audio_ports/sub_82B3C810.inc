// sub_82B3C810  role: destructor -- install the vtable 0x8231BAA0 at *this, then when bit 0 of the flags is set fetch the allocator (sub_828AAF88) and free `this` through its vtable slot +12; returns `this`   lifted: skate3_recomp.68.cpp:7845 (57 lines)
// STATUS: gate-1 (its own bctrl is the allocator's free entry, and sub_828AAF88 lazily builds the heap: sub_82B3C810 indirect call, `bctrl` at 0x82B3C854)
// calls/session: boot 1926  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C810.md
namespace port_82B3C810 {

// ((-32206 & 0xFFFF) << 16) + (-17760) = 0x82320000 - 0x4560. Computed, never read off by eye:
// sub_82B23C78 shares this same `lis` and differs only in the `addi`.
constexpr uint32_t kVtable = 0x82320000u - 17760u;  // lis r11,-32206 ; addi r9,r11,-17760

constexpr uint32_t kVptr = 0;       // stw r9,0(r3) -- the only store outside this call's frame
constexpr uint32_t kFreeSlot = 12;  // the allocator vtable's free entry

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)
constexpr uint32_t kLinkSlot = 8;     // stw r12,-8(r1), i.e. frame + 88
constexpr uint32_t kSaveSlot = 16;    // std r31,-16(r1), i.e. frame + 80

REX_FUNC(Native) {
  // The frame is reproduced because both callees run on it; its slots are inside the 96 bytes
  // this call allocates, so neither is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  // stw r12,-8(r1)
  REX_STORE_U64(entry_sp - kSaveSlot, ctx.r31.u64);  // std r31,-16(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;
  const uint32_t destroy = ctx.r4.u32 & 0x1u;  // clrlwi r10,r4,31 ; cmplwi cr6,r10,0

  ctx.r31.u64 = ctx.r3.u64;              // mr r31,r3 -- before the store, as lifted
  REX_STORE_U32(self + kVptr, kVtable);  // stw r9,0(r3)

  // beq cr6,0x82b3c858 -- an even flags word destroys nothing
  if (destroy != 0) {
    // bl 0x828aaf88 -- takes no argument; r3 (this) and r4 (the flags) are still live and are
    // passed through untouched, exactly as the original leaves them. Returns the allocator.
    skate3::audio::GuestCall(ctx, base, sub_828AAF88);
    const uint32_t allocator = ctx.r3.u32;
    const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);  // lwz r11,0(r3)
    ctx.r5.s64 = 0;                                           // li r5,0
    ctx.r4.u64 = ctx.r31.u64;                                 // mr r4,r31
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);           // lwz r10,12(r11) ; mtctr r10
    ctx.lr = 0x82B3C858;                                      // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  }

  // loc_82B3C858
  ctx.r3.u64 = ctx.r31.u64;                            // mr r3,r31 -- returns `this`
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;               // addi r1,r1,96
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveSlot);  // ld r31,-16(r1)
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3C810
SKATE3_PORT(82B3C810, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
