// sub_82B50B80  role: the XMA output drain -- produce r5 sample frames into the float descriptor r4 out of the System object r3, returning how many it produced: bind the next segment when the current one is spent (crediting every stream with the segment's sub-frame remainder), then per stream read the hardware write offset, retire the credit the decoder has already produced, and hand one chunk to sub_82B50380 to de-interleave into the output channels; a stream with too few decoded bytes gets sub_82B4FE40 called on it and, if its context has also gone idle for long enough, the whole System is reinitialised (silence flag set, every context reprogrammed through sub_82B4FC00, every stream record cleared, the fill slot stepped) and the block is filled with zeroes instead   lifted: skate3_recomp.68.cpp:57220 (757 lines)
// STATUS: gate-1 (seven XMA imports at depth 0: XMAGetOutputBufferWriteOffset, XMAGetOutputBufferReadOffset, XMAIsOutputBufferValid, XMADisableContext, XMASetOutputBufferReadOffset, XMASetOutputBufferValid, XMAEnableContext -- moving a context's read offset and re-enabling it are decoder-hardware effects no memory rewind undoes)
// calls/session: boot 131630  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B50B80.md
namespace port_82B50B80 {

// The System object (r3/r28). The offsets it shares with the rest of the XMA family are named as
// sub_82B4FC00.inc and sub_82B4FE40.inc name them.
constexpr uint32_t kSegmentTable = 36;    // +0x24 u32 BYTE OFFSET to the 24-byte segment table
constexpr uint32_t kOutChannels = 46;     // +0x2E u8  channels the silence fill writes
constexpr uint32_t kFillSlot = 48;        // +0x30 u8  the slot the feeder binds next
constexpr uint32_t kPlaySlot = 49;        // +0x31 u8  the slot this drain is playing
constexpr uint32_t kSlotWrap = 50;        // +0x32 u8  the wrap bound for both
constexpr uint32_t kStreamArray = 52;     // +0x34 -> the rw_xma_stream array
constexpr uint32_t kCurrentSegment = 56;  // +0x38 -> the bound segment
constexpr uint32_t kQueuedBytes = 60;     // +0x3C bytes queued, cleared by the reinit
constexpr uint32_t kStreamCount = 68;     // +0x44 u32 element count, re-read every iteration
constexpr uint32_t kSegmentLeft = 72;     // +0x48 sample frames left in the bound segment
constexpr uint32_t kPendingSkip = 76;     // +0x4C samples carried to the next segment's credit
constexpr uint32_t kRateCode = 80;        // +0x50 the sample-rate code sub_82B4FC00 parks
constexpr uint32_t kSilence = 84;         // +0x54 u8  set by the reinit: fill with zeroes
constexpr uint32_t kReinitFlag = 85;      // +0x55 u8  the feeder's flag; suppresses the wrap credit

// rw_xma_stream, 0x1C bytes (docs/rw_audio_structs.h names all of these).
constexpr uint32_t kStreamStride = 28;
constexpr uint32_t kStreamContext = 0;    // -> {id, in_buf0, in_buf1, out_buf, work_buf}
constexpr uint32_t kStreamSource = 4;
constexpr uint32_t kStreamRemaining = 8;
constexpr uint32_t kStreamCursor = 12;    // out_cursor, a BYTE offset that wraps at 0x1800
constexpr uint32_t kStreamCredit = 16;    // sample_credit, in sample frames
constexpr uint32_t kStreamChannels = 24;  // u8, 1 or 2
constexpr uint32_t kStreamSelect = 25;    // buffer_sel, cleared by the reinit
constexpr uint32_t kContextId = 0;        // the handle every XMA import takes in r3
constexpr uint32_t kContextOutBuf = 12;   // -> the 16-bit output ring

// A segment, 24 bytes, at object + [object+36] + 24*slot.
constexpr uint32_t kSegmentFirst = 8;     // first sample frame
constexpr uint32_t kSegmentLast = 12;     // one past the last; zero means "no segment"
constexpr uint32_t kSegmentAligned = 20;  // u8: clear means 384 frames of offset apply

// The output descriptor (r4/r21), the same layout sub_82B3D8E0 reads.
constexpr uint32_t kOutData = 4;    // u32 -> planar float32
constexpr uint32_t kOutStride = 14; // u16 floats between one channel's block and the next

constexpr uint32_t kRingBytes = 6144;    // 0x1800, the output ring: 24 blocks of 256 bytes
constexpr uint32_t kFrameSamples = 512;  // one XMA frame; the segment remainder is taken mod this
constexpr uint32_t kOffsetSamples = 384; // added when a segment's +20 is clear
constexpr uint32_t kIdleTicks = 4;       // the retry gives up once the counter has moved this far
constexpr uint32_t kBusyBit = 0x80;      // rlwinm rX,rY,0,0,24 on a byte is this one bit

// The seven kernel imports, by the guest address generated/skate3_init.cpp registers for each
// thunk. Calling them through the registered address lands on exactly the function the direct `bl`
// would, and keeps this port free of any direct kernel-symbol reference (lint check 6). A `bl` does
// not write ctr, so the bare REX_CALL_INDIRECT_FUNC(constant) form sub_82B4FC00.inc and
// sub_82B4FE40.inc use is the more faithful of the two spellings.
constexpr uint32_t kXMAGetOutputBufferWriteOffset = 0x82F9CBF4;
constexpr uint32_t kXMAGetOutputBufferReadOffset = 0x82F9CC94;
constexpr uint32_t kXMAIsOutputBufferValid = 0x82F9CC74;
constexpr uint32_t kXMASetOutputBufferReadOffset = 0x82F9CC14;
constexpr uint32_t kXMASetOutputBufferValid = 0x82F9CCC4;
constexpr uint32_t kXMADisableContext = 0x82F9CC44;
constexpr uint32_t kXMAEnableContext = 0x82F9CC54;

constexpr uint32_t kFrameBytes = 224;    // stwu r1,-224(r1)
// addi r10,r1,76 ; stwu r11,4(r10) -- so the first channel pointer lands at frame+80, which is
// where the sub_82B50380 call reads its two destinations from.
constexpr uint32_t kChannelSlots = 80;

// cmpw / cmpwi: a SIGNED compare of the low words, whatever the 64-bit registers hold above them.
inline int32_t Word(uint64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }

// `mullw rD,rChannels,rSamples` then `rlwinm rD,rD,1,0,30`: the low word of the 64-bit signed
// product, doubled. Every "bytes for N sample frames" here is this, because the ring holds 16-bit
// samples; the doubling cannot lose a bit to the rotate because the mask only clears bit 0.
inline uint64_t RingBytes(uint64_t samples, uint64_t channels) {
  const int64_t product = static_cast<int64_t>(Word(channels)) * static_cast<int64_t>(Word(samples));
  return static_cast<uint32_t>(static_cast<uint32_t>(product) << 1);
}

// divwu: RexGlue lowers it as "zero when the divisor is zero", and the `twllei rB,0` that guards it
// is a separate instruction whose position differs between the three sites -- so each site spells
// the divide and the trap in its own order rather than sharing a helper.
inline uint64_t Divwu(uint64_t a, uint64_t b) {
  return static_cast<uint32_t>(b) ? static_cast<uint32_t>(static_cast<uint32_t>(a) /
                                                          static_cast<uint32_t>(b))
                                  : 0;
}

// The epilogue both returns share: `addi r1,r1,224` then __restgprlr_17, which reloads r17..r31 and
// the link register from the frame this call is giving back.
void Restore(PPCContext& ctx, uint64_t entry_lr, const PPCRegister* saved) {
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,224
  PPCRegister* const slots[] = {&ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21,
                                &ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26,
                                &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (size_t i = 0; i < 15; i++) *slots[i] = saved[i];
  ctx.r12.u64 = entry_lr;  // lwz r12,-8(r1)
  ctx.lr = entry_lr;       // mtlr r12
}

REX_FUNC(Native) {
  // __savegprlr_17 spills r17..r31 and the link register at -8..-128 off the entry stack pointer --
  // inside the 224 bytes the stwu below claims -- and __restgprlr_17 reloads all of them. r17-r31
  // are compared, so the entry values are kept here rather than the spills reproduced.
  const PPCRegister saved[15] = {ctx.r17, ctx.r18, ctx.r19, ctx.r20, ctx.r21,
                                 ctx.r22, ctx.r23, ctx.r24, ctx.r25, ctx.r26,
                                 ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-224(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;  // mr r28,r3
  const uint32_t out = ctx.r4.u32;     // mr r21,r4
  uint64_t wanted = ctx.r5.u64;        // mr r19,r5 -- sample frames still owed to the caller

  // cmpwi cr6,r5,0 ; bne cr6,0x82b50bbc -- SIGNED, and only zero returns early.
  if (Word(ctx.r5.u64) == 0) {
    ctx.r3.s64 = 0;  // li r3,0
    Restore(ctx, entry_lr, saved);
    return;
  }

  uint64_t produced = 0;  // r20, the running output frame position and the result
  uint64_t decoded = 0;   // r29, frames the last pass advanced `produced` by; loop-carried
  uint64_t stall = 0;     // r17, the latch that lets the retry below happen only once

  do {
    // loc_82B50BC0 -- the outer loop: bind a segment when the last one is spent, drain a chunk,
    // repeat until the caller's frame count is satisfied.
    // lwz r11,72(r28) ; cmpwi cr6,r11,0 ; bne cr6,0x82b50c8c -- SIGNED
    if (Word(REX_LOAD_U32(object + kSegmentLeft)) == 0) {
      const uint64_t slot = REX_LOAD_U8(object + kPlaySlot);         // lbz r10,49(r28)
      uint64_t offset_samples = 0;                                   // mr r11,r22
      const uint64_t table = REX_LOAD_U32(object + kSegmentTable);   // lwz r8,36(r28)
      // rotlwi r9,r10,1 ; add r10,r10,r9 ; rlwinm r10,r10,3,0,28 -- 24*slot, exact because the
      // slot comes from an lbz: 3*slot is under 2^10 and 24*slot is already a multiple of 8.
      // add r10,r10,r8 ; add r10,r10,r28 -- the table is embedded in the object.
      const uint64_t at = 24 * slot + table + object;
      // lwz r9,12(r10) ; subfic r8,r9,0 ; subfe r6,r7,r7 ; and r6,r6,r10 -- a BRANCHLESS null
      // check: the mask is all ones when +12 is non-zero and zero when it is, so an empty segment
      // leaves `segment` at 0 and every load below addresses low memory directly. Reproduced.
      const uint64_t mask = REX_LOAD_U32(uint32_t(at) + kSegmentLast) == 0 ? 0 : ~uint64_t(0);
      const uint32_t segment = uint32_t(at & mask);
      // lbz r5,20(r6) ; cmplwi cr6,r5,0 ; bne cr6,0x82b50c0c
      if (REX_LOAD_U8(segment + kSegmentAligned) == 0) {
        offset_samples = kOffsetSamples;  // li r11,384
      }

      // loc_82B50C0C
      const uint64_t first = REX_LOAD_U32(segment + kSegmentFirst);   // lwz r9,8(r6)
      uint64_t seen = 0;                                              // mr r10,r22
      const uint64_t pending = REX_LOAD_U32(object + kPendingSkip);    // lwz r8,76(r28)
      const uint64_t biased = first + offset_samples;                  // add r7,r9,r11
      const uint32_t count = REX_LOAD_U32(object + kStreamCount);      // lwz r5,68(r28)
      // srawi r4,r7,9 ; addze r3,r4 -- the compiler's SIGNED divide by 512 truncating toward zero;
      // rlwinm r7,r3,9,0,22 multiplies it straight back, so the pair is a round-down to an XMA
      // frame boundary and what survives is the remainder.
      const int32_t frames = Word(biased) / int32_t(kFrameSamples);
      const uint64_t aligned = uint32_t(uint32_t(frames) * kFrameSamples);
      // subf r9,r7,r9 ; add r9,r9,r8 ; add r7,r9,r11 -- all 64-bit on zero-extended halves.
      const uint64_t credit = (first - aligned) + pending + offset_samples;

      // cmplwi cr6,r5,0 ; ble cr6,0x82b50c70 -- an UNSIGNED compare against zero, so `ble` can
      // only mean equal: any non-zero count enters the loop.
      if (count != 0) {
        uint64_t stream_offset = 0;  // mr r9,r22
        do {
          // loc_82B50C44
          const uint32_t array = REX_LOAD_U32(object + kStreamArray);  // lwz r11,52(r28)
          seen += 1;                                                   // addi r10,r10,1
          const uint32_t stream = uint32_t(array + uint32_t(stream_offset));  // add r11,r11,r9
          stream_offset += kStreamStride;                              // addi r9,r9,28
          // addi r8,r11,16 is dead: the load below overwrites r8 immediately.
          const uint64_t owed = REX_LOAD_U32(stream + kStreamCredit) + credit;  // lwz ; add
          REX_STORE_U32(stream + kStreamCredit, uint32_t(owed));       // stw r8,16(r11)
          // lwz r5,68(r28) ; cmplw cr6,r10,r5 ; blt -- UNSIGNED, bound re-read every pass
        } while (uint32_t(seen) < REX_LOAD_U32(object + kStreamCount));
      }

      // loc_82B50C70
      REX_STORE_U32(object + kPendingSkip, 0);                       // stw r22,76(r28)
      const uint64_t last = REX_LOAD_U32(segment + kSegmentLast);     // lwz r11,12(r6)
      const uint64_t begin = REX_LOAD_U32(segment + kSegmentFirst);   // lwz r10,8(r6)
      REX_STORE_U32(object + kSegmentLeft, uint32_t(last - begin));   // subf r9,r10,r11 ; stw
      REX_STORE_U32(object + kCurrentSegment, segment);               // stw r6,56(r28)
      REX_STORE_U8(object + kSilence, 0);                             // stb r22,84(r28)
    }

    // loc_82B50C8C -- this pass drains min(owed, frames left), SIGNED.
    const uint64_t left = REX_LOAD_U32(object + kSegmentLeft);  // lwz r11,72(r28)
    // cmpw cr6,r19,r11 ; mr r25,r11 ; bgt ; mr r25,r19
    const uint64_t chunk = Word(wanted) > Word(left) ? left : wanted;

    // loc_82B50CA0
    uint64_t index = 0;     // mr r24,r22 -- the stream index
    uint64_t underrun = 0;  // mr r27,r22 -- set when a stream had too little decoded
    // lbz r11,84(r28) ; cmplwi cr6,r11,0 ; bne cr6,0x82b51004 -- the silence flag skips the drain
    if (REX_LOAD_U8(object + kSilence) == 0) {
      uint64_t stream_offset = 0;  // mr r23,r22 -- 28*index, carried separately
      while (true) {
        // loc_82B50CB8 -- cmplw cr6,r24,r11 ; blt ; cmplwi cr6,r27,0 ; beq cr6,0x82b51004. The
        // index bound is UNSIGNED and only checked when `underrun` is clear: the retry below
        // re-enters here with it set, which is also the only way the body could run on an index
        // past the count (it cannot in practice -- the retry never advances the index).
        if (!(uint32_t(index) < REX_LOAD_U32(object + kStreamCount)) && underrun == 0) break;

        // loc_82B50CCC
        const uint32_t array = REX_LOAD_U32(object + kStreamArray);  // lwz r11,52(r28)
        // add r31,r11,r23 in 64 bits; lwzx r11,r11,r23 addresses with the low word. Only the low
        // word is ever used to address, exactly as the lifted form does.
        const uint32_t stream = uint32_t(array + uint32_t(stream_offset));

        // lwzx r11,r11,r23 ; lwz r3,0(r11) ; bl 0x82f9cbf4
        ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
        ctx.lr = 0x82B50CE0;
        REX_CALL_INDIRECT_FUNC(kXMAGetOutputBufferWriteOffset);
        // lwz r10,0(r31) -- the context pointer is RELOADED after every import, which clobbers the
        // volatile registers. Every reload below is the original's.
        const uint32_t context = REX_LOAD_U32(stream + kStreamContext);
        // rlwinm r9,r3,8,0,23 -- the write offset scaled by 256 into a byte position in the ring.
        const uint64_t written = uint32_t(uint32_t(ctx.r3.u32) << 8);
        const uint64_t cursor = REX_LOAD_U32(stream + kStreamCursor);  // lwz r8,12(r31)
        uint64_t avail = written - cursor;                             // subf r30,r8,r9
        ctx.r3.u64 = REX_LOAD_U32(context + kContextId);               // lwz r3,0(r10)
        ctx.lr = 0x82B50CF8;
        REX_CALL_INDIRECT_FUNC(kXMAGetOutputBufferReadOffset);
        const uint64_t read_offset = ctx.r3.u64;  // mr r26,r3

        // cmpwi cr6,r30,0 ; bgt cr6,0x82b50d30 -- SIGNED
        if (!(Word(avail) > 0)) {
          // lbz r11,85(r28) ; cmplwi cr6,r11,0 ; bne -- the feeder's reinit flag suppresses the
          // whole-ring credit below.
          if (REX_LOAD_U8(object + kReinitFlag) == 0) {
            bool wrapped = Word(avail) != 0;  // cmpwi cr6,r30,0 ; bne cr6,0x82b50d2c
            if (!wrapped) {
              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.lr = 0x82B50D24;
              REX_CALL_INDIRECT_FUNC(kXMAIsOutputBufferValid);
              wrapped = ctx.r3.s32 == 0;  // cmpwi cr6,r3,0 ; bne cr6,0x82b50d30
            }
            // loc_82B50D2C -- the write offset has lapped the cursor, or the buffer went invalid
            // with nothing between the two: a whole ring is available.
            if (wrapped) avail += kRingBytes;  // addi r30,r30,6144
          }
        }

        // loc_82B50D30 -- retire whatever credit the decoder has already produced.
        const uint64_t held = REX_LOAD_U32(stream + kStreamCredit);  // lwz r8,16(r31)
        // cmplwi cr6,r8,0 ; beq ; cmpwi cr6,r30,0 ; beq cr6,0x82b50d94
        if (held != 0 && Word(avail) != 0) {
          const uint64_t lanes = REX_LOAD_U8(stream + kStreamChannels);  // lbz r9,24(r31)
          const uint64_t owed = RingBytes(held, lanes);  // mullw r10,r9,r8 ; rlwinm r10,r10,1,0,30
          uint64_t take = avail;                         // mr r11,r30
          // cmplw cr6,r30,r10 ; ble cr6,0x82b50d60 -- UNSIGNED
          if (uint32_t(avail) > uint32_t(owed)) take = owed;  // mr r11,r10
          // loc_82B50D60
          uint64_t moved = take + REX_LOAD_U32(stream + kStreamCursor);  // lwz r10,12(r31) ; add
          REX_STORE_U32(stream + kStreamCursor, uint32_t(moved));        // stw r10,12(r31)
          // cmplwi cr6,r10,6144 ; blt cr6,0x82b50d7c -- the wrap is a SECOND store to the same
          // word, so both values pass through memory.
          if (uint32_t(moved) >= kRingBytes) {
            moved -= kRingBytes;                                     // addi r10,r10,-6144
            REX_STORE_U32(stream + kStreamCursor, uint32_t(moved));  // stw r10,12(r31)
          }
          // loc_82B50D7C -- rlwinm r10,r9,1,0,30: bytes per sample frame.
          const uint64_t per_frame = uint32_t(uint32_t(lanes) << 1);
          avail -= take;                                     // subf r30,r11,r30
          const uint64_t retired = Divwu(take, per_frame);    // divwu r9,r11,r10
          if (Word(per_frame) == 0) ppc_trap(ctx, base, 0);   // twllei r10,0 -- AFTER the divide
          REX_STORE_U32(stream + kStreamCredit, uint32_t(held - retired));  // subf r8,r9,r8 ; stw
        }

        // loc_82B50D94
        const uint64_t lanes = REX_LOAD_U8(stream + kStreamChannels);  // lbz r7,24(r31)
        const uint64_t needed = RingBytes(chunk, lanes);  // mullw r11,r7,r25 ; rlwinm r10,r11,1,0,30
        // cmpw cr6,r30,r10 ; blt cr6,0x82b50e70 -- SIGNED ; cmpwi cr6,r25,0 ; beq cr6,0x82b50e70
        if (Word(avail) >= Word(needed) && Word(chunk) != 0) {
          const uint64_t at_cursor = REX_LOAD_U32(stream + kStreamCursor);  // lwz r6,12(r31)
          const uint64_t per_frame = uint32_t(uint32_t(lanes) << 1);  // rlwinm r11,r7,1,0,30
          underrun = 0;                                               // mr r27,r22
          const uint64_t to_end = uint64_t(kRingBytes) - at_cursor;    // subfic r10,r6,6144
          if (Word(per_frame) == 0) ppc_trap(ctx, base, 0);            // twllei r11,0 -- BEFORE it
          const uint64_t fits = Divwu(to_end, per_frame);              // divwu r11,r10,r11
          uint64_t wrap = 0;  // mr r30,r22
          decoded = chunk;    // mr r29,r25
          // cmpw cr6,r25,r11 ; ble cr6,0x82b50de0 -- SIGNED: split the chunk at the ring's end.
          if (Word(chunk) > Word(fits)) {
            wrap = chunk - fits;  // subf r30,r11,r25
            decoded = fits;       // mr r29,r11
          }

          // loc_82B50DE0 -- build one output pointer per channel on the frame.
          // cmplwi cr6,r7,0 ; beq cr6,0x82b50e18
          if (lanes != 0) {
            const uint64_t stride = REX_LOAD_U16(out + kOutStride);        // lhz r8,14(r21)
            const uint64_t count_lanes = REX_LOAD_U8(stream + kStreamChannels);  // lbz r5,24(r31)
            const uint32_t data = REX_LOAD_U32(out + kOutData);            // lwz r9,4(r21)
            // mullw r4,r8,r24 ; rlwinm r11,r4,3,0,28 ; add r11,r11,r9 -- 8*stride*index, i.e. two
            // channels per stream. NOTE the running output position is NOT part of this, though the
            // silence fill at loc_82B51028 does add it.
            const int64_t product =
                static_cast<int64_t>(Word(stride)) * static_cast<int64_t>(Word(index));
            uint64_t slot = uint64_t(uint32_t(uint32_t(product) << 3)) + data;
            const uint64_t step = __builtin_rotateleft32(uint32_t(stride), 2);  // rotlwi r8,r8,2
            uint32_t at_slot = frame + kChannelSlots;  // addi r10,r1,76, then the stwu's +4
            uint64_t remaining = count_lanes;          // mtctr r5
            do {
              // loc_82B50E0C
              REX_STORE_U32(at_slot, uint32_t(slot));  // stwu r11,4(r10)
              at_slot += 4;
              slot = step + slot;  // add r11,r8,r11
              // bdnz: ctr is decremented and its LOW word tested, so a zero count would wrap --
              // unreachable here, the branch above already excluded it.
            } while (uint32_t(--remaining) != 0);
          }

          // loc_82B50E18 -- (dst0, dst1, ring, cursor, channels, first, wrap). dst1 is loaded even
          // for a mono stream, from whatever the frame slot held. r6 is still the cursor loaded
          // above and r7 the channel count from loc_82B50D94.
          const uint32_t record = REX_LOAD_U32(stream + kStreamContext);      // lwz r11,0(r31)
          const uint32_t dst1 = REX_LOAD_U32(frame + kChannelSlots + 4);      // lwz r4,84(r1)
          const uint32_t dst0 = REX_LOAD_U32(frame + kChannelSlots);          // lwz r3,80(r1)
          const uint32_t ring = REX_LOAD_U32(record + kContextOutBuf);        // lwz r5,12(r11)
          skate3::audio::GuestCall(ctx, base, sub_82B50380, dst0, dst1, ring, at_cursor, lanes,
                                   decoded, wrap);  // bl 0x82b50380
          // loc_82B50E34
          const uint64_t after = REX_LOAD_U8(stream + kStreamChannels);  // lbz r11,24(r31)
          // cmpwi cr6,r30,0 ; beq cr6,0x82b50e54 -- SIGNED
          if (Word(wrap) != 0) {
            // The block wrapped: the cursor restarts from the ring base.
            const uint64_t bytes = RingBytes(wrap, after);  // mullw r10,r11,r30 ; rlwinm r9,..,1
            decoded = chunk;                                // mr r29,r25
            REX_STORE_U32(stream + kStreamCursor, uint32_t(bytes));  // stw r9,12(r31)
          } else {
            // loc_82B50E54
            const uint64_t bytes = RingBytes(decoded, after);  // mullw r9,r11,r29
            const uint64_t was = REX_LOAD_U32(stream + kStreamCursor);  // lwz r10,12(r31)
            decoded = chunk;                                            // mr r29,r25
            REX_STORE_U32(stream + kStreamCursor, uint32_t(bytes + was));  // add r8,r11,r10 ; stw
          }
        } else {
          // loc_82B50E70 -- too little decoded for this chunk: push more input at the decoder and
          // remember that this stream stalled.
          underrun = 1;                                               // mr r27,r18
          skate3::audio::GuestCall(ctx, base, sub_82B4FE40, object);  // mr r3,r28 ; bl 0x82b4fe40
        }

        // loc_82B50E7C -- the read offset the context should now be at.
        const uint64_t at_now = REX_LOAD_U32(stream + kStreamCursor);  // lwz r11,12(r31)
        uint64_t want_read = 0;                                        // mr r30,r22
        // cmplwi cr6,r11,6144 ; bne cr6,0x82b50e94
        if (uint32_t(at_now) == kRingBytes) {
          REX_STORE_U32(stream + kStreamCursor, 0);  // stw r22,12(r31) -- exactly at the end wraps
        } else {
          // loc_82B50E94 -- rotlwi r9,r10,10 is channels*1024 bytes, one group of 4 subframes per
          // channel; the quotient times channels*4 is the offset in 128-sample subframes.
          const uint64_t lanes_now = REX_LOAD_U8(stream + kStreamChannels);  // lbz r10,24(r31)
          const uint64_t group = __builtin_rotateleft32(uint32_t(lanes_now), 10);
          const uint64_t groups = Divwu(at_now, group);  // divwu r8,r11,r9
          if (Word(group) == 0) ppc_trap(ctx, base, 0);  // twllei r9,0 -- AFTER the divide
          // mullw r7,r8,r10 ; rlwinm r30,r7,2,0,29
          const int64_t product =
              static_cast<int64_t>(Word(groups)) * static_cast<int64_t>(Word(lanes_now));
          want_read = uint32_t(uint32_t(uint32_t(product)) << 2);
        }

        // loc_82B50EAC -- cmpw cr6,r26,r30 ; beq cr6,0x82b50eec. Reprogramming the read offset
        // takes the context down and back up; this is the pair of effects gate 1 turns on.
        if (Word(read_offset) != Word(want_read)) {
          ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
          ctx.r4.s64 = 1;  // li r4,1
          ctx.lr = 0x82B50EC4;
          REX_CALL_INDIRECT_FUNC(kXMADisableContext);  // bl 0x82f9cc44
          ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
          ctx.r4.u64 = want_read;  // mr r4,r30
          ctx.lr = 0x82B50ED4;
          REX_CALL_INDIRECT_FUNC(kXMASetOutputBufferReadOffset);  // bl 0x82f9cc14
          ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
          ctx.lr = 0x82B50EE0;
          REX_CALL_INDIRECT_FUNC(kXMASetOutputBufferValid);  // bl 0x82f9ccc4
          ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
          ctx.lr = 0x82B50EEC;
          REX_CALL_INDIRECT_FUNC(kXMAEnableContext);  // bl 0x82f9cc54
        }

        // loc_82B50EEC -- cmplwi cr6,r27,0 ; bne cr6,0x82b50f00
        if (underrun == 0) {
          index += 1;                      // addi r24,r24,1
          stream_offset += kStreamStride;  // addi r23,r23,28
          continue;                        // b 0x82b50cb8
        }

        // loc_82B50F00 -- the stalled tail. sub_82EE02B0 returns [[0x8200063C]+16], a counter, and
        // `underrun` is 1 on every path that reaches here, so this is that counter less one.
        skate3::audio::GuestCall(ctx, base, sub_82EE02B0);  // bl 0x82ee02b0
        const uint64_t ticks = ctx.r3.u64 - underrun;       // subf r30,r27,r3
        ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
        ctx.lr = 0x82B50F14;
        REX_CALL_INDIRECT_FUNC(kXMAEnableContext);  // bl 0x82f9cc54
        // lwz r10,0(r31) ; lwz r11,0(r10) -- the context's first word, used as a POINTER here
        // rather than as the handle the imports take.
        const uint32_t handle = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
        bool busy = true;  // loc_82B50F48: mr r11,r18
        // cmplwi cr6,r11,0 ; beq cr6,0x82b50f48
        if (handle != 0) {
          // lbz r10,8(r11) ; rlwinm r9,r10,0,0,24 ; cmplwi ; bne cr6,0x82b50f48
          if ((REX_LOAD_U8(handle + 8) & kBusyBit) == 0) {
            // lbz r11,12(r11) ; rlwinm r10,r11,0,0,24 ; mr r11,r22 ; beq cr6,0x82b50f4c
            busy = (REX_LOAD_U8(handle + 12) & kBusyBit) != 0;
          }
        }

        // loc_82B50F4C -- clrlwi r11,r11,24 ; cmplwi cr6,r11,0 ; bne cr6,0x82b50f6c
        if (busy) {
          stall = 2;  // loc_82B50F6C: li r17,2
        } else if (uint32_t(ticks) < kIdleTicks) {  // cmplwi cr6,r30,4 ; bge cr6,0x82b50f74
          // cmpwi cr6,r17,0 ; beq cr6,0x82b50cb8 -- the retry: back to the top of the stream loop
          // with `underrun` still set and the index NOT advanced, so the same stream is polled
          // again; the latch makes it happen at most once per call.
          if (Word(stall) == 0) continue;
        } else {
          stall = 1;  // loc_82B50F74: mr r17,r18
        }

        // loc_82B50F78 -- the reinit. Everything from here is the "give up on this segment" path:
        // the block becomes silence, every context is reprogrammed, every stream record is cleared,
        // and the fill slot is stepped so the feeder starts on the next segment.
        REX_STORE_U8(object + kSilence, 1);  // stb r18,84(r28)
        // mr r3,r28 ; lwz r4,80(r28) ; bl 0x82b4fc00
        skate3::audio::GuestCall(ctx, base, sub_82B4FC00, object, REX_LOAD_U32(object + kRateCode));
        // lwz r10,68(r28) ; lwz r11,52(r28) ; mulli r10,r10,28 ; add r9,r10,r11 ; cmplw ; bge
        uint32_t walk = REX_LOAD_U32(object + kStreamArray);
        uint64_t end = uint64_t(REX_LOAD_U32(object + kStreamCount)) * kStreamStride + walk;
        if (uint32_t(walk) < uint32_t(end)) {
          do {
            // loc_82B50FA0 -- five fields per stream, in the original's own order.
            REX_STORE_U32(walk + kStreamRemaining, 0);  // stw r22,8(r11)
            REX_STORE_U32(walk + kStreamCursor, 0);     // stw r22,12(r11)
            REX_STORE_U8(walk + kStreamSelect, 0);      // stb r22,25(r11)
            REX_STORE_U32(walk + kStreamCredit, 0);     // stw r22,16(r11)
            REX_STORE_U32(walk + kStreamSource, 0);     // stw r22,4(r11)
            walk += kStreamStride;                      // addi r11,r11,28
            // Both the base and the count are RE-READ every pass: lwz r9,52(r28) ; lwz r10,68(r28)
            end = uint64_t(REX_LOAD_U32(object + kStreamCount)) * kStreamStride +
                  REX_LOAD_U32(object + kStreamArray);
          } while (uint32_t(walk) < uint32_t(end));  // cmplw cr6,r11,r9 ; blt
        }

        // loc_82B50FD0 -- the fill slot follows the play slot, wrapped at +50. Three stores to the
        // same byte, all reproduced: the play slot, then that plus one, then zero on the wrap.
        const uint64_t play = REX_LOAD_U8(object + kPlaySlot);  // lbz r11,49(r28)
        REX_STORE_U8(object + kFillSlot, uint8_t(play));        // stb r11,48(r28)
        // clrlwi r11,r11,24 ; addi r10,r11,1 ; clrlwi r9,r10,24
        const uint64_t next = uint8_t(uint32_t(play) + 1);
        REX_STORE_U8(object + kFillSlot, uint8_t(next));  // stb r9,48(r28)
        // lbz r8,50(r28) ; cmplw cr6,r9,r8 ; blt cr6,0x82b50ff8 -- UNSIGNED
        if (uint32_t(next) >= REX_LOAD_U8(object + kSlotWrap)) {
          REX_STORE_U8(object + kFillSlot, 0);  // stb r22,48(r28)
        }
        // loc_82B50FF8
        REX_STORE_U32(object + kQueuedBytes, 0);                    // stw r22,60(r28)
        skate3::audio::GuestCall(ctx, base, sub_82B4FE40, object);  // mr r3,r28 ; bl 0x82b4fe40
        break;  // falls into loc_82B51004
      }
    }

    // loc_82B51004 -- the silence fill: zero this chunk in every output channel, at the running
    // position, and count it as produced.
    // lbz r11,84(r28) ; cmplwi cr6,r11,0 ; beq cr6,0x82b5105c
    if (REX_LOAD_U8(object + kSilence) != 0) {
      const uint64_t lanes = REX_LOAD_U8(object + kOutChannels);  // lbz r11,46(r28)
      decoded = chunk;                                           // mr r29,r25
      uint64_t lane = 0;                                         // mr r31,r22
      // cmplwi cr6,r11,0 ; beq cr6,0x82b5105c
      if (lanes != 0) {
        const uint64_t bytes = uint32_t(uint32_t(chunk) << 2);  // rlwinm r30,r25,2,0,29
        do {
          // loc_82B51028
          const uint64_t stride = REX_LOAD_U16(out + kOutStride);  // lhz r11,14(r21)
          const uint32_t data = REX_LOAD_U32(out + kOutData);      // lwz r10,4(r21)
          // mullw r11,r11,r31 ; add r9,r11,r20 ; rlwinm r11,r9,2,0,29 ; add r3,r11,r10
          const int64_t product =
              static_cast<int64_t>(Word(stride)) * static_cast<int64_t>(Word(lane));
          const uint64_t words = uint64_t(uint32_t(product)) + produced;
          const uint64_t at = uint64_t(uint32_t(uint32_t(words) << 2)) + data;
          skate3::audio::GuestCall(ctx, base, sub_82F52040, at, 0, bytes);  // memset(dst, 0, n)
          lane += 1;  // addi r31,r31,1
          // lbz r8,46(r28) ; cmplw cr6,r31,r8 ; blt -- UNSIGNED, bound re-read every pass
        } while (uint32_t(lane) < REX_LOAD_U8(object + kOutChannels));
      }
    }

    // loc_82B5105C
    const uint64_t still = REX_LOAD_U32(object + kSegmentLeft);  // lwz r11,72(r28)
    produced = decoded + produced;                               // add r20,r29,r20
    wanted = wanted - chunk;                                     // subf r19,r25,r19
    const uint64_t rest = still - chunk;                         // subf. r10,r25,r11
    REX_STORE_U32(object + kSegmentLeft, uint32_t(rest));        // stw r10,72(r28)
    // bne 0x82b510b8 (cr0 from subf., the LOW word SIGNED) ; cmpwi cr6,r25,0 ; beq 0x82b510b8
    if (Word(rest) == 0 && Word(chunk) != 0) {
      // The segment is spent: carry the distance to the next 512-frame XMA boundary into the next
      // segment's credit, unless this block was silence.
      const uint32_t segment = REX_LOAD_U32(object + kCurrentSegment);  // lwz r11,56(r28)
      const uint64_t flag = REX_LOAD_U8(segment + kSegmentAligned);     // lbz r10,20(r11)
      uint64_t last = REX_LOAD_U32(segment + kSegmentLast);             // lwz r11,12(r11)
      // cmplwi cr6,r10,0 ; bne cr6,0x82b51094
      if (flag == 0) last += kOffsetSamples;  // addi r11,r11,384
      // loc_82B51094 -- clrlwi r11,r11,23 keeps the low NINE bits: the position inside the frame.
      const uint64_t within = uint32_t(last) & 0x1FFu;
      // cmplwi cr6,r11,0 ; beq cr6,0x82b510a8
      if (within != 0) {
        // subfic r11,r11,512 ; stw r11,76(r28)
        REX_STORE_U32(object + kPendingSkip, uint32_t(uint64_t(kFrameSamples) - within));
      }
      // loc_82B510A8 -- lbz r11,84(r28) ; cmplwi ; beq cr6,0x82b510b8
      if (REX_LOAD_U8(object + kSilence) != 0) {
        REX_STORE_U32(object + kPendingSkip, 0);  // stw r22,76(r28)
      }
    }

    // loc_82B510B8
    skate3::audio::GuestCall(ctx, base, sub_82B4FE40, object);  // mr r3,r28 ; bl 0x82b4fe40
    // cmpwi cr6,r19,0 ; bne cr6,0x82b50bc0 -- SIGNED
  } while (Word(wanted) != 0);

  ctx.r3.u64 = produced;  // mr r3,r20
  Restore(ctx, entry_lr, saved);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B50B80
SKATE3_PORT(82B50B80, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
