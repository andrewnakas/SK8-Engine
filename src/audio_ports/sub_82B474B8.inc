// sub_82B474B8  role: walk the four variable-length seek streams until the record that contains a target position is found, and publish that record into the object   lifted: skate3_recomp.68.cpp:34171 (233 lines)
// STATUS: verified
// calls/session: boot 1881  play 1545  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B474B8.md
namespace port_82B474B8 {

constexpr uint32_t kFrameBytes = 272;  // stwu r1,-272(r1)

// The frame layout this call builds for the decoder, exactly as port_82B47658/port_82B47010
// describe it: one shared cursor object whose +0 is the live byte pointer into the stream, four
// 16-byte sub-objects {cursor* +0, accumulated +4, remaining +8, flag +12} all pointing at it, and
// the four-word result sub_82B47658 fills in.
constexpr uint32_t kDecoded = 80;  // addi r3,r1,80 -- sub_82B47658's out: one word per stream
constexpr uint32_t kCursor = 96;   // addi r4,r1,96 -- the shared cursor object (in)
constexpr uint32_t kStreamOffsets[4] = {100, 116, 132, 148};  // in+4, in+20, in+36, in+52
constexpr uint32_t kSubCursor = 0;
constexpr uint32_t kSubAccumulated = 4;
constexpr uint32_t kSubRemaining = 8;
constexpr uint32_t kSubFlag = 12;

// r3 = the object the found record is published into. Nothing in docs/rw_audio_structs.h names it,
// so these names describe the arithmetic rather than claiming a recovered field name.
constexpr uint32_t kPublishBase = 4;      // 0, or the entry value of this word plus stream 1's sum
constexpr uint32_t kPublishPosition = 8;  // the record's own start position
constexpr uint32_t kPublishSkip = 12;     // (target - length) - position
constexpr uint32_t kPublishLength = 16;   // min(target - position, [+28]), signed
constexpr uint32_t kPublishOffset = 20;   // stream 0's running sum
constexpr uint32_t kPublishExact = 32;    // u8: 1 when the mode word is exactly 1
constexpr uint32_t kPublishFirst = kPublishBase;
constexpr uint32_t kPublishBytes = 20;    // [+4, +24): the five words above, one span
constexpr uint32_t kMinLength = 28;       // read only; caps the published length

// Read summary for the replay: the four streams are interleaved in one byte run through the shared
// cursor and the walk length is not bounded from entry state, so a fixed window is recorded.
constexpr uint32_t kStreamReadSummary = 1024;

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;   // mr r30,r3
  const uint64_t stream = ctx.r4.u64;   // stw r4,96(r1)
  const uint64_t target = ctx.r5.u64;   // mr r26,r5

  // __savegprlr_20 spills r20-r31 into the red zone and both exits restore them; the twelve are
  // spilled only because of the five guest calls. This body keeps them in C++ locals and never
  // writes ctx.r20-r31, so their entry values stand and the compared set is untouched.
  const uint32_t entry_sp = ctx.r1.u32;
  // stwu r1,-272(r1). REPRODUCED: the decoder state below lives in this frame and its address is
  // handed to sub_82B47658 and sub_82B47010, whose own 112-byte frames must sit under it.
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;

  REX_STORE_U32(frame + kCursor, uint32_t(stream));  // stw r4,96(r1)
  // lwz r10,28(r3) -- read here, before every zero store, and read again inside the loop.
  const uint64_t min_length_entry = REX_LOAD_U32(object + kMinLength);

  // The four sub-objects zeroed, then their cursor field pointed at the shared cursor object. The
  // lifted body zeroes the four +0 words first and interleaves the rest; every one of these stores
  // is to a distinct address in this call's own frame with no load between them, so grouping them
  // per sub-object is the same program.
  for (uint32_t k = 0; k < 4; k++) {
    const uint32_t sub = frame + kStreamOffsets[k];
    REX_STORE_U32(sub + kSubCursor, 0);       // stw r31,100(r1) / 116 / 132 / 148
    REX_STORE_U32(sub + kSubAccumulated, 0);  // stw r31,104(r1) / 120 / 136 / 152
    REX_STORE_U32(sub + kSubRemaining, 0);    // stw r31,108(r1) / 124 / 140 / 156
    REX_STORE_U8(sub + kSubFlag, 0);          // stb r31,112(r1) / 128 / 144 / 160
  }
  for (uint32_t k = 0; k < 4; k++) {
    // stw r11,100(r1) ; stw r9,116(r1) ; stw r8,132(r1) ; stw r7,148(r1) -- all four r1+96
    REX_STORE_U32(frame + kStreamOffsets[k], uint32_t(ctx.r1.s64 + kCursor));
  }

  // subf r6,r10,r5 -- 64-bit on two zero-extended words, so a target below the minimum length
  // borrows into the high half; only the low word is ever compared.
  const uint64_t span = target - min_length_entry;
  // subfic r5,r6,0 ; rlwinm r10,r6,1,31,31 ; addme r5,r10 ; and r22,r5,r6 -- the mask is
  // sign(low word) + carry(low word == 0) - 1, which is all ones only when the low word is
  // positive and non-zero. So: the span when it is a positive int32, else zero.
  const uint64_t carry = uint32_t(span) == 0 ? 1u : 0u;      // subfic sets ca on r6.u32 <= 0
  const uint64_t sign = (uint32_t(span) >> 31) & 1u;
  const uint64_t mask = sign + carry - 1;
  const uint64_t limit = mask & span;  // and r22,r5,r6

  // bl 0x82b47658 -- prime all four streams and leave their first values at r1+80..r1+92.
  skate3::audio::GuestCall(ctx, base, sub_82B47658, uint64_t(ctx.r1.s64 + kDecoded),
                           uint64_t(ctx.r1.s64 + kCursor));

  uint64_t step = REX_LOAD_U32(frame + kDecoded + 8);          // lwz r28,88(r1) -- stream 2
  const uint64_t base_entry = REX_LOAD_U32(object + kPublishBase);  // lwz r21,4(r30), before any store

  int64_t result = 1;  // li r20,1 ; the r3 of both loc_82B4763C exits
  // cmpwi cr6,r28,0 ; blt cr6,0x82b4763c -- a negative first step ends the walk at once, with 1.
  if (int32_t(uint32_t(step)) >= 0) {
    uint64_t mode = REX_LOAD_U32(frame + kDecoded + 12);     // lwz r3,92(r1)  -- stream 3
    uint64_t value_b = REX_LOAD_U32(frame + kDecoded + 4);   // lwz r24,84(r1) -- stream 1
    uint64_t value_a = REX_LOAD_U32(frame + kDecoded + 0);   // lwz r23,80(r1) -- stream 0
    uint64_t position = 0;  // mr r29,r31
    uint64_t sum_a = 0;     // mr r27,r31
    uint64_t sum_b = 0;     // mr r25,r31

    for (;;) {  // loc_82B47584
      // cmpw cr6,r29,r22 ; bgt ; add r11,r28,r29 ; cmpw cr6,r22,r11 ; blt -- all signed on the
      // low words, and short-circuiting exactly as the two branches do.
      const bool in_range = int32_t(uint32_t(position)) <= int32_t(uint32_t(limit)) &&
                            int32_t(uint32_t(limit)) < int32_t(uint32_t(step + position));
      // loc_82B47598: cmpwi cr6,r3,1 ; bne cr6,0x82b475f4 -- mode 1 publishes every record.
      if (in_range || int32_t(uint32_t(mode)) == 1) {
        // loc_82B475A0: cmpwi cr6,r24,0 ; beq -- a zero stream-1 value publishes a bare zero.
        if (int32_t(uint32_t(value_b)) == 0) {
          REX_STORE_U32(object + kPublishBase, 0);  // loc_82B475B4: stw r31,4(r30)
        } else {
          // add r11,r25,r21 -- 64-bit on two zero-extended words; the stw keeps the low one.
          REX_STORE_U32(object + kPublishBase, uint32_t(sum_b + base_entry));  // stw r11,4(r30)
        }
        // loc_82B475B8
        const uint64_t min_length = REX_LOAD_U32(object + kMinLength);  // lwz r11,28(r30), reloaded
        const uint64_t remaining = target - position;                   // subf r10,r29,r26
        REX_STORE_U32(object + kPublishPosition, uint32_t(position));   // stw r29,8(r30)
        // cmpw cr6,r10,r11 ; bge cr6,0x82b475d0 ; mr r11,r10 -- signed min on the low words.
        const uint64_t length = int32_t(uint32_t(remaining)) < int32_t(uint32_t(min_length))
                                    ? remaining
                                    : min_length;
        // addi r10,r3,-1 ; cntlzw r8,r10 ; rlwinm r6,r8,27,31,31 -- bit 5 of the leading-zero
        // count, which is set only for a count of 32, i.e. only when the mode word is exactly 1.
        const uint32_t mode_less_one = uint32_t(mode - 1);
        const uint32_t leading = mode_less_one == 0 ? 32u : uint32_t(__builtin_clz(mode_less_one));
        REX_STORE_U32(object + kPublishLength, uint32_t(length));  // stw r11,16(r30)
        const uint64_t tail = target - length;                     // subf r9,r11,r26
        REX_STORE_U32(object + kPublishOffset, uint32_t(sum_a));   // stw r27,20(r30)
        // subf r7,r29,r9
        REX_STORE_U32(object + kPublishSkip, uint32_t(tail - position));      // stw r7,12(r30)
        REX_STORE_U8(object + kPublishExact, uint8_t((leading >> 5) & 1u));   // stb r6,32(r30)
      }

      // loc_82B475F4
      position = step + position;  // add r29,r28,r29 -- 64-bit
      // cmpw cr6,r26,r29 ; blt cr6,0x82b47648 -- walked past the target: return 0.
      if (int32_t(uint32_t(target)) < int32_t(uint32_t(position))) {
        result = 0;  // mr r3,r31
        break;
      }
      sum_a = value_a + sum_a;  // add r27,r23,r27 -- 64-bit
      sum_b = value_b + sum_b;  // add r25,r24,r25 -- 64-bit

      // Four bl 0x82b47010, one per stream, in this order; each returns that stream's next value.
      skate3::audio::GuestCall(ctx, base, sub_82B47010,
                               uint64_t(ctx.r1.s64 + kStreamOffsets[0]));  // addi r3,r1,100
      value_a = ctx.r3.u64;  // mr r23,r3
      skate3::audio::GuestCall(ctx, base, sub_82B47010,
                               uint64_t(ctx.r1.s64 + kStreamOffsets[1]));  // addi r3,r1,116
      value_b = ctx.r3.u64;  // mr r24,r3
      skate3::audio::GuestCall(ctx, base, sub_82B47010,
                               uint64_t(ctx.r1.s64 + kStreamOffsets[2]));  // addi r3,r1,132
      step = ctx.r3.u64;     // mr r28,r3
      skate3::audio::GuestCall(ctx, base, sub_82B47010,
                               uint64_t(ctx.r1.s64 + kStreamOffsets[3]));  // addi r3,r1,148
      mode = ctx.r3.u64;     // r3 is carried straight into the next iteration's compare

      // cmpwi cr6,r28,0 ; bge cr6,0x82b47584 -- a negative step ends the walk, with 1.
      if (int32_t(uint32_t(step)) < 0) break;
    }
  }

  ctx.r3.s64 = result;                    // mr r3,r20 / mr r3,r31
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,272, on the full 64-bit register
}

// Outside this call's own frame the write set is fixed: five words at [+4, +24) and the byte at
// +32, whatever the walk does. Everything the decoder writes -- the shared cursor's byte pointer
// and each sub-object's accumulator, run length and flag (port_82B47658/port_82B47010 enumerate
// exactly those) -- lands in the 272-byte frame this call builds, which is its own and is not
// windowed. sub_82B46EA0 writes only its caller's frame scratch.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;  // the publish stores would go through null; not comparable
  if (uint64_t(object) + kPublishExact + 1 > 0x100000000ull) return false;  // would wrap 2^32

  // The frame writes are deliberately not declared, so the object must not sit inside the frame:
  // a frame store landing on a windowed byte would be neither rewound nor compared.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame_lo = entry_sp - kFrameBytes;
  if (uint64_t(object) + kPublishExact + 1 > frame_lo && uint64_t(object) + kPublishFirst < entry_sp) {
    return false;
  }

  spec.write(object + kPublishFirst, kPublishBytes);  // +4, +8, +12, +16, +20
  spec.write(object + kPublishExact, 1);             // stb r6,32(r30)

  // +4 is read once before the loop and +28 is re-read inside it; neither is an address, and +28
  // is outside both windows, so the walk stays derived from entry state.
  spec.read(object + kPublishBase, 4);
  spec.read(object + kMinLength, 4);
  const uint64_t stream = ctx.r4.u32;
  uint64_t len = kStreamReadSummary;
  if (stream + len > 0x100000000ull) len = 0x100000000ull - stream;
  spec.read(uint32_t(stream), uint32_t(len));
  return true;
}

}  // namespace port_82B474B8
SKATE3_PORT(82B474B8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
