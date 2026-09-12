// sub_82B3DEA8  role: write one block of words back into the stream's wrapping ring, in two runs   lifted: skate3_recomp.68.cpp:11159 (132 lines)
// STATUS: verified
// calls/session: boot 266604  play 379592  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3DEA8.md
namespace port_82B3DEA8 {

// The stream object (r3). Nothing in docs/rw_audio_structs.h names it; these names are shared
// with the rest of the family -- sub_82B3DB90, sub_82B3DC48, sub_82B3DD90, sub_82B3DF90 -- which
// all take the same object and touch the same cells.
constexpr uint32_t kObjRing = 0;        // the float ring's byte base
constexpr uint32_t kObjSpanHigh = 20;   // sub_82B3DB90's kSpanHigh, sub_82B3DF90's kEnd
constexpr uint32_t kObjSpanLow = 24;    // sub_82B3DB90's kSpanLow; the ring spans high-low words
constexpr uint32_t kObjPosition = 52;   // the running word position this block is placed from

// The caller's output record (r6): +16 is one past the last word produced. sub_82B3DD90 writes
// it from sub_82B3DC48's return and the caller advances it block by block.
constexpr uint32_t kOutEnd = 16;

// rlwinm rN,rX,2,0,29 -- the low word scaled by four, zero-extended back to 64 bits.
inline uint64_t WordsToBytes(uint64_t value) {
  return (uint64_t(uint32_t(value)) << 2) & 0xFFFFFFFCull;
}

// Everything both copies need. Native and Windows share it so they cannot drift apart, and it is
// all fixed at entry: every load in the original happens before the first memcpy, and the second
// copy's arguments come from non-volatile registers, never from a reload.
struct Plan {
  uint64_t span = 0;        // r3 -- LOAD(obj+20), the divisor the twllei guards
  uint64_t guard = 0;       // r8 -- the twlgei operand
  uint64_t cursor = 0;      // r3 at the branch: where the first run is written, wrap applied
  uint64_t ring_start = 0;  // r31 -- where the wrapped run is written
  uint64_t source = 0;      // r27 -- the block, counted back from the output record's end
  bool copies = false;      // false on the bge: the call writes nothing at all
  uint64_t first_bytes = 0; // r29 at the first copy
  uint64_t rest_bytes = 0;  // r5 at the second copy
};

inline Plan MakePlan(PPCContext& ctx, uint8_t* base) {
  Plan plan{};
  const uint32_t object = ctx.r3.u32;                                    // mr r11,r3
  const uint64_t count = ctx.r5.u64;                                     // mr r30,r5
  const uint64_t position = REX_LOAD_U32(object + kObjPosition);         // lwz r7,52(r3)
  const uint64_t span = REX_LOAD_U32(object + kObjSpanHigh);             // lwz r3,20(r3)
  // rotlwi r10,r7,1 -- the position doubled, on the low word only.
  const uint64_t doubled = __builtin_rotateleft32(uint32_t(position), 1);
  const uint64_t source_end = REX_LOAD_U32(ctx.r6.u32 + kOutEnd);        // lwz r6,16(r6)
  // mullw r4,r3,r4 -- the full 64-bit product of the sign-extended low words, so a carry into
  // bit 32 survives into the address arithmetic below.
  const uint64_t block = uint64_t(int64_t(int32_t(uint32_t(span))) * int64_t(ctx.r4.s32));
  const uint64_t ring = REX_LOAD_U32(object + kObjRing);                 // lwz r9,0(r11)
  const uint64_t lag = REX_LOAD_U32(object + kObjSpanLow);               // lwz r8,24(r11)
  // divw r31,r7,r3
  const uint64_t quotient = uint32_t(
      (int32_t(uint32_t(span)) &&
       !(int32_t(uint32_t(position)) == INT32_MIN && int32_t(uint32_t(span)) == -1))
          ? int32_t(uint32_t(position)) / int32_t(uint32_t(span))
          : 0);
  const uint64_t doubled_less_one = uint64_t(int64_t(doubled) - 1);      // addi r29,r10,-1
  const uint64_t block_bytes = WordsToBytes(block);                      // rlwinm r10,r4,2,0,29
  // mullw r4,r31,r3 -- again 64-bit; the remainder below is a 64-bit subtraction of it.
  const uint64_t whole =
      uint64_t(int64_t(int32_t(uint32_t(quotient))) * int64_t(int32_t(uint32_t(span))));
  plan.ring_start = block_bytes + ring;                                  // add r31,r10,r9
  const uint64_t remainder = position - whole;                           // subf r10,r4,r7
  const uint64_t span_bytes = WordsToBytes(span);                        // rlwinm r9,r3,2,0,29
  const uint64_t offset = remainder + lag;                               // add r10,r10,r8
  plan.guard = span & ~doubled_less_one;                                 // andc r8,r3,r29
  const uint64_t offset_bytes = WordsToBytes(offset);                    // rlwinm r10,r10,2,0,29
  const uint64_t count_bytes = WordsToBytes(count);                      // rlwinm r5,r5,2,0,29
  const uint64_t cursor = offset_bytes + plan.ring_start;                // add r10,r10,r31
  plan.span = span;                                                      // twllei r3,0 operand
  plan.source = source_end - count_bytes;                                // subf r27,r5,r6
  const uint64_t ring_end = span_bytes + plan.ring_start;                // add r9,r9,r31
  plan.cursor = cursor;                                                  // mr r3,r10

  // cmplw cr6,r10,r31 ; blt cr6 ; cmplw cr6,r9,r10 ; bgt cr6 -- a cursor below the ring's start,
  // or at or past its end, is off the window and gets the wrap span added. Both compares are
  // unsigned on the low words.
  if (uint32_t(cursor) < uint32_t(plan.ring_start) || !(uint32_t(ring_end) > uint32_t(cursor))) {
    const uint64_t high = REX_LOAD_U32(object + kObjSpanHigh);           // lwz r8,20(r11)
    const uint64_t low = REX_LOAD_U32(object + kObjSpanLow);             // lwz r7,24(r11)
    // subf r6,r7,r8 ; rlwinm r11,r6,2,0,29 ; add r3,r11,r10
    plan.cursor = WordsToBytes(high - low) + cursor;
  }

  // subf r11,r31,r9 ; srawi r10,r11,2 ; cmpw cr6,r30,r10 ; bge cr6 -- a block at least as long as
  // the ring itself writes nothing.
  const int32_t ring_words = int32_t(uint32_t(ring_end - plan.ring_start)) >> 2;
  if (int32_t(uint32_t(count)) >= ring_words) return plan;
  plan.copies = true;

  // subf r11,r3,r9 ; srawi r28,r11,2 ; cmpw cr6,r30,r28 ; blt cr6 ; mr r28,r30 -- the first run
  // reaches the ring's end, clamped to the block.
  uint64_t run = uint64_t(int64_t(int32_t(uint32_t(ring_end - plan.cursor)) >> 2));
  if (int32_t(uint32_t(count)) < int32_t(uint32_t(run))) run = count;
  plan.first_bytes = WordsToBytes(run);                                  // rlwinm r29,r28,2,0,29
  // subf r11,r28,r30 ; rlwinm r5,r11,2,0,29
  plan.rest_bytes = WordsToBytes(count - run);
  return plan;
}

REX_FUNC(Native) {
  const Plan plan = MakePlan(ctx, base);
  // twllei r3,0 -- a zero span. ppc_trap(type 0) only warns and returns, so the body carries on
  // with the divw's zero quotient, exactly as the lifted form does.
  if (int32_t(uint32_t(plan.span)) == 0) ppc_trap(ctx, base, 0);
  // twlgei r8,-1 -- the divide-overflow guard.
  if (int32_t(uint32_t(plan.guard)) == -1) ppc_trap(ctx, base, 0);

  if (!plan.copies) {
    // bge cr6,0x82b3df88 -- straight to the epilogue with the cursor still in r3.
    ctx.r3.u64 = plan.cursor;
    return;
  }
  // bl 0x82edf460 -- memcpy(cursor, source, first_bytes): the run that reaches the ring's end.
  skate3::audio::GuestCall(ctx, base, sub_82EDF460, plan.cursor, plan.source, plan.first_bytes);
  // subf r11,r28,r30 ; add r4,r29,r27 -- 64-bit on two zero-extended words, carry kept.
  const uint64_t wrapped_source = plan.first_bytes + plan.source;
  // bl 0x82edf460 -- the remainder from the ring's start. Length zero when the first run took
  // the whole block; sub_82EDF460 still returns its own r3, which is what this function leaves.
  skate3::audio::GuestCall(ctx, base, sub_82EDF460, plan.ring_start, wrapped_source,
                           plan.rest_bytes);
  // No `mr r3,...` follows the second call: r3 is whatever sub_82EDF460 returned, its own
  // destination. Left implicit so a ported memcpy is exercised the same way.
}

// Writes: {cursor, first_bytes} and {ring_start, rest_bytes}, both performed by sub_82EDF460 (the
// XDK vector memcpy, which writes exactly [r3, r3+r5) on every path -- its dcbzl lines round up
// into the destination and are then fully overwritten, and its spills go below its own r1). This
// function has no stores of its own outside its frame.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  spec.read(object + kObjRing, 4);
  spec.read(object + kObjSpanHigh, 8);   // +20, +24
  spec.read(object + kObjPosition, 4);
  spec.read(ctx.r6.u32 + kOutEnd, 4);

  const Plan plan = MakePlan(ctx, base);
  if (!plan.copies) return true;  // the bge path stores nothing; r3 is still compared

  const uint64_t total = plan.first_bytes + plan.rest_bytes;
  // A negative word count anywhere above turns into a ~4 GB length here; over the budget, so
  // not comparable rather than guessed at.
  if (total > skate3::audio::kPortWatchCap) return false;
  const uint32_t first = uint32_t(plan.cursor);
  const uint32_t second = uint32_t(plan.ring_start);
  if (uint64_t(first) + plan.first_bytes > 0x100000000ull) return false;  // the span would wrap
  if (uint64_t(second) + plan.rest_bytes > 0x100000000ull) return false;
  if (first == 0 && plan.first_bytes != 0) return false;  // a copy to guest 0; not comparable
  if (second == 0 && plan.rest_bytes != 0) return false;
  spec.write(first, uint32_t(plan.first_bytes));
  spec.write(second, uint32_t(plan.rest_bytes));
  const uint32_t source = uint32_t(plan.source);
  if (uint64_t(source) + total <= 0x100000000ull) spec.read(source, uint32_t(total));
  return true;
}

}  // namespace port_82B3DEA8
SKATE3_PORT(82B3DEA8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
