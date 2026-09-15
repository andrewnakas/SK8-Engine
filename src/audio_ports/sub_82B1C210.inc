// sub_82B1C210  role: clamp a block of N values into their per-value [min,max] ranges, then broadcast the block to the record's listener list   lifted: skate3_recomp.66.cpp:63915 (99 lines)
// STATUS: partial -- compared 15804, skipped 304, zero divergence (session g1_splits, 150 s scripted); substantive -- the clamp loop runs on the compared side. NEVER promoted: the skipped path has a native body but no comparison. (comparable only when the value block's header word is zero -- that is what guards the sub_828E29C0 broadcast, and the clamp loop, the whole substance of the function, runs on both sides of it)
// calls/session: boot 25894  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C210.md
namespace port_82B1C210 {

// The first two words are sub_828E29C0's handle pair (holder, token); this function does not
// touch them, it only hands the record over.
constexpr uint32_t kClampEnabled = 8;  // u8: zero means the range table is absent
constexpr uint32_t kCount = 9;         // u8: N, RELOADED on every iteration
constexpr uint32_t kTableBase = 12;    // N pairs of (min,max) int32 when enabled, else the
                                       // value block starts here directly
constexpr uint32_t kRangeStride = 8;   // one (min,max) pair
constexpr uint32_t kBlockHeader = 0;   // on the value block: must be non-zero to broadcast
constexpr uint32_t kBlockValues = 4;   // the N clamped words, and the payload pointer passed on

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  const uint64_t entry_lr = ctx.lr;  // mflr r12 / stw r12,-8(r1)

  // lbz r11,8(r3) / cmplwi cr6,r11,0 / beq cr6,0x82b1c28c
  uint64_t block;  // r5
  if (REX_LOAD_U8(self + kClampEnabled) == 0) {
    block = static_cast<uint64_t>(self) + kTableBase;  // addi r5,r3,12
  } else {
    const uint32_t count = REX_LOAD_U8(self + kCount);   // lbz r9,9(r3)
    uint64_t range = static_cast<uint64_t>(self) + kTableBase;  // addi r11,r3,12
    // rotlwi r10,r9,3 then add r5,r10,r11 -- a rotate, not a shift, but r9 came from an lbz
    // so it is 0..255 and no bit can wrap out of bit 31. The add is 64-bit on zero-extended
    // operands, as RexGlue lifts it.
    block = static_cast<uint64_t>(__builtin_rotateleft32(count, 3)) + range;
    // cmpwi cr6,r9,0 / ble cr6,0x82b1c290 -- signed, though a byte can never be negative
    if (static_cast<int32_t>(count) > 0) {
      uint64_t slot = block;  // mr r8,r5
      for (int32_t done = 0;;) {
        // lwz r9,0(r11) / lwz r10,4(r8) / lwz r7,4(r11) -- three loads in this order, and the
        // value is read from the very address the store below writes.
        const int32_t lo = static_cast<int32_t>(REX_LOAD_U32(static_cast<uint32_t>(range)));
        int32_t value = static_cast<int32_t>(
            REX_LOAD_U32(static_cast<uint32_t>(slot) + kBlockValues));
        const int32_t hi = static_cast<int32_t>(
            REX_LOAD_U32(static_cast<uint32_t>(range) + 4));
        if (value < lo) {
          value = lo;  // cmpw cr6,r10,r9 / bge / mr r10,r9 / b 0x82b1c270
        } else if (value > hi) {
          value = hi;  // cmpw cr6,r10,r7 / ble / mr r10,r7
        }
        done++;  // addi r6,r6,1
        // stwu r10,4(r8) -- store WITH update: the pointer advances to the address written.
        slot += kBlockValues;
        REX_STORE_U32(static_cast<uint32_t>(slot), static_cast<uint32_t>(value));
        range += kRangeStride;  // addi r11,r11,8
        // lbz r10,9(r3) / cmpw cr6,r6,r10 / blt -- the count is re-read every iteration, so a
        // listener or another thread shortening it mid-loop shortens the loop.
        if (done >= static_cast<int32_t>(REX_LOAD_U8(self + kCount))) break;
      }
    }
  }

  // lwz r11,0(r5) / cmpwi cr6,r11,0 / beq cr6,0x82b1c2a4
  if (static_cast<int32_t>(REX_LOAD_U32(static_cast<uint32_t>(block) + kBlockHeader)) != 0) {
    // addi r4,r5,4 / bl 0x828e29c0 -- r3 passes through untouched, r4 is the value array.
    ctx.lr = 0x82B1C2A4;
    skate3::audio::GuestCall(ctx, base, sub_828E29C0, ctx.r3.u64, block + kBlockValues);
  }
  ctx.r3.s64 = 0;    // li r3,0 -- the callee's result is discarded
  ctx.lr = entry_lr;  // lwz r12,-8(r1) / mtlr r12
}

// Split by path. sub_828E29C0's `bctrl` fires once per registered listener and is guarded by the
// value block's header word -- which this function never writes, only tests, so its entry value is
// the value the body tests. Header zero: the broadcast never happens and the clamp loop is fully
// comparable in one span. Header non-zero: false, so the macro runs the original and counts a
// PortSkip rather than firing every listener a second time on rewound memory.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  spec.read(self + kClampEnabled, 2);  // +8 the flag, +9 the count

  // The same arithmetic Native does, from the same entry bytes: with the range table absent the
  // value block starts at +12, otherwise 8N past it.
  uint32_t count = 0;
  uint64_t block;
  if (REX_LOAD_U8(self + kClampEnabled) == 0) {
    block = static_cast<uint64_t>(self) + kTableBase;  // addi r5,r3,12
  } else {
    count = REX_LOAD_U8(self + kCount);  // lbz r9,9(r3)
    block = static_cast<uint64_t>(__builtin_rotateleft32(count, 3)) +
            (static_cast<uint64_t>(self) + kTableBase);  // rotlwi r10,r9,3 ; add r5,r10,r11
    spec.read(self + kTableBase, kRangeStride * count);  // the (min,max) pairs the loop reads
  }
  const uint32_t values = static_cast<uint32_t>(block);
  spec.read(values + kBlockHeader, 4);

  // lwz r11,0(r5) ; cmpwi cr6,r11,0 ; beq -- non-zero broadcasts the block through sub_828E29C0,
  // whose `bctrl` walks an unbounded subscriber list and calls each node's stored function
  // pointer. Neither the targets nor the count is knowable from entry state, and a listener is
  // free to do anything, so that path is not replayable.
  if (static_cast<int32_t>(REX_LOAD_U32(values + kBlockHeader)) != 0) return false;

  // stwu r10,4(r8), N times, four bytes apart with no gaps: one contiguous span, at most 1,020
  // bytes because the count is an `lbz`. The loop re-reads +9 every iteration, so the trip count
  // is not fixed at entry in general -- but every store lands at block+4 or above and
  // block >= self+12, so nothing in the call can move +9 and the count cannot grow.
  if (static_cast<int32_t>(count) > 0) spec.write(values + kBlockValues, 4 * count);
  return true;
}

}  // namespace port_82B1C210
SKATE3_PORT(82B1C210, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
