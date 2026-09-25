// sub_82B39690  role: unlink a node from its list and push it onto the free list   lifted: skate3_recomp.67.cpp:66886 (82 lines)
// STATUS: verified
// calls/session: boot 3994  play 4428  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B39690.md
namespace port_82B39690 {

// The node: {prev at +0, next at +4, which-list byte at +12}. The manager: {free head at +12,
// list A head at +16, list B head at +20, count at +24}. Names describe the arithmetic only.
constexpr uint32_t kNodePrev = 0;
constexpr uint32_t kNodeNext = 4;
constexpr uint32_t kNodeWhich = 12;
constexpr uint32_t kFreeHead = 12;
constexpr uint32_t kHeadA = 16;
constexpr uint32_t kHeadB = 20;
constexpr uint32_t kCount = 24;

REX_FUNC(Native) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t node = ctx.r4.u32;
  // lbz r11,12(r4) selects which list head can name this node.
  const uint32_t which = REX_LOAD_U8(node + kNodeWhich);
  const uint32_t head_field = which != 0 ? kHeadA : kHeadB;
  const uint32_t head = REX_LOAD_U32(manager + head_field);
  if (node == head) {
    REX_STORE_U32(manager + head_field, REX_LOAD_U32(head + kNodePrev));  // stw r11,16/20(r3)
  }
  // Unlink: the next node's prev, then the prev node's next. Both values are re-read from the
  // node, so the reads stay in the original order relative to the stores.
  const uint32_t next = REX_LOAD_U32(node + kNodeNext);
  if (static_cast<int32_t>(next) != 0) {
    REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));  // stw r10,0(r11)
  }
  const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
  if (static_cast<int32_t>(prev) != 0) {
    REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));  // stw r10,4(r11)
  }
  // Push onto the free list, then re-read the free head exactly as the original does.
  const uint32_t free_head = REX_LOAD_U32(manager + kFreeHead);
  REX_STORE_U32(node + kNodeNext, 0);          // stw r10,4(r4)
  REX_STORE_U32(node + kNodePrev, free_head);  // stw r11,0(r4)
  const uint32_t free_again = REX_LOAD_U32(manager + kFreeHead);  // lwz r11,12(r3) again
  if (free_again != 0) {
    REX_STORE_U32(free_again + kNodeNext, node);  // stw r4,4(r11)
  }
  REX_STORE_U32(manager + kFreeHead, node);                            // stw r4,12(r3)
  REX_STORE_U32(manager + kCount, REX_LOAD_U32(manager + kCount) - 1);  // addi r11,r11,-1
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t node = ctx.r4.u32;
  // The free head is re-read after the node's two words are written. If it lives inside them,
  // the second read returns a value produced during the call and the back-link address is not
  // derivable from entry state -- gate 2. Predicted, not assumed away.
  const uint32_t free_field = manager + kFreeHead;
  if (free_field + 3 >= node + kNodePrev && free_field <= node + kNodeNext + 3) return false;
  const uint32_t which = REX_LOAD_U8(node + kNodeWhich);
  const uint32_t head_field = which != 0 ? kHeadA : kHeadB;
  const uint32_t head = REX_LOAD_U32(manager + head_field);
  if (node == head) spec.write(manager + head_field, 4);
  const uint32_t next = REX_LOAD_U32(node + kNodeNext);
  const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
  if (static_cast<int32_t>(next) != 0) spec.write(next + kNodePrev, 4);
  if (static_cast<int32_t>(prev) != 0) spec.write(prev + kNodeNext, 4);
  spec.write(node + kNodePrev, 8);
  const uint32_t free_head = REX_LOAD_U32(free_field);
  if (free_head != 0) spec.write(free_head + kNodeNext, 4);
  spec.write(manager + kFreeHead, 4);
  spec.write(manager + kCount, 4);
  spec.read(manager + kFreeHead, 16);
  spec.read(node, 16);
  if (node == head && head != 0) spec.read(head + kNodePrev, 4);
  return true;
}

}  // namespace port_82B39690
SKATE3_PORT(82B39690, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
