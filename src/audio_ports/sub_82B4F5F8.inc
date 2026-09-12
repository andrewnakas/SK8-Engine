// sub_82B4F5F8  role: decode one unsigned variable-length integer, 1 to 5 bytes   lifted: skate3_recomp.68.cpp:53957 (127 lines)
// STATUS: verified
// calls/session: boot 14948  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B4F5F8.md
namespace port_82B4F5F8 {

// The prefix thresholds, the same ladder sub_82B46EA0 uses for its signed variant.
constexpr int32_t kOneByteLimit = 192;
constexpr int32_t kTwoByteLimit = 240;
constexpr int32_t kThreeByteLimit = 252;
constexpr int32_t kFourByteLimit = 255;
constexpr uint32_t kTwoByteBias = 192;
constexpr uint32_t kThreeByteBias = 12480;
constexpr uint32_t kFourByteBias = 786432u + 12480u;  // addis r10,r6,12 then addi r10,r10,12480

REX_FUNC(Native) {
  const uint32_t stream = ctx.r3.u32;
  const uint32_t b0 = REX_LOAD_U8(stream + 0);  // lbz r10,0(r3)
  // Every compare is cmpwi against the zero-extended byte, so the ladder is on 0..255.
  uint32_t value;
  uint32_t length;
  if (static_cast<int32_t>(b0) < kOneByteLimit) {
    value = b0;                                                    // stw r10,0(r4)
    length = 1;
  } else if (static_cast<int32_t>(b0) < kTwoByteLimit) {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);
    // rlwinm r11,r9,0,18,15 clears bits 14 and 15 of the 16-bit pair, then +192.
    value = (((b0 << 8) | b1) & 0xFFFF3FFFu) + kTwoByteBias;
    length = 2;
  } else if (static_cast<int32_t>(b0) < kThreeByteLimit) {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);
    const uint32_t b2 = REX_LOAD_U8(stream + 2);
    // rlwinm r11,r11,0,12,7 clears bits 20..23, then +12480.
    value = ((((b0 << 8) | b1) << 8 | b2) & 0xFF0FFFFFu) + kThreeByteBias;
    length = 3;
  } else if (static_cast<int32_t>(b0) < kFourByteLimit) {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);
    const uint32_t b2 = REX_LOAD_U8(stream + 2);
    const uint32_t b3 = REX_LOAD_U8(stream + 3);
    // rlwinm r8,r10,24,6,7 keeps the low two bits of the prefix as bits 24..25; the payload is
    // the low 26 bits of the following three bytes.
    const uint32_t high = (b0 & 0x3u) << 24;
    const uint32_t low = ((((b1 << 8) | b2) << 8) | b3) & 0x3FFFFFFu;
    value = (low | high) + kFourByteBias;
    length = 4;
  } else {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);
    const uint32_t b2 = REX_LOAD_U8(stream + 2);
    const uint32_t b3 = REX_LOAD_U8(stream + 3);
    const uint32_t b4 = REX_LOAD_U8(stream + 4);
    value = (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;  // no bias on the widest form
    length = 5;
  }
  REX_STORE_U32(ctx.r4.u32 + 0, value);
  ctx.r3.s64 = length;  // li r3,1..5 -- the bytes consumed
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  spec.write(ctx.r4.u32 + 0, 4);
  // The prefix byte decides how many follow, so the read set is exact rather than worst-case.
  const uint32_t b0 = REX_LOAD_U8(ctx.r3.u32 + 0);
  const int32_t prefix = static_cast<int32_t>(b0);
  uint32_t bytes = 1;
  if (prefix >= kFourByteLimit) bytes = 5;
  else if (prefix >= kThreeByteLimit) bytes = 4;
  else if (prefix >= kTwoByteLimit) bytes = 3;
  else if (prefix >= kOneByteLimit) bytes = 2;
  spec.read(ctx.r3.u32, bytes);
  return true;
}

}  // namespace port_82B4F5F8
SKATE3_PORT(82B4F5F8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
