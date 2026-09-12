// sub_82B4F8C8  role: the acquire half of sub_82B4FAF8 -- install the vtable at +0, point +52 at the 8-byte-aligned array embedded at self+95, zero (channels+1)/2 entries of 28 bytes through sub_82F52040, publish that pair count at +68, then per pair take the global critical section, pop the oldest node off the anchor at 0x830BDEE4, push it onto the head at 0x830BDEE0, release the lock and record the object (node - 20) in the array entry with 2 channels, or 1 for the odd last one; if any pop finds the anchor empty, walk every entry back onto the anchor under the same lock and return 0, else return 1   lifted: skate3_recomp.68.cpp:54348 (315 lines)
// STATUS: gate-1 (RtlEnterCriticalSection / RtlLeaveCriticalSection: it takes the lock itself, twice per pair on the rollback path, census depth 0 -- and the pops it makes from a global free list are exactly what a rewind cannot put back)
// calls/session: boot 1924  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B4F8C8.md
namespace port_82B4F8C8 {

// Constant addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye. The
// names match sub_82B4FAF8, which is the release half over the same three globals.
// lis r11,-32206 -> 0x82320000 ; addi r8,r11,-17660
constexpr uint32_t kVtable = 0x8231BB04;  // stw r8,0(r3) -- what this object becomes
// lis r25,-31992 -> 0x83080000 ; lwz r3,17904(r25)
constexpr uint32_t kLockSlot = 0x830845F0;  // holds the RTL_CRITICAL_SECTION*, reloaded per use
// lis r19,-31988 -> 0x830C0000 ; lwz/stw -8480(r19)
constexpr uint32_t kSpareHead = 0x830BDEE0;  // sub_82B4FAF8's name for it; this function PUSHES
                                             // every node it takes onto it, so it is the live head
// lis r11,-31988 ; addi r29,r11,-8476 -- four bytes above kSpareHead
constexpr uint32_t kAnchor = 0x830BDEE4;

// The anchor's three words, named as sub_82B4FAF8 named them. That port pushes at +4 and only ever
// writes +0 while it is null; this one pops from +0, which confirms the reading: +0 is the far end
// of the chain and +4 the newest node.
constexpr uint32_t kAnchorEnd = 0;
constexpr uint32_t kAnchorFront = 4;
constexpr uint32_t kAnchorCount = 8;

// The intrusive node, 20 bytes into the object each array entry points at. kLinkPrev leads toward
// the front (the newer node), kLinkNext toward the end.
constexpr uint32_t kNodeOffset = 20;
constexpr uint32_t kLinkPrev = 0;
constexpr uint32_t kLinkNext = 4;

// The object (r3/r31). Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kVptr = 0;
constexpr uint32_t kChannels = 46;     // lbz r27,46(r3) -- consumed two per entry
constexpr uint32_t kEntryArray = 52;   // stw r7,52(r3) -- (self + 95) rounded DOWN to 8
constexpr uint32_t kArrayInline = 95;  // addi r10,r3,95 ; rlwinm r7,r10,0,0,28
constexpr uint32_t kField60 = 60;      // stw 0
constexpr uint32_t kEntryCount = 68;   // stw r26,68(r31) -- the pair count sub_82B4FAF8 re-reads
constexpr uint32_t kField72 = 72;      // stw 0
constexpr uint32_t kField76 = 76;      // stw 0
constexpr uint32_t kFlag84 = 84;       // stb 0
constexpr uint32_t kFlag85 = 85;       // stb 1
constexpr uint32_t kEntryStride = 28;  // addi r28,r28,28 ; mulli r5,r26,28
constexpr uint32_t kEntryChannels = 24;  // stb 24(r11) -- 2, or 1 for the odd last entry

constexpr uint32_t kFrameBytes = 192;  // stwu r1,-192(r1)
// __savegprlr_19 / __restgprlr_19 slots, negative offsets from the ENTRY sp, all thirteen plus the
// link slot inside the 192 bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR19 = 112;
constexpr uint32_t kSaveR20 = 104;
constexpr uint32_t kSaveR21 = 96;
constexpr uint32_t kSaveR22 = 88;
constexpr uint32_t kSaveR23 = 80;
constexpr uint32_t kSaveR24 = 72;
constexpr uint32_t kSaveR25 = 64;
constexpr uint32_t kSaveR26 = 56;
constexpr uint32_t kSaveR27 = 48;
constexpr uint32_t kSaveR28 = 40;
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

// The two kernel imports the `bl`s reach. probe/ports/lint.py check 6 forbids naming an import
// thunk from a port body -- that rule exists so a guest callee goes through GuestCall and is
// exercised by its own hook, which a kernel import cannot be -- so both are reached through their
// guest addresses, which is the same code the `bl` enters. The one difference from the original is
// that this writes ctr, which the `bl` does not; ctr is volatile, outside the compared set, and
// this body never runs anyway (gate-1). Same constants as sub_82B4FAF8.
constexpr uint32_t kRtlEnterCriticalSection = 0x82F9CB44;  // bl 0x82f9cb44
constexpr uint32_t kRtlLeaveCriticalSection = 0x82F9CB54;  // bl 0x82f9cb54

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3

  // The frame is reproduced because sub_82F52040 and both imports run on it.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                             // mflr r12
  REX_STORE_U64(entry_sp - kSaveR19, ctx.r19.u64);  // bl 0x82f520f4 -> std r19,-112(r1)
  REX_STORE_U64(entry_sp - kSaveR20, ctx.r20.u64);  //                  std r20,-104(r1)
  REX_STORE_U64(entry_sp - kSaveR21, ctx.r21.u64);  //                  std r21,-96(r1)
  REX_STORE_U64(entry_sp - kSaveR22, ctx.r22.u64);  //                  std r22,-88(r1)
  REX_STORE_U64(entry_sp - kSaveR23, ctx.r23.u64);  //                  std r23,-80(r1)
  REX_STORE_U64(entry_sp - kSaveR24, ctx.r24.u64);  //                  std r24,-72(r1)
  REX_STORE_U64(entry_sp - kSaveR25, ctx.r25.u64);  //                  std r25,-64(r1)
  REX_STORE_U64(entry_sp - kSaveR26, ctx.r26.u64);  //                  std r26,-56(r1)
  REX_STORE_U64(entry_sp - kSaveR27, ctx.r27.u64);  //                  std r27,-48(r1)
  REX_STORE_U64(entry_sp - kSaveR28, ctx.r28.u64);  //                  std r28,-40(r1)
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);  //                  std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);  //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);  //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32); //                  stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;    // stwu r1,-192(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  // lbz r27,46(r3) ; addi r9,r27,1 ; rlwinm r26,r9,31,1,31 -- the rotate-by-31 with a 0x7FFFFFFF
  // mask is an unsigned shift right by one: (channels + 1) / 2 pairs.
  uint64_t channels = REX_LOAD_U8(self + kChannels);
  const uint64_t pairs = (channels + 1) >> 1;
  // addi r10,r3,95 ; rlwinm r7,r10,0,0,28 -- rotate by zero, mask 0xFFFFFFF8: rounded DOWN to 8.
  const uint32_t array = static_cast<uint32_t>(self + kArrayInline) & 0xFFFFFFF8u;

  REX_STORE_U32(self + kVptr, kVtable);                      // stw r8,0(r3)
  REX_STORE_U32(self + kEntryArray, array);                  // stw r7,52(r3)
  const uint64_t zeroed = pairs * kEntryStride;              // mulli r5,r26,28 -- 64-bit
  REX_STORE_U32(self + kField72, 0);                         // stw r20,72(r3)
  REX_STORE_U32(self + kField60, 0);                         // stw r20,60(r3)
  REX_STORE_U32(self + kField76, 0);                         // stw r20,76(r3)
  REX_STORE_U8(self + kFlag84, 0);                           // stb r20,84(r3)
  const uint32_t memset_base = REX_LOAD_U32(self + kEntryArray);  // lwz r3,52(r3) -- RELOADED
  REX_STORE_U32(self + kEntryCount, static_cast<uint32_t>(pairs));  // stw r26,68(r31)
  REX_STORE_U8(self + kFlag85, 1);                           // stb r24,85(r31)
  uint64_t failed = 0;                                       // mr r23,r20
  // bl 0x82f52040 -- memset(array, 0, pairs * 28).
  skate3::audio::GuestCall(ctx, base, sub_82F52040, static_cast<uint64_t>(memset_base), 0ull,
                           zeroed);

  // cmplwi cr6,r26,0 ; beq cr6,0x82b4fa0c -- zero channels means zero pairs and no allocation.
  if (pairs != 0) {
    uint64_t offset = 0;         // mr r28,r20 -- the byte offset into the entry array
    uint64_t remaining = pairs;  // mr r21,r26
    do {
      // loc_82B4F954: lwz r3,17904(r25) ; bl 0x82f9cb44 -- the lock POINTER is loaded from the
      // slot, once per pair, and again for each release.
      ctx.r3.u64 = REX_LOAD_U32(kLockSlot);
      ctx.ctr.u64 = kRtlEnterCriticalSection;
      ctx.lr = 0x82B4F95C;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

      const uint32_t node = REX_LOAD_U32(kAnchor + kAnchorEnd);  // lwz r30,0(r29)
      // cmplwi cr6,r30,0 ; beq cr6,0x82b4f99c
      if (node == 0) {
        // loc_82B4F99C: nothing left to hand out. The lock is released, the failure flag is set,
        // and the loop STILL RUNS its remaining passes -- a later pass can succeed. Reproduced.
        ctx.r3.u64 = REX_LOAD_U32(kLockSlot);
        ctx.ctr.u64 = kRtlLeaveCriticalSection;
        ctx.lr = 0x82B4F9A4;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        failed = 1;  // mr r23,r24
      } else {
        // Pop it off the anchor's far end: the node one step toward the front becomes the new end.
        const uint32_t newer = REX_LOAD_U32(node + kLinkPrev);  // lwz r11,0(r30)
        REX_STORE_U32(kAnchor + kAnchorEnd, newer);             // stw r11,0(r29)
        // bne cr6,0x82b4f984 -- an emptied chain clears the front too.
        if (newer == 0) {
          REX_STORE_U32(kAnchor + kAnchorFront, 0);  // stw r20,4(r29)
        } else {
          REX_STORE_U32(newer + kLinkNext, 0);       // loc_82B4F984: stw r20,4(r11)
        }
        // loc_82B4F988: addi r11,r11,-1 is 64-bit and the store keeps the low word, so a count
        // already at zero writes 0xFFFFFFFF.
        const uint64_t count = REX_LOAD_U32(kAnchor + kAnchorCount) - 1;  // lwz r11,8(r29)
        REX_STORE_U32(kAnchor + kAnchorCount, static_cast<uint32_t>(count));
        // The `cmplwi cr6,r30,0 ; bne cr6,0x82b4f9ac` here re-tests a value already known
        // non-zero, so loc_82B4F9AC always follows.

        // loc_82B4F9AC: push the node onto the live head. The head is read, then RE-READ after the
        // two stores into the node, which could alias it if the head ever pointed at this node.
        const uint32_t head = REX_LOAD_U32(kSpareHead);  // lwz r11,-8480(r19)
        REX_STORE_U32(node + kLinkNext, 0);              // stw r20,4(r30)
        REX_STORE_U32(node + kLinkPrev, head);           // stw r11,0(r30)
        const uint32_t head_again = REX_LOAD_U32(kSpareHead);
        // cmplwi cr6,r11,0 ; beq cr6,0x82b4f9c8
        if (head_again != 0) REX_STORE_U32(head_again + kLinkNext, node);  // stw r30,4(r11)
        REX_STORE_U32(kSpareHead, node);  // loc_82B4F9C8: stw r30,-8480(r19)

        ctx.r3.u64 = REX_LOAD_U32(kLockSlot);
        ctx.ctr.u64 = kRtlLeaveCriticalSection;
        ctx.lr = 0x82B4F9D4;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bl 0x82f9cb54

        // lwz r10,52(r31) ; addi r11,r30,-20 ; stwx r11,r10,r28 -- the array records the OBJECT,
        // twenty bytes below the node.
        const uint32_t base_now = REX_LOAD_U32(self + kEntryArray);
        const uint64_t object = static_cast<uint64_t>(node) - kNodeOffset;
        REX_STORE_U32(base_now + static_cast<uint32_t>(offset), static_cast<uint32_t>(object));
        // lwz r11,52(r31) ; add r11,r11,r28 -- the base is loaded a SECOND time for the tag store.
        const uint32_t base_again = REX_LOAD_U32(self + kEntryArray);
        const uint64_t slot = static_cast<uint64_t>(base_again) + offset;
        // cmplwi cr6,r27,1 ; ble cr6,0x82b4f9fc -- UNSIGNED, and the compare was issued before the
        // two loads above, on the channel count as it stood at the top of this pass.
        if (channels > 1) {
          channels = channels - 2;  // addi r27,r27,-2
          REX_STORE_U8(static_cast<uint32_t>(slot) + kEntryChannels, 2);  // stb r22,24(r11)
        } else {
          // loc_82B4F9FC: the odd last entry carries one channel, and the count is NOT decremented.
          REX_STORE_U8(static_cast<uint32_t>(slot) + kEntryChannels, 1);  // stb r24,24(r11)
        }
      }
      // loc_82B4FA00: addic. r21,r21,-1 ; addi r28,r28,28 ; bne 0x82b4f954
      ctx.xer.ca = static_cast<uint32_t>(remaining) > 0;  // addic. sets CA; xer is not compared
      remaining = remaining - 1;
      offset = offset + kEntryStride;
    } while (static_cast<int32_t>(static_cast<uint32_t>(remaining)) != 0);
  }

  // loc_82B4FA0C: clrlwi r11,r23,24 ; cmplwi cr6,r11,0 ; beq cr6,0x82b4fae8 -- the flag is 0 or 1,
  // so the mask is a no-op. No failure means success.
  if ((failed & 0xFF) == 0) {
    ctx.r3.s64 = 1;  // loc_82B4FAE8: li r3,1
  } else {
    // Roll every entry that did get a node back onto the anchor. cmplwi cr6,r26,0 ; beq -> 0.
    if (pairs != 0) {
      uint64_t offset = 0;      // mr r30,r20
      uint64_t remaining = pairs;  // r26 itself is the countdown here
      do {
        // loc_82B4FA24
        const uint32_t array_now = REX_LOAD_U32(self + kEntryArray);  // lwz r11,52(r31)
        const uint32_t object =
            REX_LOAD_U32(array_now + static_cast<uint32_t>(offset));  // lwzx r10,r11,r30
        // cmplwi cr6,r10,0 ; beq cr6,0x82b4fad0 -- an entry the failed pass never filled.
        if (object != 0) {
          ctx.r3.u64 = REX_LOAD_U32(kLockSlot);
          ctx.ctr.u64 = kRtlEnterCriticalSection;
          ctx.lr = 0x82B4FA3C;
          REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bl 0x82f9cb44

          // Everything is recomputed inside the lock: lwz r11,52(r31) ; lwz r10,-8480(r19) ;
          // lwzx r11,r11,r30 ; addi r11,r11,20.
          const uint32_t array_locked = REX_LOAD_U32(self + kEntryArray);
          const uint32_t head = REX_LOAD_U32(kSpareHead);
          const uint64_t held = REX_LOAD_U32(array_locked + static_cast<uint32_t>(offset));
          uint32_t node = static_cast<uint32_t>(held + kNodeOffset);
          // cmplw cr6,r11,r10 ; bne cr6,0x82b4fa5c -- popping the live head through kLinkPrev.
          if (node == head) {
            REX_STORE_U32(kSpareHead, REX_LOAD_U32(head + kLinkPrev));  // lwz r10,0(r10)
          }
          // loc_82B4FA5C: the unlink, both sides guarded separately and SIGNED against zero.
          const uint32_t next = REX_LOAD_U32(node + kLinkNext);  // lwz r10,4(r11)
          if (static_cast<int32_t>(next) != 0) {
            REX_STORE_U32(next + kLinkPrev, REX_LOAD_U32(node + kLinkPrev));  // stw r9,0(r10)
          }
          // loc_82B4FA70: the prev link is RELOADED, not carried across that store.
          const uint32_t prev = REX_LOAD_U32(node + kLinkPrev);  // lwz r10,0(r11)
          if (static_cast<int32_t>(prev) != 0) {
            // lwz r11,4(r11) ; stw r11,4(r10) -- this clobbers r11; loc_82B4FA84 rebuilds it.
            REX_STORE_U32(prev + kLinkNext, REX_LOAD_U32(node + kLinkNext));
          }

          // loc_82B4FA84: the array base, the entry and the +20 are all recomputed, after stores
          // that could alias the array.
          const uint32_t array_after = REX_LOAD_U32(self + kEntryArray);
          const uint64_t held_again = REX_LOAD_U32(array_after + static_cast<uint32_t>(offset));
          node = static_cast<uint32_t>(held_again + kNodeOffset);

          REX_STORE_U32(node + kLinkPrev, 0);  // stw r20,0(r11)
          REX_STORE_U32(node + kLinkNext,
                        REX_LOAD_U32(kAnchor + kAnchorFront));  // lwz r10,4(r29) ; stw r10,4(r11)
          // lwz r10,0(r29) ; cmplwi cr6,r10,0 ; bne cr6,0x82b4fab0
          if (REX_LOAD_U32(kAnchor + kAnchorEnd) == 0) {
            REX_STORE_U32(kAnchor + kAnchorEnd, node);  // stw r11,0(r29)
          } else {
            // loc_82B4FAB0: the front is RELOADED here and is still the old front; the store that
            // replaces it is four instructions further down.
            const uint32_t front = REX_LOAD_U32(kAnchor + kAnchorFront);
            REX_STORE_U32(front + kLinkPrev, node);  // stw r11,0(r10)
          }
          // loc_82B4FAB8: the count is read before the front is stored and written after.
          const uint64_t bumped = REX_LOAD_U32(kAnchor + kAnchorCount) + 1;  // lwz r10,8(r29)
          REX_STORE_U32(kAnchor + kAnchorFront, node);                       // stw r11,4(r29)
          ctx.r3.u64 = REX_LOAD_U32(kLockSlot);                              // lwz r3,17904(r25)
          REX_STORE_U32(kAnchor + kAnchorCount, static_cast<uint32_t>(bumped));  // stw r10,8(r29)
          ctx.ctr.u64 = kRtlLeaveCriticalSection;
          ctx.lr = 0x82B4FAD0;
          REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bl 0x82f9cb54
        }
        // loc_82B4FAD0: addic. r26,r26,-1 ; addi r30,r30,28 ; bne 0x82b4fa24
        ctx.xer.ca = static_cast<uint32_t>(remaining) > 0;  // addic. sets CA; xer is not compared
        remaining = remaining - 1;
        offset = offset + kEntryStride;
      } while (static_cast<int32_t>(static_cast<uint32_t>(remaining)) != 0);
    }
    ctx.r3.s64 = 0;  // loc_82B4FADC: li r3,0
  }

  // Both exits share this epilogue: addi r1,r1,192 ; b __restgprlr_19.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.r19.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR19);   // ld r19,-112(r1)
  ctx.r20.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR20);   // ld r20,-104(r1)
  ctx.r21.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR21);   // ld r21,-96(r1)
  ctx.r22.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR22);   // ld r22,-88(r1)
  ctx.r23.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR23);   // ld r23,-80(r1)
  ctx.r24.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR24);   // ld r24,-72(r1)
  ctx.r25.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR25);   // ld r25,-64(r1)
  ctx.r26.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR26);   // ld r26,-56(r1)
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

}  // namespace port_82B4F8C8
SKATE3_PORT(82B4F8C8, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
