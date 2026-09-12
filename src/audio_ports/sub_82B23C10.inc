// sub_82B23C10  role: how much of stream `index` is still unconsumed: element limit minus its read cursor, 0 when the element is unbound   lifted: skate3_recomp.67.cpp:14645 (47 lines)
// STATUS: verified
// calls/session: boot 128966  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B23C10.md
namespace port_82B23C10 {

// Nothing in docs/rw_audio_structs.h names this object. It is reached as Player + 0x150
// (rw_player.decoder) from sub_82B29288, with the index byte coming out of the Player's
// +0x54 table (entry +8). The layout below is read off this function and sub_82B3CA60,
// which uses the same "field at +40 is an offset from the object" idiom; the names are a
// reading, not documentation.
//   +28  u32  cursor of the stream that is currently active
//   +36  u32  byte offset FROM THE OBJECT (not a pointer) of an array of 24-byte elements
//   +49  u8   index of the currently active stream
// per element, stride 24:
//   +8   u32  that stream's saved cursor (stale for the active stream)
//   +12  u32  limit; 0 means the element is unbound
constexpr uint32_t kActiveCursor = 28;
constexpr uint32_t kStreamTable = 36;
constexpr uint32_t kActiveIndex = 49;
constexpr uint32_t kElementStride = 24;
constexpr uint32_t kElementCursor = 8;
constexpr uint32_t kElementLimit = 12;

// element address for `index`, from entry state; shared by Native and Windows.
inline uint32_t ElementAddress(uint8_t* base, uint32_t object, uint32_t index) {
  // rlwinm r11,r4,1,23,30 ; add r11,r10,r11 ; rlwinm r11,r11,3,0,28
  //   index <= 255, so the first rotate is ((index & 0xFF) << 1) exactly, the sum is 3*index,
  //   and the second rotate is a plain << 3 whose mask 0xFFFFFFF8 clears nothing: 24 * index.
  const uint32_t element_offset = index * kElementStride;
  const uint32_t table = REX_LOAD_U32(object + kStreamTable);  // lwz r9,36(r3)
  // add r11,r11,r9 ; add r9,r11,r3 -- 64-bit adds, but the load truncates to u32, and
  // addition mod 2^32 is associative, so u32 arithmetic gives the same effective address.
  return object + table + element_offset;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32 & 0xFF;  // clrlwi r10,r4,24
  const uint32_t element = ElementAddress(base, object, index);

  // lwz r11,12(r9)  -- zero-extended, and the subtractions below are 64-bit
  const uint64_t limit = REX_LOAD_U32(element + kElementLimit);
  // cmpwi cr6,r11,0 ; bne cr6,0x82b23c40
  if (limit == 0) {
    ctx.r3.s64 = 0;  // li r3,0
    return;          // blr
  }

  // loc_82B23C40
  const uint32_t active = REX_LOAD_U8(object + kActiveIndex);  // lbz r8,49(r3)
  // cmplw cr6,r10,r8 ; bne cr6,0x82b23c58 -- unsigned, both sides zero-extended bytes
  if (index == active) {
    // The active stream's saved cursor is stale; the live one lives on the object.
    const uint64_t live = REX_LOAD_U32(object + kActiveCursor);  // lwz r10,28(r3)
    // subf r3,r10,r11 -- 64-bit subtract of two zero-extended u32s: when the cursor is past
    // the limit this leaves 0xFFFFFFFF_xxxxxxxx, which callers read as a negative s32.
    ctx.r3.u64 = limit - live;
    return;  // blr
  }

  // loc_82B23C58
  const uint64_t saved = REX_LOAD_U32(element + kElementCursor);  // lwz r10,8(r9)
  ctx.r3.u64 = limit - saved;  // subf r3,r10,r11 (same 64-bit form)
}

// No stores on any path: leaf, result in r3 only, so the write set is empty. The reads are
// declared exactly (they are what a replay needs): the table offset, the element's cursor and
// limit pair, and -- only reached past the limit check, but harmless to record -- the active
// index and the live cursor. Reading the element here is as safe as the original, which loads
// +12 unconditionally, and REX_LOAD_* masks the address into the guest's 4 GB window.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32 & 0xFF;
  spec.read(object + kActiveCursor, 4);
  spec.read(object + kStreamTable, 4);
  spec.read(object + kActiveIndex, 1);
  spec.read(ElementAddress(base, object, index) + kElementCursor, 8);  // +8 cursor, +12 limit
  return true;
}

}  // namespace port_82B23C10
SKATE3_PORT(82B23C10, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
