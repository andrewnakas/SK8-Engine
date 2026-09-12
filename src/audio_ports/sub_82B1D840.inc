// sub_82B1D840  role: copy count words into a destination block and mark it filled   lifted: skate3_recomp.66.cpp:67201 (40 lines)
// STATUS: verified
// calls/session: boot 235  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D840.md
namespace port_82B1D840 {

constexpr uint32_t kCountByte = 24;   // u8 element count on the destination
constexpr uint32_t kFilledByte = 25;  // set to 1 on the way out
constexpr uint32_t kFirstWord = 28;   // stwu 4(r9) with r9 = dest+24 lands here first

REX_FUNC(Native) {
  const uint32_t source = ctx.r3.u32;
  const uint32_t dest = ctx.r4.u32;
  uint32_t count = REX_LOAD_U8(dest + kCountByte);  // lbz r10,24(r4)
  if (count != 0) {
    uint32_t from = source;
    uint32_t to = dest + kFirstWord;
    for (uint32_t i = 0;;) {
      REX_STORE_U32(to, REX_LOAD_U32(from));  // lwzu r8,4(r10) ; stwu r8,4(r9)
      i++;
      // lbz r8,24(r4) -- the count is RELOADED every iteration, so a destination that overlaps
      // its own count byte changes the trip count mid-loop. Reproduced rather than hoisted.
      count = REX_LOAD_U8(dest + kCountByte);
      if (static_cast<int32_t>(i) >= static_cast<int32_t>(count)) break;
      from += 4;
      to += 4;
    }
  }
  REX_STORE_U8(dest + kFilledByte, 1);  // li r11,1 ; stb r11,25(r4)
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t dest = ctx.r4.u32;
  const uint32_t count = REX_LOAD_U8(dest + kCountByte);  // u8, so at most 255 words
  spec.write(dest + kFilledByte, 1);
  if (count != 0) {
    // The copy only ever ascends from dest+28, so no store can reach the count byte at +24 and
    // shorten the loop; the span is therefore exactly count words even though the count is
    // reloaded. A destination whose words overlap its own +24 would break that, which cannot
    // happen for a block starting at +28.
    spec.write(dest + kFirstWord, 4 * count);
    spec.read(ctx.r3.u32, 4 * count);
  }
  spec.read(dest + kCountByte, 2);
  return true;
}

}  // namespace port_82B1D840
SKATE3_PORT(82B1D840, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
