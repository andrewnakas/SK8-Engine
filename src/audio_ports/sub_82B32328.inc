// sub_82B32328  role: flush the record -- raise the +477 flush flag, then walk the 48-byte request ring at record + u16[+464] from the read cursor at +468, handing every busy entry to sub_82B33F98, and finish by resetting the whole +432..+477 block (cursors to 0, +472 to 16, the flag down)   lifted: skate3_recomp.67.cpp:49756 (125 lines)
// STATUS: gate-1 (sub_82B33F98 frees through the core allocator's vtable one level down: its bctrl at 0x82B340A4, `u32[u32[u32[owner+36] + 0] + 12]`; reached from `bl 0x82b33f98` at 0x82B32374)
// calls/session: boot 2494  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B32328.md
namespace port_82B32328 {

// The same ~478-byte record port_82B32870 and port_82B32480 work on. This function is what
// defines the +432..+477 block: it writes ten of those fields on the way out, which is how the
// record's size is known. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kWord432 = 432;     // u32, zeroed
constexpr uint32_t kWord436 = 436;     // u32, zeroed
constexpr uint32_t kRingOffset = 464;  // u16 (lhz, not lwz): byte offset of the request ring
constexpr uint32_t kByte467 = 467;     // u8, zeroed
constexpr uint32_t kReadIndex = 468;   // u8, this ring's read cursor; zeroed at the end
constexpr uint32_t kByte469 = 469;     // u8, zeroed
constexpr uint32_t kSlotCount = 470;   // u8, the ring length: the cursor wraps at it
constexpr uint32_t kStateByte = 472;   // u8, set to 16 -- the only nonzero value written
constexpr uint32_t kByte473 = 473;     // u8, zeroed
constexpr uint32_t kByte474 = 474;     // u8, zeroed
constexpr uint32_t kOtherRingIndex = 475;  // u8, the read cursor of the OTHER ring (the 20-slot
                                           // 16-byte one at +100 that port_82B32480 drains)
constexpr uint32_t kFlushing = 477;    // u8, 1 for the duration of this call; port_82B32480 reads
                                       // it to skip its release calls while this runs

constexpr uint32_t kEntryStride = 48;  // rotlwi 1 ; add ; rlwinm 4,0,27 -- index * 48
constexpr uint32_t kEntryBusy = 46;    // u8, nonzero selects the entry (not a specific value)

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)
constexpr uint32_t kLinkSlot = 8;     // stw r12,-8(r1), i.e. frame + 88
constexpr uint32_t kSaveSlot = 16;    // std r31,-16(r1), i.e. frame + 80

// rotlwi r10,r11,1 ; add r7,r11,r10 ; rlwinm r11,r7,4,0,27 -- index*3 then *16. The adds are
// 64-bit on zero-extended operands, as RexGlue lowers them; every `index` here comes from a u8
// load, so the triple is at most 765, the rotate cannot carry into the high word that the
// 0xFFFFFFF0 mask drops, and the mask's low four bits are already zero.
uint64_t SlotOffset(uint32_t index) {
  const uint64_t triple = uint64_t(index) + uint64_t(__builtin_rotateleft32(index, 1));
  static_assert(kEntryStride == 48, "index*3*16");
  return (uint64_t(uint32_t(triple) << 4)) & 0xFFFFFFF0u;
}

REX_FUNC(Native) {
  // The frame is reproduced because sub_82B33F98 runs on it; both slots are inside the 96 bytes
  // this call allocates, so neither is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  // stw r12,-8(r1)
  REX_STORE_U64(entry_sp - kSaveSlot, ctx.r31.u64);  // std r31,-16(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;                             // mr r31,r3
  uint32_t index = REX_LOAD_U8(self + kReadIndex);              // lbz r11,468(r3)
  uint32_t ring_off = REX_LOAD_U16(self + kRingOffset);         // lhz r9,464(r3)
  REX_STORE_U8(self + kFlushing, 1);                            // stb r8,477(r3), r8 = 1
  // add r11,r11,r9 ; add r6,r11,r3 -- the ring is EMBEDDED: the u16 at +464 is a byte offset
  // inside the record, not a pointer (both terms are added to the record).
  uint32_t entry = uint32_t(SlotOffset(index) + uint64_t(ring_off) + uint64_t(self));

  // lbz r5,46(r6) ; cmplwi cr6,r5,0 ; beq cr6,0x82b323c4 -- the lifted form is a do-while with
  // the same test at the bottom, against the entry the new cursor selects.
  while (REX_LOAD_U8(entry + kEntryBusy) != 0) {
    // loc_82B3236C -- mr r3,r31 ; lbz r4,468(r31) ; bl 0x82b33f98. The index argument is
    // RELOADED from memory rather than carried in a register. This is the gate-1 call.
    skate3::audio::GuestCall(ctx, base, sub_82B33F98, self, REX_LOAD_U8(self + kReadIndex));

    // lbz r11,468(r31) ; lbz r9,470(r31) ; addi r11,r11,1 ; clrlwi r10,r11,24 -- both bytes are
    // reloaded after the call, which is free to have changed either.
    const uint32_t next = (REX_LOAD_U8(self + kReadIndex) + 1u) & 0xFFu;
    const uint32_t slots = REX_LOAD_U8(self + kSlotCount);
    // subf r8,r10,r9 ; subfic r7,r8,0 ; subfe r5,r6,r6 ; and r4,r5,r10 -- subfic sets CA only
    // when `slots - next` is exactly zero, and `~r6 + r6` is all ones whatever r6 holds, so
    // subfe leaves 0 on the wrap and all ones otherwise. The wrap is EQUALITY: a cursor already
    // past the count would never come back.
    index = (next == slots) ? 0u : next;
    REX_STORE_U8(self + kReadIndex, uint8_t(index));       // stb r4,468(r31)
    ring_off = REX_LOAD_U16(self + kRingOffset);            // lhz r10,464(r31) -- after the store
    entry = uint32_t(SlotOffset(index) + uint64_t(ring_off) + uint64_t(self));
  }

  // loc_82B323C4 -- the reset, in the lifted store order. +472 is the only nonzero value.
  REX_STORE_U8(self + kFlushing, 0);         // stb r11,477(r31)
  REX_STORE_U8(self + kByte469, 0);          // stb r11,469(r31)
  REX_STORE_U8(self + kByte467, 0);          // stb r11,467(r31)
  REX_STORE_U8(self + kReadIndex, 0);        // stb r11,468(r31)
  REX_STORE_U32(self + kWord432, 0);         // stw r11,432(r31)
  REX_STORE_U32(self + kWord436, 0);         // stw r11,436(r31)
  REX_STORE_U8(self + kByte473, 0);          // stb r11,473(r31)
  REX_STORE_U8(self + kByte474, 0);          // stb r11,474(r31)
  REX_STORE_U8(self + kOtherRingIndex, 0);   // stb r11,475(r31)
  REX_STORE_U8(self + kStateByte, 16);       // stb r10,472(r31), r10 = 16

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

}  // namespace port_82B32328
SKATE3_PORT(82B32328, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
