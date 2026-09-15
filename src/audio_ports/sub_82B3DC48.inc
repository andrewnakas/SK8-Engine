// sub_82B3DC48  role: rank the output segments by size, then fill each one out of the ring   lifted: skate3_recomp.68.cpp:10818 (184 lines)
// STATUS: verified
// calls/session: boot 533208  play 759184  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3DC48.md
namespace port_82B3DC48 {

// The stream object (r3). Names shared with sub_82B3DB90, sub_82B3DD90, sub_82B3DEA8 and
// sub_82B3DF90, which take the same object; nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kObjDest = 8;        // the destination buffer the segments are cut from
constexpr uint32_t kObjSpanHigh = 20;   // sub_82B3DB90's kSpanHigh, sub_82B3DF90's kEnd
constexpr uint32_t kObjSpanLow = 24;    // sub_82B3DB90's kSpanLow

// The 16-byte ring window (r4), built by sub_82B3DD90 on its frame and read by sub_82B3DB90.
constexpr uint32_t kWinBase = 0;        // sub_82B3DB90's kRingBase
constexpr uint32_t kWinEnd = 4;         // sub_82B3DB90's kRingEnd
constexpr uint32_t kWinCursor = 12;     // sub_82B3DB90's kCursor

// The segment array (r5): 16 bytes per segment, at most two.
constexpr uint32_t kSegNeed = 0;        // words wanted; the ranking key
constexpr uint32_t kSegCap = 4;         // the cap sub_82B3DD90 derived from the object's block
constexpr uint32_t kSegRank = 8;        // which segment this slot selects; written here
constexpr uint32_t kSegOut = 12;        // where the segment's words landed; written here
constexpr uint32_t kSegStride = 16;
constexpr uint32_t kMaxSegments = 2;    // sub_82B3DD90, the only caller, passes 1 or 2

// rlwinm rN,rX,2,0,29 -- the low word scaled by four, zero-extended back to 64 bits.
inline uint64_t WordsToBytes(uint64_t value) {
  return (uint64_t(uint32_t(value)) << 2) & 0xFFFFFFFCull;
}

// addi rN,rX,31 ; rlwinm rN,rN,0,0,26 -- a word count rounded up to a multiple of 32, on the
// low word.
inline uint64_t RoundUp32(uint64_t value) {
  return uint64_t(uint32_t(uint32_t(value) + 31u) & 0xFFFFFFE0u);
}

// One trip of the loop, up to the call. `rank` is the selector: the body reloads it from the
// slot the way the original does, the window builder predicts it (it is written earlier in this
// same call, from values readable at entry).
struct Step {
  uint32_t entry = 0;     // r11 -- &segments[rank]
  uint64_t out = 0;       // the word stored at entry + kSegOut
  uint64_t limit = 0;     // r6 at the call to sub_82B3DB90
  uint64_t want = 0;      // r7 at the call
  uint64_t need_up = 0;   // r29
  bool over = false;      // cmpw cr6,r29,r31 ; bgt -- the second branch
};

inline Step MakeStep(uint8_t* base, uint32_t segments_base, uint32_t rank, uint64_t dest,
                     uint64_t running) {
  Step step{};
  // rlwinm r11,r11,4,0,27 ; add r11,r11,r26 -- 64-bit add, but the loads use the low word.
  step.entry = uint32_t(uint64_t((rank << 4) & 0xFFFFFFF0u) + uint64_t(segments_base));
  const uint64_t need = REX_LOAD_U32(step.entry + kSegNeed);              // lwz r10,0(r11)
  const uint64_t cap = REX_LOAD_U32(step.entry + kSegCap);                // lwz r9,4(r11)
  step.need_up = RoundUp32(need);                                         // addi r8,r10,31 ; rlwinm r29
  const uint64_t pad = step.need_up - need;                               // subf r10,r10,r29
  // cmpw cr6,r29,r31 -- signed on the low words.
  step.over = int32_t(uint32_t(step.need_up)) > int32_t(uint32_t(running));
  // add r9,r9,r10 ; addi r7,r9,31 ; rlwinm r7,r7,0,0,26
  const uint64_t cap_up = RoundUp32(cap + pad);
  const uint64_t slack = step.need_up - cap_up;                           // subf r9,r7,r29
  if (!step.over) {
    step.out = WordsToBytes(pad) + dest;   // rlwinm r10,r10,2,0,29 ; add r10,r10,r30
    step.limit = step.need_up;             // mr r6,r29
    step.want = cap_up;                    // r7 still holds roundup32(cap + pad)
  } else {
    const uint64_t behind = pad - step.need_up;        // subf r10,r29,r10
    const uint64_t left = running - slack;             // subf r8,r9,r31 ; subf r3,r24,r8
    // add r7,r10,r31 ; rlwinm r9,r7,2,0,29 ; add r4,r9,r30
    step.out = WordsToBytes(behind + running) + dest;
    // xoris r5,r24,32768 ; addc r11,r3,r5 ; subfe r9,r24,r24 ; and r7,r9,r8 -- the branch-free
    // max(left, 0): the carry out of left + 0x80000000 is set exactly when left's low word is
    // negative, and subfe then builds 0 or all-ones from it. All 64 bits of left survive.
    const bool carry = uint32_t(uint32_t(left) + 0x80000000u) < uint32_t(left);
    step.want = (carry ? 0ull : ~0ull) & left;
    step.limit = running;                  // mr r6,r31
  }
  return step;
}

REX_FUNC(Native) {
  const uint64_t object = ctx.r3.u64;         // mr r28,r3 -- all 64 bits, and mr r3,r28 back
  const uint64_t window = ctx.r4.u64;         // mr r27,r4
  const uint32_t segments_base = ctx.r5.u32;  // mr r26,r5
  const int32_t segments = ctx.r6.s32;
  const uint64_t segments_wide = ctx.r6.u64;

  REX_STORE_U32(segments_base + kSegRank, 0);  // stw r24,8(r5) -- r24 is zero throughout
  // cmpwi cr6,r6,2 ; bne cr6 -- two segments get ranked, the larger first.
  if (segments == 2) {
    const uint32_t first = REX_LOAD_U32(segments_base + kSegNeed);                 // lwz r10,0(r5)
    const uint32_t second = REX_LOAD_U32(segments_base + kSegStride + kSegNeed);   // lwz r9,16(r5)
    // cmpw cr6,r10,r9 ; bge cr6 -- signed on the low words.
    if (int32_t(first) < int32_t(second)) {
      REX_STORE_U32(segments_base + kSegStride + kSegRank, 0);  // stw r24,24(r5)
      REX_STORE_U32(segments_base + kSegRank, 1);               // stw r10,0(r11)
    } else {
      REX_STORE_U32(segments_base + kSegRank, 0);               // stw r24,0(r11) -- zero again
      REX_STORE_U32(segments_base + kSegStride + kSegRank, 1);  // stw r10,24(r26)
    }
  }
  const uint32_t lead = REX_LOAD_U32(segments_base + kSegRank);   // lwz r10,0(r11)
  uint64_t dest = REX_LOAD_U32(uint32_t(object) + kObjDest);      // lwz r30,8(r28)
  // rlwinm r9,r10,4,0,27 ; lwzx r10,r9,r26 ; addi r8,r10,31 ; rlwinm r31,r8,0,0,26
  uint64_t running =
      RoundUp32(REX_LOAD_U32(uint32_t(((lead << 4) & 0xFFFFFFF0u) + segments_base)));
  if (segments <= 0) {   // ble cr6,0x82b3dd80 -- cr6 is still the cmpwi against zero
    ctx.r3.u64 = dest;   // mr r3,r30
    return;
  }

  uint32_t rank_slot = segments_base + kSegRank;  // mr r25,r11
  uint64_t left = segments_wide;                  // mr r23,r6
  do {
    const uint32_t rank = REX_LOAD_U32(rank_slot);   // lwz r11,0(r25)
    const Step step = MakeStep(base, segments_base, rank, dest, running);
    // stw r10,12(r11) on the first branch, stw r4,12(r11) on the second -- both before the call.
    REX_STORE_U32(step.entry + kSegOut, uint32_t(step.out));
    // bl 0x82b3db90 -- (object, window, dest, limit, want). r10 is also set to zero on the
    // second branch; sub_82B3DB90 does not read it, and it is not an ABI-preserved register.
    skate3::audio::GuestCall(ctx, base, sub_82B3DB90, object, window, dest, step.limit, step.want);
    // subf r31,r3,r29 on the first branch, add r31,r3,r31 on the second.
    running = step.over ? (ctx.r3.u64 + running) : (step.need_up - ctx.r3.u64);
    // rlwinm r11,r3,2,0,29 ; add r30,r11,r30
    dest = WordsToBytes(ctx.r3.u64) + dest;
    left = uint64_t(int64_t(left) - 1);   // addic. r23,r23,-1
    rank_slot += kSegStride;              // addi r25,r25,16
  } while (int32_t(uint32_t(left)) != 0); // bne 0x82b3dcc4
  ctx.r3.u64 = dest;   // mr r3,r30
}

// What one sub_82B3DB90 call writes and returns. Its own port (sub_82B3DB90.inc, MakePlan) is the
// reference; this is the same arithmetic duplicated, because a window builder may not run the
// callee. Keep the two in step. The call writes its two runs back to back, covering exactly
// 4*want bytes at dest -- but only while the first run is a sane word count, so a run that would
// go negative (a ~4 GB memcpy length) refuses instead of being guessed at.
struct Copy {
  uint64_t want = 0;   // the return value, and the words written at dest
  // Where sub_82B3DB90 reads from, so this call's read set can name it. Declaring the source is
  // what makes a recorded vector replayable: without it the recording holds the ring's header and
  // not its data, and 206 of 206 vectors were unreplayable at the ring.
  uint64_t src = 0;
  int32_t run = 0;     // words from src to the ring end, before any wrap
  bool known = false;
};

inline Copy PredictCopy(uint8_t* base, uint32_t object, uint32_t window, uint64_t limit,
                        uint64_t want_in) {
  Copy copy{};
  if (int32_t(uint32_t(want_in)) == 0) {  // cmpwi cr6,r7,0 -- returns zero, writes nothing
    copy.known = true;
    return copy;
  }
  // cmpw cr6,r31,r6 ; blt cr6 ; mr r31,r6
  copy.want = (int32_t(uint32_t(want_in)) < int32_t(uint32_t(limit))) ? want_in : limit;

  const uint64_t cursor = REX_LOAD_U32(window + kWinCursor);
  const uint32_t ring_base = REX_LOAD_U32(window + kWinBase);
  uint64_t src = cursor - WordsToBytes(limit);   // subf r4,r10,r11, 64-bit
  bool inside = false;
  if (!(uint32_t(src) < ring_base)) {
    inside = REX_LOAD_U32(window + kWinEnd) > uint32_t(src);
  }
  if (!inside) {
    const uint64_t high = REX_LOAD_U32(object + kObjSpanHigh);
    const uint64_t low = REX_LOAD_U32(object + kObjSpanLow);
    src += WordsToBytes(high - low);
  }
  const uint32_t ring_end = REX_LOAD_U32(window + kWinEnd);   // re-read, as the original does
  const int32_t run = int32_t(uint32_t(uint64_t(ring_end) - src)) >> 2;  // srawi r29,r10,2
  copy.src = src;
  copy.run = run;
  if (run < 0) return copy;                              // known stays false
  if (int32_t(uint32_t(copy.want)) < 0) return copy;     // the second run would be ~4 GB
  copy.known = true;
  return copy;
}

inline bool Overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
}

// Writes: the rank word of every slot the ranking touches, one output word per segment, and the
// destination run of every sub_82B3DB90 call. Everything derives from entry state: the ranking
// reads only the two need fields, and the per-segment arithmetic is pure.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t window = ctx.r4.u32;
  const uint32_t segments_base = ctx.r5.u32;
  const int32_t segments = ctx.r6.s32;

  spec.read(object + kObjDest, 4);
  spec.read(object + kObjSpanHigh, 8);
  spec.read(window + kWinBase, 16);
  spec.read(segments_base, kSegStride * kMaxSegments);

  spec.write(segments_base + kSegRank, 4);  // stw r24,8(r5), on every path
  if (segments == 2) spec.write(segments_base + kSegStride + kSegRank, 4);
  if (segments <= 0) return true;  // ble: the unconditional rank store is all of it
  // Three or more segments would read a rank word this call never wrote; not enumerable.
  if (segments > int32_t(kMaxSegments)) return false;

  // The ranking, replayed: slot 0 selects segment 1 only when two segments and the first wants
  // less than the second.
  uint32_t ranks[kMaxSegments] = {0, 0};
  if (segments == 2) {
    const uint32_t first = REX_LOAD_U32(segments_base + kSegNeed);
    const uint32_t second = REX_LOAD_U32(segments_base + kSegStride + kSegNeed);
    if (int32_t(first) < int32_t(second)) {
      ranks[0] = 1;
      ranks[1] = 0;
    } else {
      ranks[0] = 0;
      ranks[1] = 1;
    }
  }
  uint64_t dest = REX_LOAD_U32(object + kObjDest);
  uint64_t running =
      RoundUp32(REX_LOAD_U32(uint32_t(((ranks[0] << 4) & 0xFFFFFFF0u) + segments_base)));

  for (int32_t i = 0; i < segments && i < int32_t(kMaxSegments); i++) {
    const Step step = MakeStep(base, segments_base, ranks[i], dest, running);
    spec.write(step.entry + kSegOut, 4);
    const Copy copy = PredictCopy(base, object, window, step.limit, step.want);
    if (!copy.known) return false;
    const uint64_t bytes = 4ull * uint64_t(uint32_t(copy.want));
    if (bytes > skate3::audio::kPortWatchCap) return false;
    // The source runs this segment's copy reads, split at the ring end exactly as the callee
    // splits them. Reads are never rewound and do not count against the window budget.
    {
      const uint64_t first_words =
          (uint64_t(uint32_t(copy.want)) < uint64_t(uint32_t(copy.run)))
              ? uint64_t(uint32_t(copy.want))
              : uint64_t(uint32_t(copy.run));
      spec.read(uint32_t(copy.src), uint32_t(4ull * first_words));
      if (uint64_t(uint32_t(copy.want)) > first_words) {
        spec.read(REX_LOAD_U32(window + kWinBase),
                  uint32_t(4ull * (uint64_t(uint32_t(copy.want)) - first_words)));
      }
    }
    const uint32_t at = uint32_t(dest);
    if (bytes != 0) {
      if (at == 0) return false;                                 // a copy to guest 0
      if (uint64_t(at) + bytes > 0x100000000ull) return false;   // the span would wrap
      // The next trip re-reads the segment array, and sub_82B3DB90 re-reads the window and the
      // object; a copy that lands on any of them moves the rest of the write set mid-call.
      if (Overlaps(at, bytes, segments_base, kSegStride * kMaxSegments)) return false;
      if (Overlaps(at, bytes, window, 16)) return false;
      if (Overlaps(at, bytes, object, 64)) return false;
      spec.write(at, uint32_t(bytes));
    }
    running = step.over ? (copy.want + running) : (step.need_up - copy.want);
    dest = WordsToBytes(copy.want) + dest;
  }
  return true;
}

}  // namespace port_82B3DC48
SKATE3_PORT(82B3DC48, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
