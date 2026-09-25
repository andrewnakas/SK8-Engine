// sub_82B3C9D8  role: advance a position (+28) by r4; on hitting the current 24-byte segment's end (+12) clear it, step the u8 index (+49) mod count (+50), and reload the position from the new segment's +8   lifted: skate3_recomp.68.cpp:8110 (70 lines)
// STATUS: verified
// calls/session: boot 119335  play 178780  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C9D8.md
namespace port_82B3C9D8 {

// Layout read (nothing in docs/rw_audio_structs.h names this object):
//   +28  u32  position   advanced by r4 each call; reloaded from a segment's start on rollover
//   +36  u32  table      byte offset, from the object, of an array of 24-byte segments
//   +49  u8   index      current segment
//   +50  u8   count      segments; the index wraps to 0 when it reaches this
// Segment k lives at object + table + 24*k:  +8 u32 start, +12 u32 end (zeroed when hit)
constexpr uint32_t kPosition = 28;
constexpr uint32_t kTableOffset = 36;
constexpr uint32_t kIndex = 49;
constexpr uint32_t kCount = 50;
constexpr uint32_t kHeaderBytes = 51;  // +0 .. +50, everything above
constexpr uint32_t kSegmentStride = 24;
constexpr uint32_t kSegmentStart = 8;
constexpr uint32_t kSegmentEnd = 12;

// rotlwi rA,rI,1 ; add rA,rI,rA ; rlwinm rA,rA,3,0,28 ; add rA,rA,rTable ; add rA,rA,r3
// The index is a zero-extended byte, so the rotate is a plain doubling and 3*index << 3 never
// reaches bit 32: the scaled term is exactly 24*index. The two adds are 64-bit in the lifted form,
// but the segment is only ever addressed through the low word, so 32-bit wrap is identical.
inline uint32_t Segment(uint32_t object, uint32_t table, uint32_t index) {
  return index * kSegmentStride + table + object;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t index = REX_LOAD_U8(object + kIndex);          // lbz r11,49(r3)
  const uint32_t table = REX_LOAD_U32(object + kTableOffset);   // lwz r9,36(r3)
  const uint32_t position = REX_LOAD_U32(object + kPosition);   // lwz r8,28(r3)
  // add r11,r8,r4 -- a 64-bit add in the lifted form; only its low word is stored and compared
  const uint32_t advanced = position + ctx.r4.u32;
  REX_STORE_U32(object + kPosition, advanced);                  // stw r11,28(r3)
  const uint32_t segment = Segment(object, table, index);       // (address from pre-store loads)
  // lwz r9,12(r10) -- after the store, as the original: it could alias +28
  const uint32_t end = REX_LOAD_U32(segment + kSegmentEnd);
  // cmpw cr6,r11,r9 ; bnelr cr6 -- equality, so the signedness of cmpw cannot matter
  if (advanced != end) return;

  REX_STORE_U32(segment + kSegmentEnd, 0);                      // stw r9,12(r10)   (r9 = 0)
  // lbz r11,49(r3) ; addi r11,r11,1 ; clrlwi r10,r11,24 -- reloaded after the store, as the original
  const uint32_t next = (REX_LOAD_U8(object + kIndex) + 1) & 0xFF;
  REX_STORE_U8(object + kIndex, static_cast<uint8_t>(next));    // stb r10,49(r3)
  const uint32_t count = REX_LOAD_U8(object + kCount);          // lbz r8,50(r3)
  // cmplw cr6,r10,r8 ; blt cr6,loc_82B3CA34 -- unsigned
  if (!(next < count)) {
    REX_STORE_U8(object + kIndex, 0);                           // stb r9,49(r3)    (r9 = 0)
  }

  // loc_82B3CA34: lbz r11,49(r3) ; lwz r10,36(r3) -- both reloaded, as the original
  const uint32_t current = REX_LOAD_U8(object + kIndex);
  const uint32_t table_now = REX_LOAD_U32(object + kTableOffset);
  // rotlwi ; add ; rlwinm ; add ; add r10,r11,r3 ; lwz r9,8(r10)
  const uint32_t start = REX_LOAD_U32(Segment(object, table_now, current) + kSegmentStart);
  REX_STORE_U32(object + kPosition, start);                     // stw r9,28(r3)
  // blr -- r3 and r4 are left as they arrived; the function is void
}

// Writes: +28 (u32, every path, twice on rollover), +49 (u8, rollover path) and the current
// segment's end word. That segment's address comes only from +36 and +49, both loaded before
// the first store and never reloaded for the store, so it is known from entry state. Declare
// all three unconditionally: a window over bytes the early-return path leaves alone is harmless.
// Reads: the header, the current segment's start/end, and the start word of both possible next
// segments ((index+1) & 0xFF, or 0 after the wrap).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  const uint32_t index = REX_LOAD_U8(object + kIndex);
  const uint32_t table = REX_LOAD_U32(object + kTableOffset);
  const uint32_t segment = Segment(object, table, index);
  spec.write(object + kPosition, 4);
  spec.write(object + kIndex, 1);
  spec.write(segment + kSegmentEnd, 4);
  spec.read(object, kHeaderBytes);
  spec.read(segment + kSegmentStart, 8);
  spec.read(Segment(object, table, (index + 1) & 0xFF) + kSegmentStart, 4);
  spec.read(Segment(object, table, 0) + kSegmentStart, 4);
  return true;
}

}  // namespace port_82B3C9D8
SKATE3_PORT(82B3C9D8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
