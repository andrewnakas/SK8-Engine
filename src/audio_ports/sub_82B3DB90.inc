// sub_82B3DB90  role: copy up to n words out of a wrapping ring, in two runs   lifted: skate3_recomp.68.cpp:10708 (109 lines)
// STATUS: verified
// calls/session: boot 940743  play 1306629  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3DB90.md
namespace port_82B3DB90 {

// Nothing in docs/rw_audio_structs.h names either structure; the names describe the arithmetic.
//
// r4 -- the ring: base, end, and the cursor the read is measured back from.
constexpr uint32_t kRingBase = 0;
constexpr uint32_t kRingEnd = 4;
constexpr uint32_t kCursor = 12;
// r3 -- the owner: two counts whose difference, in words, is the ring's wrap span.
constexpr uint32_t kSpanHigh = 20;
constexpr uint32_t kSpanLow = 24;

// rlwinm rN,rX,2,0,29 -- the low word scaled by four, the result zero-extended to 64 bits.
inline uint64_t WordsToBytes(uint64_t value) {
  return (uint64_t(uint32_t(value)) << 2) & 0xFFFFFFFCull;
}

// Everything the two copies need, all of it derivable from entry state: the body and the window
// builder share this so they cannot drift apart.
struct Plan {
  bool empty = false;      // r7 == 0: nothing is copied and zero is returned
  uint64_t want = 0;       // r31: the clamped word count, and the return value
  uint64_t src = 0;        // r4 at the first copy
  uint64_t run_bytes = 0;  // r30: the first copy's length
  uint64_t rest_bytes = 0; // r5 at the second copy
};

inline Plan MakePlan(PPCContext& ctx, uint8_t* base) {
  Plan plan{};
  // cmpwi cr6,r7,0 ; bne cr6 -- a zero request copies nothing.
  if (ctx.r7.s32 == 0) {
    plan.empty = true;
    return plan;
  }
  // cmpw cr6,r31,r6 ; blt cr6 ; mr r31,r6 -- clamp the request to the window. The compare is
  // signed on the low words and the mr keeps whichever register's full 64 bits.
  plan.want = (ctx.r7.s32 < ctx.r6.s32) ? ctx.r7.u64 : ctx.r6.u64;

  const uint32_t ring = ctx.r4.u32;                                 // mr r28,r4
  const uint32_t cursor = REX_LOAD_U32(ring + kCursor);             // lwz r11,12(r28)
  const uint64_t behind = WordsToBytes(ctx.r6.u32);                 // rlwinm r10,r6,2,0,29
  const uint32_t ring_base = REX_LOAD_U32(ring + kRingBase);        // lwz r9,0(r28)
  // subf r4,r10,r11 is 64-bit: a borrow leaves the high word set, and that value is what the
  // wrap add below and the copy's source register carry on with. Only the compares truncate.
  uint64_t src = uint64_t(cursor) - behind;

  // cmplw cr6,r4,r9 ; blt cr6 -- below the base wraps; otherwise the end has to be above it.
  bool inside = false;
  if (!(uint32_t(src) < ring_base)) {
    const uint32_t ring_end = REX_LOAD_U32(ring + kRingEnd);  // lwz r11,4(r28)
    inside = ring_end > uint32_t(src);                        // cmplw cr6,r11,r4 ; bgt cr6
  }
  if (!inside) {
    const uint32_t high = REX_LOAD_U32(ctx.r3.u32 + kSpanHigh);  // lwz r11,20(r3)
    const uint32_t low = REX_LOAD_U32(ctx.r3.u32 + kSpanLow);    // lwz r10,24(r3)
    // subf r9,r10,r11 ; rlwinm r11,r9,2,0,29 ; add r4,r11,r4 -- all 64-bit but for the rlwinm.
    src += WordsToBytes(uint64_t(high) - uint64_t(low));
  }
  plan.src = src;

  const uint32_t ring_end = REX_LOAD_U32(ring + kRingEnd);  // lwz r11,4(r28) -- read again
  // subf r10,r4,r11 ; srawi r29,r10,2 -- words left to the end, on the low word, signed and
  // sign-extended back to 64 bits.
  const uint32_t left = uint32_t(uint64_t(ring_end) - src);
  uint64_t run = uint64_t(int64_t(static_cast<int32_t>(left) >> 2));
  // cmpw cr6,r31,r29 ; blt cr6 ; mr r29,r31 -- min, signed on the low words.
  if (static_cast<int32_t>(uint32_t(plan.want)) < static_cast<int32_t>(uint32_t(run))) {
    run = plan.want;
  }
  plan.run_bytes = WordsToBytes(run);               // rlwinm r30,r29,2,0,29
  plan.rest_bytes = WordsToBytes(plan.want - run);  // subf r11,r29,r31 ; rlwinm r5,r11,2,0,29
  return plan;
}

REX_FUNC(Native) {
  const uint32_t ring = ctx.r4.u32;  // mr r28,r4
  const uint64_t dst = ctx.r5.u64;   // mr r27,r5
  const Plan plan = MakePlan(ctx, base);
  if (plan.empty) {
    ctx.r3.s64 = 0;  // li r3,0
    return;
  }
  // bl 0x82edf460 -- memcpy(dst, src, run_bytes): the run that reaches the end of the ring.
  skate3::audio::GuestCall(ctx, base, sub_82EDF460, dst, plan.src, plan.run_bytes);
  // add r3,r30,r27 -- 64-bit on two zero-extended words, so the carry into bit 32 is kept.
  const uint64_t tail = plan.run_bytes + dst;
  // lwz r4,0(r28) -- re-read AFTER the first copy, as the original does, so a copy that lands
  // on the ring descriptor feeds the same wrapped source it would on hardware.
  const uint32_t wrapped = REX_LOAD_U32(ring + kRingBase);
  // bl 0x82edf460 -- the remainder, from the base. Length zero when the run covered it all.
  skate3::audio::GuestCall(ctx, base, sub_82EDF460, tail, uint64_t(wrapped), plan.rest_bytes);
  ctx.r3.u64 = plan.want;  // mr r3,r31
}

// This function has no stores of its own beyond its frame: everything written is written by
// sub_82EDF460 (the XDK vector memcpy, which writes exactly [r3, r3+r5) on every path -- its
// dcbzl lines are rounded up into the destination and then fully overwritten, and its own
// spills go below its r1). So the write set is the two destination runs:
//     {r5, 4*run}  and  {r5 + 4*run, 4*(want-run)}
// both lengths fixed in registers before the first copy, so nothing here depends on a value
// produced during the call -- the second copy's SOURCE is re-read from the ring mid-call, but a
// source does not move the write set.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint64_t dst = ctx.r5.u64;
  spec.read(ctx.r4.u32 + kRingBase, 16);     // +0 base, +4 end, +12 cursor
  spec.read(ctx.r3.u32 + kSpanHigh, 8);      // +20, +24

  const Plan plan = MakePlan(ctx, base);
  if (plan.empty) return true;  // the early return stores nothing

  // A negative word count turns into a ~4 GB length here (the original would then run a copy
  // that size); over the budget, so not comparable.
  if (plan.run_bytes + plan.rest_bytes > skate3::audio::kPortWatchCap) return false;
  const uint32_t first = uint32_t(dst);
  const uint32_t second = uint32_t(plan.run_bytes + dst);
  if (uint64_t(first) + plan.run_bytes > 0x100000000ull) return false;   // the span would wrap
  if (uint64_t(second) + plan.rest_bytes > 0x100000000ull) return false;
  if (first == 0 && plan.run_bytes != 0) return false;   // a copy to guest 0; not comparable
  if (second == 0 && plan.rest_bytes != 0) return false;
  spec.write(first, uint32_t(plan.run_bytes));
  spec.write(second, uint32_t(plan.rest_bytes));
  return true;
}

}  // namespace port_82B3DB90
SKATE3_PORT(82B3DB90, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
