// sub_828E30B8  role: validate a handle, then unlink a node from its list   lifted: skate3_recomp.47.cpp:4965 (81 lines)
// STATUS: verified
// calls/session: boot 143  play 115  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_828E30B8.md
namespace port_828E30B8 {

constexpr uint32_t kOwner = 0;     // the object the handle refers to
constexpr uint32_t kGeneration = 4;  // negative means already invalid
constexpr uint32_t kOwnerGeneration = 12;
constexpr uint32_t kNodePrev = 0;
constexpr uint32_t kNodeNext = 4;
constexpr int32_t kErrorNoOwner = -6;
constexpr int32_t kErrorStale = -3;

REX_FUNC(Native) {
  const uint32_t handle = ctx.r3.u32;
  const int32_t generation = static_cast<int32_t>(REX_LOAD_U32(handle + kGeneration));
  if (generation < 0) {
    ctx.r3.s64 = generation;  // already invalid: returned unchanged, sign-extended
    return;
  }
  const uint32_t owner = REX_LOAD_U32(handle + kOwner);
  if (owner == 0) {
    ctx.r3.s64 = kErrorNoOwner;  // li r9,-6 ; mr r3,r9
    return;
  }
  if (generation != static_cast<int32_t>(REX_LOAD_U32(owner + kOwnerGeneration))) {
    // Stale handle: mark it invalid and report. Store order is +4 then +0, as lifted.
    REX_STORE_U32(handle + kGeneration, static_cast<uint32_t>(kErrorStale));
    REX_STORE_U32(handle + kOwner, 0);
    ctx.r3.s64 = kErrorStale;
    return;
  }
  // The handle is live: unlink ctx.r4's node from the owner's list.
  const uint32_t node = ctx.r4.u32;
  const uint32_t head = REX_LOAD_U32(owner + kNodePrev);  // lwz r11,0(r10)
  if (node == head) {
    REX_STORE_U32(owner + kNodePrev, REX_LOAD_U32(head + kNodePrev));  // stw r11,0(r10)
  }
  const uint32_t next = REX_LOAD_U32(node + kNodeNext);
  if (static_cast<int32_t>(next) != 0) {
    REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));  // stw r10,0(r11)
  }
  const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
  if (static_cast<int32_t>(prev) != 0) {
    REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));  // stw r10,4(r11)
  }
  ctx.r3.s64 = 0;  // li r3,0 -- success
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t handle = ctx.r3.u32;
  const int32_t generation = static_cast<int32_t>(REX_LOAD_U32(handle + kGeneration));
  spec.read(handle, 8);
  if (generation < 0) return true;  // writes nothing
  const uint32_t owner = REX_LOAD_U32(handle + kOwner);
  if (owner == 0) return true;      // writes nothing
  spec.read(owner + kOwnerGeneration, 4);
  if (generation != static_cast<int32_t>(REX_LOAD_U32(owner + kOwnerGeneration))) {
    spec.write(handle + kOwner, 8);  // the stale path writes both words of the handle
    return true;
  }
  const uint32_t node = ctx.r4.u32;
  const uint32_t head = REX_LOAD_U32(owner + kNodePrev);
  const uint32_t next = REX_LOAD_U32(node + kNodeNext);
  const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
  if (node == head) {
    spec.write(owner + kNodePrev, 4);
    if (head != 0) spec.read(head + kNodePrev, 4);
  }
  if (static_cast<int32_t>(next) != 0) spec.write(next + kNodePrev, 4);
  if (static_cast<int32_t>(prev) != 0) spec.write(prev + kNodeNext, 4);
  spec.read(owner + kNodePrev, 4);
  spec.read(node, 8);
  return true;
}

}  // namespace port_828E30B8
SKATE3_PORT(828E30B8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
