// sub_82B33C08  role: hand the caller the next stream chunk for one 80-byte slot -- either pop a queued buffer from the slot's +40 through sub_82EC1290 and report its +8/+4, or read a {tag:u8, length:u24 big-endian} header straight off the raw cursor at +48 and advance that cursor (rewinding it from +52 instead when the tag is 69), writing the three results through the r5/r6/r7 out pointers; tags 69, 85 and (when the parallel record's +47 is set) 72 are control chunks, so the buffer is released through sub_82EC1378 and the whole thing retries   lifted: skate3_recomp.67.cpp:53365 (174 lines)
// STATUS: gate-1 (RtlEnterCriticalSection / RtlLeaveCriticalSection: sub_82EC1290 takes the lock, census depth 1 -- and it pops a queue, which a rewind cannot put back)
// calls/session: boot 1071  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33C08.md
namespace port_82B33C08 {

// The owner object (r3) -- the same two arrays sub_82B33870 addresses.
constexpr uint32_t kSlotArray = 96;      // lwz r9,96(r3)
constexpr uint32_t kParallelBias = 464;  // lhz r11,464(r3) -- a byte bias off the object itself

// A slot (r31 = *(owner+96) + index*80).
constexpr uint32_t kQueue = 40;   // u32 -- the queue sub_82EC1290 pops and sub_82EC1378 returns to
constexpr uint32_t kCursor = 48;  // u32 -- the raw read cursor, rewritten on the inline path
constexpr uint32_t kRewind = 52;  // u32 -- where the cursor restarts when the tag is kTagRestart

// The parallel record (r26 = owner + *(owner+464) + index*48).
constexpr uint32_t kAllowTag72 = 47;  // u8 -- gates the 72 tag being treated as a control chunk

// A queued buffer, as sub_82EC1290 returns it.
constexpr uint32_t kBufferSize = 4;  // -> the r6 out pointer
constexpr uint32_t kBufferData = 8;  // -> the r5 out pointer

// The chunk header at the raw cursor: one tag byte then a 24-bit big-endian length.
constexpr uint32_t kTagRestart = 69;  // 'E' -- rewinds the cursor from +52 on the inline path
constexpr uint32_t kTagGated = 72;    // 'H' -- a control chunk only when the record's +47 is set
constexpr uint32_t kTagSkip = 85;     // 'U' -- always a control chunk
// The ASCII reading of 69/72/85 is a reading, not established; the decimal values are what the
// compares use.

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)

// rlwinm r,N,0,31-N then add: both halves are zero-extended 32-bit quantities, but the add is
// 64 bit, so a carry out of bit 31 survives into the register the loads address through.
uint64_t ScaleByFive(uint64_t index) {
  const uint64_t times_four = uint64_t((uint32_t(index) << 2) & 0xFFFFFFFCu);  // rlwinm r8,r4,2,0,29
  return index + times_four;                                                   // add r7,r4,r8
}
uint64_t ScaleByThree(uint64_t index) {
  const uint64_t times_two = uint64_t((uint32_t(index) << 1) & 0xFFFFFFFEu);  // rlwinm r10,r4,1,0,30
  return index + times_two;                                                   // add r10,r4,r10
}
uint64_t TimesSixteen(uint64_t value) {
  return uint64_t((uint32_t(value) << 4) & 0xFFFFFFF0u);  // rlwinm r8,r10,4,0,27
}

REX_FUNC(Native) {
  // __savegprlr_26 spills r26-r31 and the link register into this call's own frame and
  // __restgprlr_26 reloads them; the harness compares r13-r31, so all six entry values have to
  // survive here. The spill slots are inside the 144 bytes the stwu claims, so they are not
  // windowed and are not reproduced.
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-144(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t owner_reg = ctx.r3.u64;
  const uint32_t owner = uint32_t(owner_reg);
  const uint64_t index_reg = ctx.r4.u64;
  const uint32_t out_data = ctx.r5.u32;    // mr r29,r5 -- receives the data pointer
  const uint32_t out_size = ctx.r6.u32;    // mr r28,r6 -- receives the size/length
  const uint32_t out_buffer = ctx.r7.u32;  // mr r30,r7 -- receives the queued buffer, or zero

  const uint32_t bias = REX_LOAD_U16(owner + kParallelBias);  // lhz r11,464(r3)
  const uint32_t array = REX_LOAD_U32(owner + kSlotArray);    // lwz r9,96(r3)
  // add r31,r10,r9
  const uint32_t slot = uint32_t(TimesSixteen(ScaleByFive(index_reg)) + uint64_t(array));
  // add r11,r11,r8 ; add r26,r11,r3
  const uint32_t record =
      uint32_t(uint64_t(bias) + TimesSixteen(ScaleByThree(index_reg)) + owner_reg);

  for (;;) {
    // loc_82B33C50
    const uint32_t queue = REX_LOAD_U32(slot + kQueue);  // lwz r3,40(r31)
    // cmplwi cr6,r3,0 ; beq cr6,0x82b33c80
    if (queue != 0) {
      // bl 0x82ec1290 -- takes the global critical section and pops the queue. Gate 1.
      skate3::audio::GuestCall(ctx, base, sub_82EC1290, uint64_t(queue));
      const uint32_t buffer = ctx.r3.u32;
      REX_STORE_U32(out_buffer, buffer);  // stw r3,0(r30)
      // cmplwi cr6,r3,0 ; beq cr6,0x82b33d2c
      if (buffer == 0) {
        // loc_82B33D2C -- an empty queue clears both other out words and returns.
        REX_STORE_U32(out_data, 0);  // stw r27,0(r29)
        REX_STORE_U32(out_size, 0);  // stw r27,0(r28)
        break;
      }
      const uint32_t data = REX_LOAD_U32(buffer + kBufferData);  // lwz r11,8(r3)
      const uint32_t size = REX_LOAD_U32(buffer + kBufferSize);  // lwz r10,4(r3)
      REX_STORE_U32(out_data, data);  // stw r11,0(r29)
      REX_STORE_U32(out_size, size);  // stw r10,0(r28)
    } else {
      // loc_82B33C80 -- the inline path: no queue, so the header is read straight off the cursor.
      const uint32_t cursor = REX_LOAD_U32(slot + kCursor);  // lwz r11,48(r31)
      REX_STORE_U32(out_buffer, 0);                          // stw r27,0(r30) -- before the loads
      const uint32_t b1 = REX_LOAD_U8(cursor + 1);           // lbz r10,1(r11)
      const uint32_t b2 = REX_LOAD_U8(cursor + 2);           // lbz r9,2(r11)
      const uint32_t high = __builtin_rotateleft32(b1, 8);   // rotlwi r8,r10,8 -- b1 << 8
      const uint32_t b3 = REX_LOAD_U8(cursor + 3);           // lbz r7,3(r11)
      const uint32_t tag = REX_LOAD_U8(cursor + 0);          // lbz r6,0(r11)
      const uint64_t pair = uint64_t(high | b2);             // or r5,r8,r9
      REX_STORE_U32(out_data, cursor);                       // stw r11,0(r29) -- the cursor itself
      // rlwinm r4,r5,8,0,23 ; or r10,r4,r7 -- the 24-bit big-endian length b1:b2:b3.
      const uint64_t length = uint64_t((uint32_t(pair) << 8) & 0xFFFFFF00u) | uint64_t(b3);
      REX_STORE_U32(out_size, uint32_t(length));  // stw r10,0(r28)
      // cmplwi cr6,r6,69 ; bne cr6,0x82b33cc0
      uint64_t advanced;
      if (tag == kTagRestart) {
        advanced = REX_LOAD_U32(slot + kRewind);  // lwz r11,52(r31) -- a rewind, not an advance
      } else {
        // loc_82B33CC0 -- add r11,r11,r10, in 64 bits on the cursor and the length.
        advanced = uint64_t(cursor) + length;
      }
      // loc_82B33CC4
      REX_STORE_U32(slot + kCursor, uint32_t(advanced));  // stw r11,48(r31)
    }

    // loc_82B33CC8 -- the data pointer is RELOADED from the out word, not carried from above, so
    // an out pointer that aliases something the stores touched is honoured.
    const uint32_t header = REX_LOAD_U32(out_data);  // lwz r11,0(r29)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b33d34
    if (header == 0) break;
    const uint32_t tag = REX_LOAD_U8(header + 0);  // lbz r11,0(r11)
    // Three compares in this order: 69, then 72, then 85; anything else returns.
    if (tag == kTagGated) {
      // loc_82B33D08 -- lbz r11,47(r26) ; beq cr6,0x82b33d34
      if (REX_LOAD_U8(record + kAllowTag72) == 0) break;
    } else if (tag != kTagRestart && tag != kTagSkip) {
      break;
    }

    // loc_82B33D14, and the identical block the 85 arm has of its own: release the buffer if
    // there is one, then retry from the top.
    const uint32_t held = REX_LOAD_U32(out_buffer);  // lwz r4,0(r30)
    // cmplwi cr6,r4,0 ; beq cr6,0x82b33c50 -- nothing to release, just retry.
    if (held != 0) {
      // lwz r3,40(r31) ; bl 0x82ec1378 -- the queue is re-read, not reused from the top.
      skate3::audio::GuestCall(ctx, base, sub_82EC1378, uint64_t(REX_LOAD_U32(slot + kQueue)),
                               uint64_t(held));
    }
  }

  // loc_82B33D34 -- no `li r3,...` on any path: the results are the three out words, and r3 is
  // whatever the last load or call left there.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
  ctx.lr = entry_lr;                      // __restgprlr_26 restores lr and r26-r31
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B33C08
SKATE3_PORT(82B33C08, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
