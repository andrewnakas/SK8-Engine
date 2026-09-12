// sub_82B49100  role: retire an object -- unlink it from one of its owner's two lists, reset the three gains and every handle's field, drop it from the owner's handle array, then push it onto the other list   lifted: skate3_recomp.68.cpp:38323 (156 lines)
// STATUS: verified
// calls/session: boot 832  play 857  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B49100.md
namespace port_82B49100 {

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)

// The object in r3. Only +16 is named anywhere: it is the same owner pointer
// recomp/src/audio_ports/sub_82B49438.inc calls kOwner, on the same structure. The rest are
// numbers, because docs/rw_audio_structs.h does not describe this layout.
constexpr uint32_t kGains = 0;         // three floats at +0, +4, +8
constexpr uint32_t kGainBytes = 12;
constexpr uint32_t kOwner = 16;        // lwz r9,16(r3)
constexpr uint32_t kNode = 28;         // an intrusive list node: +0 next (self+28), +4 prev (+32)
constexpr uint32_t kNodeNext = 0;
constexpr uint32_t kNodePrev = 4;
constexpr uint32_t kNodeBytes = 8;
constexpr uint32_t kField60 = 60;      // zeroed, meaning unknown
constexpr uint32_t kHandleCount = 68;  // u8, RELOADED every loop iteration
constexpr uint32_t kState = 71;        // u8: 1 = linked on the owner's +20 list, 2 = retired
constexpr uint32_t kArgSlot = 76;      // r4 is parked here; the handle array starts one word on
constexpr uint32_t kHandles = 80;      // handle pointers, kHandleCount of them
constexpr uint32_t kHandleField = 36;  // stw r30,36(r9) -- zeroed on each handle

// The owner's two list heads. The node is taken off the +20 list and pushed onto the +16 list.
constexpr uint32_t kOwnerHead20 = 20;
constexpr uint32_t kOwnerHead16 = 16;

// The owner fields sub_82B49438 uses, restated so the mirror is visible here.
constexpr uint32_t kOwnerEntryArray = 108;
constexpr uint32_t kOwnerEntryCount = 280;  // u16
constexpr uint32_t kOwnerEntryBytes = 8;
constexpr uint32_t kMaxEntries = 1024;  // the same bound sub_82B49438.inc uses

// A hard bound on the handle loop so the window builder stays inside the span budget. The count is
// a u8; this only keeps the array honest.
constexpr uint32_t kMaxHandles = 16;

// lis r10,-32234 ; lfs f0,23056(r10) -- ((-32234 & 0xFFFF) << 16) + 23056. The same constant
// port_82B269C0 calls kZeroFloat.
constexpr uint32_t kZeroFloat = 0x82165A10;

// lfs: the word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: the double rounded back to single, its bits stored. Kept as a round trip rather than a bit
// copy, because that is what the original does.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return static_cast<uint64_t>(a) < static_cast<uint64_t>(b) + blen &&
         static_cast<uint64_t>(b) < static_cast<uint64_t>(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3
  const uint32_t arg = ctx.r4.u32;   // parked at self+76

  // stwu r1,-112(r1). REPRODUCED: sub_82B49438 builds its own frame from this one and (per its
  // port) relies on that, because the memcpy leaf under sub_82F4DC60 spills at -8 off its caller's
  // r1. Skip the move and that spill lands 112 bytes high.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  const uint32_t state = REX_LOAD_U8(self + kState);  // lbz r11,71(r3)
  if (state == 2) {  // cmplwi cr6,r11,2 ; beq cr6,0x82b491f4 -- already retired, not one store
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,112
    return;
  }

  const uint32_t node = self + kNode;  // addi r11,r3,28
  if (state == 1) {  // cmplwi cr6,r11,1 ; bne cr6,0x82b49170
    const uint32_t owner = REX_LOAD_U32(self + kOwner);        // lwz r9,16(r3)
    const uint32_t head = REX_LOAD_U32(owner + kOwnerHead20);  // lwz r10,20(r9)
    if (node == head) {  // cmplw cr6,r11,r10 ; bne cr6,0x82b49148
      // lwz r10,0(r10) -- through the head pointer, which is this node on this branch.
      REX_STORE_U32(owner + kOwnerHead20, REX_LOAD_U32(head + kNodeNext));  // stw r10,20(r9)
    }
    // loc_82B49148: cmpwi is SIGNED here and at loc_82B4915C, but only against zero.
    const uint32_t prev = REX_LOAD_U32(node + kNodePrev);  // lwz r10,4(r11)
    if (static_cast<int32_t>(prev) != 0) {
      // lwz r9,0(r11) ; stw r9,0(r10) -- next is re-read from the node, not carried.
      REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(node + kNodeNext));
    }
    // loc_82B4915C: lwz r10,0(r11) -- RELOADED after the store above, which matters only if that
    // store aliased the node. Windows() refuses that case.
    const uint32_t next = REX_LOAD_U32(node + kNodeNext);
    if (static_cast<int32_t>(next) != 0) {
      REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(node + kNodePrev));  // stw r11,4(r10)
    }
  }

  // loc_82B49170. The count is read BEFORE the four stores below, and re-read inside the loop.
  const uint32_t count = REX_LOAD_U8(self + kHandleCount);  // lbz r9,68(r31)
  REX_STORE_U32(self + kArgSlot, arg);                      // stw r4,76(r31)
  REX_STORE_U32(self + kField60, 0);                        // stw r30,60(r31), r30 = 0 throughout
  REX_STORE_U8(self + kState, 2);                           // li r8,2 ; stb r8,71(r31)

  ctx.fpscr.disableFlushModeUnconditional();       // emitted by the lfs
  const double zero = LoadSingle(base, kZeroFloat);  // lfs f0,23056(r10)
  StoreSingle(base, self + kGains + 0, zero);      // stfs f0,0(r31)
  StoreSingle(base, self + kGains + 4, zero);      // stfs f0,4(r31)
  StoreSingle(base, self + kGains + 8, zero);      // stfs f0,8(r31)

  if (count != 0) {  // cmplwi cr6,r9,0 ; beq cr6,0x82b491c4
    uint32_t cursor = self + kArgSlot;  // addi r10,r31,76
    uint32_t index = 0;                 // mr r11,r30
    do {                                // loc_82B491AC
      cursor += 4;                      // lwzu r9,4(r10) -- pre-increment, so self+80 is first
      const uint32_t handle = REX_LOAD_U32(cursor);
      index++;                                        // addi r11,r11,1
      REX_STORE_U32(handle + kHandleField, 0);        // stw r30,36(r9)
      // lbz r9,68(r31) ; cmplw cr6,r11,r9 ; blt -- the count is reloaded every iteration.
    } while (index < REX_LOAD_U8(self + kHandleCount));
  }

  // loc_82B491C4: mr r3,r31 ; bl 0x82b49438 -- drop this object from the owner's handle array. A
  // verified port, so GuestCall on the live context keeps it behind its own hook.
  skate3::audio::GuestCall(ctx, base, sub_82B49438, self);

  const uint32_t owner = REX_LOAD_U32(self + kOwner);        // lwz r10,16(r31), RELOADED
  const uint32_t first = REX_LOAD_U32(owner + kOwnerHead16);  // lwz r9,16(r10)
  REX_STORE_U32(node + kNodePrev, 0);                        // stw r30,32(r31)
  REX_STORE_U32(node + kNodeNext, first);                    // stw r9,28(r31)
  // lwz r9,16(r10) -- read a SECOND time, after those two stores.
  const uint32_t first_again = REX_LOAD_U32(owner + kOwnerHead16);
  if (first_again != 0) {  // cmplwi cr6,r9,0 ; beq cr6,0x82b491f0
    REX_STORE_U32(first_again + kNodePrev, node);  // stw r11,4(r9)
  }
  REX_STORE_U32(owner + kOwnerHead16, node);  // loc_82B491F0: stw r11,16(r10)

  // loc_82B491F4: addi r1,r1,112, then lr, r30 and r31 come back off the frame. Nothing here writes
  // r30 or r31, so only the stack pointer has to be restored.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
}

// Every byte the call writes, from entry state, in the three phases the body has -- the phase a span
// belongs to matters, because each re-read only has to be safe from the stores that PRECEDE it.
//
//   A  the unlink and the reset: owner+20, prev+0, next+4, self+76, self+60, self+71, self+0..11,
//      and handle+36 for each of the kHandleCount handles
//   B  sub_82B49438's own write set, mirrored from recomp/src/audio_ports/sub_82B49438.inc:
//      owner+280 (u16) and the memmove over [array + 8*index, + 8*(count-1-index))
//   C  the relink: self+28..35, the old first node's +4, and owner+16
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  if (self == 0) return false;  // the original would load the state byte through null

  struct Span {
    uint32_t addr;
    uint32_t len;
  };
  Span spans[skate3::audio::kPortMaxSpans];
  uint32_t n = 0;
  bool overflow = false;
  auto add = [&](uint32_t addr, uint32_t len) {
    if (len == 0) return;
    for (uint32_t i = 0; i < n; i++) {
      if (spans[i].addr == addr && spans[i].len == len) return;
    }
    if (n >= skate3::audio::kPortMaxSpans) {
      overflow = true;
      return;
    }
    spans[n].addr = addr;
    spans[n].len = len;
    n++;
  };
  // True if [addr, addr+len) is touched by any of the first `upto` spans, i.e. by the stores that
  // happen before the read this guard protects.
  auto clashes = [&](uint32_t addr, uint32_t len, uint32_t upto) {
    for (uint32_t i = 0; i < upto && i < n; i++) {
      if (Overlaps(addr, len, spans[i].addr, spans[i].len)) return true;
    }
    return false;
  };

  const uint32_t state = REX_LOAD_U8(self + kState);
  spec.read(self + kState, 1);
  if (state == 2) return true;  // the early exit writes nothing at all

  const uint32_t owner = REX_LOAD_U32(self + kOwner);
  if (owner == 0) return false;  // both remaining paths address the owner's lists
  spec.read(self + kOwner, 4);

  // ---- phase A ----
  if (state == 1) {
    // Declared unconditionally: whether the head happens to be this node is a prediction the
    // window set does not need to make, and a byte nothing writes compares equal anyway.
    add(owner + kOwnerHead20, 4);
    const uint32_t next = REX_LOAD_U32(self + kNode + kNodeNext);
    const uint32_t prev = REX_LOAD_U32(self + kNode + kNodePrev);
    spec.read(owner + kOwnerHead20, 4);
    spec.read(self + kNode, kNodeBytes);
    if (prev != 0) add(prev + kNodeNext, 4);
    if (next != 0) add(next + kNodePrev, 4);
    // The node's two words are re-read after the owner+20 store and again after the prev+0 store.
    // If either landed on the node, the second and third unlink addresses would be produced during
    // the call rather than derived from entry state: gate 2.
    if (Overlaps(owner + kOwnerHead20, 4, self + kNode, kNodeBytes)) return false;
    if (prev != 0 && Overlaps(prev + kNodeNext, 4, self + kNode, kNodeBytes)) return false;
  }

  add(self + kArgSlot, 4);
  add(self + kField60, 4);
  add(self + kState, 1);
  add(self + kGains, kGainBytes);

  const uint32_t count = REX_LOAD_U8(self + kHandleCount);
  spec.read(self + kHandleCount, 1);
  if (count > kMaxHandles) return false;  // keeps the handle loop inside the span budget
  for (uint32_t i = 1; i <= count; i++) {
    const uint32_t slot = self + kArgSlot + 4 * i;
    const uint32_t handle = REX_LOAD_U32(slot);
    spec.read(slot, 4);
    if (handle == 0) return false;  // the original would store through null
    add(handle + kHandleField, 4);
  }
  if (overflow) return false;
  const uint32_t after_a = n;

  // The count and the handle pointers are re-read inside the loop, so phase A must not touch them.
  if (clashes(self + kHandleCount, 1, after_a)) return false;
  if (count != 0 && clashes(self + kHandles, 4 * count, after_a)) return false;

  // ---- phase B: sub_82B49438, mirrored from its port ----
  // Its inputs must survive phase A, or the spans below are not the ones it will write.
  if (clashes(self + kOwner, 4, after_a)) return false;
  if (clashes(owner + kOwnerEntryCount, 2, after_a)) return false;
  if (clashes(owner + kOwnerEntryArray, 4, after_a)) return false;
  const int32_t entries = static_cast<int32_t>(REX_LOAD_U16(owner + kOwnerEntryCount));
  spec.read(owner + kOwnerEntryCount, 2);
  if (entries > 0) {
    if (entries > static_cast<int32_t>(kMaxEntries)) return false;
    const uint32_t array = REX_LOAD_U32(owner + kOwnerEntryArray);
    if (array == 0) return false;  // the search would read entries off null
    const uint32_t bytes = static_cast<uint32_t>(entries) * kOwnerEntryBytes;
    spec.read(owner + kOwnerEntryArray, 4);
    spec.read(array, bytes);
    if (clashes(array, bytes, after_a)) return false;
    // Its own gate-2 guard: the count store must not land on the owner pointer it reloads.
    if (Overlaps(owner + kOwnerEntryCount, 2, self + kOwner, 4)) return false;
    int32_t index = 0;
    bool found = false;
    for (; index < entries; index++) {
      if (REX_LOAD_U32(array + static_cast<uint32_t>(index) * kOwnerEntryBytes) == self) {
        found = true;
        break;
      }
    }
    if (found) {
      add(owner + kOwnerEntryCount, 2);
      // dst = array + 8*index, length = 8 * ((count-1) - index), the count being the halfword it
      // has just stored -- (entries-1) truncated to 16 bits, so still entry state.
      const uint64_t remaining = static_cast<uint16_t>(static_cast<uint32_t>(entries) - 1u);
      const uint64_t tail = remaining - static_cast<uint64_t>(static_cast<uint32_t>(index));
      const uint64_t lo = static_cast<uint64_t>(static_cast<uint32_t>(index)) & 0xFFFFFFFFull;
      const uint64_t offset = __builtin_rotateleft64(lo | (lo << 32), 3) & 0xFFFFFFF8u;
      const uint64_t tlo = tail & 0xFFFFFFFFull;
      const uint64_t length = __builtin_rotateleft64(tlo | (tlo << 32), 3) & 0xFFFFFFF8u;
      const uint64_t dst = static_cast<uint64_t>(array) + offset;
      if (dst + length > 0x100000000ull) return false;  // the span would wrap 2^32
      add(static_cast<uint32_t>(dst), static_cast<uint32_t>(length));
    }
  }
  if (overflow) return false;
  const uint32_t after_b = n;

  // ---- phase C ----
  add(self + kNode, kNodeBytes);  // stw r30,32(r31) ; stw r9,28(r31)
  const uint32_t after_c1 = n;
  const uint32_t first = REX_LOAD_U32(owner + kOwnerHead16);
  spec.read(owner + kOwnerHead16, 4);
  if (first != 0) add(first + kNodePrev, 4);
  add(owner + kOwnerHead16, 4);
  if (overflow) return false;

  // self+16 is reloaded straight after sub_82B49438, before phase C's stores.
  if (clashes(self + kOwner, 4, after_b)) return false;
  // owner+16 is read twice, the second time after the two node stores of phase C, so it must
  // survive everything up to and including them. Its own store is the last thing this call does.
  if (clashes(owner + kOwnerHead16, 4, after_c1)) return false;

  spec.read(self, 36);  // the gains, the owner, the node -- read back by the relink
  for (uint32_t i = 0; i < n; i++) spec.write(spans[i].addr, spans[i].len);
  return true;
}

}  // namespace port_82B49100
SKATE3_PORT(82B49100, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
