// sub_82B1E290  role: the expression-evaluator tick -- when the frame delta in f1 differs from the cached one, recount how many deltas fit inside one update period and republish the period, its frame count and the derived scale global; then count the countdown down and, on the frame it reaches zero, reload it and walk the registered evaluator list, interpreting each node's record stream by dispatching every opcode byte through the function table at 0x82FD3600 and wiring each call's r3 into the node's operand block   lifted: skate3_recomp.67.cpp:956 (212 lines)
// STATUS: gate-1 (its own bctrl is the opcode table's dispatch, so the callee is data: sub_82B1E290 indirect call, `bctrl` at 0x82B1E37C; census indirect 1, depth 0)
// calls/session: boot 17448  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1E290.md
namespace port_82B1E290 {

// Every address below is ((lis_imm & 0xFFFF) << 16) + offset, computed, not read off by eye.
// lis r11/r9/r8/r7,-31993 -> 0x8307 << 16 = 0x83070000: four words of one evaluator global block.
constexpr uint32_t kScaleGlobal = 0x830775D8;  // +30168  f32, count * delta * kScaleUnit
constexpr uint32_t kDeltaCache = 0x830775DC;   // +30172  f32, the delta this count was made for
constexpr uint32_t kFrameCount = 0x830775E0;   // +30176  u32, deltas per period (the reload value)
constexpr uint32_t kCountdown = 0x830775E4;    // +30180  u32, frames left until the next walk

constexpr uint32_t kPeriodNumer = 0x8231A844;  // lis r10,-32206 ; lfs -22460  -- the period
constexpr uint32_t kPeriodDenom = 0x82FD35F4;  // lis r6,-32003  ; lfs 13812
constexpr uint32_t kZeroFloat = 0x82165A10;    // lis r5,-32234  ; lfs 23056   -- 0.0
constexpr uint32_t kScaleUnit = 0x82256FE8;    // lis r9,-32219  ; lfs 28648

constexpr uint32_t kListHead = 0x83036F4C;  // lis r10,-31997 ; lwz 28492 -- head of the node list
constexpr uint32_t kOpTable = 0x82FD3600;   // lis r10,-32003 ; addi r29,r10,13824

// The node list: singly linked, three of its words used here.
constexpr uint32_t kNodeNext = 0;     // lwz r28,0(r11)
constexpr uint32_t kNodeProgram = 8;  // lwz r30,8(r11)  -- the record stream
constexpr uint32_t kNodeBlock = 12;   // lwz r31,12(r11) -- the operand block the ops run against

// A record: u8 opcode (255 ends the stream), u8 pair count, 2 unread bytes, then `count` pairs of
// { u32 src, u32 dst } block-relative offsets, then one u32 added to the block pointer.
constexpr uint32_t kRecordOpcode = 0;
constexpr uint32_t kRecordPairs = 1;
constexpr uint32_t kRecordBody = 4;
constexpr uint32_t kPairBytes = 8;
constexpr uint32_t kEndOpcode = 255;
constexpr int32_t kSrcIsResult = -1;  // cmpwi cr6,r10,-1 -- take the bctrl's r3 instead of a load

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
constexpr uint32_t kConvertSlot = 80;  // std r10,80(r1) ; lfd f0,80(r1) -- inside the own frame
// __savegprlr_28 / __restgprlr_28 slots, negative offsets from the ENTRY sp, all inside the 128
// bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR28 = 40;
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

double AddSingle(double a, double b) { return double(float(a + b)); }
double MulSingle(double a, double b) { return double(float(a * b)); }
double DivSingle(double a, double b) { return double(float(a / b)); }

REX_FUNC(Native) {
  const double delta = ctx.f1.f64;  // the only argument: the frame delta, in f1

  // The frame is reproduced because every dispatched opcode runs on it, and because the fcfid
  // round trip below goes through frame + 80.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U64(entry_sp - kSaveR28, ctx.r28.u64);   // bl 0x82f52118 -> std r28,-40(r1)
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);   //                  std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);   //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);   //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  //                  stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  ctx.fpscr.disableFlushModeUnconditional();
  const double cached = LoadSingle(base, kDeltaCache);  // lfs f0,30172(r11)

  uint64_t countdown;  // r10 at loc_82B1E32C
  uint64_t period;     // r11 at loc_82B1E32C -- the value the countdown reloads from
  // fcmpu cr6,f1,f0 ; beq cr6,0x82b1e324 -- exact float equality, NaN takes the recount.
  if (delta != cached) {
    StoreSingle(base, kDeltaCache, delta);  // stfs f1,30172(r11)
    const double limit = DivSingle(LoadSingle(base, kPeriodNumer),
                                   LoadSingle(base, kPeriodDenom));  // fdivs f13,f13,f12
    double accumulated = LoadSingle(base, kZeroFloat);                // lfs f0,23056(r5) -- 0.0
    uint64_t count = 0;                                               // li r11,0
    double next;
    // loc_82B1E2DC: count the deltas that fit under the limit. NOT bounded: a delta of zero or
    // below never advances `next`, so the original spins here forever. Reproduced, not guarded.
    do {
      ctx.fpscr.disableFlushModeUnconditional();
      accumulated = AddSingle(accumulated, delta);  // fadds f0,f0,f1
      count = count + 1;                           // addi r11,r11,1
      next = AddSingle(accumulated, delta);         // fadds f12,f0,f1
      // fcmpu cr6,f12,f13 ; blt cr6,0x82b1e2dc -- a NaN clears lt and leaves the loop.
    } while (next < limit);

    const uint64_t count_word = count & 0xFFFFFFFFull;    // clrldi r10,r11,32
    REX_STORE_U32(kFrameCount, static_cast<uint32_t>(count));  // stw r11,30176(r9)
    // std r10,80(r1) ; lfd f0,80(r1) ; fcfid f13,f0 -- the integer-to-double round trip through
    // the own frame; the doubleword written is the masked copy, not the raw counter.
    REX_STORE_U64(frame + kConvertSlot, count_word);
    const int64_t spilled = static_cast<int64_t>(REX_LOAD_U64(frame + kConvertSlot));
    const double as_double = double(spilled);            // fcfid f13,f0
    const double as_single = double(float(as_double));   // frsp f12,f13
    const double scaled = MulSingle(as_single, delta);   // fmuls f11,f12,f1
    StoreSingle(base, kScaleGlobal,
                MulSingle(scaled, LoadSingle(base, kScaleUnit)));  // fmuls f0,f11,f0 ; stfs 30168
    // mr r10,r11 happens before the float tail: both the countdown and the reload value are the
    // fresh count, so a changed delta always restarts the period from full.
    countdown = count;
    period = count;
  } else {
    // loc_82B1E324
    countdown = REX_LOAD_U32(kCountdown);  // lwz r10,30180(r8)
    period = REX_LOAD_U32(kFrameCount);    // lwz r11,30176(r9)
  }

  // loc_82B1E32C: addic. r10,r10,-1 -- 64-bit decrement, cr0 tests only the LOW word, and the
  // store keeps the low word. A countdown of 0 therefore becomes 0xFFFFFFFF and does not tick.
  ctx.xer.ca = static_cast<uint32_t>(countdown) > 0;  // addic. sets CA; xer is not compared
  countdown = countdown - 1;
  REX_STORE_U32(kCountdown, static_cast<uint32_t>(countdown));  // stw r10,30180(r8)
  // bne 0x82b1e3f0
  if (static_cast<int32_t>(static_cast<uint32_t>(countdown)) == 0) {
    REX_STORE_U32(kCountdown, static_cast<uint32_t>(period));  // stw r11,30180(r8)
    uint64_t node = REX_LOAD_U32(kListHead);                   // lwz r11,28492(r10)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b1e3f0 -- an empty list is the common case off-tick.
    while (node != 0) {
      // loc_82B1E354
      uint64_t record = REX_LOAD_U32(static_cast<uint32_t>(node) + kNodeProgram);  // lwz r30,8
      const uint64_t next_node = REX_LOAD_U32(static_cast<uint32_t>(node) + kNodeNext);  // r28
      uint64_t block = REX_LOAD_U32(static_cast<uint32_t>(node) + kNodeBlock);  // lwz r31,12
      uint32_t opcode = REX_LOAD_U8(static_cast<uint32_t>(record) + kRecordOpcode);  // lbz r11,0
      // cmplwi cr6,r11,255 ; beq cr6,0x82b1e3e4 -- an immediately terminated stream does nothing.
      while (opcode != kEndOpcode) {
        // rlwinm r11,r11,2,0,29 on entry, rotlwi r11,r11,2 on the loop back edge: for a byte both
        // are opcode * 4, so one expression is exact for both.
        const uint32_t slot = (opcode << 2) & 0xFFFFFFFCu;
        // loc_82B1E370: lwzx r11,r11,r29 ; mr r3,r31 ; mtctr r11 ; bctrl -- THE dispatch. The
        // target is a word of the table at 0x82FD3600, i.e. data, which is what fails gate 1.
        ctx.ctr.u64 = REX_LOAD_U32(slot + kOpTable);
        ctx.r3.u64 = block;
        ctx.lr = 0x82B1E380;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        const uint32_t result = ctx.r3.u32;  // stwx stores the LOW word of the op's r3

        uint32_t pairs = REX_LOAD_U8(static_cast<uint32_t>(record) + kRecordPairs);  // lbz r10,1
        uint64_t pair = record + kRecordBody;  // addi r11,r30,4
        uint64_t index = 0;                    // li r8,0
        // cmplwi cr6,r10,0 ; beq cr6,0x82b1e3c8
        if (pairs != 0) {
          for (;;) {
            // loc_82B1E394
            const uint32_t src = REX_LOAD_U32(static_cast<uint32_t>(pair));      // lwz r10,0(r11)
            const uint32_t dst = REX_LOAD_U32(static_cast<uint32_t>(pair) + 4);  // lwz r9,4(r11)
            // cmpwi cr6,r10,-1 ; bne cr6,0x82b1e3ac -- SIGNED against -1.
            if (static_cast<int32_t>(src) == kSrcIsResult) {
              // stwx r3,r9,r31 -- the op's result lands in the block.
              REX_STORE_U32(dst + static_cast<uint32_t>(block), result);
            } else {
              // loc_82B1E3AC: lwzx r10,r10,r31 ; stwx r10,r9,r31 -- a block-to-block copy.
              const uint32_t moved = REX_LOAD_U32(src + static_cast<uint32_t>(block));
              REX_STORE_U32(dst + static_cast<uint32_t>(block), moved);
            }
            // loc_82B1E3B4: the count is RELOADED every iteration, so a record the dispatched op
            // rewrote changes the trip count mid-loop. Kept as lifted.
            pairs = REX_LOAD_U8(static_cast<uint32_t>(record) + kRecordPairs);  // lbz r10,1(r30)
            index = index + 1;                                                 // addi r8,r8,1
            pair = pair + kPairBytes;                                          // addi r11,r11,8
            // cmpw cr6,r8,r10 ; blt cr6,0x82b1e394 -- SIGNED 32-bit on the low words.
            if (static_cast<int32_t>(static_cast<uint32_t>(index)) >=
                static_cast<int32_t>(pairs))
              break;
          }
        }

        // loc_82B1E3C8: the word after the pairs advances the block pointer, and the next record
        // header begins four bytes further on.
        const uint64_t advance = REX_LOAD_U32(static_cast<uint32_t>(pair));  // lwz r11,0(r11)
        record = pair + 4;                                                   // addi r30,r11,4
        block = advance + block;                                             // add r31,r11,r31
        opcode = REX_LOAD_U8(static_cast<uint32_t>(record) + kRecordOpcode);  // lbz r11,0(r30)
        // bne cr6,0x82b1e370
      }
      // loc_82B1E3E4: mr r11,r28 ; cmplwi cr6,r28,0 ; bne cr6,0x82b1e354
      node = next_node;
    }
  }

  // loc_82B1E3F0: addi r1,r1,128 ; b __restgprlr_28. r3 is never set by this function: on the
  // walk path it holds the last dispatched op's result, on every other path the caller's own r3.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.r28.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR28);   // ld r28,-40(r1)
  ctx.r29.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR29);   // ld r29,-32(r1)
  ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR30);   // ld r30,-24(r1)
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR31);   // ld r31,-16(r1)
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
}

// Never called: the SKATE3_PORT macro skips the shadow branch for a gate-labelled port. Kept so
// the file has the same shape as every other port; the write set it would declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1E290
SKATE3_PORT(82B1E290, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
