// sub_82B1BF98  role: tear down one element of a described object -- walk the layout descriptor at +0 and, in order, unlink the element's first embedded node from the container's +12 list (sub_828E2E08), release u16[+32] handles of 28-byte stride (sub_828E30B8), unlink the second node 20 bytes on from the +8 list (sub_828E2EA0), release u16[+34] variable-stride handles (sub_828E2D78), then for u8[+36] entries of the offset table at descriptor+60 call vtable slot 0 on the pointer at each sub-object's +8 and zero it, and for the u8[+39] entries that follow call sub_828E2C78 on theirs; finally decrement the descriptor's instance count at +28 and free the element through the allocator held at [0x8307762C]+36   lifted: skate3_recomp.66.cpp:63558 (247 lines)
// STATUS: gate-1 (its own two bctrl sites plus two callees that reach a third: sub_82B1BF98 indirect call, the release slot 0 at `bctrl` 0x82B1C0B8 and the allocator's +12 free of the element itself at `bctrl` 0x82B1C140; and sub_828E2E08 / sub_828E2EA0 free the container at depth 1. Census indirect 2, depth 0)
// calls/session: boot 154  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1BF98.md
namespace port_82B1BF98 {

// Nothing in docs/rw_audio_structs.h describes either object. The self record is three words.
constexpr uint32_t kDescriptor = 0;  // lwz r10,0(r3)  -- the layout, reloaded before nearly every use
constexpr uint32_t kElement = 4;     // lwz r11,4(r3)  -- the instance being torn down
constexpr uint32_t kContainer = 8;   // lwz r3,8(r3)   -- the list container the nodes belong to

// The descriptor. All six counts and flags are read live, every one of them reloaded per iteration.
constexpr uint32_t kInstanceCount = 28;  // u16, decremented once at the end
constexpr uint32_t kHandleCount = 32;    // u16, the 28-byte-stride run
constexpr uint32_t kSecondCount = 34;    // u16, the variable-stride run
constexpr uint32_t kSlotCount = 36;      // u8,  entries of the offset table released by vtable 0
constexpr uint32_t kFirstNodeFlag = 37;  // u8,  non-zero: the element is in the +12 list
constexpr uint32_t kSecondNodeFlag = 38; // u8,  non-zero: the element is in the +8 list
constexpr uint32_t kTailCount = 39;      // u8,  entries of the offset table released by 828E2C78
constexpr uint32_t kOffsetTable = 60;    // addi r29,r11,60 -- u32 offsets, the two runs contiguous

constexpr uint32_t kFirstNode = 24;      // addi r30,r11,24 -- the element's first embedded node
constexpr uint32_t kSecondNodeStep = 20; // addi r30,r30,20 -- the second node, 20 bytes on
constexpr uint32_t kHandleStride = 28;   // addi r30,r30,28 -- the first run's fixed stride
constexpr uint32_t kHandleNode = 8;      // addi r4,r30,8 -- the node inside each handle record
constexpr uint32_t kHeld = 8;            // lwz r3,8(r30) -- the pointer each slot owns

// The variable strides: a byte plus a constant, times four.
constexpr uint32_t kSecondNodeSizeByte = 16;  // lbz r11,16(r30) ; addi r11,r11,5
constexpr uint32_t kSecondNodeSizeBias = 5;
constexpr uint32_t kSecondSizeByte = 24;      // lbz r10,24(r7)  ; addi r10,r10,7
constexpr uint32_t kSecondSizeBias = 7;

constexpr uint32_t kVptr = 0;         // lwz r11,0(r3)
constexpr uint32_t kSlotRelease = 0;  // lwz r10,0(r11) -- bctrl 0x82B1C0B8
constexpr uint32_t kSlotFree = 12;    // lwz r11,12(r6) -- bctrl 0x82B1C140
constexpr uint32_t kAllocatorField = 36;  // lwz r3,36(r10)

// lis r9,-31993 ; lwz r10,30252(r9) -- ((-31993 & 0xFFFF) << 16) + 30252. The same 0x83070000
// page as the evaluator globals sub_82B1E290 publishes.
constexpr uint32_t kSystemGlobal = 0x8307762C;

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
// __savegprlr_27 / __restgprlr_27 slots, negative offsets from the ENTRY sp, all inside the 128
// bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR27 = 48;
constexpr uint32_t kSaveR28 = 40;
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

// rlwinm rX,rX,2,0,29: the rotate is 64-bit over the doubled low word, but the 0xFFFFFFFC mask
// clears the two bits that wrap in, so the value is the low word times four, modulo 2^32.
uint64_t TimesFour(uint64_t value) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(value)) << 2) & 0xFFFFFFFCull;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3

  // The frame is reproduced because all five named callees and both indirect targets run on it.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U64(entry_sp - kSaveR27, ctx.r27.u64);   // bl 0x82f52114 -> std r27,-48(r1)
  REX_STORE_U64(entry_sp - kSaveR28, ctx.r28.u64);   //                  std r28,-40(r1)
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);   //                  std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);   //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);   //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  //                  stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  // `descriptor` tracks r11/r10 across the whole body. It is RELOADED at every point the lifted
  // form reloads it, and the value live at the tail is the last of those reloads, never a fresh
  // one -- which matters because the callees below could write self+0.
  uint32_t descriptor = REX_LOAD_U32(self + kDescriptor);            // lwz r10,0(r3)
  uint64_t cursor = static_cast<uint64_t>(REX_LOAD_U32(self + kElement)) + kFirstNode;  // r30

  // lbz r9,37(r10) ; cmplwi cr6,r9,0 ; beq cr6,0x82b1bfd0
  if (REX_LOAD_U8(descriptor + kFirstNodeFlag) != 0) {
    // bl 0x828e2e08 -- unlink the first node from the container's +12 list. Result ignored.
    skate3::audio::GuestCall(ctx, base, sub_828E2E08,
                             static_cast<uint64_t>(REX_LOAD_U32(self + kContainer)), cursor);
    cursor = cursor + kSecondNodeStep;  // addi r30,r30,20
  }

  // loc_82B1BFD0
  descriptor = REX_LOAD_U32(self + kDescriptor);  // lwz r11,0(r31)
  uint64_t tail_index = 0;                       // li r28,0 -- also the zero stored in loop three
  uint64_t handle_index = 0;                     // mr r7,r28
  // lhz r10,32(r11) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1c00c -- UNSIGNED against zero.
  if (REX_LOAD_U16(descriptor + kHandleCount) != 0) {
    for (;;) {
      // loc_82B1BFE8: bl 0x828e30b8 -- validate the handle at r30 and unlink the node at r30+8.
      skate3::audio::GuestCall(ctx, base, sub_828E30B8, cursor, cursor + kHandleNode);
      descriptor = REX_LOAD_U32(self + kDescriptor);  // lwz r11,0(r31)
      handle_index = handle_index + 1;                // addi r7,r7,1
      cursor = cursor + kHandleStride;                // addi r30,r30,28
      const uint32_t count = REX_LOAD_U16(descriptor + kHandleCount);  // lhz r10,32(r11)
      // cmpw cr6,r7,r10 ; blt cr6,0x82b1bfe8 -- SIGNED on the low words, count RELOADED.
      if (static_cast<int32_t>(static_cast<uint32_t>(handle_index)) >=
          static_cast<int32_t>(count))
        break;
    }
  }

  // loc_82B1C00C: mr r7,r30 -- the second run starts where the first stopped.
  uint64_t second = cursor;
  // lbz r11,38(r11) -- on the descriptor as last loaded, not a fresh read.
  if (REX_LOAD_U8(descriptor + kSecondNodeFlag) != 0) {
    // bl 0x828e2ea0 -- unlink the second node from the container's +8 list. Result ignored.
    skate3::audio::GuestCall(ctx, base, sub_828E2EA0,
                             static_cast<uint64_t>(REX_LOAD_U32(self + kContainer)), cursor);
    // lbz r11,16(r30) ; addi r11,r11,5 ; rlwinm r11,r11,2,0,29 ; add r7,r11,r30
    const uint64_t size =
        static_cast<uint64_t>(REX_LOAD_U8(static_cast<uint32_t>(cursor) + kSecondNodeSizeByte)) +
        kSecondNodeSizeBias;
    second = TimesFour(size) + cursor;
  }

  // loc_82B1C038
  descriptor = REX_LOAD_U32(self + kDescriptor);  // lwz r11,0(r31)
  uint64_t second_index = 0;                     // mr r6,r28
  // lhz r10,34(r11) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1c07c
  if (REX_LOAD_U16(descriptor + kSecondCount) != 0) {
    for (;;) {
      // loc_82B1C04C: bl 0x828e2d78 -- the +8-generation variant of the handle unlink.
      skate3::audio::GuestCall(ctx, base, sub_828E2D78, second, second + kHandleNode);
      // The stride is read from the record AFTER the call, so a callee that rewrote the byte
      // changes where the next record starts. Kept as lifted.
      const uint64_t size =
          static_cast<uint64_t>(REX_LOAD_U8(static_cast<uint32_t>(second) + kSecondSizeByte)) +
          kSecondSizeBias;                           // lbz r10,24(r7) ; addi r10,r10,7
      descriptor = REX_LOAD_U32(self + kDescriptor);  // lwz r11,0(r31)
      second_index = second_index + 1;                // addi r6,r6,1
      const uint32_t count = REX_LOAD_U16(descriptor + kSecondCount);  // lhz r9,34(r11)
      second = TimesFour(size) + second;                               // add r7,r10,r7
      // cmpw cr6,r6,r9 ; blt cr6,0x82b1c04c -- SIGNED on the low words.
      if (static_cast<int32_t>(static_cast<uint32_t>(second_index)) >=
          static_cast<int32_t>(count))
        break;
    }
  }

  // loc_82B1C07C: addi r29,r11,60 -- the offset table, on the descriptor as last loaded.
  uint64_t offset_entry = static_cast<uint64_t>(descriptor) + kOffsetTable;  // r29
  uint64_t slot_index = 0;                                                  // mr r27,r28
  // lbz r10,36(r11) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1c0d4
  if (REX_LOAD_U8(descriptor + kSlotCount) != 0) {
    for (;;) {
      // loc_82B1C090
      const uint32_t offset = REX_LOAD_U32(static_cast<uint32_t>(offset_entry));  // lwz r11,0(r29)
      offset_entry = offset_entry + 4;                                            // addi r29,r29,4
      const uint32_t element = REX_LOAD_U32(self + kElement);  // lwz r10,4(r31) -- RELOADED here
      const uint64_t slot = static_cast<uint64_t>(offset) + element;  // add r30,r11,r10 -- 64-bit
      const uint32_t held = REX_LOAD_U32(static_cast<uint32_t>(slot) + kHeld);  // lwz r3,8(r30)
      // cmplwi cr6,r3,0 ; beq cr6,0x82b1c0c0
      if (held != 0) {
        // lwz r11,0(r3) ; lwz r10,0(r11) ; mtctr r10 ; bctrl -- THE release, vtable slot 0.
        ctx.r3.u64 = held;
        const uint32_t vtable = REX_LOAD_U32(held + kVptr);
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotRelease);
        ctx.lr = 0x82B1C0BC;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        // stw r28,8(r30) -- r28 is still the zero from loc_82B1BFD0; it only becomes a counter in
        // the loop below.
        REX_STORE_U32(static_cast<uint32_t>(slot) + kHeld, static_cast<uint32_t>(tail_index));
      }
      // loc_82B1C0C0
      descriptor = REX_LOAD_U32(self + kDescriptor);              // lwz r11,0(r31)
      slot_index = slot_index + 1;                                // addi r27,r27,1
      const uint32_t count = REX_LOAD_U8(descriptor + kSlotCount);  // lbz r10,36(r11)
      // cmpw cr6,r27,r10 ; blt cr6,0x82b1c090 -- SIGNED on the low words.
      if (static_cast<int32_t>(static_cast<uint32_t>(slot_index)) >= static_cast<int32_t>(count))
        break;
    }
  }

  // loc_82B1C0D4: lbz r10,39(r11) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1c114
  if (REX_LOAD_U8(descriptor + kTailCount) != 0) {
    // addi r30,r29,-4 -- the second run continues exactly where the first stopped, and `lwzu`
    // pre-increments, so the first read is at r29 itself.
    uint64_t entry = offset_entry - 4;
    for (;;) {
      // loc_82B1C0E4: lwzu r11,4(r30)
      entry = entry + 4;
      const uint32_t offset = REX_LOAD_U32(static_cast<uint32_t>(entry));
      const uint32_t element = REX_LOAD_U32(self + kElement);  // lwz r10,4(r31) -- RELOADED here
      const uint64_t slot = static_cast<uint64_t>(offset) + element;  // add r11,r11,r10
      const uint32_t held = REX_LOAD_U32(static_cast<uint32_t>(slot) + kHeld);  // lwz r3,8(r11)
      // cmplwi cr6,r3,0 ; beq cr6,0x82b1c100
      if (held != 0) {
        // bl 0x828e2c78 -- one argument; r4 is not set up.
        skate3::audio::GuestCall(ctx, base, sub_828E2C78, static_cast<uint64_t>(held));
      }
      // loc_82B1C100
      descriptor = REX_LOAD_U32(self + kDescriptor);                // lwz r11,0(r31)
      tail_index = tail_index + 1;                                  // addi r28,r28,1
      const uint32_t count = REX_LOAD_U8(descriptor + kTailCount);  // lbz r10,39(r11)
      // cmpw cr6,r28,r10 ; blt cr6,0x82b1c0e4 -- SIGNED on the low words.
      if (static_cast<int32_t>(static_cast<uint32_t>(tail_index)) >= static_cast<int32_t>(count))
        break;
    }
  }

  // loc_82B1C114: one instance fewer of this description. addi r8,r10,-1 is 64-bit and the `sth`
  // keeps the low halfword, so a count already at 0 stores 0xFFFF.
  const uint32_t instances = REX_LOAD_U16(descriptor + kInstanceCount);  // lhz r10,28(r11)
  REX_STORE_U16(descriptor + kInstanceCount,
                static_cast<uint16_t>(instances - 1));  // sth r8,28(r11)

  // lwz r10,30252(r9) ; lwz r4,4(r31) ; lwz r3,36(r10) ; lwz r6,0(r3) ; lwz r11,12(r6) ;
  // mtctr r11 ; bctrl -- free(allocator, element, 0): the element destroys itself here.
  const uint32_t system = REX_LOAD_U32(kSystemGlobal);
  const uint32_t element = REX_LOAD_U32(self + kElement);
  const uint32_t allocator = REX_LOAD_U32(system + kAllocatorField);
  const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);
  ctx.r3.u64 = allocator;
  ctx.r4.u64 = element;
  ctx.r5.s64 = 0;  // li r5,0, set before the sth in the lifted order; nothing reads it until here
  ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotFree);
  ctx.lr = 0x82B1C144;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  // addi r1,r1,128 ; b __restgprlr_27. No `li r3,...`: r3 is whatever the free left.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.r27.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR27);   // ld r27,-48(r1)
  ctx.r28.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR28);   // ld r28,-40(r1)
  ctx.r29.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR29);   // ld r29,-32(r1)
  ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR30);   // ld r30,-24(r1)
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR31);   // ld r31,-16(r1)
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
}

// Never called: the SKATE3_PORT macro skips the shadow branch for a gate-labelled port. Kept so
// the file has the same shape as every other port; the write set it would declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1BF98
SKATE3_PORT(82B1BF98, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
