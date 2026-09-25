// sub_82B3C870  role: fill the next 24-byte slot of the object's request ring from r4..r10, notify the owner through the object's own table slot +0, then advance the write index at +47 with a wrap at +50 -- returns the slot index used, or 0 when the slot was still busy   lifted: skate3_recomp.68.cpp:7903 (109 lines)
// STATUS: gate-1 (its own bctrl is a virtual notify, so the callee is whatever the table holds: sub_82B3C870 indirect call, `bctrl` at 0x82B3C8E4)
// calls/session: boot 4788  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C870.md
namespace port_82B3C870 {

// Nothing in docs/rw_audio_structs.h names this object; plain offsets are honest. The ring is
// EMBEDDED: the slot array starts at `self + u32[self+36]`, not at a pointer stored there --
// both terms are added to `self` (`add r11,r11,r3` then `add r30,r11,r31`).
constexpr uint32_t kVptr = 0;           // self +0  -> the object's own dispatch table
constexpr uint32_t kNotifySlot = 0;     // that table +0: the entry this call ends on
constexpr uint32_t kLatch = 28;         // u32, written only while the write index still reads +49
constexpr uint32_t kSlotArrayOff = 36;  // u32, byte offset of the slot array, relative to `self`
constexpr uint32_t kWriteIndex = 47;    // u8, the slot this call fills; also the return value
constexpr uint32_t kLatchIndex = 49;    // u8, compared against the RELOADED write index
constexpr uint32_t kSlotCount = 50;     // u8, the ring length: the index wraps at it

constexpr uint32_t kSlotStride = 24;  // rotlwi 1 ; add ; rlwinm 3,0,28 -- index * 24

// The slot's fields, named by the register each one takes.
constexpr uint32_t kSlotWord0 = 0;   // r4
constexpr uint32_t kSlotWord4 = 4;   // r8
constexpr uint32_t kSlotWord8 = 8;   // r7, and the value latched into self+28
constexpr uint32_t kSlotBusy = 12;   // r5; nonzero ON ENTRY means the slot is not yet consumed
constexpr uint32_t kSlotWord16 = 16; // r10
constexpr uint32_t kSlotByte20 = 20; // r6
constexpr uint32_t kSlotByte21 = 21; // r9

constexpr uint32_t kFrameBytes = 112;              // stwu r1,-112(r1)
constexpr uint32_t kIndirectReturn = 0x82B3C8E8u;  // the bctrl's return address

REX_FUNC(Native) {
  // The frame is reproduced because the indirect callee runs on it. __savegprlr_29's spills of
  // r29..r31 land inside these 112 bytes, so they are part of this call's own frame and are not
  // windowed; they are not reproduced either, because this body keeps those three values in
  // locals and never writes the guest registers, so there is nothing to restore.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f5211c
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-112(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t index = REX_LOAD_U8(ctx.r3.u32 + kWriteIndex);           // lbz r29,47(r3)
  const uint32_t self = ctx.r3.u32;                                      // mr r31,r3
  const uint64_t array_off = REX_LOAD_U32(ctx.r3.u32 + kSlotArrayOff);   // lwz r3,36(r3)

  // rotlwi r11,r29,1 ; add r11,r29,r11 ; rlwinm r11,r11,3,0,28 -- index*3 then *8. The adds are
  // 64-bit on zero-extended operands, as RexGlue lowers them; `index` came from a u8 load, so
  // the triple is at most 765 and the rotate cannot carry into the high word, which is what the
  // 0xFFFFFFF8 mask would drop. The mask's low three bits are already zero.
  const uint64_t triple = index + uint64_t(__builtin_rotateleft32(uint32_t(index), 1));
  const uint64_t slot_off = (uint64_t(uint32_t(triple) << 3)) & 0xFFFFFFF8u;
  static_assert(kSlotStride == 24, "index*3*8");
  // add r11,r11,r3 ; add r30,r11,r31 -- 64-bit, truncated where it is used as an address.
  const uint32_t slot = uint32_t(slot_off + array_off + uint64_t(self));

  // lwz r3,12(r30) ; cmpwi cr6,r3,0 ; beq cr6,0x82b3c8b4 -- a nonzero +12 means the consumer
  // has not taken the previous request out of this slot.
  if (REX_LOAD_U32(slot + kSlotBusy) != 0) {
    // Nothing is written on this path, and the caller gets 0 -- the same value a successful
    // fill of slot 0 returns. Reproduced as it stands; it is the original's ambiguity.
    ctx.r3.s64 = 0;                         // li r3,0
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112
    ctx.lr = entry_lr;                      // b 0x82f5216c
    return;
  }

  // loc_82B3C8B4 -- the fill, in the lifted store order: +0, +8, +4, +12, +16, +21, +20. The
  // two `mr`s that set up the call's arguments are interleaved with these stores in the
  // original; they are done below instead, after the last store that reads r4.
  REX_STORE_U32(slot + kSlotWord0, ctx.r4.u32);    // stw r4,0(r30)
  REX_STORE_U32(slot + kSlotWord8, ctx.r7.u32);    // stw r7,8(r30)
  REX_STORE_U32(slot + kSlotWord4, ctx.r8.u32);    // stw r8,4(r30)
  REX_STORE_U32(slot + kSlotBusy, ctx.r5.u32);     // stw r5,12(r30)
  REX_STORE_U32(slot + kSlotWord16, ctx.r10.u32);  // stw r10,16(r30)
  REX_STORE_U8(slot + kSlotByte21, ctx.r9.u8);     // stb r9,21(r30)
  REX_STORE_U8(slot + kSlotByte20, ctx.r6.u8);     // stb r6,20(r30)

  // lwz r8,0(r31) ; lwz r7,0(r8) ; mtctr r7 ; bctrl -- notify(self, index) through the object's
  // own table. This is the gate-1 call: the target is data, and whatever it does to the ring
  // cannot be undone by rewinding windows.
  const uint32_t vtable = REX_LOAD_U32(self + kVptr);
  ctx.ctr.u64 = REX_LOAD_U32(vtable + kNotifySlot);
  ctx.r3.u64 = self;   // mr r3,r31
  ctx.r4.u64 = index;  // mr r4,r29
  ctx.lr = kIndirectReturn;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  // lbz r6,49(r31) ; lbz r5,47(r31) ; cmplw cr6,r5,r6 ; bne cr6,0x82b3c900
  // +47 is RELOADED here: the notify above can have advanced it, so this is not `index`.
  const uint32_t latch_index = REX_LOAD_U8(self + kLatchIndex);
  const uint32_t write_index = REX_LOAD_U8(self + kWriteIndex);
  if (write_index == latch_index) {
    // lwz r11,8(r30) ; stw r11,28(r31) -- reloaded from the slot, not reused from r7.
    REX_STORE_U32(self + kLatch, REX_LOAD_U32(slot + kSlotWord8));
  }

  // loc_82B3C900 -- advance the write index, wrapping at +50. +47 is reloaded a third time.
  const uint32_t next = (REX_LOAD_U8(self + kWriteIndex) + 1u) & 0xFFu;  // lbz ; addi ; clrlwi
  REX_STORE_U8(self + kWriteIndex, uint8_t(next));                       // stb r10,47(r31)
  // lbz r9,50(r31) ; cmplw cr6,r10,r9 ; blt cr6,0x82b3c924 -- unsigned, so `>=` wraps.
  if (next >= REX_LOAD_U8(self + kSlotCount)) {
    REX_STORE_U8(self + kWriteIndex, 0);  // stb r11,47(r31) -- a second store to the same byte
  }

  // loc_82B3C924
  ctx.r3.u64 = index;                     // mr r3,r29 -- the slot this call filled
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112
  ctx.lr = entry_lr;                      // b 0x82f5216c
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3C870
SKATE3_PORT(82B3C870, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
