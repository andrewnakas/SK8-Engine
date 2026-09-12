// sub_82B31680  role: re-point a mix link -- detach the target's node from whatever list it is on, then push it on the source's list and copy the source's format fields into it   lifted: skate3_recomp.67.cpp:47854 (89 lines)
// STATUS: verified
// calls/session: boot 3248  play 3773  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B31680.md
namespace port_82B31680 {

// r3 = the link record. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kLinkTarget = 4;   // lwz r31,4(r3)  -- the object holding the mix node
constexpr uint32_t kLinkSource = 12;  // lwz r10,12(r30) -- the object owning the list to join

// The mix node embedded in the target at +56. The same layout sub_82B34BD0 and sub_82B31480's
// inline unlink walk: two links, a mirrored value, the owner's item, the owner, a u16 and a u8.
constexpr uint32_t kNode = 56;          // addi r11,r31,56
constexpr uint32_t kNodeNext = 0;
constexpr uint32_t kNodePrev = 4;
constexpr uint32_t kNodeValue = 8;      // mirrors owner+8
constexpr uint32_t kNodeItem = 12;      // the owner's item; non-zero means "this node is linked"
constexpr uint32_t kNodeOwner = 16;
constexpr uint32_t kNodeCount = 20;     // u16
constexpr uint32_t kNodeChannels = 22;  // u8 -- also selects sub_82B31480's two paths
constexpr uint32_t kNodeBytes = 23;     // +0..+22 inclusive: everything this call rewrites
constexpr uint32_t kNodeTail = 15;      // +8..+22: the part sub_82B31480's unlink clears

// The list owner inside the source, at +52. `addi r9,r10,52` is what lands in the node's owner
// field, and sub_82B34BD0 reads exactly these offsets off it.
constexpr uint32_t kOwner = 52;
constexpr uint32_t kOwnerItem = 0;   // lwz r8,52(r10)
constexpr uint32_t kOwnerHead = 4;   // lwz r8,56(r10) -- the list head
constexpr uint32_t kOwnerValue = 8;  // lwz r6,60(r10)
constexpr uint32_t kOwnerCount = 44; // lhz r5,96(r10)

constexpr uint32_t kItemChannels = 42;  // lbz r3,42(r4) -- the same +42 count sub_82B27E20 reads

// stwu r1,-112(r1). Reproduced, unlike most ports' frames: sub_82B31480 spills r12/r30/r31 at
// -8/-16/-24 off the CALLER's r1 before its own stwu, so without the move its frame lands on top
// of where this function's spills went and the out-of-window footprint stops matching.
constexpr uint32_t kFrameSize = 112;

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t link = ctx.r3.u32;                          // mr r30,r3
  const uint32_t target = REX_LOAD_U32(link + kLinkTarget);  // lwz r31,4(r3)
  // The original spills r30 and r31 into its red zone and restores them on the way out, so its
  // exit values ARE its entry values; the harness compares r13-r31.
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister entry_r1 = ctx.r1;

  // stwu r1,-112(r1): the back chain, with the same value the original writes, then r1 moves.
  const uint32_t frame = entry_r1.u32 - kFrameSize;
  REX_STORE_U32(frame, entry_r1.u32);
  ctx.r1.u32 = frame;

  // mr r3,r31 ; bl 0x82b31480 -- detach the node. On the branch this port compares
  // (u8[node+22] == 0) that is an inline unlink with no further calls of its own.
  skate3::audio::GuestCall(ctx, base, sub_82B31480, uint64_t(target));

  const uint32_t source = REX_LOAD_U32(link + kLinkSource);  // lwz r10,12(r30)
  // cmplwi cr6,r10,0 ; beq 0x82b31704 -- a link with no source just leaves the node detached.
  if (source != 0) {
    const uint32_t node = target + kNode;    // addi r11,r31,56
    const uint32_t owner = source + kOwner;  // addi r9,r10,52

    REX_STORE_U32(node + kNodeItem, REX_LOAD_U32(owner + kOwnerItem));    // lwz r8,52 ; stw 68(r31)
    REX_STORE_U32(node + kNodeValue, REX_LOAD_U32(owner + kOwnerValue));  // lwz r6,60 ; stw 64(r31)
    REX_STORE_U16(node + kNodeCount,
                  uint16_t(REX_LOAD_U16(owner + kOwnerCount)));  // lhz r5,96 ; sth r5,76(r31)
    // lwz r4,52(r10) -- the item pointer is loaded a SECOND time rather than reused, so it is
    // reloaded here too.
    const uint32_t item = REX_LOAD_U32(owner + kOwnerItem);
    REX_STORE_U8(node + kNodeChannels,
                 uint8_t(REX_LOAD_U8(item + kItemChannels)));  // lbz r3,42(r4) ; stb r3,78(r31)
    REX_STORE_U32(node + kNodeOwner, owner);                                // stw r9,72(r31)

    // Head insert. The head is read, parked as the node's next, the node's prev cleared, and then
    // the head is read AGAIN -- after those two stores -- to back-link it. Reproduced as two
    // loads, because the second one is what the original uses for the address.
    REX_STORE_U32(node + kNodeNext, REX_LOAD_U32(owner + kOwnerHead));  // lwz r8,56 ; stw 56(r31)
    REX_STORE_U32(node + kNodePrev, 0);                                 // stw r7,60(r31)
    const uint32_t head = REX_LOAD_U32(owner + kOwnerHead);             // lwz r10,56(r10)
    // cmplwi cr6,r10,0 ; beq 0x82b31700
    if (head != 0) REX_STORE_U32(head + kNodePrev, node);  // stw r11,4(r10)
    REX_STORE_U32(owner + kOwnerHead, node);               // stw r11,4(r9)
  }

  // loc_82B31704
  ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,112
  ctx.r3.s64 = 16;                       // li r3,16 -- on every path
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t link = ctx.r3.u32;
  if (link == 0) return false;
  const uint32_t target = REX_LOAD_U32(link + kLinkTarget);
  if (target == 0) return false;  // sub_82B31480 would walk its node off null
  const uint32_t node = target + kNode;
  const uint32_t source = REX_LOAD_U32(link + kLinkSource);

  spec.read(link + kLinkTarget, 4);
  spec.read(link + kLinkSource, 4);
  spec.read(node, 24);

  // sub_82B31480 branches on u8[node+22]. Non-zero means the node still holds per-channel deltas,
  // and it flushes them through sub_82B468C0 -- a dispatcher whose three arms reach sub_82B3BED8
  // (25 vector stores) and sub_82B426D0 (two byte-write loops) three levels down. That write set
  // is not enumerable from entry state inside the budget, so the path is declined rather than
  // guessed at: gate 2, on this path only.
  if (REX_LOAD_U8(node + kNodeChannels) != 0) return false;

  // The other arm of sub_82B31480: an inline unlink, taken only when the node names an item.
  uint32_t unlink[3] = {0, 0, 0};
  uint32_t nunlink = 0;
  const uint32_t entry_next = REX_LOAD_U32(node + kNodeNext);
  const uint32_t entry_prev = REX_LOAD_U32(node + kNodePrev);
  if (REX_LOAD_U32(node + kNodeItem) != 0) {  // lwz r10,68(r31) ; cmplwi ; beq
    const uint32_t old_owner = REX_LOAD_U32(node + kNodeOwner);
    if (old_owner == 0) return false;  // the original reads u32[old_owner+4]
    spec.read(old_owner + kOwnerHead, 4);
    // stw r10,4(r9), only when this node is the head it names.
    if (REX_LOAD_U32(old_owner + kOwnerHead) == node) unlink[nunlink++] = old_owner + kOwnerHead;
    if (int32_t(entry_prev) != 0) unlink[nunlink++] = entry_prev + kNodeNext;  // stw r9,0(r10)
    if (int32_t(entry_next) != 0) unlink[nunlink++] = entry_next + kNodePrev;  // stw r9,4(r10)
    // The unlink re-reads the node's two link words after the first two of those stores. If either
    // lands on them, the third store's address is produced during the call: gate 2.
    for (uint32_t i = 0; i < 3 && i < nunlink; i++) {
      if (i < 2 && Overlaps(unlink[i], 4, node + kNodeNext, 8)) return false;
    }
    for (uint32_t i = 0; i < 3 && i < nunlink; i++) spec.write(unlink[i], 4);
    spec.write(node + kNodeValue, kNodeTail);  // +8, +12, +16, +20 (u16), +22 (u8)
  }

  if (source == 0) return true;  // nothing else is written
  const uint32_t owner = source + kOwner;

  // Every source field is read AFTER at least one store into the node. Requiring the four of them
  // to sit outside the node block is what keeps them entry-exact -- and it is the honest statement
  // of what this builder assumes: the source and the target are different objects.
  if (Overlaps(owner + kOwnerItem, 12, node, kNodeBytes)) return false;
  if (Overlaps(owner + kOwnerCount, 2, node, kNodeBytes)) return false;
  spec.read(owner + kOwnerItem, 12);
  spec.read(owner + kOwnerCount, 2);
  const uint32_t item = REX_LOAD_U32(owner + kOwnerItem);
  spec.read(item + kItemChannels, 1);  // a null item reads guest address 42, as the original does

  spec.write(node, kNodeBytes);        // +0..+22, the whole node
  spec.write(owner + kOwnerHead, 4);   // stw r11,4(r9)

  // `stw r11,4(r10)` stores through the head read the SECOND time. The unlink can have written
  // owner+4 first (it is the same field when the node is being moved inside the list it already
  // sits on), so the address is one of three values, all known at entry: the head as it stands,
  // u32[node+0] (what both of the unlink's head-side stores would have left there) or
  // u32[node+4] (what its prev-side store would have). The union of the three is declared -- a
  // provable superset, rather than a case analysis that has to be right.
  const uint32_t candidates[3] = {REX_LOAD_U32(owner + kOwnerHead), entry_next, entry_prev};
  const uint32_t ncand = nunlink != 0 ? 3u : 1u;
  for (uint32_t i = 0; i < 3 && i < ncand; i++) {
    if (candidates[i] != 0) spec.write(candidates[i] + kNodePrev, 4);
  }
  return true;
}

}  // namespace port_82B31680
SKATE3_PORT(82B31680, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
