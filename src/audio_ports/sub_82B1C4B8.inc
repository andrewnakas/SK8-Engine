// sub_82B1C4B8  role: stepping cursor: return +20 if it lies in [+0,+4]; else, when +16 > 0, step +8 by s8 +12 wrapping over +4 to +0 and under +0 to +4; return +8   lifted: skate3_recomp.66.cpp:64309 (60 lines)
// STATUS: verified
// calls/session: boot 49460  play 52502  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C4B8.md
namespace port_82B1C4B8 {

// Nothing in docs/rw_audio_structs.h names this 24-byte object; plain offsets are honest.
constexpr uint32_t kLow = 0;       // s32  lower bound
constexpr uint32_t kHigh = 4;      // s32  upper bound
constexpr uint32_t kCurrent = 8;   // s32  the cursor, the only word written
constexpr uint32_t kStep = 12;     // s8   signed step
constexpr uint32_t kEnabled = 16;  // s32  stepping happens only when > 0
constexpr uint32_t kProbe = 20;    // s32  returned as-is when inside [low, high]

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                     // mr r11,r3
  const uint32_t probe = REX_LOAD_U32(object + kProbe);   // lwz r3,20(r3)
  const uint32_t low = REX_LOAD_U32(object + kLow);       // lwz r7,0(r11)

  // cmpw cr6,r3,r7 ; blt cr6,loc_82B1C4D8
  if (!(static_cast<int32_t>(probe) < static_cast<int32_t>(low))) {
    const uint32_t high = REX_LOAD_U32(object + kHigh);  // lwz r10,4(r11)
    // cmpw cr6,r3,r10 ; blelr cr6 -- inside [low, high]: r3 already holds the zero-extended probe
    if (static_cast<int32_t>(probe) <= static_cast<int32_t>(high)) {
      ctx.r3.u64 = probe;
      return;  // blelr cr6
    }
  }

  // loc_82B1C4D8
  const int32_t enabled = static_cast<int32_t>(REX_LOAD_U32(object + kEnabled));  // lwz r10,16(r11)
  // cmpwi cr6,r10,0 ; ble cr6,loc_82B1C51C
  if (enabled > 0) {
    const int8_t step = static_cast<int8_t>(REX_LOAD_U8(object + kStep));  // lbz r9,12(r11) ; extsb r8,r9
    const uint32_t current = REX_LOAD_U32(object + kCurrent);              // lwz r10,8(r11)
    const uint32_t high = REX_LOAD_U32(object + kHigh);                    // lwz r9,4(r11)
    // add r10,r8,r10 -- a 64-bit add, but only its low word is stored and compared
    const uint32_t next = static_cast<uint32_t>(static_cast<int32_t>(step)) + current;
    REX_STORE_U32(object + kCurrent, next);  // stw r10,8(r11)
    // cmpw cr6,r10,r9 ; ble cr6,loc_82B1C510
    if (static_cast<int32_t>(next) > static_cast<int32_t>(high)) {
      REX_STORE_U32(object + kCurrent, low);  // stw r7,8(r11)
      ctx.r3.u64 = low;                       // rotlwi r3,r7,0 (zero-extended)
      return;                                 // blr
    }
    // loc_82B1C510: cmpw cr6,r10,r7 ; bge cr6,loc_82B1C51C
    if (static_cast<int32_t>(next) < static_cast<int32_t>(low)) {
      REX_STORE_U32(object + kCurrent, high);  // stw r9,8(r11)
    }
  }

  // loc_82B1C51C -- reloaded after the store(s)
  ctx.r3.u64 = REX_LOAD_U32(object + kCurrent);  // lwz r3,8(r11)
}                                                // blr

// All three stores go to the same word, +8, off the entry pointer. Declared unconditionally:
// the in-range early return writes nothing, and a window over an untouched word is harmless.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.write(object + kCurrent, 4);  // stw r10 / stw r7 / stw r9, 8(r11)
  spec.read(object, 24);             // bounds, cursor, step, enable, probe
  return true;
}

}  // namespace port_82B1C4B8
SKATE3_PORT(82B1C4B8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
