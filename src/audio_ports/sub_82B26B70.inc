// sub_82B26B70  role: read r4 bits MSB-first from a byte stream, advancing the bit cursor   lifted: skate3_recomp.67.cpp:21737 (64 lines)
// STATUS: verified
// calls/session: boot 14455  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B26B70.md
namespace port_82B26B70 {

constexpr uint32_t kDataPointer = 0;  // the byte stream
constexpr uint32_t kBitCursor = 4;    // absolute bit position, reloaded every iteration

REX_FUNC(Native) {
  const uint32_t reader = ctx.r3.u32;
  const uint32_t data = REX_LOAD_U32(reader + kDataPointer);  // lwz r6,0(r3)
  uint64_t remaining = ctx.r4.u64;                            // subf. r4,r11,r4 works in 64 bits
  uint32_t accumulator = 0;                                   // li r3,0
  for (;;) {
    const uint32_t cursor = REX_LOAD_U32(reader + kBitCursor);  // lwz r10,4(r8)
    const uint32_t bit_in_byte = cursor & 0x7;                  // clrlwi r9,r10,29
    const uint32_t byte_index = cursor >> 3;                    // rlwinm r7,r10,29,3,31
    // subfic r11,r9,8 then a cmplw/ble pair: take the rest of this byte, or all that is left.
    uint32_t take = 8 - bit_in_byte;
    if (take > static_cast<uint32_t>(remaining)) {
      take = static_cast<uint32_t>(remaining);  // mr r11,r4 -- compared as 32-bit unsigned
    }
    // subfic r31,r11,8 ; subf r31,r9,r31 ; clrlwi r31,r31,24
    const uint32_t shift = (8 - take - bit_in_byte) & 0xFF;
    const uint32_t byte = REX_LOAD_U8(data + byte_index);  // lbzx r7,r6,r7
    // slw of 0 by 0 is 0, and take is at most 8, so no shift here can reach the 0x20 case the
    // lifted slw/srw guard against.
    accumulator = static_cast<uint32_t>(accumulator << take);   // slw r3,r3,r11
    const uint32_t mask = static_cast<uint32_t>(1u << take) - 1;  // slw r9,r5,r11 ; addi r9,r9,-1
    accumulator |= (byte >> shift) & mask;                      // srw ; and ; or r3,r9,r3
    REX_STORE_U32(reader + kBitCursor, cursor + take);          // stw r7,4(r8)
    remaining -= take;
    // bne 0x82b26b84 on cr0 from the 32-bit signed compare of the remaining count
    if (static_cast<int32_t>(static_cast<uint32_t>(remaining)) == 0) break;
  }
  ctx.r3.u64 = accumulator;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t reader = ctx.r3.u32;
  const uint32_t data = REX_LOAD_U32(reader + kDataPointer);
  const uint32_t cursor = REX_LOAD_U32(reader + kBitCursor);
  const uint64_t bits = ctx.r4.u64;
  if (bits > 4096) return false;  // the read span would be unreasonable; not this function's use
  spec.write(reader + kBitCursor, 4);
  spec.read(reader, 8);
  // The bytes the loop can touch: from the cursor's byte through the last bit requested. The
  // cursor is reloaded each iteration, but the only writer is this function's own +4 store, and
  // the stream bytes are never written, so the span is exact.
  const uint32_t first = cursor >> 3;
  const uint32_t last = static_cast<uint32_t>((cursor + bits + 7) >> 3);
  spec.read(data + first, (last - first) + 1);
  return true;
}

}  // namespace port_82B26B70
SKATE3_PORT(82B26B70, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
