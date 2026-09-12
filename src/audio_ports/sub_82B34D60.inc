// sub_82B34D60  role: drain the voice list at +4 of its argument, unlinking and clearing every node, then tail-dispatch a virtual on the object at +8   lifted: skate3_recomp.67.cpp:55900 (92 lines)
// STATUS: gate-1 (indirect call: sub_82B34D60's own tail bctr through the manager at r4+36, vtable slot +12)
// calls/session: boot 244  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34D60.md
namespace port_82B34D60 {

// The node layout is the one port_82B34BD0 unlinks, field for field: same three pointer
// words, same token at +12, same owner at +16, same +20/+22 pair cleared last.
constexpr uint32_t kNodePrev = 0;
constexpr uint32_t kNodeNext = 4;
constexpr uint32_t kNodeLive = 8;
constexpr uint32_t kNodeToken = 12;  // zero means "not linked": this loop SKIPS such a node
constexpr uint32_t kNodeOwner = 16;
constexpr uint32_t kNodeShort = 20;
constexpr uint32_t kNodeCount = 22;

constexpr uint32_t kListHead = 4;     // on the argument: head of the list being drained
constexpr uint32_t kTailObject = 8;   // on the argument: what the closing virtual is handed
constexpr uint32_t kManagerOwner = 36;  // on r4: the object that owns the dispatch table
constexpr uint32_t kVtableRelease = 12; // vtable slot taken, at +12 off the table pointer

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t manager = ctx.r4.u32;

  // lwz r11,4(r3) / cmpwi cr6,r11,0 / beq cr6,0x82b34ddc
  uint32_t node = REX_LOAD_U32(self + kListHead);
  while (static_cast<int32_t>(node) != 0) {
    // lwz r10,12(r11) / cmplwi / beq cr6,0x82b34dd0 -- a node with a zero token is skipped
    // WITHOUT being unlinked, and the head is then re-read unchanged at the bottom: the
    // original spins here forever. Reproduced, bug for bug; see the note.
    if (REX_LOAD_U32(node + kNodeToken) != 0) {
      // lwz r9,16(r11) / lwz r10,4(r9) / cmplw cr6,r11,r10
      const uint32_t owner = REX_LOAD_U32(node + kNodeOwner);
      const uint32_t head = REX_LOAD_U32(owner + kListHead);
      if (node == head) {
        // lwz r10,0(r10) / stw r10,4(r9) -- advance the owner's head past this node
        REX_STORE_U32(owner + kListHead, REX_LOAD_U32(head + kNodePrev));
      }
      // lwz r10,4(r11) / cmpwi / beq -- both pointers are RELOADED per branch, as lifted
      const uint32_t next = REX_LOAD_U32(node + kNodeNext);
      if (static_cast<int32_t>(next) != 0) {
        REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));  // stw r9,0(r10)
      }
      const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
      if (static_cast<int32_t>(prev) != 0) {
        REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));  // stw r9,4(r10)
      }
      // r8 was zeroed once before the loop (li r8,0) and never touched again.
      REX_STORE_U32(node + kNodeToken, 0);  // stw r8,12(r11)
      REX_STORE_U32(node + kNodeOwner, 0);  // stw r8,16(r11)
      REX_STORE_U32(node + kNodeLive, 0);   // stw r8,8(r11)
      REX_STORE_U8(node + kNodeCount, 0);   // stb r8,22(r11)
      REX_STORE_U16(node + kNodeShort, 0);  // sth r8,20(r11)
    }
    // lwz r11,4(r3) / cmpwi / bne cr6,0x82b34d70 -- the head is re-read, not cached
    node = REX_LOAD_U32(self + kListHead);
  }

  // lwz r11,8(r3) / cmplwi cr6,r11,0 / beqlr cr6 -- nothing to release, r3 is left as it came
  const uint32_t tail_object = REX_LOAD_U32(self + kTailObject);
  if (tail_object == 0) {
    return;
  }
  // lwz r3,36(r4) / mr r4,r11 / li r5,0 / lwz r11,0(r3) / lwz r10,12(r11) / mtctr r10 / bctr
  // A TAIL branch, not a call: lr is left as this function's caller set it, so the target
  // returns straight past us and its r3 is this function's result.
  ctx.r3.u64 = REX_LOAD_U32(manager + kManagerOwner);
  ctx.r4.u64 = tail_object;
  ctx.r5.s64 = 0;
  const uint32_t vtable = REX_LOAD_U32(ctx.r3.u32 + 0);
  ctx.ctr.u64 = REX_LOAD_U32(vtable + kVtableRelease);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B34D60
SKATE3_PORT(82B34D60, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
