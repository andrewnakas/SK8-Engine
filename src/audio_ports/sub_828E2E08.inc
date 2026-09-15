// sub_828E2E08  role: unlink a node from the list whose cursor is at container+12, decrement the count at +4, and when it reaches zero free the container through the global allocator at 0x83083CCC (vtable slot +12); returns 0   lifted: skate3_recomp.47.cpp:4567 (89 lines)
// STATUS: partial -- compared 164, skipped 0, zero divergence (session g1_splits, 150 s scripted); substantive -- the unlink compares; no call was the container's last. NEVER promoted: the skipped path has a native body but no comparison. (comparable on every call that is not the container's last -- the free at 0x828E2E88 fires only when u32[container+4] == 1 and an allocator is installed, both readable at entry, so the whole unlink compares on the other calls)
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

// Two 4-byte guest accesses that share a byte. `stw`/`lwz` need no alignment on this target, so
// equal addresses are not the only overlap; 64-bit arithmetic so the compare cannot wrap.
inline bool Overlaps(uint32_t a, uint32_t b) {
  return static_cast<uint64_t>(a) + 4 > b && static_cast<uint64_t>(b) + 4 > a;
}

// Split by path. The free is the only unreplayable thing in this function and it fires only when
// the decrement lands on zero AND an allocator is installed -- `u32[container+4] == 1` and
// `u32[0x83083CCC] != 0`, both readable before the call. Every other call is a four-word unlink,
// which is the part carrying the aliasing subtleties and the part worth comparing.
//
// The prediction is only sound if no store this function makes can move a word read after it, so
// each store address is checked against the later reads first. A list corrupted into one of those
// shapes returns false rather than guessing; that costs a comparison, where a wrong window costs
// correctness.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t container = ctx.r3.u32;
  const uint32_t node = ctx.r4.u32;
  const uint32_t cursor_ea = container + kCursor;
  const uint32_t count_ea = container + kCount;

  // stw r11,12(r3) happens before prev, next, the count and the allocator cell are read.
  const bool moves_cursor = (node == REX_LOAD_U32(cursor_ea));
  if (moves_cursor &&
      (Overlaps(cursor_ea, node + kNext) || Overlaps(cursor_ea, node + kPrev) ||
       Overlaps(cursor_ea, count_ea) || Overlaps(cursor_ea, kAllocatorPtr))) {
    return false;
  }
  const uint32_t prev = REX_LOAD_U32(node + kPrev);
  const uint32_t next = REX_LOAD_U32(node + kNext);
  // stw r10,0(r11) happens before next, the count and the allocator cell are read. Writing the
  // node's own `next` through a self-link stores the value already there, so that case is safe.
  if (prev != 0 && ((prev != node && Overlaps(prev + kNext, node + kNext)) ||
                    Overlaps(prev + kNext, count_ea) || Overlaps(prev + kNext, kAllocatorPtr))) {
    return false;
  }
  // stw r10,4(r11) happens before the count and the allocator cell are read.
  if (next != 0 &&
      (Overlaps(next + kPrev, count_ea) || Overlaps(next + kPrev, kAllocatorPtr))) {
    return false;
  }

  // addic. r11,r11,-1 ; bne -- the last registration frees the container through vtable slot +12
  // of the allocator at 0x83083CCC. Replaying that frees it twice, and no window rewinds an
  // allocator's book-keeping.
  if (REX_LOAD_U32(count_ea) == 1 && REX_LOAD_U32(kAllocatorPtr) != 0) return false;

  if (moves_cursor) spec.write(cursor_ea, 4);  // stw r11,12(r3)
  if (prev != 0) spec.write(prev + kNext, 4);  // stw r10,0(r11)
  if (next != 0) spec.write(next + kPrev, 4);  // stw r10,4(r11)
  spec.write(count_ea, 4);                     // stw r11,4(r3)
  spec.read(count_ea, 12);                     // +4 the count, +12 the cursor
  spec.read(node + kNext, 8);                  // the node's two links
  spec.read(kAllocatorPtr, 4);
  return true;
}

}  // namespace port_828E2E08
SKATE3_PORT(828E2E08, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
