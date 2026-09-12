// sub_82B50270  role: advance a bit cursor by a field read from the stream, wrapping the page   lifted: skate3_recomp.68.cpp:55765 (162 lines)
// STATUS: verified
// calls/session: boot 2913  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B50270.md
namespace port_82B50270 {

constexpr int32_t kSplitPoint = 16337;  // past this, the read straddles two windows
constexpr uint32_t kPageBits = 16352;   // the cursor wraps here and the base advances
constexpr uint32_t kPageBytes = 2048;

// Read three bytes at a byte offset and left-align them in a word: b0<<24 | b1<<16 | b2<<8.
uint32_t ThreeBytesAligned(uint8_t* base, uint32_t buffer, int32_t byte_offset) {
  const uint32_t at = buffer + static_cast<uint32_t>(byte_offset);
  const uint32_t b0 = REX_LOAD_U8(at);
  const uint32_t b1 = REX_LOAD_U8(at + 1);
  const uint32_t b2 = REX_LOAD_U8(at + 2);
  // rotlwi r8,r8,8 ; or ; rlwinm 8,0,23 ; or ; rlwinm 8,0,23
  return (((((b0 << 8) | b1) << 8) | b2) << 8) & 0xFFFFFF00u;
}

// srawi r,x,3 followed by addze: a division by 8 that truncates toward zero.
int32_t DivideByEightTowardZero(int32_t value) {
  const int32_t shifted = value >> 3;
  const bool carry = value < 0 && (static_cast<uint32_t>(value) & 0x7u) != 0;
  return shifted + (carry ? 1 : 0);
}

REX_FUNC(Native) {
  const uint32_t cursor_field = ctx.r4.u32;
  const uint32_t base_field = ctx.r3.u32;
  const int32_t cursor = static_cast<int32_t>(REX_LOAD_U32(cursor_field + 0));  // lwz r5,0(r4)
  // Bits that spill past the split point are taken from a second read below.
  int32_t spill = 0;
  if (cursor > kSplitPoint) {
    spill = cursor - kSplitPoint;  // addi r7,r5,-16337
  }
  const int32_t first_width = 15 - spill;             // subfic r11,r7,15
  const uint32_t buffer = REX_LOAD_U32(base_field + 0);  // lwz r10,0(r3)
  uint32_t value = 0;
  if (first_width > 0) {
    const int32_t position = cursor + 32;                       // addi r9,r5,32
    const int32_t byte_offset = DivideByEightTowardZero(position);
    const int32_t bit_in_group = position - 8 * DivideByEightTowardZero(position);
    const uint32_t word = ThreeBytesAligned(base, buffer, byte_offset);
    // slw then srw, both 32-bit, with the shift counts below 32 by construction.
    const uint32_t shifted = static_cast<uint32_t>(word << (bit_in_group & 0x1F));
    value = shifted >> ((32 - first_width) & 0x1F);
  }
  int32_t second_position = cursor + first_width + 64;  // add r11,r5,r11 ; addi r11,r11,64
  if (spill > 0) {
    const uint32_t already = static_cast<uint32_t>(value << (spill & 0x1F));  // slw r6,r8,r7
    const int32_t byte_offset = DivideByEightTowardZero(second_position);
    const int32_t bit_in_group =
        second_position - 8 * DivideByEightTowardZero(second_position);
    const uint32_t word = ThreeBytesAligned(base, buffer, byte_offset);
    const uint32_t shifted = static_cast<uint32_t>(word << (bit_in_group & 0x1F));
    value = (shifted >> ((32 - spill) & 0x1F)) | already;
  }
  const uint32_t advanced = static_cast<uint32_t>(cursor) + value;  // add r11,r5,r8
  if (advanced < kPageBits) {
    REX_STORE_U32(cursor_field + 0, advanced);  // stw r11,0(r4)
    return;
  }
  // Past the page: the buffer base steps on and the cursor keeps the remainder.
  REX_STORE_U32(base_field + 0, buffer + kPageBytes);       // stw r10,0(r3)
  REX_STORE_U32(cursor_field + 0, advanced - kPageBits);    // stw r9,0(r4)
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t cursor_field = ctx.r4.u32;
  const uint32_t base_field = ctx.r3.u32;
  const int32_t cursor = static_cast<int32_t>(REX_LOAD_U32(cursor_field + 0));
  const uint32_t buffer = REX_LOAD_U32(base_field + 0);
  spec.write(cursor_field + 0, 4);
  spec.write(base_field + 0, 4);  // written only on the wrap path; unwritten compares equal
  spec.read(cursor_field + 0, 4);
  spec.read(base_field + 0, 4);
  // The two three-byte reads, at offsets derived from the entry cursor alone.
  const int32_t spill = cursor > kSplitPoint ? cursor - kSplitPoint : 0;
  const int32_t first_width = 15 - spill;
  if (first_width > 0) {
    spec.read(buffer + static_cast<uint32_t>(DivideByEightTowardZero(cursor + 32)), 3);
  }
  if (spill > 0) {
    const int32_t second = cursor + first_width + 64;
    spec.read(buffer + static_cast<uint32_t>(DivideByEightTowardZero(second)), 3);
  }
  return true;
}

}  // namespace port_82B50270
SKATE3_PORT(82B50270, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
