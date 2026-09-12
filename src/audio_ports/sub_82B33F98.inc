// sub_82B33F98  role: cancel one slot -- release the decoder held at the 48-byte record's +8 through sub_82B3C930, mark every consecutive ring entry belonging to this slot state 2 (which is what sub_82B32480 retires) while advancing the cursor at +474, run the retire pass, drop the slot's reference at +36 and free its buffer at +28 through the owner's allocator, clear the record's state byte at +46, and finally -- when the slot's +75 reads 1 and the holder at self+12 is not already queued -- latch the holder's float from +40 to +48 and push its +28 node onto the list at u32[holder+16]+20   lifted: skate3_recomp.67.cpp:53874 (210 lines)
// STATUS: gate-1 (its own bctrl is the allocator's free entry -- `bctrl` at 0x82B340A4 through u32[u32[u32[self+8]+36]+12] -- and all three direct callees are gate-1 too: sub_82B3C930 (four indirect calls), sub_82B32480 and sub_82B47D58 (RtlEnterCriticalSection))
// calls/session: boot 1925  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33F98.md
namespace port_82B33F98 {

// The same ~478-byte record and the same two parallel arrays as sub_82B32480, sub_82B32550,
// sub_82B332D0 and sub_82B33870. Nothing in docs/rw_audio_structs.h names any of it, so the
// offsets are plain and the names are borrowed from the ports that established them.
constexpr uint32_t kOwner = 8;            // u32 -> the owner; owner+36 is the allocator object
constexpr uint32_t kHolder = 12;          // u32 -> the record the tail block pushes onto a list
constexpr uint32_t kSlotArray = 96;       // u32 -> the 80-byte slot array (sub_82B32480's
                                          //        kBudgetArray, indexed by a ring entry's +14)
constexpr uint32_t kParallelBias = 464;   // u16 byte bias of the 48-byte array, from self
constexpr uint32_t kCancelIndex = 474;    // u8 cursor: the claim cursor is +473 (sub_82B32550)
                                          //            and the retire cursor +475 (sub_82B32480)

// The 20-slot, 16-byte ring embedded at self+100 -- sub_82B32480's names exactly.
constexpr uint32_t kEntries = 100;
constexpr uint32_t kEntryStride = 16;
constexpr uint32_t kEntryState = 13;  // u8: 0 free, 2 finished; this function writes 2
constexpr uint32_t kEntryClass = 14;  // u8 the slot index that owns the entry
constexpr uint32_t kRingSlots = 20;   // addi r10,r11,-20 -- the wrap point

// The 48-byte record (r29).
constexpr uint32_t kRecordDecoder = 8;  // u32 released through sub_82B3C930, then zeroed
constexpr uint32_t kRecordState = 46;   // u8 cleared here; sub_82B332D0 tests it for exactly 1

// The 80-byte slot (r28).
constexpr uint32_t kSlotBuffer = 28;  // u32 freed through the owner's allocator
constexpr uint32_t kSlotRef = 36;     // u32 handed to sub_82B47D58 (a refcount drop)
constexpr uint32_t kSlotArmed = 75;   // u8; exactly 1 enables the tail block

// The allocator pair, the same one sub_82B32870 uses.
constexpr uint32_t kAllocator = 36;  // owner+36 -> allocator object
constexpr uint32_t kVptr = 0;        // allocator+0 -> its vtable
constexpr uint32_t kFreeSlot = 12;   // the allocator vtable's free entry

// The holder at self+12, and the list it is pushed onto.
constexpr uint32_t kHolderList = 16;      // u32 -> the list owner
constexpr uint32_t kHolderNode = 28;      // the 8-byte link node: +0 chain, +4 back pointer
constexpr uint32_t kHolderFloatSrc = 40;  // f32 copied to +48
constexpr uint32_t kHolderFloatDst = 48;
constexpr uint32_t kHolderQueued = 71;    // u8; nonzero means already on the list
constexpr uint32_t kListHead = 20;        // u32 head, reloaded after the node stores

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)

// rlwinm r,N,0,31-N then add: both halves are zero-extended 32-bit quantities but the add is
// 64 bit, so a carry out of bit 31 survives into the register the loads address through. Kept in
// 64 bits and narrowed only where the lifted form uses `.u32`.
inline uint64_t ScaleByThree(uint64_t index) {
  const uint64_t times_two = uint64_t((uint32_t(index) << 1) & 0xFFFFFFFEu);  // rlwinm r11,r4,1,0,30
  return index + times_two;                                                   // add r11,r4,r11
}
inline uint64_t ScaleByFive(uint64_t index) {
  const uint64_t times_four = uint64_t((uint32_t(index) << 2) & 0xFFFFFFFCu);  // rlwinm r9,r4,2,0,29
  return index + times_four;                                                   // add r7,r4,r9
}
inline uint64_t TimesSixteen(uint64_t value) {
  return uint64_t((uint32_t(value) << 4) & 0xFFFFFFF0u);  // rlwinm r9,r11,4,0,27
}

// lbz ; rotlwi r11,r11,4 ; add r11,r11,r31 -- the cursor came from a byte load, so the rotate is
// index*16 and cannot carry; the lifted form addresses through `.u32`, so this stays 32 bit.
inline uint32_t RingEntry(uint32_t self, uint32_t cursor) {
  return self + (cursor * kEntryStride) + kEntries;
}

REX_FUNC(Native) {
  // __savegprlr_26 spills r26-r31 and the link register; the harness compares r13-r31, so the
  // entry values have to survive here too. The spill slots are this call's own, not windowed.
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  ctx.r12.u64 = ctx.lr;  // mflr r12 -- the value __restgprlr_26 puts back in lr on the way out

  // The frame is reproduced because all four callees run on it. Nothing in it is passed by
  // address, so none of it is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-144(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t self_reg = ctx.r3.u64;   // mr r31,r3
  const uint64_t index_reg = ctx.r4.u64;  // mr r30,r4
  const uint32_t self = uint32_t(self_reg);
  const uint32_t index = uint32_t(index_reg);

  const uint32_t bias = REX_LOAD_U16(self + kParallelBias);  // lhz r8,464(r3)
  const uint32_t array = REX_LOAD_U32(self + kSlotArray);    // lwz r10,96(r3)
  const uint32_t owner = REX_LOAD_U32(self + kOwner);        // lwz r27,8(r3)
  // add r9,r8,r9 ; add r29,r9,r3 -- the 48-byte record, biased from the object itself.
  const uint32_t record =
      uint32_t(uint64_t(bias) + TimesSixteen(ScaleByThree(index_reg)) + self_reg);
  // add r28,r11,r10 -- the 80-byte slot, off the pointer at +96.
  const uint32_t slot = uint32_t(TimesSixteen(ScaleByFive(index_reg)) + uint64_t(array));

  // lwz r3,8(r29) ; cmplwi cr6,r3,0 ; beq cr6,0x82b33ff4 -- UNSIGNED.
  const uint32_t decoder = REX_LOAD_U32(record + kRecordDecoder);
  if (decoder != 0) {
    // bl 0x82b3c930 -- EVENT_STOP's decoder teardown; four indirect calls of its own.
    skate3::audio::GuestCall(ctx, base, sub_82B3C930, decoder);
    REX_STORE_U32(record + kRecordDecoder, 0);  // stw r26,8(r29), r26 = 0
  }

  // loc_82B33FF4 -- the cancel scan. Every iteration RELOADS the cursor at +474, because the
  // store below writes it and because the entry addresses derive from it.
  // lbz r9,114(r10) ; cmplw cr6,r9,r30 ; bne cr6,0x82b3406c -- the first entry must belong to
  // this slot, compared UNSIGNED against the full 32-bit index.
  if (REX_LOAD_U8(RingEntry(self, REX_LOAD_U8(self + kCancelIndex)) + kEntryClass) == index) {
    for (;;) {  // loc_82B34010
      const uint32_t cursor = REX_LOAD_U8(self + kCancelIndex);  // lbz r11,474(r31)
      const uint32_t entry = RingEntry(self, cursor);
      const uint32_t state = REX_LOAD_U8(entry + kEntryState);  // lbz r10,113(r11)
      // cmplwi cr6,r10,2 ; beq -> out ; cmplwi cr6,r10,0 ; beq -> out. Already-finished and
      // free entries both stop the scan; only an in-flight entry is cancelled.
      if (state == 2 || state == 0) break;
      REX_STORE_U8(entry + kEntryState, 2);  // stb r9,113(r11), r9 = 2 from before the loop

      // lbz r11,474(r31) ; addi r11,r11,1 ; clrlwi r11,r11,24 -- the cursor is reloaded again.
      const uint32_t next = (REX_LOAD_U8(self + kCancelIndex) + 1u) & 0xFFu;
      // addi r10,r11,-20 ; subfic r10,r10,0 ; subfe r7,r8,r8 ; and r6,r7,r11 -- subfic leaves CA
      // set only when next-20 is exactly zero, and `~r8 + r8` is all ones whatever r8 holds, so
      // subfe yields 0 on the wrap and all ones otherwise. The wrap, written as the wrap it is.
      const uint32_t wrapped = (next == kRingSlots) ? 0u : next;
      REX_STORE_U8(self + kCancelIndex, uint8_t(wrapped));  // stb r6,474(r31)

      // clrlwi r5,r6,24 ; rotlwi r11,r5,4 ; add r4,r11,r31 ; lbz r3,114(r4) ; cmplw ; beq -> loop
      if (REX_LOAD_U8(RingEntry(self, wrapped) + kEntryClass) != index) break;
    }
  }

  // loc_82B3406C -- bl 0x82b32480(self): retire everything now marked 2. Takes the heap lock one
  // level down.
  skate3::audio::GuestCall(ctx, base, sub_82B32480, self_reg);

  const uint32_t reference = REX_LOAD_U32(slot + kSlotRef);  // lwz r3,36(r28)
  // cmplwi cr6,r3,0 ; beq cr6,0x82b34084
  if (reference != 0) {
    // bl 0x82b47d58 -- a refcount drop; takes the heap lock on the last reference.
    skate3::audio::GuestCall(ctx, base, sub_82B47D58, reference);
  }

  // loc_82B34084
  const uint32_t buffer = REX_LOAD_U32(slot + kSlotBuffer);  // lwz r4,28(r28)
  // cmplwi cr6,r4,0 ; beq cr6,0x82b340a8
  if (buffer != 0) {
    const uint32_t allocator = REX_LOAD_U32(owner + kAllocator);  // lwz r3,36(r27)
    ctx.r4.u64 = buffer;                                          // r4 is still the load above
    ctx.r3.u64 = allocator;
    ctx.r5.s64 = 0;                                           // li r5,0
    const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);  // lwz r11,0(r3)
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);           // lwz r10,12(r11) ; mtctr r10
    ctx.lr = 0x82B340A8;                                      // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  }

  // loc_82B340A8
  REX_STORE_U8(record + kRecordState, 0);  // stb r26,46(r29)

  // lbz r11,75(r28) ; cmplwi cr6,r11,1 ; bne cr6,0x82b34100 -- exactly 1, not nonzero.
  if (REX_LOAD_U8(slot + kSlotArmed) != 1) {
    // loc_82B34100 -- nothing else to do.
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
    ctx.lr = ctx.r12.u64;                   // __restgprlr_26 restores lr and r26-r31
    ctx.r26 = saved_r26;
    ctx.r27 = saved_r27;
    ctx.r28 = saved_r28;
    ctx.r29 = saved_r29;
    ctx.r30 = saved_r30;
    ctx.r31 = saved_r31;
    return;
  }

  const uint32_t holder = REX_LOAD_U32(self + kHolder);  // lwz r11,12(r31)
  // lbz r10,71(r11) ; cmplwi cr6,r10,0 ; bne cr6,0x82b34100 -- already queued, nothing to do.
  if (REX_LOAD_U8(holder + kHolderQueued) == 0) {
    const uint32_t list = REX_LOAD_U32(holder + kHolderList);  // lwz r9,16(r11)
    const uint32_t node = holder + kHolderNode;                // addi r10,r11,28

    // lfs f0,40(r11) then stfs f0,48(r11) -- a single-precision round trip through the double
    // register, kept as the lifted form has it so a NaN payload behaves identically.
    ctx.fpscr.disableFlushModeUnconditional();
    PPCRegister value{};
    value.u32 = REX_LOAD_U32(holder + kHolderFloatSrc);  // lfs f0,40(r11)
    ctx.f0.f64 = double(value.f32);
    value.f32 = float(ctx.f0.f64);
    REX_STORE_U32(holder + kHolderFloatDst, value.u32);  // stfs f0,48(r11)
    REX_STORE_U8(holder + kHolderQueued, 1);             // stb r8,71(r11), r8 = 1

    const uint32_t head = REX_LOAD_U32(list + kListHead);  // lwz r7,20(r9)
    REX_STORE_U32(node + 0, head);                         // stw r7,28(r11)
    REX_STORE_U32(node + 4, 0);                            // stw r26,32(r11)
    // lwz r11,20(r9) -- the head is RELOADED after those two stores rather than reused from r7.
    const uint32_t head_again = REX_LOAD_U32(list + kListHead);
    // cmplwi cr6,r11,0 ; beq cr6,0x82b340fc
    if (head_again != 0) {
      REX_STORE_U32(head_again + 4, node);  // stw r10,4(r11)
    }
    // loc_82B340FC
    REX_STORE_U32(list + kListHead, node);  // stw r10,20(r9)
  }

  // loc_82B34100
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
  ctx.lr = ctx.r12.u64;                   // __restgprlr_26 restores lr and r26-r31
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note -- and it
// is enumerable; gate 1 is the only blocker, and it blocks on every path.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B33F98
SKATE3_PORT(82B33F98, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
