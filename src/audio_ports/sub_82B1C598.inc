// sub_82B1C598  role: draw the next entry from a shuffle bag -- pick a random remaining slot, swap it to the cursor, advance and wrap -- and return base + the drawn value   lifted: skate3_recomp.66.cpp:64435 (164 lines)
// STATUS: verified
// calls/session: boot 680746  play 784687  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C598.md
namespace port_82B1C598 {

// r3 = the bag. Nothing in docs/rw_audio_structs.h names it, so the offsets stay plain.
constexpr uint32_t kGateOffset = 0;  // u16  SELF-relative byte offset of a word; 0 there = inert
constexpr uint32_t kWidth = 2;       // u8   1 selects byte elements, anything else halfwords
constexpr uint32_t kWrapFlag = 3;    // s8   set to 1 by the draw that wrapped, else 0; also
                                     //      subtracted from the count to shrink the next range
constexpr uint32_t kBase = 4;        // u32  added to the drawn element to form the result
constexpr uint32_t kCursor = 8;      // u16  index of the next slot to fill
constexpr uint32_t kCount = 10;      // u16  element count
constexpr uint32_t kResult = 12;     // u32  the cached result; also the swap's temporary
constexpr uint32_t kElements = 16;   // the element array: u8[] or u16[]
constexpr uint32_t kHeaderBytes = 16;

// lis r11,-31993 ; addi r11,r11,30192 -- 0x83070000 + 0x75F0. sub_82B1F360's six-word counter, the
// only state the draw depends on. Address computed as ((imm & 0xFFFF) << 16) + offset, not by eye.
constexpr uint32_t kCounter = 0x830775F0;
constexpr uint32_t kCounterBytes = 24;

// addi rX,rY,8 ; rlwinm rX,rX,1,0,30 -- the 64-bit value doubled, keeping only the low word with
// bit 0 cleared. Element k of the halfword array therefore sits at self + 2*(k+8) = self+16+2k.
inline uint32_t HalfOffset(uint64_t index_plus_eight) {
  return (uint32_t(index_plus_eight) << 1) & 0xFFFFFFFEu;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t gate_off = REX_LOAD_U16(self + kGateOffset);  // lhz r11,0(r3)
  // lwzx r10,r11,r3 ; cmpwi cr6,r10,0 ; bne -- the bag is inert: hand back the cached result.
  if (REX_LOAD_U32(gate_off + self) == 0) {
    ctx.r3.u64 = REX_LOAD_U32(self + kResult);  // lwz r3,12(r3) -- zero-extended
    return;
  }

  // loc_82B1C5C8. The cursor is read BEFORE the draw; every other field after it.
  const uint32_t cursor = REX_LOAD_U16(self + kCursor);  // lhz r30,8(r31)
  skate3::audio::GuestCall(ctx, base, sub_82B1F360);     // bl 0x82b1f360 -- no arguments
  const uint64_t draw = ctx.r3.u64;                      // the FULL 64-bit counter word

  const int64_t bias = static_cast<int8_t>(REX_LOAD_U8(self + kWrapFlag));  // lbz ; extsb r9,r11
  const uint64_t count = REX_LOAD_U16(self + kCount);                       // lhz r10,10(r31)
  const uint32_t width_sel = REX_LOAD_U8(self + kWidth);                    // lbz r8,2(r31)
  // subf r7,r9,r10 ; subf r6,r30,r7 -- 64-bit on a sign-extended byte and two zero-extended
  // halfwords; only the LOW word of the result is the divisor and the multiplicand.
  const uint64_t range64 = count - static_cast<uint64_t>(bias) - uint64_t(cursor);
  const uint32_t range = static_cast<uint32_t>(range64);
  // divwu r5,r3,r6 -- RexGlue yields 0 on a zero divisor rather than faulting
  const uint32_t quotient = range != 0 ? uint32_t(draw) / range : 0u;
  // twllei r6,0 -- traps when the low word of the range is zero. Kept because the original keeps
  // it; ppc_trap(type 0) warns and returns, and Windows() declines that call, so it is never
  // compared (see the note: the store that follows a zero range is wild).
  if (static_cast<int32_t>(range) == 0) ppc_trap(ctx, base, 0);
  // mullw r4,r5,r6 ; subf r11,r4,r3 ; add r11,r30,r11 -- 64-bit throughout, so the remainder
  // carries the counter's high half with it; only the low word ever reaches an address.
  const uint64_t product = static_cast<uint64_t>(int64_t(int32_t(quotient)) * int64_t(int32_t(range)));
  const uint64_t slot = uint64_t(cursor) + (draw - product);

  // cmplwi cr6,r8,1 ; bne cr6,0x82b1c634 -- the compare is set up before the arithmetic above.
  if (width_sel == 1) {
    // Byte elements. (addi r10,r11,16 is dead: computed, never used.)
    const uint32_t picked = uint32_t(slot) + self;  // add r11,r11,r31
    const uint32_t at_cursor = cursor + self;       // add r9,r30,r31
    const uint32_t value = REX_LOAD_U8(picked + kElements);                // lbz r8,16(r11)
    REX_STORE_U32(self + kResult, value);                                  // stw r8,12(r31)
    const uint32_t held = REX_LOAD_U8(at_cursor + kElements);              // lbz r7,16(r9)
    REX_STORE_U8(picked + kElements, uint8_t(held));                       // stb r7,16(r11)
    const uint32_t temp = REX_LOAD_U32(self + kResult);                    // lwz r6,12(r31)
    const uint32_t cursor_now = REX_LOAD_U16(self + kCursor);              // lhz r11,8(r31)
    REX_STORE_U8(cursor_now + self + kElements, uint8_t(temp));            // stb r6,16(r5)
  } else {
    // loc_82B1C634 -- halfword elements.
    const uint32_t picked = HalfOffset(slot + 8);         // addi r11,r11,8 ; rlwinm r11,r11,1,0,30
    const uint32_t at_cursor = HalfOffset(cursor + 8);    // addi r10,r30,8 ; rlwinm r9,r10,1,0,30
    const uint32_t value = REX_LOAD_U16(picked + self);                    // lhzx r8,r11,r31
    REX_STORE_U32(self + kResult, value);                                  // stw r8,12(r31)
    const uint32_t held = REX_LOAD_U16(at_cursor + self);                  // lhzx r7,r9,r31
    REX_STORE_U16(picked + self, uint16_t(held));                          // sthx r7,r11,r31
    const uint32_t temp = REX_LOAD_U32(self + kResult);                    // lwz r5,12(r31)
    const uint32_t cursor_now = REX_LOAD_U16(self + kCursor);              // lhz r11,8(r31)
    REX_STORE_U16(HalfOffset(uint64_t(cursor_now) + 8) + self, uint16_t(temp));  // sthx r5,r4,r31
  }

  // loc_82B1C668 -- every field reloaded after the swap's stores.
  const uint32_t cursor_next = REX_LOAD_U16(self + kCursor);  // lhz r9,8(r31)
  const uint32_t drawn = REX_LOAD_U32(self + kResult);        // lwz r10,12(r31)
  const uint32_t base_value = REX_LOAD_U32(self + kBase);     // lwz r11,4(r31)
  const uint32_t limit = REX_LOAD_U16(self + kCount);         // lhz r8,10(r31)
  const uint32_t advanced = (cursor_next + 1) & 0xFFFFu;      // addi r9,r9,1 ; clrlwi r7,r9,16
  // add r3,r11,r10 on two zero-extended words: the sum is 64-BIT and a carry lands in bit 32, so
  // r3 keeps it and the store keeps only the low word.
  ctx.r3.u64 = uint64_t(base_value) + uint64_t(drawn);
  REX_STORE_U16(self + kCursor, uint16_t(advanced));          // sth r7,8(r31)
  REX_STORE_U32(self + kResult, uint32_t(ctx.r3.u64));        // stw r3,12(r31)
  // cmplw cr6,r7,r8 ; blt cr6,0x82b1c6a8
  if (advanced < limit) {
    REX_STORE_U8(self + kWrapFlag, 0);  // loc_82B1C6A8: li r11,0 ; stb r11,3(r31)
  } else {
    REX_STORE_U16(self + kCursor, 0);   // li r11,0 ; sth r11,8(r31)
    REX_STORE_U8(self + kWrapFlag, 1);  // li r10,1 ; stb r10,3(r31)
  }
}

// Every byte the call writes, from entry state. The picked slot is not predicted -- that would mean
// reimplementing sub_82B1F360's counter here -- but it is bounded: the index lies in
// [cursor, cursor + range), and the cursor slot is the lowest member of that interval, so ONE span
// over the whole interval covers both halves of the swap.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t gate_off = REX_LOAD_U16(self + kGateOffset);

  spec.read(self, kHeaderBytes);
  spec.read(gate_off + self, 4);
  // lwzx r10,r11,r3 ; beq -- the inert path writes nothing, and its one input is entry state.
  if (REX_LOAD_U32(gate_off + self) == 0) return true;

  // +2, +3 and +10 are read AFTER sub_82B1F360 has run. If its counter overlapped the header, the
  // values driving the range would change mid-call and none of this would derive from entry state.
  if (uint64_t(self) < uint64_t(kCounter) + kCounterBytes &&
      uint64_t(kCounter) < uint64_t(self) + kHeaderBytes) {
    return false;
  }

  const uint32_t cursor = REX_LOAD_U16(self + kCursor);
  const int64_t bias = static_cast<int8_t>(REX_LOAD_U8(self + kWrapFlag));
  const uint64_t count = REX_LOAD_U16(self + kCount);
  const uint32_t width = REX_LOAD_U8(self + kWidth) == 1 ? 1u : 2u;
  const uint32_t range =
      static_cast<uint32_t>(count - static_cast<uint64_t>(bias) - uint64_t(cursor));
  // A zero range traps (twllei) and then indexes with the raw counter word: the store lands
  // wherever that points, which no window can cover. Never compared.
  if (range == 0) return false;

  const uint64_t span = uint64_t(width) * range;
  const uint64_t first = uint64_t(self) + kElements + uint64_t(width) * cursor;
  if (first + span > 0x100000000ull) return false;  // the array would wrap 2^32
  // The array starts at +16 and the span only grows upward, so it cannot reach the header fields
  // that +8, +4, +10 and +12 are reloaded from after the swap. That is why no alias check is
  // needed for those four, unlike the wrap above.
  if (span + kCounterBytes + 7 > skate3::audio::kPortWatchCap) return false;

  spec.write(uint32_t(first), uint32_t(span));  // the picked slot and the cursor slot
  spec.write(self + kWrapFlag, 1);              // stb r11/r10,3(r31)
  spec.write(self + kCursor, 2);                // sth r7,8(r31) and sth r11,8(r31)
  spec.write(self + kResult, 4);                // stw r8,12(r31) twice over
  spec.write(kCounter, kCounterBytes);          // sub_82B1F360's whole six-word counter
  spec.read(uint32_t(first), uint32_t(span));
  spec.read(kCounter, kCounterBytes);
  return true;
}

}  // namespace port_82B1C598
SKATE3_PORT(82B1C598, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
