// sub_82B48F28  role: tear down an object -- release each non-null child of the array at +80 through its own vtable pair (slot 0 then slot 12), then, when the r4 flag's low byte is clear, unlink the node embedded at +28 from whichever of the owner's five list heads the type byte at +71 selects (1->+20, 3->+28 before sub_82B49438; 2->+16, 4->+24, 5->+32 after it, and only when it returned 0), splicing the neighbours each time; finally free the object itself through the owner's allocator   lifted: skate3_recomp.68.cpp:38064 (258 lines)
// STATUS: gate-1 (three of its own bctrl: the per-child acquire/release pair at 0x82B48F6C and 0x82B48F84, and the allocator free at 0x82B490F0 -- sub_82B48F28 indirect call)
// calls/session: boot 2152  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B48F28.md
namespace port_82B48F28 {

// Nothing in docs/rw_audio_structs.h names this object; plain offsets are honest. The list-node
// field names follow sub_82B34BD0's verified reading of the same two-word link pair: the word at
// +0 is the one a list head is replaced with, the word at +4 the one whose target's +0 is fixed
// up. Same splice shape, same head-update shape.
constexpr uint32_t kVptr = 0;         // any object +0 -> its vtable
constexpr uint32_t kAcquireSlot = 0;  // child vtable +0:  called first, with the child in r3
constexpr uint32_t kFreeSlot = 12;    // child vtable +12: called second, with (child, 0)

constexpr uint32_t kOwner = 16;       // u32 -> the owner holding the five list heads
constexpr uint32_t kNode = 28;        // the list node EMBEDDED in the object: +28 and +32
constexpr uint32_t kPrev = 0;         // node +0
constexpr uint32_t kNext = 4;         // node +4
constexpr uint32_t kChildCount = 68;  // u8, RELOADED every iteration of the child loop
constexpr uint32_t kTypeByte = 71;    // u8, selects the head; RELOADED after sub_82B49438
constexpr uint32_t kChildren = 80;    // u32[kChildCount], stride 4

// The owner's five list heads. Which one applies is the type byte, and the two groups are split
// by sub_82B49438's verdict -- see the branch structure below.
constexpr uint32_t kHeadType2 = 16;
constexpr uint32_t kHeadType1 = 20;
constexpr uint32_t kHeadType4 = 24;
constexpr uint32_t kHeadType3 = 28;
constexpr uint32_t kHeadType5 = 32;

constexpr uint32_t kAllocator = 36;  // owner+36 -> the allocator object; its vtable +12 frees

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)

// bctrl return addresses, reproduced so a callee that reads lr sees what it would see.
constexpr uint32_t kAcquireReturn = 0x82B48F70u;
constexpr uint32_t kChildFreeReturn = 0x82B48F88u;
constexpr uint32_t kObjectFreeReturn = 0x82B490F4u;

// loc_82B48FF8..loc_82B4901C, and loc_82B490AC..loc_82B490D0 -- byte for byte the same pair of
// conditional stores, so it is written once. Both reloads of the node's own words are kept: the
// original re-reads +0 and +4 after the store that could alias them.
void SpliceOut(uint8_t* base, uint32_t node) {
  const uint32_t next = REX_LOAD_U32(node + kNext);  // lwz r10,4(r11)
  // cmpwi cr6,r10,0 -- SIGNED against zero, as lifted
  if (static_cast<int32_t>(next) != 0) {
    REX_STORE_U32(next + kPrev, REX_LOAD_U32(node + kPrev));  // lwz r9,0(r11) ; stw r9,0(r10)
  }
  const uint32_t prev = REX_LOAD_U32(node + kPrev);  // lwz r10,0(r11)
  if (static_cast<int32_t>(prev) != 0) {
    REX_STORE_U32(prev + kNext, REX_LOAD_U32(node + kNext));  // lwz r11,4(r11) ; stw r11,4(r10)
  }
}

// All five head cases are this, with only the head offset differing: if the owner's head still
// points AT this object's embedded node, move the head on to the node's +0.
void DetachFromHead(uint8_t* base, uint32_t self, uint32_t head_offset) {
  const uint32_t owner = REX_LOAD_U32(self + kOwner);        // lwz r9,16(r29)
  const uint32_t head = REX_LOAD_U32(owner + head_offset);   // lwz r10,<head>(r9)
  // addi r11,r29,28 ; cmplw cr6,r11,r10 -- UNSIGNED pointer compare
  if (self + kNode == head) {
    REX_STORE_U32(owner + head_offset, REX_LOAD_U32(head + kPrev));  // lwz r10,0(r10) ; stw
  }
}

REX_FUNC(Native) {
  // The frame is reproduced because all four callees run on it. __savegprlr_28's spills of
  // r28..r31 and lr land inside these 128 bytes, so they are this call's own frame and are not
  // windowed; they are not reproduced either, because this body keeps those four values in
  // locals and never writes the guest registers, so __restgprlr_28 would have nothing to undo.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f52118
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;   // mr r29,r3
  const uint32_t flag = ctx.r4.u32;   // mr r28,r4

  // lbz r11,68(r3) ; cmplwi cr6,r11,0 ; beq cr6,0x82b48f9c
  if (REX_LOAD_U8(self + kChildCount) != 0) {
    uint32_t slot = self + kChildren;  // addi r31,r3,80
    for (int32_t i = 0;;) {            // li r30,0
      // loc_82B48F50
      const uint32_t child = REX_LOAD_U32(slot);  // lwz r11,0(r31)
      // cmplwi cr6,r11,0 ; beq cr6,0x82b48f88
      if (child != 0) {
        // rotlwi r3,r11,0 -- a zero-extending move, not an arithmetic rotate
        const uint32_t vtable = REX_LOAD_U32(child + kVptr);     // lwz r11,0(r3)
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kAcquireSlot);       // lwz r10,0(r11) ; mtctr r10
        ctx.r3.u64 = child;
        ctx.lr = kAcquireReturn;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);                     // bctrl

        // lwz r3,0(r31) -- the child pointer is RELOADED; the call above may have replaced it.
        const uint32_t child_again = REX_LOAD_U32(slot);
        const uint32_t vtable2 = REX_LOAD_U32(child_again + kVptr);  // lwz r9,0(r3)
        ctx.ctr.u64 = REX_LOAD_U32(vtable2 + kFreeSlot);             // lwz r8,12(r9) ; mtctr r8
        ctx.r3.u64 = child_again;
        ctx.r4.s64 = 0;                                              // li r4,0
        ctx.lr = kChildFreeReturn;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);                         // bctrl
      }
      // loc_82B48F88 -- the count is reloaded every iteration, after the two calls.
      const int32_t count = static_cast<int32_t>(REX_LOAD_U8(self + kChildCount));  // lbz r11,68
      i++;        // addi r30,r30,1
      slot += 4;  // addi r31,r31,4
      // cmpw cr6,r30,r11 ; blt cr6,0x82b48f50 -- SIGNED
      if (i >= count) break;
    }
  }

  // loc_82B48F9C -- clrlwi r11,r28,24 ; cmplwi cr6,r11,0 ; bne cr6,0x82b490d4
  // A non-zero flag byte skips the whole unlink half and goes straight to the object free.
  if ((flag & 0xFFu) == 0) {
    const uint32_t node = self + kNode;  // addi r11,r29,28, formed in each branch below

    // lbz r11,71(r29) ; cmplwi cr6,r11,1 ; bne cr6,0x82b48fd4 ; ... cmplwi cr6,r11,3
    const uint32_t type = REX_LOAD_U8(self + kTypeByte);
    if (type == 1) {
      DetachFromHead(base, self, kHeadType1);  // lwz r10,20(r9) ; stw r10,20(r9)
      SpliceOut(base, node);
    } else if (type == 3) {
      DetachFromHead(base, self, kHeadType3);  // lwz r10,28(r9) ; stw r10,28(r9)
      SpliceOut(base, node);
    }
    // Any other type value falls through to loc_82B49020 with no splice at all.

    // loc_82B49020 -- mr r3,r29 ; bl 0x82b49438
    skate3::audio::GuestCall(ctx, base, sub_82B49438, self);
    // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; bne cr6,0x82b490d4 -- only its LOW BYTE is tested
    if ((ctx.r3.u32 & 0xFFu) == 0) {
      // lbz r11,71(r29) -- RELOADED: sub_82B49438 runs between the two reads of the type byte.
      const uint32_t type_again = REX_LOAD_U8(self + kTypeByte);
      if (type_again == 2) {
        DetachFromHead(base, self, kHeadType2);  // lwz r10,16(r9) ; stw r10,16(r9)
        SpliceOut(base, node);
      } else if (type_again == 4) {
        DetachFromHead(base, self, kHeadType4);  // lwz r10,24(r9) ; stw r10,24(r9)
        SpliceOut(base, node);
      } else if (type_again == 5) {
        DetachFromHead(base, self, kHeadType5);  // lwz r10,32(r9) ; stw r10,32(r9)
        SpliceOut(base, node);
      }
      // 0, 1 and 3 reach loc_82B490D4 directly: the first half already handled 1 and 3.
    }
  }

  // loc_82B490D4 -- free the object itself: allocator->free(object, 0), the allocator being
  // owner+36 and the entry its vtable's +12, the same pair sub_82B32870 and sub_82B34D60 use.
  const uint32_t owner = REX_LOAD_U32(self + kOwner);            // lwz r11,16(r29)
  const uint32_t allocator = REX_LOAD_U32(owner + kAllocator);   // lwz r3,36(r11)
  const uint32_t vtable = REX_LOAD_U32(allocator + kVptr);       // lwz r10,0(r3)
  ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);                // lwz r9,12(r10) ; mtctr r9
  ctx.r3.u64 = allocator;
  ctx.r4.u64 = self;  // mr r4,r29
  ctx.r5.s64 = 0;     // li r5,0
  ctx.lr = kObjectFreeReturn;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);                           // bctrl

  // No `li r3,...` on any path: r3 is whatever the allocator's free entry left.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,128
  ctx.lr = entry_lr;                      // b 0x82f52168 (__restgprlr_28)
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B48F28
SKATE3_PORT(82B48F28, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
