// sub_82B46EA0  role: decode one SIGNED zigzag variable-length integer, 1 to 5 bytes   lifted: skate3_recomp.68.cpp:33299 (150 lines)
// STATUS: verified
// calls/session: boot 32604  play 26780  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B46EA0.md
namespace port_82B46EA0 {

// The same prefix ladder as the unsigned sibling sub_82B4F5F8: the first byte's value picks
// the width, and the payload is the remaining bits.
constexpr int32_t kOneByteLimit = 192;
constexpr int32_t kTwoByteLimit = 240;
constexpr int32_t kThreeByteLimit = 252;
constexpr int32_t kFourByteLimit = 255;

// Each bias is exactly half the unsigned sibling's (192, 12480, 786432+12480), because the
// payload here is zigzagged: one magnitude step costs two encoded steps.
constexpr uint32_t kTwoByteBias = 96;           // addi r11,r11,96
constexpr uint32_t kThreeByteBias = 6240;       // addi r11,r11,6240  (shared tail loc_82B46F1C)
constexpr uint32_t kFourByteHighBias = 393216;  // addis r11,r9,6

// rlwinm rX,rY,8,0,23 -- rotate the 32-bit word left 8 and keep bits 0..23. The byte that
// wraps into the low end is zero on every path that uses this, but rotate rather than assume.
uint32_t RotateInByte(uint32_t value) { return __builtin_rotateleft32(value, 8) & 0xFFFFFF00u; }

REX_FUNC(Native) {
  const uint32_t stream = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const uint32_t b0 = REX_LOAD_U8(stream + 0);  // lbz r10,0(r3)
  // Every ladder compare is cmpwi on the zero-extended byte, so it is a compare on 0..255.
  uint64_t value;    // r11: the magnitude, biased, before the sign step
  uint32_t low_bit;  // r10: the zigzag sign, always bit 0 of the LAST byte of the code
  uint32_t length;   // r3
  if (static_cast<int32_t>(b0) < kOneByteLimit) {
    // rlwinm r11,r10,31,1,31 -- rotate right one and drop the bit that lands in bit 31.
    value = b0 >> 1;
    low_bit = b0 & 1u;  // clrlwi r10,r10,31
    length = 1;         // li r3,1
  } else if (static_cast<int32_t>(b0) < kTwoByteLimit) {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);  // lbz r11,1(r11)
    const uint32_t pair = (((b0 << 8) & 0xFFFFFF00u) | b1);  // rlwinm r10,r10,8,0,23 ; or r9,r10,r11
    low_bit = b1 & 1u;                                       // clrlwi r10,r11,31
    // srawi r8,r9,1 then rlwinm r11,r8,0,19,16: halve, then clear bits 13 and 14 -- which is
    // where the 0xC0 prefix lands once the 16-bit pair has been shifted down one.
    value = ((static_cast<uint32_t>(static_cast<int32_t>(pair) >> 1)) & 0xFFFF9FFFu) + kTwoByteBias;
    length = 2;  // li r3,2
  } else if (static_cast<int32_t>(b0) < kThreeByteLimit) {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);  // lbz r9,1(r11)
    const uint32_t b2 = REX_LOAD_U8(stream + 2);  // lbz r7,2(r11)
    const uint32_t pair = (((b0 << 8) & 0xFFFFFF00u) | b1);  // rlwinm r8,r10,8,0,23 ; or r6,r9,r8
    low_bit = b2 & 1u;                                       // clrlwi r10,r7,31
    // rlwinm r11,r6,0,20,15 strips the 0xF0 prefix from the pair BEFORE the third byte is
    // appended, so the payload is 12 + 8 bits.
    const uint32_t trio = RotateInByte(pair & 0xFFFF0FFFu) | b2;
    value = static_cast<uint32_t>(static_cast<int32_t>(trio) >> 1);  // srawi r11,r8,1
    value += kThreeByteBias;                                         // loc_82B46F1C
    length = 3;  // li r3,3
  } else if (static_cast<int32_t>(b0) < kFourByteLimit) {
    const uint32_t b1 = REX_LOAD_U8(stream + 1);  // lbz r9,1(r11)
    const uint32_t b2 = REX_LOAD_U8(stream + 2);  // lbz r7,2(r11)
    const uint32_t b3 = REX_LOAD_U8(stream + 3);  // lbz r5,3(r11)
    // rlwinm r8,r10,16,14,15 keeps the low two bits of the prefix as bits 16..17; the middle
    // pair contributes its low 18 bits (clrlwi r7,r11,14) and the last byte its top 7.
    const uint32_t high = __builtin_rotateleft32(b0, 16) & 0x30000u;
    const uint32_t mid = (__builtin_rotateleft32(b1, 8) | b2) & 0x3FFFFu;
    low_bit = b3 & 1u;                                    // clrlwi r10,r5,31
    const uint32_t quad = RotateInByte(mid | high) | (b3 & 0xFFFFFFFEu);  // rlwinm r9,r5,0,0,30
    value = static_cast<uint32_t>(static_cast<int32_t>(quad) >> 1);  // srawi r9,r11,1
    value += kFourByteHighBias;                                      // addis r11,r9,6
    value += kThreeByteBias;                                         // loc_82B46F1C
    length = 4;  // li r3,4
  } else {
    // loc_82B46F7C -- the widest form is the raw 32-bit two's-complement value: no zigzag, no
    // bias, and it never reaches the sign step below.
    const uint32_t b1 = REX_LOAD_U8(stream + 1);  // lbz r10,1(r11)
    const uint32_t b2 = REX_LOAD_U8(stream + 2);  // lbz r9,2(r11)
    const uint32_t b3 = REX_LOAD_U8(stream + 3);  // lbz r7,3(r11)
    const uint32_t b4 = REX_LOAD_U8(stream + 4);  // lbz r6,4(r11)
    const uint32_t wide =
        RotateInByte(RotateInByte(__builtin_rotateleft32(b1, 8) | b2) | b3) | b4;
    REX_STORE_U32(out + 0, wide);  // stw r8,0(r4)
    ctx.r3.s64 = 5;                // li r3,5
    return;                        // blr
  }
  // loc_82B46F20: the odd encodings are the negatives. subfic is 64-bit, and the srawi above
  // never produced a negative (each payload is at most 26 bits), so the only 64-bit effect is
  // the borrow out of bit 31 -- which the stw below discards anyway.
  if (static_cast<int32_t>(low_bit) != 0) {         // cmpwi cr6,r10,0 ; beq
    value = static_cast<uint64_t>(-1) - value;      // subfic r11,r11,-1
  }
  REX_STORE_U32(out + 0, static_cast<uint32_t>(value));  // stw r11,0(r4)
  ctx.r3.s64 = length;                                   // li r3,1..4
}

// One 4-byte store at r4, plus the 1..5 bytes of the code as reads. The prefix byte decides the
// width, so the read set is exact rather than worst-case.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  spec.write(ctx.r4.u32 + 0, 4);
  const int32_t prefix = static_cast<int32_t>(REX_LOAD_U8(ctx.r3.u32 + 0));
  uint32_t bytes = 1;
  if (prefix >= kFourByteLimit) bytes = 5;
  else if (prefix >= kThreeByteLimit) bytes = 4;
  else if (prefix >= kTwoByteLimit) bytes = 3;
  else if (prefix >= kOneByteLimit) bytes = 2;
  spec.read(ctx.r3.u32, bytes);
  return true;
}

}  // namespace port_82B46EA0
SKATE3_PORT(82B46EA0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
