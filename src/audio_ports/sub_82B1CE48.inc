// sub_82B1CE48  role: clear slot +4*(s16[+2]+2); if 0 < depth(+4) <= capacity(+0), copy word +8 into slot +4*(depth+2) and record depth in s16[+2]; return word +12   lifted: skate3_recomp.66.cpp:65704 (46 lines)
// STATUS: verified
// calls/session: boot 45121  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CE48.md
namespace port_82B1CE48 {

// Nothing in docs/rw_audio_structs.h names this object. The +0 u8 / +4 s32 pair and the
// `0 < depth <= capacity` test are the same shape as sub_82B1CE18 (its address neighbour), so
// those two names are shared with that port; the rest are plain offsets.
constexpr uint32_t kCapacity = 0;       // u8
constexpr uint32_t kRecordedSlot = 2;   // s16  slot index written by the previous taken call
constexpr uint32_t kDepth = 4;          // s32
constexpr uint32_t kWord8 = 8;          // u32  copied into the new slot
constexpr uint32_t kWord12 = 12;        // u32  returned

// addi rX,rY,2 ; rlwinm rZ,rX,2,0,29 : the rotate-and-mask is a 32-bit shift of the low word,
// so a negative index wraps the offset below the object exactly as the original does.
inline uint32_t SlotOffset(int32_t index) {
  return (static_cast<uint32_t>(index) + 2u) << 2;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r11,r3
  const int32_t recorded =
      static_cast<int16_t>(REX_LOAD_U16(object + kRecordedSlot));  // lhz r10,2(r3) ; extsh r10,r10
  REX_STORE_U32(SlotOffset(recorded) + object, 0u);                // li r9,0 ; stwx r9,r7,r3

  // Everything below is loaded AFTER that store, which lands on +0 (recorded == -2), +4 (-1),
  // +8 (0) or +12 (1) for those indices. Keep the loads after it.
  const int32_t depth = static_cast<int32_t>(REX_LOAD_U32(object + kDepth));  // lwz r10,4(r3)
  // cmpwi cr6,r10,0 ; ble cr6,loc_82B1CE94
  if (depth > 0) {
    const int32_t capacity = static_cast<int32_t>(REX_LOAD_U8(object + kCapacity));  // lbz r9,0(r3)
    // cmpw cr6,r10,r9 ; bgt cr6,loc_82B1CE94
    if (!(depth > capacity)) {
      const uint32_t value = REX_LOAD_U32(object + kWord8);           // lwz r9,8(r3)
      REX_STORE_U32(SlotOffset(depth) + object, value);               // addi r10,r10,2 ; rlwinm r8 ; stwx r9,r8,r3
      const uint32_t depth_again = REX_LOAD_U32(object + kDepth);     // lwz r7,4(r3)
      REX_STORE_U16(object + kRecordedSlot, static_cast<uint16_t>(depth_again));  // sth r7,2(r3)
    }
  }

  // loc_82B1CE94 -- reloaded after the stores, either of which can land on +12
  ctx.r3.u64 = REX_LOAD_U32(object + kWord12);  // lwz r3,12(r11)
}                                               // blr

// Three stores. The first is at an address known from entry memory (s16 at +2). The other two
// happen only on the taken branch, whose test reads +4 and +0 AFTER the first store -- so the
// prediction replays that store's effect: it zeroes the depth word when it lands on +4, and the
// capacity byte when it lands on +0 (both are then the not-taken outcome). Store addresses are
// all object + 4k, so an overlap is always exact equality. depth is 1..255 when taken, so the
// second slot is object + 12 .. object + 1028.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  spec.read(object, 16);  // +0 capacity, +2 recorded slot, +4 depth, +8 and +12

  const int32_t recorded = static_cast<int16_t>(REX_LOAD_U16(object + kRecordedSlot));
  const uint32_t cleared = object + SlotOffset(recorded);
  spec.write(cleared, 4);  // stwx r9,r7,r3

  const int32_t depth =
      cleared == object + kDepth ? 0 : static_cast<int32_t>(REX_LOAD_U32(object + kDepth));
  const int32_t capacity =
      cleared == object + kCapacity ? 0 : static_cast<int32_t>(REX_LOAD_U8(object + kCapacity));
  if (depth > 0 && depth <= capacity) {
    spec.write(object + SlotOffset(depth), 4);  // stwx r9,r8,r3
    spec.write(object + kRecordedSlot, 2);      // sth r7,2(r3)
  }
  return true;
}

}  // namespace port_82B1CE48
SKATE3_PORT(82B1CE48, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
