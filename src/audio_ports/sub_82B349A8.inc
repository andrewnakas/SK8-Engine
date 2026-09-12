// sub_82B349A8  role: advance a ring cursor and latch the new entry's parameters   lifted: skate3_recomp.67.cpp:55346 (91 lines)
// STATUS: verified
// calls/session: boot 786  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B349A8.md
namespace port_82B349A8 {

constexpr uint32_t kCursor = 469;       // u8, advanced every call
constexpr uint32_t kCursorLimit = 470;  // u8, the wrap point
constexpr uint32_t kTableBase = 464;    // u16 byte offset to the entry table
constexpr uint32_t kEntryStride = 48;   // (i + i*2) * 16
constexpr uint32_t kLatchedA = 424;     // f32
constexpr uint32_t kLatchedB = 428;     // f32
constexpr uint32_t kFlagWord = 432;
constexpr uint32_t kLatchedWord = 436;
constexpr uint32_t kBusyByte = 471;

uint32_t EntryAddress(uint8_t* base, uint32_t object, uint32_t cursor) {
  // rotlwi r8,r11,1 ; add r11,r11,r8 ; rlwinm r11,r11,4,0,27 -- cursor*3 then *16, on 32 bits.
  const uint32_t scaled = (cursor + __builtin_rotateleft32(cursor, 1)) * 16u;
  return object + REX_LOAD_U16(object + kTableBase) + scaled;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t cursor = REX_LOAD_U8(object + kCursor);      // lbz r11,469(r3)
  const uint32_t limit = REX_LOAD_U8(object + kCursorLimit);  // lbz r9,470(r3)
  const uint32_t advanced = (cursor + 1) & 0xFF;              // addi/clrlwi
  REX_STORE_U8(object + kCursor, static_cast<uint8_t>(advanced));  // stb r7,469(r3)
  if (advanced == limit) {
    REX_STORE_U8(object + kCursor, 0);  // stb r10,469(r3) -- wrap, written as a second store
  }
  // The cursor is RELOADED here rather than reusing either value above, because the two stores
  // could alias the byte. Reproduce the reload.
  const uint32_t current = REX_LOAD_U8(object + kCursor);
  REX_STORE_U32(object + kFlagWord, 0);     // stw r10,432(r3)
  REX_STORE_U32(object + kLatchedWord, 0);  // stw r10,436(r3)
  const uint32_t entry = EntryAddress(base, object, current);
  const uint32_t state = REX_LOAD_U8(entry + 46);  // lbz r9,46(r11)
  // state 4 or 0 means "no latch"; state 1 also skips. Anything else latches the entry.
  bool latch = !(static_cast<int32_t>(state) == 4 || static_cast<int32_t>(state) == 0);
  if (latch && state != 1) {
    REX_STORE_U32(object + kFlagWord, 0);  // stw r10,432(r3) again, as lifted
    ctx.fpscr.disableFlushModeUnconditional();
    PPCRegister temp{};
    temp.u32 = REX_LOAD_U32(entry + 12);  // lfs f0,12(r11)
    temp.f32 = float(double(temp.f32));
    REX_STORE_U32(object + kLatchedA, temp.u32);
    temp.u32 = REX_LOAD_U32(entry + 16);  // lfs f13,16(r11)
    temp.f32 = float(double(temp.f32));
    REX_STORE_U32(object + kLatchedB, temp.u32);
    REX_STORE_U32(object + kLatchedWord, REX_LOAD_U32(entry + 20));  // stw r11,436(r3)
  }
  REX_STORE_U8(object + kBusyByte, 0);  // stb r10,471(r3)
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  // Every store is at a fixed offset from r3, so the write set does not depend on the reloaded
  // cursor at all -- only the READS do, and those need no window.
  spec.write(object + kLatchedA, 16);  // 424, 428, 432, 436
  spec.write(object + kCursor, 1);
  spec.write(object + kBusyByte, 1);
  // The entry the reload selects: the cursor after advancing and wrapping, computed the same way.
  const uint32_t cursor = REX_LOAD_U8(object + kCursor);
  const uint32_t limit = REX_LOAD_U8(object + kCursorLimit);
  const uint32_t advanced = (cursor + 1) & 0xFF;
  const uint32_t current = advanced == limit ? 0u : advanced;
  const uint32_t entry = EntryAddress(base, object, current);
  spec.read(object + kTableBase, 8);   // the table offset, the cursor and its limit
  spec.read(entry + 12, 12);           // the two floats and the word
  spec.read(entry + 46, 1);            // the state byte
  return true;
}

}  // namespace port_82B349A8
SKATE3_PORT(82B349A8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
