// sub_82B4FAF8  role: under the global audio critical section, walk the object's array of 28-byte entries at +52 (count at +68), and for each one take the intrusive list node embedded 20 bytes into the pointer in that entry, unlink it from whatever list it is in (popping the spare head at 0x830BDEE0 if it is that node), and push it onto the front of the global list anchored at 0x830BDEE4, bumping the count at 0x830BDEEC   lifted: skate3_recomp.68.cpp:54664 (147 lines)
// STATUS: gate-1 (RtlEnterCriticalSection / RtlLeaveCriticalSection: it takes the lock itself, census depth 0)
// calls/session: boot 1917  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B4FAF8.md
namespace port_82B4FAF8 {

// Constant addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye.
// lis r30,-31992 -> 0x83080000 ; lwz r3,17904(r30)
constexpr uint32_t kLockSlot = 0x830845F0;  // holds the RTL_CRITICAL_SECTION*, reloaded per use
// lis r6,-31988 -> 0x830C0000 ; lwz r9,-8480(r6)
constexpr uint32_t kSpareHead = 0x830BDEE0;  // a pointer this walk pops if it names the node
// lis r11,-31988 ; addi r10,r11,-8476 -> the anchor r10 addresses, four bytes above kSpareHead
constexpr uint32_t kAnchor = 0x830BDEE4;

// The anchor's three words, named for what this function does with them. +0 is written only while
// it is null, so it holds the first node ever pushed -- the far end of the chain; +4 always takes
// the newest node. That reading comes from this one site.
constexpr uint32_t kAnchorEnd = 0;     // lwz/stw 0(r10)
constexpr uint32_t kAnchorFront = 4;   // lwz/stw 4(r10)
constexpr uint32_t kAnchorCount = 8;   // lwz/stw 8(r10)

// The intrusive node, 20 bytes into the object each array entry points at.
constexpr uint32_t kNodeOffset = 20;   // addi r11,r11,20
constexpr uint32_t kLinkPrev = 0;      // the direction the spare head is popped through
constexpr uint32_t kLinkNext = 4;

// The object (r3/r31).
constexpr uint32_t kEntryArray = 52;   // lwz r11,52(r31) -> base of the entry array
constexpr uint32_t kEntryCount = 68;   // u32, RE-READ at the bottom of every iteration
constexpr uint32_t kEntryStride = 28;  // addi r8,r8,28

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)

// The two kernel imports the `bl`s reach. probe/ports/lint.py check 6 forbids naming an import
// thunk directly from a port body -- that rule exists so a guest callee goes through GuestCall and
// is exercised by its own hook, which a kernel import has no way to be -- so both are reached
// through their guest addresses in the dispatch table, which is the same code the `bl` enters.
// The one difference from the original is that this writes ctr, which the `bl` does not; ctr is
// volatile and outside the compared set, and this body never runs anyway (gate-1).
constexpr uint32_t kRtlEnterCriticalSection = 0x82F9CB44;  // bl 0x82f9cb44
constexpr uint32_t kRtlLeaveCriticalSection = 0x82F9CB54;  // bl 0x82f9cb54

REX_FUNC(Native) {
  // The prologue spills r12 (the link register), r30 and r31 into this call's own frame and the
  // epilogue reloads all three. The harness compares r13-r31, so the entry values have to survive
  // here; the spill slots are inside the 112 bytes the stwu claims, so they are not windowed and
  // are not reproduced.
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-112(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;  // mr r31,r3

  // lwz r3,17904(r30) ; bl 0x82f9cb44 -- the lock POINTER is loaded from the slot, not the slot
  // address; it is loaded again for the release, not kept across the walk.
  ctx.r3.u64 = REX_LOAD_U32(kLockSlot);
  ctx.ctr.u64 = kRtlEnterCriticalSection;
  ctx.lr = 0x82B4FB1C;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  // lwz r11,68(r31) ; cmplwi cr6,r11,0 ; ble cr6,0x82b4fbe0 -- an UNSIGNED compare against zero,
  // so `ble` can only mean equal: any nonzero count enters the loop.
  if (REX_LOAD_U32(object + kEntryCount) != 0) {
    uint64_t seen = 0;    // li r5,0 ; mr r7,r5
    uint64_t offset = 0;  // mr r8,r5 -- the byte offset into the entry array
    uint32_t count;
    do {
      // loc_82B4FB40
      const uint32_t array = REX_LOAD_U32(object + kEntryArray);  // lwz r11,52(r31)
      const uint32_t spare = REX_LOAD_U32(kSpareHead);            // lwz r9,-8480(r6)
      // lwzx r11,r11,r8 -- a 32-bit wrapping sum of two zero-extended halves, then addi +20 in
      // 64 bits; only the low word is ever used to address, exactly as the lifted form does.
      const uint64_t entry = REX_LOAD_U32(uint32_t(array + uint32_t(offset)));
      uint32_t node = uint32_t(entry + kNodeOffset);
      // cmplw cr6,r11,r9 ; bne cr6,0x82b4fb60 -- when the node IS the spare head, advance that
      // head through the prev link before unlinking.
      if (node == spare) {
        REX_STORE_U32(kSpareHead, REX_LOAD_U32(spare + kLinkPrev));  // lwz r9,0(r9) ; stw r9,-8480(r6)
      }

      // loc_82B4FB60 -- the unlink, both sides guarded separately.
      const uint32_t next = REX_LOAD_U32(node + kLinkNext);  // lwz r9,4(r11)
      // cmpwi cr6,r9,0 ; beq cr6,0x82b4fb74
      if (next != 0) {
        REX_STORE_U32(next + kLinkPrev, REX_LOAD_U32(node + kLinkPrev));  // lwz r4,0(r11) ; stw r4,0(r9)
      }
      // loc_82B4FB74 -- the prev link is RELOADED, not carried from the store above.
      const uint32_t prev = REX_LOAD_U32(node + kLinkPrev);  // lwz r9,0(r11)
      // cmpwi cr6,r9,0 ; beq cr6,0x82b4fb88
      if (prev != 0) {
        // lwz r11,4(r11) ; stw r11,4(r9) -- this clobbers r11, but loc_82B4FB88 rebuilds the
        // node address from scratch, so the clobber is dead.
        REX_STORE_U32(prev + kLinkNext, REX_LOAD_U32(node + kLinkNext));
      }

      // loc_82B4FB88 -- the array base, the entry and the +20 are all recomputed here rather than
      // reused, and the recompute happens after stores that could alias the entry array.
      const uint32_t array_again = REX_LOAD_U32(object + kEntryArray);  // lwz r11,52(r31)
      const uint64_t entry_again = REX_LOAD_U32(uint32_t(array_again + uint32_t(offset)));
      node = uint32_t(entry_again + kNodeOffset);

      REX_STORE_U32(node + kLinkPrev, 0);                                  // stw r5,0(r11)
      REX_STORE_U32(node + kLinkNext, REX_LOAD_U32(kAnchor + kAnchorFront));  // lwz r9,4(r10) ; stw r9,4(r11)
      // lwz r9,0(r10) ; cmplwi cr6,r9,0 ; bne cr6,0x82b4fbb4
      if (REX_LOAD_U32(kAnchor + kAnchorEnd) == 0) {
        REX_STORE_U32(kAnchor + kAnchorEnd, node);  // stw r11,0(r10)
      } else {
        // loc_82B4FBB4 -- the front pointer is RELOADED here; it is still the old front, because
        // the store that replaces it is four instructions further down.
        const uint32_t front = REX_LOAD_U32(kAnchor + kAnchorFront);  // lwz r9,4(r10)
        REX_STORE_U32(front + kLinkPrev, node);                       // stw r11,0(r9)
      }

      // loc_82B4FBBC -- the count is read before the front pointer is stored, and written after.
      const uint64_t bumped = REX_LOAD_U32(kAnchor + kAnchorCount) + 1;  // lwz r9,8(r10) ; addi r9,r9,1
      seen += 1;                                                         // addi r7,r7,1
      REX_STORE_U32(kAnchor + kAnchorFront, node);                       // stw r11,4(r10)
      offset += kEntryStride;                                            // addi r8,r8,28
      REX_STORE_U32(kAnchor + kAnchorCount, uint32_t(bumped));           // stw r9,8(r10)
      count = REX_LOAD_U32(object + kEntryCount);                        // lwz r11,68(r31)
      // cmplw cr6,r7,r11 ; blt cr6,0x82b4fb40 -- UNSIGNED, and the bound is re-read every pass.
    } while (uint32_t(seen) < count);
  }

  // loc_82B4FBE0
  ctx.r3.u64 = REX_LOAD_U32(kLockSlot);  // lwz r3,17904(r30)
  ctx.ctr.u64 = kRtlLeaveCriticalSection;
  ctx.lr = 0x82B4FBE8;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bl 0x82f9cb54

  // No `li r3,...` on any path: r3 is whatever the release import left.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112
  ctx.r12.u64 = entry_lr;                 // lwz r12,-8(r1)
  ctx.lr = entry_lr;                      // mtlr r12
  ctx.r30 = saved_r30;                    // ld r30,-24(r1)
  ctx.r31 = saved_r31;                    // ld r31,-16(r1)
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B4FAF8
SKATE3_PORT(82B4FAF8, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
