// sub_82B33418  role: parse one chunk header of a slot's stream and submit it to the decoder -- read the 8-byte header at r4 as two big-endian words (the first masked to 31 bits, or to 24 when the slot's +76 is nonzero; the second a count), fill the ring entry the slot's +74 names with class = the slot index, state 1 and the owner from slot+40, submit {decoder, header+8, count, ...} through sub_82B3C870 and keep the request-slot byte it returns at entry+12, add the count into the caller's counter at *r6 and into slot+20, and return header + length   lifted: skate3_recomp.67.cpp:52240 (211 lines)
// STATUS: gate-1 (sub_82B3C870 reaches an indirect call one level down: its own `bctrl` at 0x82B3C8E4, a virtual notify through the object's table slot +0 -- and it is called unconditionally on every path)
// calls/session: boot 3110  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33418.md
namespace port_82B33418 {

// The same ~478-byte record, ring and parallel arrays as sub_82B32480, sub_82B32550,
// sub_82B332D0, sub_82B33870 and sub_82B33F98. Offsets stay plain; names are borrowed from the
// ports that established them.
constexpr uint32_t kSlotArray = 96;      // u32 -> the 80-byte slot array
constexpr uint32_t kParallelBias = 464;  // u16 byte bias of the 48-byte array, from self

// The 20-slot, 16-byte ring at self+100 -- sub_82B32480's names.
constexpr uint32_t kEntries = 100;
constexpr uint32_t kEntryStride = 16;
constexpr uint32_t kEntryOwner = 4;    // u32 <- u32[slot+40]; sub_82EC1378's r3 when it retires
constexpr uint32_t kEntryExtra = 8;    // u32 zeroed, then overwritten with u32[record+32] when
                                       //     the r8 argument is nonzero
constexpr uint32_t kEntryResult = 12;  // u8 <- sub_82B3C870's request-slot index
constexpr uint32_t kEntryState = 13;   // u8 <- 1; sub_82B33F98 writes 2 and sub_82B32480 retires
constexpr uint32_t kEntryClass = 14;   // u8 <- the slot index

// The 80-byte slot (r30).
constexpr uint32_t kSlotSubmitted = 20;  // u32 the count is added here as well
constexpr uint32_t kSlotOwner = 40;      // u32 copied into the ring entry's +4
constexpr uint32_t kSlotField56 = 56;    // u32 handed to sub_82B3C870 as r8, r8-path only
constexpr uint32_t kSlotField68 = 68;    // u32 handed to sub_82B3C870 as r9, r8-path only
constexpr uint32_t kSlotRingIndex = 74;  // u8 the ring entry this submit fills (sub_82B332D0
                                         //    wrote it from sub_82B32550's claim)
constexpr uint32_t kSlotNarrowLength = 76;  // u8: 0 = 31-bit length field, nonzero = 24-bit

// The 48-byte record (r11).
constexpr uint32_t kRecordDecoder = 8;  // u32 -> the decoder, sub_82B3C870's r3
constexpr uint32_t kRecordField32 = 32; // u32 read twice on the r8-path

// The chunk header at r4: eight bytes, then the payload.
constexpr uint32_t kHeaderPayload = 8;
constexpr uint32_t kLengthMask31 = 0x7FFFFFFFu;  // clrlwi r27,r10,1

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)
constexpr uint32_t kWordSlot = 80;     // the four bytes at r1+80..83 the header words go through

// rlwinm r,N,0,31-N then add: both halves are zero-extended 32-bit quantities but the add is
// 64 bit, so a carry out of bit 31 survives into the register the loads address through. Kept in
// 64 bits and narrowed only where the lifted form uses `.u32`.
inline uint64_t ScaleByThree(uint64_t index) {
  const uint64_t times_two = uint64_t((uint32_t(index) << 1) & 0xFFFFFFFEu);  // rlwinm r10,r5,1,0,30
  return index + times_two;                                                   // add r4,r5,r10
}
inline uint64_t ScaleByFive(uint64_t index) {
  const uint64_t times_four = uint64_t((uint32_t(index) << 2) & 0xFFFFFFFCu);  // rlwinm r9,r5,2,0,29
  return index + times_four;                                                   // add r9,r5,r9
}
inline uint64_t TimesSixteen(uint64_t value) {
  return uint64_t((uint32_t(value) << 4) & 0xFFFFFFF0u);  // rlwinm r9,r9,4,0,27
}
// rlwinm rX,rX,8,0,23 -- a left shift by 8 that also clears the low byte, which is already zero
// for any value that fits in 24 bits. Kept in the mask form the lifted line uses.
inline uint64_t ShiftInByte(uint64_t value, uint32_t next) {
  return (uint64_t((uint32_t(value) << 8) & 0xFFFFFF00u)) | uint64_t(next);
}
// cntlzw then rlwinm r,r,27,31,31 -- keeps bit 5 of the leading-zero count, which is set only
// when the count is 32, i.e. only when the byte is zero. A branchless `== 0`.
inline uint64_t IsZeroByte(uint32_t value) {
  const uint32_t byte = value & 0xFF;                                 // clrlwi
  const uint32_t clz = (byte == 0) ? 32u : uint32_t(__builtin_clz(byte));
  return (clz >> 5) & 1u;
}

REX_FUNC(Native) {
  // __savegprlr_24 spills r24-r31 and the link register; the harness compares r13-r31, so the
  // entry values have to survive here too. The spill slots are this call's own, not windowed.
  const PPCRegister saved_r24 = ctx.r24;
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  ctx.r12.u64 = ctx.lr;  // mflr r12 -- the value __restgprlr_24 puts back in lr on the way out

  // The frame IS reproduced: the four bytes at r1+80 are the scratch the header words are
  // assembled through, and sub_82B3C870 runs on this frame.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-160(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;
  const uint64_t header_reg = ctx.r4.u64;  // mr r31,r4
  const uint64_t index_reg = ctx.r5.u64;
  const uint32_t header = uint32_t(header_reg);
  const uint32_t counter = ctx.r6.u32;  // mr r26,r6 -- a pointer to one u32 the caller owns
  const uint64_t arg_r7 = ctx.r7.u64;   // only its low byte is read, and only for a 0/1 flag
  const uint32_t arg_r8 = ctx.r8.u32;   // only its low byte is read, as a path selector

  const uint32_t bias = REX_LOAD_U16(self + kParallelBias);  // lhz r11,464(r3)
  const uint32_t array = REX_LOAD_U32(self + kSlotArray);    // lwz r6,96(r3)
  // add r30,r9,r6 -- the 80-byte slot.
  const uint32_t slot = uint32_t(TimesSixteen(ScaleByFive(index_reg)) + uint64_t(array));
  // add r11,r11,r10 ; add r11,r11,r3 -- the 48-byte record, biased from the object itself.
  const uint32_t record =
      uint32_t(uint64_t(bias) + TimesSixteen(ScaleByThree(index_reg)) + uint64_t(ctx.r3.u64));

  const uint32_t h1 = REX_LOAD_U8(header + 1);  // lbz r9,1(r31) -- loaded before the branch

  uint64_t length = 0;  // r27
  uint64_t count = 0;   // r28
  // lbz r10,76(r30) ; cmplwi cr6,r10,0 ; bne cr6,0x82b334b4
  if (REX_LOAD_U8(slot + kSlotNarrowLength) == 0) {
    // All eight header bytes are read before the first store; the frame and the header cannot
    // alias in practice, but the order is kept anyway.
    const uint32_t h0 = REX_LOAD_U8(header + 0);  // lbz r10,0(r31)
    const uint32_t h2 = REX_LOAD_U8(header + 2);  // lbz r6,2(r31)
    const uint32_t h3 = REX_LOAD_U8(header + 3);  // lbz r4,3(r31)
    const uint32_t h5 = REX_LOAD_U8(header + 5);  // lbz r28,5(r31)
    const uint32_t h4 = REX_LOAD_U8(header + 4);  // lbz r29,4(r31)
    const uint32_t h6 = REX_LOAD_U8(header + 6);  // lbz r25,6(r31)
    const uint32_t h7 = REX_LOAD_U8(header + 7);  // lbz r24,7(r31)

    // The compiler assembles each word by spilling four bytes to r1+80..83 and reading them back
    // as one u32; the guest is big-endian, so the word is (b0<<24)|(b1<<16)|(b2<<8)|b3. The slot
    // is written TWICE, and the second read depends on the second group of stores having landed,
    // so this is reproduced literally rather than folded into shifts.
    REX_STORE_U8(frame + kWordSlot + 0, uint8_t(h0));  // stb r10,80(r1)
    REX_STORE_U8(frame + kWordSlot + 1, uint8_t(h1));  // stb r9,81(r1)
    REX_STORE_U8(frame + kWordSlot + 2, uint8_t(h2));  // stb r6,82(r1)
    REX_STORE_U8(frame + kWordSlot + 3, uint8_t(h3));  // stb r4,83(r1)
    // lwz r10,80(r1) ; clrlwi r27,r10,1 -- the top bit of the first word is dropped.
    length = uint64_t(REX_LOAD_U32(frame + kWordSlot) & kLengthMask31);

    REX_STORE_U8(frame + kWordSlot + 0, uint8_t(h4));  // stb r29,80(r1)
    REX_STORE_U8(frame + kWordSlot + 1, uint8_t(h5));  // stb r28,81(r1)
    REX_STORE_U8(frame + kWordSlot + 2, uint8_t(h6));  // stb r25,82(r1)
    REX_STORE_U8(frame + kWordSlot + 3, uint8_t(h7));  // stb r24,83(r1)
    count = uint64_t(REX_LOAD_U32(frame + kWordSlot));  // lwz r28,80(r1)
  } else {
    // loc_82B334B4 -- the same two words, assembled with shifts instead of the stack, and the
    // length taken from bytes 1..3 ONLY: byte 0 is not part of it on this path.
    const uint32_t h2 = REX_LOAD_U8(header + 2);  // lbz r10,2(r31)
    const uint32_t h3 = REX_LOAD_U8(header + 3);  // lbz r9,3(r31)
    const uint32_t h4 = REX_LOAD_U8(header + 4);  // lbz r10,4(r31)
    const uint32_t h5 = REX_LOAD_U8(header + 5);  // lbz r6,5(r31)
    const uint32_t h6 = REX_LOAD_U8(header + 6);  // lbz r4,6(r31)
    const uint32_t h7 = REX_LOAD_U8(header + 7);  // lbz r29,7(r31)
    // rotlwi r9,r9,8 ; or r10,r9,r10 ; rlwinm r10,r10,8,0,23 ; or r27,r10,r9
    length = ShiftInByte(uint64_t(__builtin_rotateleft32(h1, 8) | h2), h3);
    // rotlwi r4,r10,8 ; or r6,r4,r6 ; rlwinm r6,r6,8,0,23 ; or r6,r6,r4 ; rlwinm r4,r6,8,0,23 ;
    // or r28,r4,r29 -- the full 32-bit big-endian word from bytes 4..7.
    count = ShiftInByte(ShiftInByte(uint64_t(__builtin_rotateleft32(h4, 8) | h5), h6), h7);
  }

  // loc_82B334F4 -- the ring entry this submit fills was chosen by sub_82B32550 and recorded at
  // slot+74. rotlwi on a byte load is index*16 and cannot carry; the lifted form addresses
  // through `.u32`, so this stays 32 bit.
  const uint32_t ring_index = REX_LOAD_U8(slot + kSlotRingIndex);  // lbz r9,74(r30)
  const uint32_t entry = self + (ring_index * kEntryStride) + kEntries;

  // addi r10,r27,-8 -- the payload length. DEAD: r10 is never read again on any path, and the
  // call below is free to clobber it. Reproduced so the register state matches for a callee
  // that happens not to write r10.
  ctx.r10.s64 = static_cast<int64_t>(length) - 8;

  REX_STORE_U8(entry + kEntryClass, uint8_t(index_reg));    // stb r5,114(r9)
  REX_STORE_U8(entry + kEntryState, 1);                     // stb r4,113(r9), r4 = 1
  REX_STORE_U32(entry + kEntryExtra, 0);                    // stw r6,108(r9), r6 = 0
  const uint32_t owner = REX_LOAD_U32(slot + kSlotOwner);   // lwz r8,40(r30)
  REX_STORE_U32(entry + kEntryOwner, owner);                // stw r8,104(r9)

  // The six arguments sub_82B3C870 takes beyond its r3: r4 = the payload, r5 = the count,
  // r6 = "the r7 argument's low byte is zero", and r7..r9, which are zero unless the r8
  // argument's low byte is set.
  uint64_t arg6 = IsZeroByte(uint32_t(arg_r7));
  uint64_t arg7 = 0;
  uint64_t arg8 = 0;
  uint64_t arg9 = 0;
  // clrlwi r3,r8,24 ; cmplwi cr6,r3,0 ; bne cr6,0x82b33554
  if ((arg_r8 & 0xFF) != 0) {
    // loc_82B33554 -- and this path ALSO overwrites the zero just stored at entry+8.
    REX_STORE_U32(entry + kEntryExtra, REX_LOAD_U32(record + kRecordField32));  // stw r9,8(r29)
    arg9 = REX_LOAD_U32(slot + kSlotField68);  // lwz r9,68(r30)
    arg8 = REX_LOAD_U32(slot + kSlotField56);  // lwz r8,56(r30)
    // lwz r7,32(r11) -- record+32 is RELOADED here rather than reused from the store above.
    arg7 = REX_LOAD_U32(record + kRecordField32);
  }

  // loc_82B33574 -- bl 0x82b3c870(decoder, header+8, count, is_zero, r7, r8, r9). This is the
  // call that fails gate 1: it notifies the owner through a function pointer.
  const uint32_t decoder = REX_LOAD_U32(record + kRecordDecoder);  // lwz r3,8(r11)
  skate3::audio::GuestCall(ctx, base, sub_82B3C870, uint64_t(decoder),
                           header_reg + kHeaderPayload,  // addi r4,r31,8
                           count,                        // mr r5,r28
                           arg6, arg7, arg8, arg9);
  REX_STORE_U8(entry + kEntryResult, ctx.r3.u8);  // stb r3,12(r29) -- the request-slot index

  // add r3,r27,r31 -- the next header. A 64-bit add of the masked length onto the ENTRY value of
  // r4, so the result carries past bit 31 if the caller's pointer was that high; the caller
  // (sub_82B33870) truncates it with `stw r3,48(r31)`.
  ctx.r3.u64 = length + header_reg;

  // Both accumulations are 64-bit adds on zero-extended words, narrowed by the store.
  const uint64_t running = REX_LOAD_U32(counter + 0);         // lwz r10,0(r26)
  REX_STORE_U32(counter + 0, uint32_t(running + count));      // add r10,r10,r28 ; stw r10,0(r26)
  const uint64_t submitted = REX_LOAD_U32(slot + kSlotSubmitted);  // lwz r11,20(r30)
  REX_STORE_U32(slot + kSlotSubmitted, uint32_t(submitted + count));  // stw r9,20(r30)

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
  ctx.lr = ctx.r12.u64;                   // __restgprlr_24 restores lr and r24-r31
  ctx.r24 = saved_r24;
  ctx.r25 = saved_r25;
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note; it is
// enumerable, so gate 1 is the only blocker.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B33418
SKATE3_PORT(82B33418, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
