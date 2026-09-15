// sub_82B3DD90  role: build the ring window and the segment array for one block, fill them, then zero the tails   lifted: skate3_recomp.68.cpp:11003 (155 lines)
// STATUS: verified
// calls/session: boot 266604  play 379592  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3DD90.md

// port_82B3DF90's two fill-length functions are pure -- they read no guest memory -- so the window
// builder below calls them instead of copying them. Their definitions are later in this same
// translation unit (ports_68a.manifest.inc includes sub_82B3DF90.inc after this file), so they are
// forward-declared here, inline as they are defined. A future manifest split that puts the two in
// different TUs turns this into a link error, which is the failure mode to want: loud, not silent.
namespace port_82B3DF90 {
inline uint32_t Count0(uint32_t limit0, uint32_t cursor, uint32_t block, uint32_t consumed);
inline uint32_t Count1(uint32_t limit1, uint32_t cursor, uint32_t block, uint32_t consumed);
}  // namespace port_82B3DF90

namespace port_82B3DD90 {

// The 0.0f cell sub_82B3DF90 fills from (lis -32234 ; lfs 23056). Named locally rather than as
// port_82B3DF90::kFillConstant: the manifest includes ports in address order, so 82B3DD90 is
// compiled before 82B3DF90 is declared, and a cross-port reference fails to build.
constexpr uint32_t kFillConstantCell = 0x82165A10u;

// ---- copied verbatim from recomp/src/audio_ports/sub_82B3DF90.inc ----
// The manifest includes ports in address order, so this file is compiled before 82B3DF90's
// namespace exists and cannot name it. These are that port's fill-length rules, which this
// window builder must predict exactly. IF sub_82B3DF90.inc CHANGES THEM, CHANGE THEM HERE.
// subfic r10,rN,0 ; rlwinm r9,rN,1,31,31 ; addme r10,r9 ; and r8,r10,rN -- the branch-free
// max(n, 0): r10 is all-ones only when n > 0 (no borrow and sign bit clear), so the AND keeps
// n exactly then and yields 0 otherwise. Only the low 32 bits are consumed afterwards.
inline uint32_t ClampNonNegative(int32_t n) { return n > 0 ? static_cast<uint32_t>(n) : 0u; }

// stfs/stfsx f0: narrow the loaded double back to single, as the lifted form does per store.
inline void StoreF0(PPCContext& ctx, uint8_t* base, uint32_t ea) {
  PPCRegister temp{};
  temp.f32 = float(ctx.f0.f64);
  REX_STORE_U32(ea, temp.u32);
}
// How many floats buffer 0 gets, from the object's fields and r4 (32-bit wrap arithmetic).
inline uint32_t Count0(uint32_t limit0, uint32_t cursor, uint32_t block, uint32_t consumed) {
  int32_t n = static_cast<int32_t>((limit0 - cursor) - consumed);  // subf r8,r11,r9 ; subf r11,r4,r8
  const int32_t cap = static_cast<int32_t>(block + 255u);          // addi r10,r10,255
  if (!(n < cap)) n = cap;  // cmpw cr6,r11,r10 ; blt cr6 ; mr r11,r10
  return ClampNonNegative(n);
}
// How many floats buffer 1 gets: capped at block+127, then (redundantly, as the original does)
// at block+255.
inline uint32_t Count1(uint32_t limit1, uint32_t cursor, uint32_t block, uint32_t consumed) {
  int32_t n = static_cast<int32_t>((limit1 - consumed) - cursor);  // subf r8,r4,r11 ; subf r11,r10,r8
  int32_t cap = static_cast<int32_t>(block + 127u);                // addi r10,r9,127
  if (!(n < cap)) n = cap;  // cmpw cr6,r11,r10 ; blt cr6 ; mr r11,r10
  cap = static_cast<int32_t>(block + 255u);                        // addi r10,r9,255
  if (!(n < cap)) n = cap;  // cmpw cr6,r11,r10 ; blt cr6 ; mr r11,r10
  return ClampNonNegative(n);
}
// ---- end of copy ----


// The stream object (r3). Names shared with sub_82B3DB90, sub_82B3DC48, sub_82B3DEA8 and
// sub_82B3DF90, which take the same object; nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kObjRing = 0;         // the float ring's byte base
constexpr uint32_t kObjBlock = 16;       // sub_82B3DF90's kBlock: the per-segment cap base
constexpr uint32_t kObjSpanHigh = 20;    // sub_82B3DB90's kSpanHigh, sub_82B3DF90's kEnd
constexpr uint32_t kObjSpanLow = 24;     // sub_82B3DB90's kSpanLow
constexpr uint32_t kObjLimit0 = 40;      // sub_82B3DF90's kLimit0: segment 0's word count
constexpr uint32_t kObjLimit1 = 44;      // sub_82B3DF90's kLimit1: segment 1's word count
constexpr uint32_t kObjFillCursor = 36;  // sub_82B3DF90's kCursor: read by it, not by this body
constexpr uint32_t kObjPosition = 52;    // the running word position
constexpr uint32_t kObjTwoBuffers = 56;  // u8: non-zero means a second segment

// The 16-byte ring window, on this function's own frame, handed to sub_82B3DC48 as r4 and read
// by sub_82B3DB90.
constexpr uint32_t kWinBase = 0;         // sub_82B3DB90's kRingBase
constexpr uint32_t kWinEnd = 4;          // sub_82B3DB90's kRingEnd
constexpr uint32_t kWinLagged = 8;       // ring end less the lag; stored, read by nobody here
constexpr uint32_t kWinCursor = 12;      // sub_82B3DB90's kCursor

// The segment array, also on this frame, handed to sub_82B3DC48 as r5.
constexpr uint32_t kSegNeed = 0;
constexpr uint32_t kSegCap = 4;
constexpr uint32_t kSegRank = 8;    // written by sub_82B3DC48, not here
constexpr uint32_t kSegOut = 12;    // written by sub_82B3DC48, read back here
constexpr uint32_t kSegStride = 16;

// The caller's output record (r6), at the caller's frame + 80. sub_82B3DEA8 reads +16 back.
constexpr uint32_t kOutSeg0 = 4;
constexpr uint32_t kOutSeg1 = 8;
constexpr uint32_t kOutEnd = 16;

// The frame the original builds, and where the two structures sit in it.
constexpr uint32_t kFrameSize = 176;
constexpr uint32_t kWindowOffset = 80;
constexpr uint32_t kSegmentsOffset = 96;

// rlwinm rN,rX,2,0,29 -- the low word scaled by four, zero-extended back to 64 bits.
inline uint64_t WordsToBytes(uint64_t value) {
  return (uint64_t(uint32_t(value)) << 2) & 0xFFFFFFFCull;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;      // mr r31,r3 -- the low word, for the loads
  const uint64_t object_wide = ctx.r3.u64; // ...and all 64 bits, for mr r3,r31 at both calls
  const uint32_t out = ctx.r6.u32;         // mr r29,r6
  const uint64_t out_wide = ctx.r6.u64;    // ...and all 64 bits, for mr r5,r29
  const uint64_t consumed = ctx.r5.u64;    // mr r30,r5
  const int32_t block_index = ctx.r4.s32;

  // stwu r1,-176(r1). Unlike most ports this one has to build its frame: the ring window and the
  // segment array live in it and their addresses are passed to sub_82B3DC48, so they need real
  // guest memory at the same place. The frame is below the caller's r1, dies with the call, and
  // is what the brief excludes from the windows; r1 is restored before returning.
  const uint32_t frame = ctx.r1.u32 - kFrameSize;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;
  const uint32_t window = frame + kWindowOffset;
  const uint32_t segments = frame + kSegmentsOffset;

  const uint64_t position = REX_LOAD_U32(object + kObjPosition);   // lwz r6,52(r3)
  const uint64_t span = REX_LOAD_U32(object + kObjSpanHigh);       // lwz r3,20(r3)
  const uint64_t reach = position + consumed;                      // add r7,r6,r30, 64-bit
  // mullw r6,r3,r4 -- the full 64-bit product of the sign-extended low words.
  const uint64_t block = uint64_t(int64_t(int32_t(uint32_t(span))) * int64_t(block_index));
  const uint64_t limit0 = REX_LOAD_U32(object + kObjLimit0);       // lwz r4,40(r31)
  const uint64_t ring = REX_LOAD_U32(object + kObjRing);           // lwz r11,0(r31)
  const uint64_t lag = REX_LOAD_U32(object + kObjSpanLow);         // lwz r10,24(r31)
  const uint64_t cap_base = REX_LOAD_U32(object + kObjBlock);      // lwz r9,16(r31)
  const uint32_t two = REX_LOAD_U8(object + kObjTwoBuffers);       // lbz r28,56(r31)
  REX_STORE_U32(segments + kSegNeed, uint32_t(limit0));            // stw r4,96(r1)
  REX_STORE_U32(segments + kSegOut, 0);                            // stw r5,108(r1)
  // divw r4,r7,r3
  const uint64_t quotient = uint32_t(
      (int32_t(uint32_t(span)) &&
       !(int32_t(uint32_t(reach)) == INT32_MIN && int32_t(uint32_t(span)) == -1))
          ? int32_t(uint32_t(reach)) / int32_t(uint32_t(span))
          : 0);
  const uint64_t block_bytes = WordsToBytes(block);                // rlwinm r8,r6,2,0,29
  // mullw r6,r4,r3 -- again 64-bit.
  const uint64_t whole =
      uint64_t(int64_t(int32_t(uint32_t(quotient))) * int64_t(int32_t(uint32_t(span))));
  const uint64_t ring_start = block_bytes + ring;                  // add r11,r8,r11
  const uint64_t remainder = reach - whole;                        // subf r8,r6,r7
  // rotlwi r6,r7,1 ; addi r6,r6,-1
  const uint64_t doubled_less_one =
      uint64_t(int64_t(uint64_t(__builtin_rotateleft32(uint32_t(reach), 1))) - 1);
  REX_STORE_U32(window + kWinBase, uint32_t(ring_start));          // stw r11,80(r1)
  const uint64_t offset = remainder + lag;                         // add r4,r8,r10
  const uint64_t span_bytes = WordsToBytes(span);                  // rlwinm r7,r3,2,0,29
  const uint64_t lag_bytes = WordsToBytes(lag);                    // rlwinm r27,r10,2,0,29
  const uint64_t ring_end = span_bytes + ring_start;               // add r10,r7,r11
  const uint64_t offset_bytes = WordsToBytes(offset);              // rlwinm r8,r4,2,0,29
  const uint64_t guard = span & ~doubled_less_one;                 // andc r4,r3,r6
  REX_STORE_U32(window + kWinEnd, uint32_t(ring_end));             // stw r10,84(r1)
  // twllei r3,0 -- ppc_trap(type 0) only warns and returns, so the body carries on with the
  // divw's zero quotient, exactly as the lifted form does.
  if (int32_t(uint32_t(span)) == 0) ppc_trap(ctx, base, 0);
  const uint64_t cursor = offset_bytes + ring_start;               // add r3,r8,r11
  const uint64_t cap0 = uint64_t(int64_t(cap_base) + 255);         // addi r11,r9,255
  const uint64_t lagged = ring_end - lag_bytes;                    // subf r10,r27,r10
  REX_STORE_U32(window + kWinCursor, uint32_t(cursor));            // stw r3,92(r1)
  uint64_t count = 1;                                              // li r6,1
  REX_STORE_U32(segments + kSegCap, uint32_t(cap0));               // stw r11,100(r1)
  // twlgei r4,-1 -- the divide-overflow guard.
  if (int32_t(uint32_t(guard)) == -1) ppc_trap(ctx, base, 0);
  REX_STORE_U32(window + kWinLagged, uint32_t(lagged));            // stw r10,88(r1)

  // cmplwi cr6,r28,0 ; beq cr6 -- a second segment, with the tighter cap.
  if (two != 0) {
    const uint64_t limit1 = REX_LOAD_U32(object + kObjLimit1);     // lwz r11,44(r31)
    const uint64_t cap1 = uint64_t(int64_t(cap_base) + 127);       // addi r10,r9,127
    count = 2;                                                     // li r6,2
    REX_STORE_U32(segments + kSegStride + kSegOut, 0);             // stw r5,124(r1)
    REX_STORE_U32(segments + kSegStride + kSegCap, uint32_t(cap1)); // stw r10,116(r1)
    REX_STORE_U32(segments + kSegStride + kSegNeed, uint32_t(limit1)); // stw r11,112(r1)
  }

  // bl 0x82b3dc48 -- (object, window, segments, count)
  skate3::audio::GuestCall(ctx, base, sub_82B3DC48, object_wide, uint64_t(window),
                           uint64_t(segments), count);
  const uint64_t seg0_out = REX_LOAD_U32(segments + kSegOut);      // lwz r11,108(r1)
  REX_STORE_U32(out + kOutEnd, ctx.r3.u32);                        // stw r3,16(r29)
  const uint64_t seg1_out = REX_LOAD_U32(segments + kSegStride + kSegOut);  // lwz r10,124(r1)
  REX_STORE_U32(out + kOutSeg0, uint32_t(seg0_out));               // stw r11,4(r29)
  const uint32_t two_again = REX_LOAD_U8(object + kObjTwoBuffers);  // lbz r9,56(r31) -- re-read
  // subfic r8,r9,0 ; subfe r6,r7,r7 ; and r11,r6,r10 -- ~r7 + r7 is all ones whatever r7 holds
  // (r7 is volatile and sub_82B3DC48 clobbered it), so this is just the carry: the second
  // pointer survives only when the flag is set, and otherwise a zero is published. Note that
  // segments + kSegStride + kSegOut was only initialised on the flag's own path, which is why
  // the mask is needed at all.
  const uint64_t mask = 0xFFFFFFFFFFFFFFFFull + (two_again == 0 ? 1ull : 0ull);
  REX_STORE_U32(out + kOutSeg1, uint32_t(mask & seg1_out));        // stw r11,8(r29)

  // bl 0x82b3df90 -- (object, consumed, out): zeroes the tail of each segment.
  skate3::audio::GuestCall(ctx, base, sub_82B3DF90, object_wide, consumed, out_wide);

  ctx.r3.s64 = 256;                 // li r3,256
  ctx.r1.u32 = frame + kFrameSize;  // addi r1,r1,176
}

// One trip of sub_82B3DC48's loop, predicted.
//
// COPIED ARITHMETIC. This is `port_82B3DC48::MakeStep()` from
// recomp/src/audio_ports/sub_82B3DC48.inc with `need` and `cap` passed in as values instead of
// loaded from the segment array -- that array is on THIS function's frame, which does not exist
// when Windows() runs, which is also why the callee's own Windows() cannot simply be called here.
// A change to MakeStep requires the same change here. `RoundUp32` is referenced rather than copied.
struct SegStep {
  uint64_t out = 0;       // the word written at segments[rank] + kSegOut, and the fill base
  uint64_t limit = 0;     // r6 at the call to sub_82B3DB90
  uint64_t want = 0;      // r7 at the call
  uint64_t need_up = 0;   // r29
  bool over = false;      // cmpw cr6,r29,r31 ; bgt
};

inline SegStep PredictStep(uint64_t need, uint64_t cap, uint64_t dest, uint64_t running) {
  SegStep step{};
  step.need_up = port_82B3DC48::RoundUp32(need);
  const uint64_t pad = step.need_up - need;
  step.over = int32_t(uint32_t(step.need_up)) > int32_t(uint32_t(running));
  const uint64_t cap_up = port_82B3DC48::RoundUp32(cap + pad);
  const uint64_t slack = step.need_up - cap_up;
  if (!step.over) {
    step.out = WordsToBytes(pad) + dest;
    step.limit = step.need_up;
    step.want = cap_up;
  } else {
    const uint64_t behind = pad - step.need_up;
    const uint64_t left = running - slack;
    step.out = WordsToBytes(behind + running) + dest;
    // The branch-free max(left, 0) the original builds out of addc/subfe.
    const bool carry = uint32_t(uint32_t(left) + 0x80000000u) < uint32_t(left);
    step.want = (carry ? 0ull : ~0ull) & left;
    step.limit = running;
  }
  return step;
}

// What one sub_82B3DB90 call copies and returns.
//
// COPIED ARITHMETIC. This is `port_82B3DC48::PredictCopy()` from the same file, with the ring
// window's three words passed in as values for the same reason (the window is on this frame too).
// A change there requires the same change here.
struct SegRun {
  uint64_t want = 0;   // the words written at dest, and sub_82B3DB90's return value
  uint64_t src = 0;    // where the first run reads from, so the read set can name it
  int32_t first_run = 0;  // words from src to the ring end, before any wrap
  bool known = false;
};

inline SegRun PredictRun(uint64_t limit, uint64_t want_in, uint64_t cursor, uint32_t ring_base,
                         uint32_t ring_end, uint64_t span_high, uint64_t span_low) {
  SegRun run{};
  if (int32_t(uint32_t(want_in)) == 0) {  // returns zero, writes nothing
    run.known = true;
    return run;
  }
  run.want = (int32_t(uint32_t(want_in)) < int32_t(uint32_t(limit))) ? want_in : limit;
  uint64_t src = cursor - WordsToBytes(limit);
  bool inside = false;
  if (!(uint32_t(src) < ring_base)) inside = ring_end > uint32_t(src);
  if (!inside) src += WordsToBytes(span_high - span_low);
  const int32_t length = int32_t(uint32_t(uint64_t(ring_end) - src)) >> 2;
  run.src = src;
  run.first_run = length;
  if (length < 0) return run;                         // a ~4 GB first run; not enumerable
  if (int32_t(uint32_t(run.want)) < 0) return run;    // nor a negative second run
  run.known = true;
  return run;
}

// Every byte the call writes, from ENTRY state only.
//
// This returned false until 2026-09-12, on the grounds that sub_82B3DF90 zero-fills through the
// segment output pointers sub_82B3DC48 computes DURING the call. It does -- but every input of that
// computation is readable at entry, because the ring window and the segment array the callee reads
// are built by THIS function (the block above) out of object fields, r4 and r5. So the chain is
// replayed here: this function's own structures, then sub_82B3DC48's ranking and per-segment steps,
// then sub_82B3DF90's two fill lengths.
//
// Three files' arithmetic is duplicated, and each copy is labelled where it sits:
//   * this function's own window/segment construction, from Native() above -- repeated rather than
//     shared because the change that landed this was not allowed to touch Native();
//   * PredictStep and PredictRun, from sub_82B3DC48.inc (see their comments);
//   * NOT the fill lengths: Count0/Count1 read no memory, so they are called.
// A change to any of those four places requires a change here.
//
// What the prediction rests on is measured, not argued:
//   * sub_82B3DC48 is verified over 245,209 comparisons, and the `out` word this needs is one it
//     writes INSIDE its own declared window (`spec.write(step.entry + kSegOut, 4)`), so those
//     comparisons check MakeStep's `out` arithmetic byte for byte.
//   * sub_82B3DB90 is verified over 284,396 comparisons with kReturnR3, and its Native returns
//     MakePlan's `want` -- the value PredictRun mirrors -- so the register compare checks it.
//   * sub_82B3DF90 is verified over 245,209 comparisons, and its fill lengths come from object
//     fields and r4 alone.
// The one assumption left is that nothing in the call moves the object fields or the two structures
// under the prediction; the overlap refusals below are what enforce that, and they are the same
// refusals both callees' own window builders make.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t out = ctx.r6.u32;
  const uint64_t consumed = ctx.r5.u64;
  const int32_t block_index = ctx.r4.s32;
  if (object == 0 || out == 0) return false;  // the original stores through both

  spec.read(object, 64);  // every field either side reads lives in +0..+59
  spec.read(kFillConstantCell, 4);

  // This function's own stores: three words in the caller's record, +4 and +8 adjacent.
  spec.write(out + kOutSeg0, 8);  // stw r11,4(r29) ; stw r11,8(r29)
  spec.write(out + kOutEnd, 4);   // stw r3,16(r29)

  // ---- the ring window and the segment array, as Native() builds them on the frame ----
  const uint64_t position = REX_LOAD_U32(object + kObjPosition);
  const uint64_t span = REX_LOAD_U32(object + kObjSpanHigh);
  const uint64_t reach = position + consumed;
  const uint64_t block = uint64_t(int64_t(int32_t(uint32_t(span))) * int64_t(block_index));
  const uint64_t limit0 = REX_LOAD_U32(object + kObjLimit0);
  const uint64_t limit1 = REX_LOAD_U32(object + kObjLimit1);
  const uint64_t ring = REX_LOAD_U32(object + kObjRing);
  const uint64_t lag = REX_LOAD_U32(object + kObjSpanLow);
  const uint64_t cap_base = REX_LOAD_U32(object + kObjBlock);
  const uint32_t two = REX_LOAD_U8(object + kObjTwoBuffers);
  const uint64_t quotient = uint32_t(
      (int32_t(uint32_t(span)) &&
       !(int32_t(uint32_t(reach)) == INT32_MIN && int32_t(uint32_t(span)) == -1))
          ? int32_t(uint32_t(reach)) / int32_t(uint32_t(span))
          : 0);
  const uint64_t whole =
      uint64_t(int64_t(int32_t(uint32_t(quotient))) * int64_t(int32_t(uint32_t(span))));
  const uint64_t ring_start = WordsToBytes(block) + ring;
  const uint64_t remainder = reach - whole;
  const uint64_t offset = remainder + lag;
  const uint64_t ring_end = WordsToBytes(span) + ring_start;
  const uint64_t cursor = WordsToBytes(offset) + ring_start;
  // The three traps the body reproduces only warn, and `kWinLagged` is read by nobody, so neither
  // affects the write set.

  // The segment array as stored: need/cap per segment, both as 32-bit words.
  const uint32_t count = two != 0 ? 2u : 1u;
  const uint64_t need[port_82B3DC48::kMaxSegments] = {uint64_t(uint32_t(limit0)),
                                                      uint64_t(uint32_t(limit1))};
  const uint64_t cap[port_82B3DC48::kMaxSegments] = {
      uint64_t(uint32_t(uint32_t(cap_base) + 255u)), uint64_t(uint32_t(uint32_t(cap_base) + 127u))};

  // ---- sub_82B3DC48: the ranking, then one sub_82B3DB90 copy per segment ----
  uint32_t ranks[port_82B3DC48::kMaxSegments] = {0, 0};
  if (count == 2 && int32_t(uint32_t(need[0])) < int32_t(uint32_t(need[1]))) {
    ranks[0] = 1;
    ranks[1] = 0;
  } else if (count == 2) {
    ranks[1] = 1;
  }
  // The destination the segments are cut from: sub_82B3DC48's kObjDest, this object's +8. Named in
  // that file rather than here because this body never reads it.
  uint64_t dest = REX_LOAD_U32(object + port_82B3DC48::kObjDest);
  uint64_t running = port_82B3DC48::RoundUp32(need[ranks[0]]);
  uint64_t seg_out[port_82B3DC48::kMaxSegments] = {0, 0};
  const uint32_t frame = ctx.r1.u32 - kFrameSize;  // where the two structures will sit

  for (uint32_t i = 0; i < count && i < port_82B3DC48::kMaxSegments; i++) {
    const uint32_t which = ranks[i];
    const SegStep step = PredictStep(need[which], cap[which], dest, running);
    const SegRun run = PredictRun(step.limit, step.want, uint64_t(uint32_t(cursor)),
                                  uint32_t(ring_start), uint32_t(ring_end), span, lag);
    if (!run.known) return false;
    seg_out[which] = step.out;
    // The ring words this copy reads, split at the ring end as sub_82B3DC48's own Windows() splits
    // them. For replay only: without them a recorded vector holds the ring's header, not its data.
    if (uint32_t(run.want) != 0) {
      const uint64_t want = uint64_t(uint32_t(run.want));
      const uint64_t first = uint64_t(uint32_t(run.first_run));
      const uint64_t first_words = want < first ? want : first;
      spec.read(uint32_t(run.src), uint32_t(4ull * first_words));
      if (want > first_words) {
        spec.read(uint32_t(ring_start), uint32_t(4ull * (want - first_words)));
      }
    }

    const uint64_t bytes = 4ull * uint64_t(uint32_t(run.want));
    if (bytes > skate3::audio::kPortWatchCap) return false;
    const uint32_t at = uint32_t(dest);
    if (bytes != 0) {
      if (at == 0) return false;                                // a copy to guest 0
      if (uint64_t(at) + bytes > 0x100000000ull) return false;   // the span would wrap
      // sub_82B3DC48's own three refusals, plus the record: every one of these is re-read during
      // the call, so a copy landing on one moves the rest of the write set mid-flight.
      if (port_82B3DC48::Overlaps(at, bytes, object, 64)) return false;
      if (port_82B3DC48::Overlaps(at, bytes, frame + kWindowOffset, 16)) return false;
      if (port_82B3DC48::Overlaps(at, bytes, frame + kSegmentsOffset,
                                  kSegStride * port_82B3DC48::kMaxSegments)) {
        return false;
      }
      if (port_82B3DC48::Overlaps(at, bytes, out + kOutSeg0, 8)) return false;
      spec.write(at, uint32_t(bytes));
    }
    running = step.over ? (run.want + running) : (step.need_up - run.want);
    dest = WordsToBytes(run.want) + dest;
  }

  // ---- sub_82B3DF90: the tail fill through the two words published at out+4 and out+8 ----
  // Segment 1's word is masked to zero unless the flag is set, which is the `and` with the subfe
  // carry above; nothing in the call writes the flag (object + 56), and a copy that could have is
  // refused above.
  const uint32_t buffer0 = uint32_t(seg_out[0]);
  const uint32_t buffer1 = two != 0 ? uint32_t(seg_out[1]) : 0u;
  const uint32_t fill_end = uint32_t(span);  // sub_82B3DF90's kEnd is this object's +20
  const uint32_t fill_cursor = REX_LOAD_U32(object + kObjFillCursor);
  if (int32_t(fill_cursor) >= int32_t(fill_end)) return true;  // bgelr: it stores nothing

  const uint32_t consumed32 = uint32_t(consumed);
  const uint32_t block32 = uint32_t(cap_base);
  const uint64_t len0 =
      4ull * Count0(uint32_t(limit0), fill_cursor, block32, consumed32);
  const uint64_t len1 =
      buffer1 != 0 ? 4ull * Count1(uint32_t(limit1), fill_cursor, block32, consumed32)
                   : 0ull;
  if (len0 + len1 > skate3::audio::kPortWatchCap) return false;
  if (len0 != 0 && buffer0 == 0) return false;  // a fill through a null pointer
  if (uint64_t(buffer0) + len0 > 0x100000000ull) return false;
  if (uint64_t(buffer1) + len1 > 0x100000000ull) return false;
  // sub_82B3DF90 reloads the pointer slot before every store and the object fields between the two
  // fills, so a fill landing on either changes its own write set. Its own Windows() refuses these.
  if (port_82B3DC48::Overlaps(buffer0, len0, out + kOutSeg0, 8)) return false;
  if (port_82B3DC48::Overlaps(buffer0, len0, object + kObjBlock, 32)) return false;
  if (port_82B3DC48::Overlaps(buffer1, len1, out + kOutSeg0, 8)) return false;
  spec.write(buffer0, uint32_t(len0));
  spec.write(buffer1, uint32_t(len1));
  return true;
}

}  // namespace port_82B3DD90
SKATE3_PORT(82B3DD90, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
