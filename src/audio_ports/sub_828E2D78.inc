// sub_828E2D78  role: validate a handle against its owner's +8 generation, then unlink a node   lifted: skate3_recomp.47.cpp:4485 (81 lines)
// STATUS: verified
// calls/session: boot 22  play 45  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_828E2D78.md
namespace port_828E2D78 {

// Identical in shape to sub_828E30B8, which checks the owner's generation at +12 instead of +8.
constexpr uint32_t kOwner = 0;
constexpr uint32_t kGeneration = 4;
constexpr uint32_t kOwnerGeneration = 8;
constexpr uint32_t kNodePrev = 0;
constexpr uint32_t kNodeNext = 4;
constexpr int32_t kErrorNoOwner = -6;
constexpr int32_t kErrorStale = -3;

REX_FUNC(Native) {
  const uint32_t handle = ctx.r3.u32;
  const int32_t generation = static_cast<int32_t>(REX_LOAD_U32(handle + kGeneration));
  if (generation < 0) {
    ctx.r3.s64 = generation;
    return;
  }
  const uint32_t owner = REX_LOAD_U32(handle + kOwner);
  if (owner == 0) {
    ctx.r3.s64 = kErrorNoOwner;
    return;
  }
  if (generation != static_cast<int32_t>(REX_LOAD_U32(owner + kOwnerGeneration))) {
    REX_STORE_U32(handle + kGeneration, static_cast<uint32_t>(kErrorStale));  // stw r9,4(r11)
    REX_STORE_U32(handle + kOwner, 0);                                        // stw r10,0(r11)
    ctx.r3.s64 = kErrorStale;
    return;
  }
  const uint32_t node = ctx.r4.u32;
  const uint32_t head = REX_LOAD_U32(owner + kNodePrev);
  if (node == head) {
    REX_STORE_U32(owner + kNodePrev, REX_LOAD_U32(head + kNodePrev));
  }
  const uint32_t next = REX_LOAD_U32(node + kNodeNext);
  if (static_cast<int32_t>(next) != 0) {
    REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));
  }
  const uint32_t prev = REX_LOAD_U32(node + kNodePrev);
  if (static_cast<int32_t>(prev) != 0) {
    REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));
  }
  ctx.r3.s64 = 0;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t handle = ctx.r3.u32;
  const int32_t generation = static_cast<int32_t>(REX_LOAD_U32(handle + kGeneration));
  spec.read(handle, 8);
  if (generation < 0) return true;
  const uint32_t owner = REX_LOAD_U32(handle + kOwner);
  if (owner == 0) return true;
  spec.read(owner + kOwnerGeneration, 4);
  if (generation != static_cast<int32_t>(REX_LOAD_U32(owner + kOwnerGeneration))) {
    spec.write(handle + kOwner, 8);
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

}  // namespace port_828E2D78
SKATE3_PORT(828E2D78, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
