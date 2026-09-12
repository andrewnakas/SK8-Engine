// sub_82B1C150  role: unlink a registration from both of its lists -- its owner's chain at owner+56 and a global chain at 0x83036F4C -- then hand the record to sub_82B1BF98   lifted: skate3_recomp.66.cpp:63806 (108 lines)
// STATUS: gate-1 (indirect call: sub_82B1BF98 at depth 1 has two bctrls, and its closure leaves the audio corpus into 82EDFEC0/82EE6260/82EE7880/82EEEA98)
// calls/session: boot 64666  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C150.md
namespace port_82B1C150 {

// Addresses computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
// lis r9,-31997 -> 0x83030000; +28492 -> 0x83036F4C.
constexpr uint32_t kGlobalHead = 0x83036F4C;

constexpr uint32_t kOwner = 0;       // self +0: the object whose chain the node sits in
constexpr uint32_t kNode = 4;        // self +4: the node, two link pairs wide
constexpr uint32_t kLinkedFlag = 12; // self +12: zero means nothing to do at all
constexpr uint32_t kOwnerHead = 56;  // owner +56: head of the per-owner chain
constexpr uint32_t kLinkPrev = 0;    // within a link pair -- same shape as port_82B34BD0
constexpr uint32_t kLinkNext = 4;
constexpr uint32_t kSecondLink = 8;  // node +8: the second pair, for the global chain

// The four-store unlink both chains use, identical in both and identical to the one in
// port_82B34BD0: each side is patched only when it exists, and both pointers are RELOADED
// inside their branch rather than cached across the store that may alias them.
void UnlinkPair(uint8_t* base, uint32_t link) {
  const uint32_t next = REX_LOAD_U32(link + kLinkNext);  // lwz r10,4(r11)
  if (static_cast<int32_t>(next) != 0) {
    REX_STORE_U32(next + kLinkPrev, REX_LOAD_U32(link + kLinkPrev));  // stw r9,0(r10)
  }
  const uint32_t prev = REX_LOAD_U32(link + kLinkPrev);  // lwz r10,0(r11)
  if (static_cast<int32_t>(prev) != 0) {
    REX_STORE_U32(prev + kLinkNext, REX_LOAD_U32(link + kLinkNext));  // stw r11,4(r10)
  }
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  const uint64_t entry_lr = ctx.lr;  // mflr r12 / stw r12,-8(r1)

  // lwz r11,12(r3) / cmpwi cr6,r11,0 / beq cr6,0x82b1c1f8
  if (static_cast<int32_t>(REX_LOAD_U32(self + kLinkedFlag)) != 0) {
    // lwz r9,0(r3) / lwz r11,4(r3) / lwz r10,56(r9) / cmplw cr6,r11,r10
    const uint32_t owner = REX_LOAD_U32(self + kOwner);
    const uint32_t node = REX_LOAD_U32(self + kNode);
    const uint32_t head = REX_LOAD_U32(owner + kOwnerHead);
    if (node == head) {
      // lwz r10,0(r10) / stw r10,56(r9) -- advance the owner's head past this node
      REX_STORE_U32(owner + kOwnerHead, REX_LOAD_U32(head + kLinkPrev));
    }
    UnlinkPair(base, node);

    // lwz r11,4(r3) / addi r11,r11,8 -- the node pointer is RE-READ, not reused: the unlink
    // above stores through pointers that could in principle alias 4(r3).
    // The addi is 64-bit, but every use of the result (cmplw, the loads, the stores) is
    // 32-bit, so the low word is all that can be observed.
    const uint32_t second = REX_LOAD_U32(self + kNode) + kSecondLink;
    // lwz r10,28492(r9) / cmplw cr6,r11,r10
    const uint32_t global_head = REX_LOAD_U32(kGlobalHead);
    if (second == global_head) {
      // lwz r10,0(r10) / stw r10,28492(r9)
      REX_STORE_U32(kGlobalHead, REX_LOAD_U32(global_head + kLinkPrev));
    }
    UnlinkPair(base, second);

    // bl 0x82b1bf98 -- r3 is still the entry record; no argument register is set up, so
    // GuestCall is called with no arguments to leave r3..r10 exactly as they arrived.
    ctx.lr = 0x82B1C1F8;
    skate3::audio::GuestCall(ctx, base, sub_82B1BF98);
  }
  ctx.r3.s64 = 0;     // li r3,0 -- the callee's result is discarded
  ctx.lr = entry_lr;  // lwz r12,-8(r1) / mtlr r12
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1C150
SKATE3_PORT(82B1C150, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
