// sub_82B49280  role: register an object in its owner's priority-ordered handle array -- grow the array through the owner's allocator when it is full (allocate a bigger block, sub_82EDF460 the old contents over, free the old block, publish the pointer at +108 and the capacity at +282), then find the first entry whose priority byte at +73 is not below this object's, sub_82F4DC60 the tail up one 8-byte slot, write {object, object+64} into the hole, bump the count at +280 and raise the high-water mark at +272; on a failed allocation instead mark the object (+71 = 2, +76 = 1) and push its embedded node at +28 onto the owner's list at +16, returning 0   lifted: skate3_recomp.68.cpp:38556 (241 lines)
// STATUS: gate-1 (both of its own bctrl sites are the owner's allocator vtable: sub_82B49280 indirect call, the +4 allocate at `bctrl` 0x82B492EC and the +12 free at `bctrl` 0x82B49328 -- a replay would allocate a second block and free the same pointer twice; census indirect 2, depth 0. docs/shadow-harness.md names these two as the reason transitive screening exists, since they also kill sub_82B48440 and sub_82B49210 one level up)
// calls/session: boot 2210  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B49280.md
namespace port_82B49280 {

// docs/rw_audio_structs.h names none of this. The offsets are the ones this function touches;
// sub_82B48F28 and sub_82B49238 describe the same embedded node at +28/+32 and the same +71.
constexpr uint32_t kOwner = 16;     // lwz r11,16(r3)  -- the object's owner, reloaded constantly
constexpr uint32_t kPayload = 64;   // lwz r8,64(r31)  -- the array entry's second word
constexpr uint32_t kKind = 71;      // stb 2,71(r31)   -- the type byte sub_82B48F28 dispatches on
constexpr uint32_t kPriority = 73;  // lbz 73(...)     -- the UNSIGNED sort key, on both objects
constexpr uint32_t kFailFlag = 76;  // stw 1,76(r31)
constexpr uint32_t kNodeNext = 28;  // stw head,28(r31) ; the node address passed around is r31+28
constexpr uint32_t kNodePrev = 32;  // stw 0,32(r31)
constexpr uint32_t kLinkPrev = 4;   // stw r11,4(r9) -- `prev` relative to a NODE address

constexpr uint32_t kListHead = 16;   // owner+16 -- head of the failed-registration list
constexpr uint32_t kAllocator = 36;  // owner+36 -- the allocator object, vtable at +0
constexpr uint32_t kEntries = 108;   // owner+108 -- the array of 8-byte entries
constexpr uint32_t kPeak = 272;      // owner+272 u32 -- high-water mark of the count
constexpr uint32_t kInitial = 276;   // owner+276 u32 -- the capacity to use when there is none
constexpr uint32_t kCount = 280;     // owner+280 u16
constexpr uint32_t kCapacity = 282;  // owner+282 u16

constexpr uint32_t kEntryObject = 0;   // stwx r31,r9,r30
constexpr uint32_t kEntryPayload = 4;  // stw r8,4(r6)
constexpr uint32_t kEntryBytes = 8;    // addi r10,r10,8 and the three `rlwinm ...,3,0,28`

constexpr uint32_t kVptr = 0;         // lwz r10,0(r3)
constexpr uint32_t kSlotAllocate = 4;  // lwz r9,4(r10)  -- bctrl 0x82B492EC
constexpr uint32_t kSlotFree = 12;     // lwz r8,12(r9)  -- bctrl 0x82B49328
constexpr uint32_t kGrowStep = 32;     // li r10,32 -- every grow after the first adds 32 entries

// lis r9,-32241 ; addi r5,r9,-9960 -- ((-32241 & 0xFFFF) << 16) - 9960, the allocation tag the
// allocate slot receives in r5. Not dereferenced here.
constexpr uint32_t kAllocTag = 0x820ED918;

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
// __savegprlr_28 / __restgprlr_28 slots, negative offsets from the ENTRY sp, all inside the 128
// bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR28 = 40;
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

// rlwinm rX,rY,3,0,28: the rotate is 64-bit over the doubled low word, but the 0xFFFFFFF8 mask
// clears the three bits that wrap in, so the value is the low word times eight, modulo 2^32.
uint64_t TimesEight(uint64_t count) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(count)) << 3) & 0xFFFFFFF8ull;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3

  // The frame is reproduced because both named callees and both indirect targets run on it.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U64(entry_sp - kSaveR28, ctx.r28.u64);   // bl 0x82f52118 -> std r28,-40(r1)
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);   //                  std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);   //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);   //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  //                  stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  bool inserting = true;  // false only when the allocation fails
  {
    const uint32_t owner = REX_LOAD_U32(self + kOwner);    // lwz r11,16(r3)
    const uint32_t count = REX_LOAD_U16(owner + kCount);   // lhz r9,280(r11)
    const uint32_t capacity = REX_LOAD_U16(owner + kCapacity);  // lhz r10,282(r11)
    // cmplw cr6,r9,r10 ; blt cr6,0x82b4933c -- UNSIGNED; room left skips the whole grow.
    if (!(count < capacity)) {
      // cmplwi cr6,r10,0 ; bne cr6,0x82b492b4 -- the first grow takes its size from +276, every
      // later one adds 32.
      const uint64_t growth =
          (capacity != 0) ? uint64_t(kGrowStep) : uint64_t(REX_LOAD_U32(owner + kInitial));

      // loc_82B492B8: the capacity is RELOADED here, so the sum below uses the second read.
      const uint32_t allocator = REX_LOAD_U32(owner + kAllocator);  // lwz r3,36(r11)
      const uint64_t old_capacity = REX_LOAD_U16(owner + kCapacity);  // lhz r11,282(r11)
      const uint64_t new_capacity = old_capacity + growth;  // add r29,r11,r10 -- 64-bit
      const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);  // lwz r10,0(r3)
      const uint64_t new_bytes = TimesEight(new_capacity);      // rlwinm r4,r29,3,0,28
      const uint64_t old_bytes =
          __builtin_rotateleft32(static_cast<uint32_t>(old_capacity), 3);  // rotlwi r28,r11,3

      // lwz r9,4(r10) ; mtctr r9 ; bctrl -- THE allocate: alloc(this, bytes, tag, 0, 16, 0).
      ctx.r3.u64 = allocator;
      ctx.r4.u64 = new_bytes;
      ctx.r5.u64 = kAllocTag;  // addi r5,r9,-9960
      ctx.r6.s64 = 0;          // li r6,0
      ctx.r7.s64 = 16;         // li r7,16
      ctx.r8.s64 = 0;          // li r8,0
      ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotAllocate);
      ctx.lr = 0x82B492F0;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      const uint32_t block = ctx.r3.u32;  // mr r30,r3

      // cmplwi cr6,r3,0 ; beq cr6,0x82b493f0
      if (block == 0) {
        // loc_82B493F0: mark the object and push its embedded node onto the owner's list.
        const uint32_t list_owner = REX_LOAD_U32(self + kOwner);  // lwz r10,16(r31)
        REX_STORE_U8(self + kKind, 2);                            // stb r11,71(r31)  (r11 = 2)
        REX_STORE_U32(self + kFailFlag, 1);                       // stw r9,76(r31)   (r9  = 1)
        const uint64_t node = static_cast<uint64_t>(self) + kNodeNext;  // addi r11,r31,28
        const uint32_t head = REX_LOAD_U32(list_owner + kListHead);     // lwz r7,16(r10)
        REX_STORE_U32(self + kNodePrev, 0);                             // stw r8,32(r31)
        REX_STORE_U32(self + kNodeNext, head);                          // stw r7,28(r31)
        // lwz r9,16(r10) -- the head is READ AGAIN after those two stores, which could alias it
        // if the node ever overlapped the owner. Kept as lifted.
        const uint32_t head_again = REX_LOAD_U32(list_owner + kListHead);
        // cmplwi cr6,r9,0 ; beq cr6,0x82b49428
        if (head_again != 0) {
          REX_STORE_U32(head_again + kLinkPrev,
                        static_cast<uint32_t>(node));  // stw r11,4(r9) -- old head's prev
        }
        // loc_82B49428
        REX_STORE_U32(list_owner + kListHead, static_cast<uint32_t>(node));  // stw r11,16(r10)
        ctx.r3.s64 = 0;  // li r3,0
        inserting = false;
      } else {
        // r3 still holds the new block; r4 is the old array and r5 its byte count.
        const uint32_t copy_owner = REX_LOAD_U32(self + kOwner);      // lwz r11,16(r31)
        const uint32_t old_array = REX_LOAD_U32(copy_owner + kEntries);  // lwz r4,108(r11)
        // bl 0x82edf460 -- copy the old contents into the new block.
        skate3::audio::GuestCall(ctx, base, sub_82EDF460, static_cast<uint64_t>(block),
                                 static_cast<uint64_t>(old_array), old_bytes);

        // lwz r10,16(r31) ; lwz r3,36(r10) ; lwz r4,108(r10) ; li r5,0 ; lwz r9,0(r3) ;
        // lwz r8,12(r9) ; mtctr r8 ; bctrl -- THE free, on the OLD array, reloaded after the copy.
        const uint32_t free_owner = REX_LOAD_U32(self + kOwner);
        const uint32_t free_allocator = REX_LOAD_U32(free_owner + kAllocator);
        const uint32_t freed = REX_LOAD_U32(free_owner + kEntries);
        const uint32_t free_vtable = REX_LOAD_U32(free_allocator + kVptr);
        ctx.r3.u64 = free_allocator;
        ctx.r4.u64 = freed;
        ctx.r5.s64 = 0;
        ctx.ctr.u64 = REX_LOAD_U32(free_vtable + kSlotFree);
        ctx.lr = 0x82B4932C;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

        // The owner is reloaded once per store, as lifted.
        const uint32_t publish_owner = REX_LOAD_U32(self + kOwner);  // lwz r6,16(r31)
        REX_STORE_U32(publish_owner + kEntries, block);              // stw r30,108(r6)
        const uint32_t capacity_owner = REX_LOAD_U32(self + kOwner);  // lwz r5,16(r31)
        REX_STORE_U16(capacity_owner + kCapacity,
                      static_cast<uint16_t>(new_capacity));  // sth r29,282(r5)
      }
    }
  }

  if (inserting) {
    // loc_82B4933C
    const uint32_t owner = REX_LOAD_U32(self + kOwner);  // lwz r9,16(r31) -- r9 survives to +108
    uint64_t index = 0;                                  // li r11,0
    // lhz r8,280(r9) -- read ONCE; the loop bound below is reloaded but the subf uses this one.
    const uint32_t used = REX_LOAD_U16(owner + kCount);
    // cmpwi cr6,r8,0 ; ble cr6,0x82b49380 -- SIGNED on a zero-extended halfword, so only 0 skips.
    if (static_cast<int32_t>(used) > 0) {
      const uint32_t priority = REX_LOAD_U8(self + kPriority);   // lbz r7,73(r31)
      uint64_t entry = REX_LOAD_U32(owner + kEntries);           // lwz r10,108(r9)
      for (;;) {
        // loc_82B49358
        const uint32_t held =
            REX_LOAD_U32(static_cast<uint32_t>(entry) + kEntryObject);  // lwz r6,0(r10)
        const uint32_t other = REX_LOAD_U8(held + kPriority);           // lbz r5,73(r6)
        // cmplw cr6,r7,r5 ; ble cr6,0x82b49380 -- UNSIGNED. Stop at the first entry this object
        // does not outrank, so equal priorities keep their order.
        if (!(priority > other)) break;
        const uint32_t live_owner = REX_LOAD_U32(self + kOwner);  // lwz r6,16(r31)
        index = index + 1;                                        // addi r11,r11,1
        entry = entry + kEntryBytes;                              // addi r10,r10,8
        const uint32_t live = REX_LOAD_U16(live_owner + kCount);  // lhz r5,280(r6)
        // cmpw cr6,r11,r5 ; blt cr6,0x82b49358 -- SIGNED on the low words.
        if (static_cast<int32_t>(static_cast<uint32_t>(index)) >= static_cast<int32_t>(live))
          break;
      }
    }

    // loc_82B49380: r9 is still the owner loaded at loc_82B4933C, not reloaded.
    const uint32_t array = REX_LOAD_U32(owner + kEntries);  // lwz r10,108(r9)
    const uint64_t offset = TimesEight(index);              // rlwinm r30,r11,3,0,28
    const uint64_t tail = static_cast<uint64_t>(used) - index;  // subf r11,r11,r8 -- 64-bit
    const uint64_t hole = static_cast<uint64_t>(array) + offset;  // add r4,r10,r30
    const uint64_t moved = TimesEight(tail);                      // rlwinm r5,r11,3,0,28
    // addi r3,r4,8 ; bl 0x82f4dc60 -- shift the tail up one slot to open the hole.
    skate3::audio::GuestCall(ctx, base, sub_82F4DC60, hole + kEntryBytes, hole, moved);

    // Every store below reloads the owner and the array, because the memmove could have moved
    // either; the insertion offset in r30 is the only thing carried across.
    const uint32_t store_owner = REX_LOAD_U32(self + kOwner);      // lwz r10,16(r31)
    const uint32_t store_array = REX_LOAD_U32(store_owner + kEntries);  // lwz r9,108(r10)
    REX_STORE_U32(store_array + static_cast<uint32_t>(offset), self);   // stwx r31,r9,r30

    const uint32_t pay_owner = REX_LOAD_U32(self + kOwner);            // lwz r7,16(r31)
    const uint32_t payload = REX_LOAD_U32(self + kPayload);            // lwz r8,64(r31)
    const uint32_t pay_array = REX_LOAD_U32(pay_owner + kEntries);     // lwz r11,108(r7)
    const uint64_t slot = static_cast<uint64_t>(pay_array) + offset;   // add r6,r11,r30
    REX_STORE_U32(static_cast<uint32_t>(slot) + kEntryPayload, payload);  // stw r8,4(r6)

    const uint32_t bump_owner = REX_LOAD_U32(self + kOwner);           // lwz r11,16(r31)
    const uint32_t before = REX_LOAD_U16(bump_owner + kCount);         // lhz r10,280(r11)
    // addi r5,r10,1 ; sth r5,280(r11) -- only the low halfword lands, so 0xFFFF wraps to 0.
    REX_STORE_U16(bump_owner + kCount, static_cast<uint16_t>(before + 1));

    const uint32_t peak_owner = REX_LOAD_U32(self + kOwner);       // lwz r11,16(r31)
    const uint32_t after = REX_LOAD_U16(peak_owner + kCount);      // lhz r10,280(r11) -- reloaded
    const uint32_t peak = REX_LOAD_U32(peak_owner + kPeak);        // lwz r3,272(r11)
    // cmplw cr6,r10,r3 ; ble cr6,0x82b493e4 -- UNSIGNED
    if (after > peak) REX_STORE_U32(peak_owner + kPeak, after);    // stw r10,272(r11)

    ctx.r3.s64 = 1;  // li r3,1
  }

  // Both exits share this epilogue: addi r1,r1,128 ; b __restgprlr_28.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
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

}  // namespace port_82B49280
SKATE3_PORT(82B49280, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
