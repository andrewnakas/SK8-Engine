// sub_828E2E08  role: unlink a node from the list whose cursor is at container+12, decrement the count at +4, and when it reaches zero free the container through the global allocator at 0x83083CCC (vtable slot +12); returns 0   lifted: skate3_recomp.47.cpp:4567 (89 lines)
// STATUS: gate-1 (its own bctrl: the free entry of the vtable at u32[u32[0x83083CCC]+0], `bctrl` at 0x828E2E88)
// calls/session: boot 154  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_828E2E08.md
namespace port_828E2E08 {

// ((-31992 & 0xFFFF) << 16) + 15564 = 0x83080000 + 0x3CCC. Computed, never read off by eye.
constexpr uint32_t kAllocatorPtr = 0x83080000u + 15564u;  // lis r11,-31992 ; lwz r11,15564(r11)
constexpr uint32_t kFreeSlot = 12;  // lwz r9,12(r10) -- the allocator vtable's free entry

constexpr uint32_t kCursor = 12;  // lwz r11,12(r3) / stw r11,12(r3) -- this list's cursor
constexpr uint32_t kCount = 4;    // lwz r11,4(r3) / stw r11,4(r3)
constexpr uint32_t kNext = 0;     // the node's forward link
constexpr uint32_t kPrev = 4;     // the node's backward link

REX_FUNC(Native) {
  const uint32_t container = ctx.r3.u32;
  const uint32_t node = ctx.r4.u32;

  // lwz r11,12(r3) ; cmplw cr6,r4,r11 ; bne cr6,0x828e2e28
  // Removing the node the cursor points at moves the cursor to its successor first.
  const uint32_t cursor = REX_LOAD_U32(container + kCursor);
  if (node == cursor) {
    // lwz r11,0(r11) ; stw r11,12(r3)
    REX_STORE_U32(container + kCursor, REX_LOAD_U32(cursor + kNext));
  }

  // loc_828E2E28: lwz r11,4(r4) ; cmpwi ; beq -- prev->next = next
  const uint32_t prev = REX_LOAD_U32(node + kPrev);
  if (prev != 0) {
    REX_STORE_U32(prev + kNext, REX_LOAD_U32(node + kNext));  // lwz r10,0(r4) ; stw r10,0(r11)
  }

  // loc_828E2E3C: lwz r11,0(r4) ; cmpwi ; beq -- next->prev = prev. Both links are re-read from
  // the node, not cached across the store above, as lifted. The node's own links are left alone.
  const uint32_t next = REX_LOAD_U32(node + kNext);
  if (next != 0) {
    REX_STORE_U32(next + kPrev, REX_LOAD_U32(node + kPrev));  // lwz r10,4(r4) ; stw r10,4(r11)
  }

  // loc_828E2E50: lwz r11,4(r3) ; addic. r11,r11,-1 ; stw r11,4(r3)
  // The decrement is 64-bit on a zero-extended load, so a count already at 0 becomes
  // 0xFFFFFFFFFFFFFFFF: the word stores 0xFFFFFFFF and cr0 does NOT see zero, so the free is
  // skipped and the count keeps wrapping down. Reproduced, not fixed.
  const uint32_t before = REX_LOAD_U32(container + kCount);
  const uint64_t remaining = static_cast<uint64_t>(before) - 1u;
  ctx.xer.ca = before > 0;  // addic. sets CA; xer is not compared, kept for fidelity
  REX_STORE_U32(container + kCount, static_cast<uint32_t>(remaining));

  // bne 0x828e2e8c -- cr0 compares the LOW word of the 64-bit result
  if (static_cast<int32_t>(static_cast<uint32_t>(remaining)) == 0) {
    const uint32_t allocator = REX_LOAD_U32(kAllocatorPtr);  // lwz r11,15564(r11)
    // cmplwi cr6,r11,0 ; beq cr6,0x828e2e8c -- no allocator installed, no free
    if (allocator != 0) {
      const uint32_t vtable = REX_LOAD_U32(allocator + 0);  // lwz r10,0(r11)
      ctx.r4.u64 = container;                               // mr r4,r3
      ctx.r5.s64 = 0;                                       // li r5,0
      ctx.r3.u64 = allocator;                               // mr r3,r11
      ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);       // lwz r9,12(r10) ; mtctr r9
      ctx.lr = 0x828E2E8C;                                  // bctrl
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    }
  }

  // loc_828E2E8C
  ctx.r3.s64 = 0;  // li r3,0 -- the only result, on every path
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_828E2E08
SKATE3_PORT(828E2E08, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
