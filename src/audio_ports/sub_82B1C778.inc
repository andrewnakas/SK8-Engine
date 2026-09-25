// sub_82B1C778  role: window trigger with hysteresis: latch +16 when +20 enters [+0,+4], clear it in [+8,+12]; +17 is the rising-edge pulse, returned in r3   lifted: skate3_recomp.66.cpp:64697 (59 lines)
// STATUS: verified
// calls/session: boot 797279  play 906149  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C778.md
namespace port_82B1C778 {

// Nothing in docs/rw_audio_structs.h names this 24-byte object; plain offsets are honest.
constexpr uint32_t kEnterLo = 0;    // int32  lower bound of the entry window
constexpr uint32_t kEnterHi = 4;    // int32  upper bound of the entry window
constexpr uint32_t kExitLo = 8;     // int32  lower bound of the exit window
constexpr uint32_t kExitHi = 12;    // int32  upper bound of the exit window
constexpr uint32_t kLatched = 16;   // u8     inside-state, held across calls
constexpr uint32_t kEdge = 17;      // u8     set to 1 for exactly the call that latches
constexpr uint32_t kValue = 20;     // int32  the value being tested

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                                              // mr r11,r3
  const int32_t value = static_cast<int32_t>(REX_LOAD_U32(object + kValue));       // lwz r10,20(r3)
  const int32_t enter_lo = static_cast<int32_t>(REX_LOAD_U32(object + kEnterLo));  // lwz r8,0(r3)

  // cmpw cr6,r10,r8 ; blt cr6,loc_82B1C7BC  -- below the entry window: go test the exit window
  bool in_entry_window = false;
  if (value >= enter_lo) {
    const int32_t enter_hi = static_cast<int32_t>(REX_LOAD_U32(object + kEnterHi));  // lwz r8,4(r3)
    // cmpw cr6,r10,r8 ; bgt cr6,loc_82B1C7BC  -- above it: same
    in_entry_window = value <= enter_hi;
  }

  if (in_entry_window) {
    const uint8_t latched = REX_LOAD_U8(object + kLatched);  // lbz r10,16(r3)
    // cmplwi cr6,r10,0 ; bne cr6,loc_82B1C7D8  -- already latched: no store at +16, fall through
    if (latched == 0) {
      ctx.r3.s64 = 1;                      // li r3,1
      REX_STORE_U8(object + kLatched, 1);  // stb r10,16(r11)   (r10 = 1)
      REX_STORE_U8(object + kEdge, 1);     // stb r10,17(r11)
      return;                              // blr
    }
  } else {
    // loc_82B1C7BC: outside the entry window; clear the latch only if inside the exit window
    const int32_t exit_lo = static_cast<int32_t>(REX_LOAD_U32(object + kExitLo));  // lwz r8,8(r11)
    // cmpw cr6,r10,r8 ; blt cr6,loc_82B1C7D8
    if (value >= exit_lo) {
      const int32_t exit_hi = static_cast<int32_t>(REX_LOAD_U32(object + kExitHi));  // lwz r8,12(r11)
      // cmpw cr6,r10,r8 ; bgt cr6,loc_82B1C7D8
      if (value <= exit_hi) {
        REX_STORE_U8(object + kLatched, 0);  // stb r9,16(r11)   (r9 = 0)
      }
    }
  }

  // loc_82B1C7D8
  ctx.r3.s64 = 0;                   // li r3,0
  REX_STORE_U8(object + kEdge, 0);  // stb r9,17(r11)
}

// Writes: +16 (on the latch and the un-latch paths) and +17 (every path). Both are one byte
// off the entry object; declare the pair unconditionally, since a window that covers an
// untouched byte is harmless and the path choice needs no prediction.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.write(object + kLatched, 2);  // +16, +17
  spec.read(object, 24);             // the four bounds, the two flags and the value at +20
  return true;
}

}  // namespace port_82B1C778
SKATE3_PORT(82B1C778, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
