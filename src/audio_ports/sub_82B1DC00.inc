// sub_82B1DC00  role: find the global list entry whose container key matches u32[arg+8], unlink it, and when that empties the list hand the global record to sub_82B489D0; returns 12 always   lifted: skate3_recomp.67.cpp:3 (115 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 5 comparable calls in the weaker profile, 8 in the stronger, against 115 lifted lines -- a handful of calls. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 5  play 8  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1DC00.md
namespace port_82B1DC00 {

// lis -31997 ; addi 28492 -- ((-31997 & 0xFFFF) << 16) + 28492, computed, not read by eye.
constexpr uint32_t kList = 0x83036F4Cu;
constexpr uint32_t kListHead = 8;    // rw_ptr  the list head, and the emptiness test
constexpr uint32_t kRecord = 12;     // the record sub_82B489D0 takes in r4
// lis -31993 ; lwz 30252
constexpr uint32_t kOwnerSlot = 0x8307762Cu;  // rw_ptr  sub_82B489D0's r3

// The links are at the node; the key lives in the object the node is embedded 80 bytes into.
constexpr uint32_t kNext = 0;           // rw_ptr
constexpr uint32_t kPrev = 4;           // rw_ptr
constexpr uint32_t kNodeInContainer = 80;
constexpr uint32_t kContainerKey = 60;  // int32, compared signed against u32[arg+8]

// sub_82B489D0's fields, for the window builder only.
constexpr uint32_t kOwnerCurrent = 180;  // rw_ptr  the record it treats as "the current one"
constexpr uint32_t kOwnerSaveA = 184;    // u32
constexpr uint32_t kOwnerSaveB = 188;    // rw_ptr
constexpr uint32_t kRecordState = 20;    // u8, set to 3 on the way out
constexpr uint32_t kRecordSpare = 16;    // u32, cleared
constexpr uint32_t kLinkBack = 8;        // u32[u32[record+0] + 8], cleared

constexpr uint32_t kFrameBytes = 96;
// A hard cap on the list walk so both the body and the window builder are visibly bounded. The
// original walks until the next pointer is null.
constexpr uint32_t kMaxNodes = 4096;
// Key and next-pointer reads declared per walked node, for replay only: two spans a node, so 64
// nodes stay well inside kPortMaxReads. A longer walk still shadows; its vector just cannot replay.
constexpr uint32_t kMaxWalkReads = 64;

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t arg = ctx.r3.u32;  // mr r9,r3
  // lwz r3,30252(r10) -- loaded into r3 at the top and left there for the call at the bottom.
  const uint32_t owner = REX_LOAD_U32(kOwnerSlot);
  ctx.r3.u64 = owner;
  const uint32_t head = REX_LOAD_U32(kList + kListHead);  // lwz r11,8(r8)

  // stwu r1,-96(r1) -- reproduced: sub_82B489D0 spills lr at r1-8 and allocates 96 bytes of its
  // own, which must land below this frame rather than on top of it.
  const uint32_t sp = ctx.r1.u32 - kFrameBytes;
  REX_STORE_U32(sp, ctx.r1.u32);  // the back chain; inside this function's own frame
  ctx.r1.u32 = sp;

  uint32_t head_now = head;  // r11, reloaded from the head word after each list fixup
  bool found = false;
  uint32_t node = 0;

  if (head != 0) {                                   // cmplwi cr6,r11,0 ; beq
    const uint32_t key = REX_LOAD_U32(arg + 8);      // lwz r9,8(r9)
    uint32_t cursor = head;  // mr r10,r11
    // Unbounded, exactly as the original: the cap that keeps the WINDOW builder finite lives in
    // Windows(), which refuses the call rather than letting the body disagree about where to stop.
    for (;;) {  // loc_82B1DC34
      const uint32_t container = cursor - kNodeInContainer;  // addi r10,r10,-80
      // lwz r7,60(r10) ; cmpw cr6,r9,r7 ; beq -- a SIGNED compare
      if (int32_t(REX_LOAD_U32(container + kContainerKey)) == int32_t(key)) {
        node = cursor;  // addi r10,r10,80
        found = true;
        break;
      }
      // lwz r10,80(r10) -- the next pointer, i.e. u32[cursor + 0]
      cursor = REX_LOAD_U32(container + kNodeInContainer);
      if (cursor == 0) break;  // cmplwi cr6,r10,0 ; bne -- falls out to `li r3,12`
    }
  }

  if (found) {
    // loc_82B1DC64
    if (node == head) {                              // cmplw cr6,r10,r11 ; bne
      head_now = REX_LOAD_U32(head + kNext);         // lwz r11,0(r11)
      REX_STORE_U32(kList + kListHead, head_now);    // stw r11,8(r8)
    }
    // loc_82B1DC78
    const uint32_t prev = REX_LOAD_U32(node + kPrev);  // lwz r9,4(r10)
    if (int32_t(prev) != 0) {                          // cmpwi cr6,r9,0 ; beq -- SIGNED
      // lwz r11,0(r10) ; stw r11,0(r9)
      REX_STORE_U32(prev + kNext, REX_LOAD_U32(node + kNext));
      head_now = REX_LOAD_U32(kList + kListHead);  // lwz r11,8(r8) -- reloaded after the store
    }
    // loc_82B1DC90 -- the next pointer is re-read here, after the store above
    const uint32_t next = REX_LOAD_U32(node + kNext);  // lwz r9,0(r10)
    if (int32_t(next) != 0) {
      // lwz r11,4(r10) ; stw r11,4(r9)
      REX_STORE_U32(next + kPrev, REX_LOAD_U32(node + kPrev));
      head_now = REX_LOAD_U32(kList + kListHead);  // lwz r11,8(r8) -- reloaded again
    }
    // loc_82B1DCA8 -- the list is now empty, so the record goes back.
    if (head_now == 0) {  // cmplwi cr6,r11,0 ; bne
      // addi r4,r8,12 ; bl 0x82b489d0. r3 still holds the word loaded at the top. r8 came out of
      // lis/addi SIGN-EXTENDED, so r4 reaches the callee as 0xFFFFFFFF83036F58; it addresses with
      // the low word only, but the register it sees is reproduced as the original leaves it.
      skate3::audio::GuestCall(ctx, base, sub_82B489D0, uint64_t(owner),
                               uint64_t(int64_t(int32_t(kList))) + kRecord);
    }
  }

  // loc_82B1DCB8 (and the walk-out path at 0x82b1dc58) -- the same constant on every path.
  ctx.r3.s64 = 12;                        // li r3,12
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,96 -- a 64-bit add, as in the lifted form
}

// sub_82B489D0(owner, record): stores u32[record+16] and u8[record+20] on every path, and then
// either the four words of the "current record" arm or, when u8[record+20] != 3, a whole further
// list move through sub_82B39690. Returns false on that last sub-path -- see the note.
bool CalleeWindows(uint8_t* base, skate3::audio::PortSpec& spec, uint32_t owner, uint32_t record) {
  if (owner == 0) return false;  // the original would load u32[180] out of the image
  spec.read(owner + kOwnerCurrent, 4);
  spec.read(record + kRecordState, 1);
  spec.read(record + kNext, 4);
  spec.write(record + kRecordSpare, 4);  // stw r8,16(r9)
  spec.write(record + kRecordState, 1);  // stb r11,20(r9)
  if (record == REX_LOAD_U32(owner + kOwnerCurrent)) {
    spec.write(owner + kOwnerSaveA, 8);  // stw r10,72(r11) ; stw r7,76(r11) -- owner+184, +188
    spec.write(record + kNext, 4);       // stw r8,0(r4)
    // stw r8,8(r7) with r7 = u32[record+0], read before that store
    spec.write(REX_LOAD_U32(record + kNext) + kLinkBack, 4);
    return true;
  }
  if (REX_LOAD_U8(record + kRecordState) == 3) {
    return true;  // beq cr6,0x82b48a30 -- only the two closing stores run
  }
  return false;  // sub_82B39690 would run: not enumerated, gate 2 on this sub-path
}

// Every byte the call writes, from entry state. The list is walked exactly as the body walks it,
// before any store, so the found node and its two neighbours are entry state.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t arg = ctx.r3.u32;
  const uint32_t owner = REX_LOAD_U32(kOwnerSlot);
  const uint32_t head = REX_LOAD_U32(kList + kListHead);
  spec.read(kOwnerSlot, 4);
  spec.read(kList + kListHead, 4);
  if (head == 0) return true;  // beq cr6,0x82b1dcb8 -- writes nothing at all
  if (arg == 0) return false;  // the original would load u32[8] out of the image
  spec.read(arg + 8, 4);

  const uint32_t key = REX_LOAD_U32(arg + 8);
  uint32_t cursor = head;
  uint32_t node = 0;
  uint32_t reads = 0;
  for (uint32_t guard = 0; guard < kMaxNodes; guard++) {
    const uint32_t container = cursor - kNodeInContainer;
    if (reads++ < kMaxWalkReads) {
      spec.read(container + kContainerKey, 4);
      spec.read(cursor, 4);  // lwz r10,80(r10): the next pointer, read when the key misses
    }
    if (int32_t(REX_LOAD_U32(container + kContainerKey)) == int32_t(key)) {
      node = cursor;
      break;
    }
    cursor = REX_LOAD_U32(container + kNodeInContainer);
    if (cursor == 0) return true;  // walked off the end: `li r3,12`, no stores
  }
  if (node == 0) return false;  // the cap was hit before the walk ended: not enumerable

  spec.read(node, 8);
  const uint32_t prev = REX_LOAD_U32(node + kPrev);
  const uint32_t next = REX_LOAD_U32(node + kNext);
  const bool fix_head = node == head;
  const bool fix_prev = int32_t(prev) != 0;
  const bool fix_next = int32_t(next) != 0;

  // The head word is re-read after each fixup, and the node's two link words are re-read after the
  // earlier stores. If any store lands on one of them, neither the later addresses nor the
  // emptiness test derive from entry state -- gate 2.
  const uint32_t head_word = kList + kListHead;
  if (fix_head && Overlaps(head_word, 4, node, 8)) return false;
  if (fix_prev && (Overlaps(prev + kNext, 4, node, 8) || Overlaps(prev + kNext, 4, head_word, 4))) {
    return false;
  }
  if (fix_next && Overlaps(next + kPrev, 4, head_word, 4)) return false;

  if (fix_head) spec.write(head_word, 4);       // stw r11,8(r8)
  if (fix_prev) spec.write(prev + kNext, 4);    // stw r11,0(r9)
  if (fix_next) spec.write(next + kPrev, 4);    // stw r11,4(r9)

  // r11 at loc_82B1DCA8: the entry head when the node was not the head (a non-head node has a
  // non-null prev, so the reload sees the untouched word), otherwise u32[node+0] -- stored to the
  // head word and read back by every reload. So the callee runs exactly when the found node was
  // the head of a one-element list, which is entry state.
  if (!(fix_head && next == 0)) return true;

  const uint32_t record = kList + kRecord;
  // Its reads happen after the three fixups above, so they must not have been overwritten.
  const skate3::audio::ShadowWindow probes[3] = {
      {owner + kOwnerCurrent, 4}, {record + kRecordState, 1}, {record + kNext, 4}};
  for (size_t i = 0; i < spec.nwin; i++) {
    for (const skate3::audio::ShadowWindow& p : probes) {
      if (Overlaps(p.addr, p.len, spec.windows[i].addr, spec.windows[i].len)) return false;
    }
  }
  return CalleeWindows(base, spec, owner, record);
}

}  // namespace port_82B1DC00
SKATE3_PORT(82B1DC00, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnR3)
