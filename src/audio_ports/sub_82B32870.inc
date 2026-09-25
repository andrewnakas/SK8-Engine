// sub_82B32870  role: stop a voice record -- reset its state through sub_82B32328, then when the state byte at +476 reads 1 clear +24 and +43 and hand the node at +72 back to the owner's pool (sub_82B489D0), and finally release the held object at +440 through the owner's allocator   lifted: skate3_recomp.67.cpp:50534 (71 lines)
// STATUS: gate-1 (its own bctrl is the allocator's free entry and sub_82B32328 reaches another: sub_82B32870 indirect call, `bctrl` at 0x82B328D0)
// calls/session: boot 1907  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B32870.md
namespace port_82B32870 {

// Nothing in docs/rw_audio_structs.h names this ~478-byte record; plain offsets are honest.
// sub_82B32328 writes +432..+477 of the same object, which is how the size is known.
constexpr uint32_t kOwner = 8;       // ->owner; owner+36 is the allocator object
constexpr uint32_t kCursor = 24;     // u32 cleared on the state==1 path
constexpr uint32_t kFlag = 43;       // u8  cleared on the state==1 path
constexpr uint32_t kPoolNode = 72;   // passed BY ADDRESS to sub_82B489D0
constexpr uint32_t kHeld = 440;      // the object released through the allocator
constexpr uint32_t kState = 476;     // u8  exactly 1 selects the release path

constexpr uint32_t kAllocator = 36;  // owner+36 -> allocator object
constexpr uint32_t kVptr = 0;        // allocator+0 -> its vtable
constexpr uint32_t kFreeSlot = 12;   // the allocator vtable's free entry

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)
constexpr uint32_t kLinkSlot = 8;     // stw r12,-8(r1), i.e. frame + 88
constexpr uint32_t kSaveSlot = 16;    // std r31,-16(r1), i.e. frame + 80

REX_FUNC(Native) {
  // The frame is reproduced because all three callees run on it; both slots are inside the 96
  // bytes this call allocates, so neither is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  // stw r12,-8(r1)
  REX_STORE_U64(entry_sp - kSaveSlot, ctx.r31.u64);  // std r31,-16(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;
  ctx.r31.u64 = ctx.r3.u64;  // mr r31,r3

  // bl 0x82b32328 -- resets +432..+477 of this same record; it is what leaves +476 in the state
  // the test below reads, so the load after it is a genuine reload, not a hoistable one.
  skate3::audio::GuestCall(ctx, base, sub_82B32328, self);

  // lbz r11,476(r31) ; cmplwi cr6,r11,1 ; bne cr6,0x82b328ac -- exactly 1, not nonzero
  if (REX_LOAD_U8(self + kState) == 1) {
    // li r11,0 ; addi r4,r31,72 -- the node address is formed before the two stores; it is a
    // constant offset off r31, so forming it at the call below is the same value.
    REX_STORE_U32(self + kCursor, 0);  // stw r11,24(r31)
    REX_STORE_U8(self + kFlag, 0);     // stb r11,43(r31)
    const uint32_t owner = REX_LOAD_U32(self + kOwner);  // lwz r3,8(r31)
    // bl 0x82b489d0 -- returns the node at +72 to the owner's pool.
    skate3::audio::GuestCall(ctx, base, sub_82B489D0, owner, self + kPoolNode);
  }

  // loc_82B328AC
  const uint32_t held = REX_LOAD_U32(self + kHeld);  // lwz r4,440(r31)
  ctx.r4.u64 = held;                                 // loaded straight into the callee's r4
  // cmplwi cr6,r4,0 ; beq cr6,0x82b328d4 -- unsigned
  if (held != 0) {
    const uint32_t owner = REX_LOAD_U32(self + kOwner);           // lwz r11,8(r31)
    ctx.r5.s64 = 0;                                               // li r5,0
    const uint32_t allocator = REX_LOAD_U32(owner + kAllocator);  // lwz r3,36(r11)
    ctx.r3.u64 = allocator;
    const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);  // lwz r10,0(r3)
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);           // lwz r9,12(r10) ; mtctr r9
    ctx.lr = 0x82B328D4;                                      // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  }

  // loc_82B328D4 -- no `li r3,...` on any path: r3 is whatever the last call left.
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

}  // namespace port_82B32870
SKATE3_PORT(82B32870, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
