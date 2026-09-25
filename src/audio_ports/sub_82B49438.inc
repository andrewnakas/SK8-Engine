// sub_82B49438  role: remove this object from its owner's handle array -- find its entry, drop the count, memmove the tail down   lifted: skate3_recomp.68.cpp:38798 (90 lines)
// STATUS: verified
// calls/session: boot 3826  play 4126  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B49438.md
namespace port_82B49438 {

// The same owner and the same array sub_82B49280 inserts into (see its note): an entry is 8 bytes,
// {u32 object, u32 payload}, and the count is a halfword.
constexpr uint32_t kOwner = 16;        // lwz r10,16(r3) -- reloaded before nearly every access
constexpr uint32_t kEntryArray = 108;  // owner+108
constexpr uint32_t kCount = 280;       // owner+280, u16
constexpr uint32_t kEntryBytes = 8;

// A bound on the search so the window builder's loop is visibly finite. The count is a u16, and a
// value this large would make the read span 8 KB; above it the object is not worth comparing.
constexpr uint32_t kMaxEntries = 1024;

// stwu r1,-96(r1). Reproduced, unlike most ports' frames: sub_82F4DC60's forward case tail-calls
// the memcpy leaf sub_82F52B30, which spills r3 at -8 off the CALLER's r1, so without the move
// that spill lands where this function's own LR spill went.
constexpr uint32_t kFrameSize = 96;

// rlwinm rX,rY,3,0,28 -- the low word shifted left three with the bottom three bits cleared,
// written out because both uses can in principle be fed a value above 2^29.
inline uint64_t Rlwinm3(uint64_t value) {
  const uint64_t lo = value & 0xFFFFFFFFu;
  return __builtin_rotateleft64(lo | (lo << 32), 3) & 0xFFFFFFF8u;
}

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const PPCRegister entry_r1 = ctx.r1;

  // stwu r1,-96(r1): the back chain with the value the original writes, then r1 moves.
  const uint32_t frame = entry_r1.u32 - kFrameSize;
  REX_STORE_U32(frame, entry_r1.u32);
  ctx.r1.u32 = frame;

  const uint32_t owner = REX_LOAD_U32(object + kOwner);          // lwz r10,16(r3)
  int64_t index = 0;                                             // li r11,0
  const int32_t count = int32_t(REX_LOAD_U16(owner + kCount));    // lhz r9,280(r10)
  bool found = false;
  // cmpwi cr6,r9,0 ; ble 0x82b49478 -- a SIGNED test on a zero-extended halfword, so only a count
  // of zero skips the search.
  if (count > 0) {
    uint32_t entry = REX_LOAD_U32(owner + kEntryArray);  // lwz r10,108(r10)
    do {                                                // loc_82B4945C
      // lwz r8,0(r10) ; cmplw cr6,r8,r3 ; beq 0x82b4948c
      if (REX_LOAD_U32(entry) == object) {
        found = true;
        break;
      }
      index += 1;              // addi r11,r11,1
      entry += kEntryBytes;    // addi r10,r10,8
    } while (int32_t(index) < count);  // cmpw cr6,r11,r9 ; blt
  }

  if (!found) {
    // loc_82B49478
    ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,96
    ctx.r3.s64 = 0;                        // li r3,0
    return;
  }

  // loc_82B4948C. The owner is reloaded from object+16 twice more, and the count re-read after the
  // store; all three reloads are reproduced rather than folded.
  const uint32_t owner2 = REX_LOAD_U32(object + kOwner);        // lwz r10,16(r3)
  const uint64_t offset = Rlwinm3(uint64_t(uint32_t(index)));   // rlwinm r9,r11,3,0,28
  const uint32_t used = REX_LOAD_U16(owner2 + kCount);          // lhz r8,280(r10)
  // addis r7,r8,1 ; addi r7,r7,-1 -- +0x10000 then -1, and the halfword store keeps only used-1.
  REX_STORE_U16(owner2 + kCount, uint16_t(used + 0xFFFFu));     // sth r7,280(r10)

  const uint32_t owner3 = REX_LOAD_U32(object + kOwner);        // lwz r5,16(r3)
  const uint32_t array = REX_LOAD_U32(owner3 + kEntryArray);    // lwz r10,108(r5)
  const uint32_t remaining = REX_LOAD_U16(owner3 + kCount);     // lhz r4,280(r5) -- the NEW count
  const uint64_t tail = uint64_t(remaining) - uint64_t(uint32_t(index));  // subf r11,r11,r4, 64-bit
  const uint64_t dst = uint64_t(array) + offset;                // add r3,r10,r9
  const uint64_t length = Rlwinm3(tail);                        // rlwinm r5,r11,3,0,28
  // bl 0x82f4dc60 -- memmove. dst < src here, so it takes the forward branch into the memcpy leaf;
  // either way it writes exactly [dst, dst+length).
  skate3::audio::GuestCall(ctx, base, sub_82F4DC60, dst, dst + 8, length);  // addi r4,r3,8

  ctx.r1.s64 = ctx.r1.s64 + kFrameSize;  // addi r1,r1,96
  ctx.r3.s64 = 1;                        // li r3,1
}

// Whether anything is written, and where, turns only on entry state: the count, the array and the
// entry contents are all read before the single store, and the search that picks the index reads
// nothing this call has written.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;
  const uint32_t owner = REX_LOAD_U32(object + kOwner);
  if (owner == 0) return false;  // the original reads the count off null
  spec.read(object + kOwner, 4);
  spec.read(owner + kCount, 2);

  const int32_t count = int32_t(REX_LOAD_U16(owner + kCount));
  if (count <= 0) return true;  // the empty owner writes nothing at all
  if (count > int32_t(kMaxEntries)) return false;  // keeps the search below visibly bounded

  const uint32_t array = REX_LOAD_U32(owner + kEntryArray);
  if (array == 0) return false;  // the search would read entries off null
  spec.read(owner + kEntryArray, 4);
  spec.read(array, uint32_t(count) * kEntryBytes);

  // The halfword store runs before object+16 is reloaded twice. If it landed on that pointer the
  // owner used for the array and the new count would be produced during the call: gate 2.
  if (Overlaps(owner + kCount, 2, object + kOwner, 4)) return false;

  int32_t index = 0;
  bool found = false;
  for (; index < count && index < int32_t(kMaxEntries); index++) {
    if (REX_LOAD_U32(array + uint32_t(index) * kEntryBytes) == object) {
      found = true;
      break;
    }
  }
  if (!found) return true;  // the miss returns 0 without a store

  spec.write(owner + kCount, 2);  // sth r7,280(r10)
  // The memmove: dst = array + 8*index, length = 8 * ((count-1) - index). The count it uses is the
  // halfword this call has just stored, so it is (count-1) truncated to 16 bits -- entry state.
  const uint32_t remaining = uint32_t(uint16_t(uint32_t(count) - 1u));
  const uint64_t tail = uint64_t(remaining) - uint64_t(uint32_t(index));
  const uint64_t dst = uint64_t(array) + Rlwinm3(uint64_t(uint32_t(index)));
  const uint64_t length = Rlwinm3(tail);
  if (dst + length > 0x100000000ull) return false;  // the span would wrap 2^32
  spec.write(uint32_t(dst), uint32_t(length));
  return true;
}

}  // namespace port_82B49438
SKATE3_PORT(82B49438, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
