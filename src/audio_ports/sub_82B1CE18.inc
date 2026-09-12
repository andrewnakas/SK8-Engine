// sub_82B1CE18  role: top-of-stack read: return entry[depth] of a {u8 capacity, s32 depth, u32 entries[]} stack, 0 if empty/over   lifted: skate3_recomp.66.cpp:65675 (28 lines)
// STATUS: verified
// calls/session: boot 1921575  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CE18.md
namespace port_82B1CE18 {

// Layout read (nothing in docs/rw_audio_structs.h names this object):
//   +0  u8   capacity  (max depth)
//   +4  s32  depth     (number of live entries)
//   +8  u32  entries[] 1-based: entry k lives at +4*(k+1), so the top is at +4*(depth+1)
constexpr uint32_t kCapacity = 0;
constexpr uint32_t kDepth = 4;

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const int32_t depth = static_cast<int32_t>(REX_LOAD_U32(object + kDepth));  // lwz r11,4(r3)
  // cmpwi cr6,r11,0 ; ble cr6,0x82b1ce40
  if (!(depth > 0)) {
    ctx.r3.s64 = 0;  // li r3,0
    return;          // blr
  }
  const int32_t capacity = static_cast<int32_t>(REX_LOAD_U8(object + kCapacity));  // lbz r10,0(r3)
  // cmpw cr6,r11,r10 ; bgt cr6,0x82b1ce40
  if (depth > capacity) {
    ctx.r3.s64 = 0;  // li r3,0
    return;          // blr
  }
  const uint32_t index = static_cast<uint32_t>(depth) + 1;  // addi r11,r11,1
  // rlwinm r10,r11,2,0,29 : rotl32 by 2 then mask 0xFFFFFFFC == 32-bit shift left by 2
  const uint32_t byte_offset = index << 2;
  ctx.r3.u64 = REX_LOAD_U32(byte_offset + object);  // lwzx r3,r10,r3 (zero-extended into r3)
}

// No stores: the result lives in r3 only. Reads are the 8-byte header and, when the original
// takes the in-range path, the one entry it returns (depth <= capacity <= 255, so bounded).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.read(object, 8);
  const int32_t depth = static_cast<int32_t>(REX_LOAD_U32(object + kDepth));
  const int32_t capacity = static_cast<int32_t>(REX_LOAD_U8(object + kCapacity));
  if (depth > 0 && depth <= capacity) {
    spec.read(object + ((static_cast<uint32_t>(depth) + 1) << 2), 4);
  }
  return true;
}

}  // namespace port_82B1CE18
SKATE3_PORT(82B1CE18, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
