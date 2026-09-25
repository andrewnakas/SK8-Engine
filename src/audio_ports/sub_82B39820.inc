// sub_82B39820  role: retire a node pool's head block -- refuse unless a second block exists and the slack (+28 capacity minus +24 in use) covers the block's entry count, then drop every free entry out of the free list at +12, evacuate every live entry by detaching its owner, pushing the node back through sub_82B39690 and handing the owner a replacement through sub_82B395E8, unlink the block from the +0/+4/+8 block list, subtract its entry count from the capacity at +28, and free the block through the global allocator's vtable slot 12   lifted: skate3_recomp.67.cpp:67109 (241 lines)
// STATUS: gate-1 (its own `bctrl` at 0x82B399C0 is the allocator's FREE entry -- the global at 0x8307762C, its +36 allocator, vtable slot 12 -- so a replay would double-free the block; sub_82B395E8 also reaches sub_82B39728, which grows the pool)
// calls/session: boot 37370  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B39820.md
namespace port_82B39820 {

// docs/rw_audio_structs.h names none of these three layouts. The names describe the use that is
// visible here and in sub_82B39690 (verified) and sub_82B395E8.
//
// The pool (r30, the entry r3):
constexpr uint32_t kBlockHead = 0;   // lwz r11,0(r3) -- the block this call retires
constexpr uint32_t kBlockTail = 4;   // stw r27,4(r30) -- cleared when the head becomes null
constexpr uint32_t kBlockCount = 8;  // decremented by one
constexpr uint32_t kFreeHead = 12;   // the free-node list head, advanced when it names a victim
constexpr uint32_t kInUse = 24;      // lwz r11,24(r30) -- the slack check's subtrahend
constexpr uint32_t kCapacity = 28;   // lwz r9,28(r30) -- reduced by the block's entry count

// The block (r28):
constexpr uint32_t kNextBlock = 0;   // lwz r10,0(r11)
constexpr uint32_t kEntryCount = 4;  // lwz r10,4(r28) -- RELOADED at both loop feet
constexpr uint32_t kEntries = 8;     // addi r31,r28,8
constexpr uint32_t kEntryStride = 16;

// An entry / node (16 bytes):
constexpr uint32_t kNodeNext = 0;   // and, on an owner, the node the owner holds
constexpr uint32_t kNodePrev = 4;
constexpr uint32_t kNodeOwner = 8;  // zero means the entry is free

// lis r10,-31993 ; lwz r11,30252(r10)
// ((-31993 & 0xFFFF) << 16) + 30252 = 0x83070000 + 0x762C. Computed, not read by eye.
constexpr uint32_t kAllocatorGlobal = 0x8307762C;
constexpr uint32_t kAllocatorSlot = 36;  // lwz r3,36(r11) -- the allocator object
constexpr uint32_t kVptr = 0;            // lwz r7,0(r3)
constexpr uint32_t kFreeSlot = 12;       // lwz r6,12(r7) -- the same free entry sub_82B32870 uses

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
// __savegprlr_27 / __restgprlr_27 slots, negative offsets from the ENTRY sp, all inside the
// 128 bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR27 = 48;
constexpr uint32_t kSaveR28 = 40;
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

// The unlink both loops perform on a node, in the original's order: advance the pool's free-list
// head if it names this node, then prev->next = next, then next->prev = prev. Both pointer tests
// are SIGNED compares against zero in the lifted form.
inline void UnlinkFromFreeList(uint8_t* base, uint32_t pool, uint32_t node) {
  // lwz r10,12(r30) ; cmplw cr6,r11,r10 ; bne -- UNSIGNED pointer compare.
  const uint32_t head = REX_LOAD_U32(pool + kFreeHead);
  if (node == head) {
    // lwz r10,0(r10) ; stw r10,12(r30)
    REX_STORE_U32(pool + kFreeHead, REX_LOAD_U32(head + kNodeNext));
  }
  // lwz r10,4(r11) ; cmpwi cr6,r10,0 ; beq
  const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
  if (static_cast<int32_t>(prev) != 0) {
    // lwz r8,0(r11) ; stw r8,0(r10)
    REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));
  }
  // lwz r10,0(r11) ; cmpwi cr6,r10,0 ; beq -- reloaded, because the store above could alias it.
  const uint32_t next = REX_LOAD_U32(node + kNodeNext);
  if (static_cast<int32_t>(next) != 0) {
    // lwz r8,4(r11) ; stw r8,4(r10)
    REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));
  }
}

REX_FUNC(Native) {
  const uint32_t pool = ctx.r3.u32;  // mr r30,r3

  // The frame is reproduced because both named callees and the indirect free run on it.
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

  // li r27,0 ; mr r3,r27 -- every early exit returns this zero.
  ctx.r3.u64 = 0;

  // lwz r11,0(r3) ; cmpwi cr6,r11,0 ; beq -- SIGNED test, no block to retire.
  const uint32_t block = REX_LOAD_U32(pool + kBlockHead);
  if (static_cast<int32_t>(block) == 0) goto epilogue;
  // lwz r10,0(r11) ; cmpwi cr6,r10,0 ; beq -- the LAST block is never retired.
  if (static_cast<int32_t>(REX_LOAD_U32(block + kNextBlock)) == 0) goto epilogue;

  {
    const uint32_t in_use = REX_LOAD_U32(pool + kInUse);       // lwz r11,24(r30)
    const uint32_t capacity = REX_LOAD_U32(pool + kCapacity);  // lwz r9,28(r30)
    // lwz r10,4(r28)
    int32_t count = static_cast<int32_t>(REX_LOAD_U32(block + kEntryCount));
    // subf r8,r11,r9 -- 64-bit on two zero-extended words, so the difference can carry into the
    // upper half; `cmpw` then looks only at the low word.
    const uint64_t slack = static_cast<uint64_t>(capacity) - static_cast<uint64_t>(in_use);
    // cmpw cr6,r10,r8 ; bgt cr6,0x82b399c8 -- not enough room elsewhere for this block's entries.
    if (count > static_cast<int32_t>(static_cast<uint32_t>(slack))) goto epilogue;

    const uint32_t entries = block + kEntries;  // addi r31,r28,8

    // Loop A, loc_82B39880: drop every FREE entry out of the free list. cmpwi cr6,r10,0 ; ble.
    if (count > 0) {
      uint32_t node = entries;  // mr r11,r31
      int32_t i = 0;            // mr r9,r27
      do {
        // lwz r10,8(r11) ; cmplwi cr6,r10,0 ; bne -- a live entry is left for loop B.
        if (REX_LOAD_U32(node + kNodeOwner) == 0) {
          UnlinkFromFreeList(base, pool, node);
        }
        // loc_82B398C8: the count is RELOADED here, and this reload is also what loop B's entry
        // test reads, so it is kept as one variable.
        count = static_cast<int32_t>(REX_LOAD_U32(block + kEntryCount));
        i++;                      // addi r9,r9,1
        node += kEntryStride;     // addi r11,r11,16
      } while (i < count);        // cmpw cr6,r9,r10 ; blt
    }

    // loc_82B398DC: the test reads the same r10 loop A left behind. r31 was never advanced by
    // loop A, so loop B starts at the first entry again.
    if (count > 0) {
      uint32_t node = entries;  // r31
      int32_t j = 0;            // mr r29,r27
      do {
        // lwz r9,8(r31) ; cmplwi cr6,r9,0 ; beq -- a free entry is already handled.
        const uint32_t owner = REX_LOAD_U32(node + kNodeOwner);
        if (owner != 0) {
          // lwz r4,0(r9) -- the node the owner currently holds. It should be `node` itself, and
          // the original does not assume it.
          const uint32_t held = REX_LOAD_U32(owner + kNodeNext);
          REX_STORE_U32(owner + kNodeNext, 0);  // stw r27,0(r9)
          REX_STORE_U32(held + kNodeOwner, 0);  // stw r27,8(r4)
          // bl 0x82b39690 -- unlinks that node from the active list and pushes it onto the free
          // list at +12, which the unlink below then immediately pops it off again.
          skate3::audio::GuestCall(ctx, base, sub_82B39690, static_cast<uint64_t>(pool),
                                   static_cast<uint64_t>(held));
          UnlinkFromFreeList(base, pool, node);
          // loc_82B39944: bl 0x82b395e8 -- gives the owner a replacement node, growing the pool
          // through sub_82B39728 if the free list is empty.
          skate3::audio::GuestCall(ctx, base, sub_82B395E8, static_cast<uint64_t>(pool),
                                   static_cast<uint64_t>(owner));
        }
        // loc_82B39950: a second, independent reload of the count (r11, not r10).
        const int32_t bound = static_cast<int32_t>(REX_LOAD_U32(block + kEntryCount));
        j++;                     // addi r29,r29,1
        node += kEntryStride;    // addi r31,r31,16
        if (j >= bound) break;   // cmpw cr6,r29,r11 ; blt
      } while (true);
    }

    // loc_82B39964: unlink the block. The head is reloaded, because the callees above may have
    // allocated a new block onto the same list.
    const uint32_t head = REX_LOAD_U32(pool + kBlockHead);  // lwz r11,0(r30)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b39990
    if (head != 0) {
      const uint32_t next = REX_LOAD_U32(head + kNextBlock);  // lwz r11,0(r11)
      // cmplwi cr6,r11,0 ; stw r11,0(r30) ; bne -- the store happens BEFORE the branch and on
      // both paths.
      REX_STORE_U32(pool + kBlockHead, next);
      if (next == 0) {
        REX_STORE_U32(pool + kBlockTail, 0);  // stw r27,4(r30)
      }
      // loc_82B39984: lwz r11,8(r30) ; addi r11,r11,-1 ; stw r11,8(r30) -- 64-bit decrement,
      // low word stored.
      const uint64_t blocks = static_cast<uint64_t>(REX_LOAD_U32(pool + kBlockCount)) - 1ull;
      REX_STORE_U32(pool + kBlockCount, static_cast<uint32_t>(blocks));
    }

    // loc_82B39990
    const uint32_t old_capacity = REX_LOAD_U32(pool + kCapacity);        // lwz r11,28(r30)
    const uint32_t retired = REX_LOAD_U32(block + kEntryCount);          // lwz r9,4(r28)
    // subf r8,r9,r11 -- 64-bit again; the low word is what is stored.
    const uint64_t new_capacity =
        static_cast<uint64_t>(old_capacity) - static_cast<uint64_t>(retired);
    REX_STORE_U32(pool + kCapacity, static_cast<uint32_t>(new_capacity));  // stw r8,28(r30)

    // The gate-1 call: the allocator's free entry, reached through a global.
    const uint32_t owner_object = REX_LOAD_U32(kAllocatorGlobal);           // lwz r11,30252(r10)
    const uint32_t allocator = REX_LOAD_U32(owner_object + kAllocatorSlot); // lwz r3,36(r11)
    ctx.r3.u64 = allocator;
    ctx.r4.u64 = block;  // mr r4,r28
    ctx.r5.s64 = 0;      // li r5,0
    const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);  // lwz r7,0(r3)
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);           // lwz r6,12(r7) ; mtctr r6
    ctx.lr = 0x82B399C4;                                      // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    ctx.r3.s64 = 1;  // li r3,1
  }

epilogue:
  // loc_82B399C8: addi r1,r1,128 ; b __restgprlr_27.
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

}  // namespace port_82B39820
SKATE3_PORT(82B39820, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
