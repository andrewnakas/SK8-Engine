// sub_82B4FE40  role: the XMA input-buffer feeder -- loop forever over two passes: pass A walks the u32[+0x44] streams in the 0x1C-stride array at +0x34 and, for each one whose next input buffer is free, DMAs up to 0x800 bytes of its source into that buffer (sub_82B4FD40), re-arms it (XMASetInputBuffer0/1Valid around XMADisableContext/XMAEnableContext), applies the pending start offset once and flips the buffer-select byte; then, when nothing is left queued in +0x3C, bind the next 24-byte segment from the table at +0x24 indexed by the wrapping byte counter at +0x30, reprogram every context when the flag at +0x55 says the format changed (sub_82B4FC00), and pass B hands each stream to sub_82B50100, accumulating the new work into +0x3C. Returns when +0x3C is non-zero or the next segment is not ready   lifted: skate3_recomp.68.cpp:55157 (392 lines)
// STATUS: gate-1 (XMA import, depth 0: XMAIsInputBuffer0Valid, XMAIsInputBuffer1Valid, XMAEnableContext, XMADisableContext, XMASetInputBuffer0Valid, XMASetInputBuffer1Valid, XMASetInputBufferReadOffset -- re-arming an input buffer and re-enabling a context are effects on the decoder hardware that no memory rewind can undo)
// calls/session: boot 134627  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B4FE40.md
namespace port_82B4FE40 {

// The System-side XMA object (r3, held in r30). docs/rw_audio_structs.h documents the stream
// array and its count; the rest is named for what this function does with it.
constexpr uint32_t kSegmentTableOffset = 36;  // +0x24  u32 BYTE OFFSET from the object to the table
constexpr uint32_t kSegmentSlot = 48;         // +0x30  u8  index into that table, wraps at +0x32
constexpr uint32_t kSegmentSlotCount = 50;    // +0x32  u8  the wrap bound
constexpr uint32_t kStreamArray = 52;         // +0x34 -> the rw_xma_stream array
constexpr uint32_t kStreamCount = 68;         // +0x44  u32, RE-READ at the bottom of both loops
constexpr uint32_t kBytesQueued = 60;         // +0x3C  u32, pass B adds to it, pass A takes from it
constexpr uint32_t kReinitFlag = 85;          // +0x55  u8, non-zero: reprogram every context once
constexpr uint32_t kStreamStride = 28;        // 0x1C, the stride sub_82B4FC00 and sub_82B50B80 use

// The 24-byte segment descriptor at object + [object+0x24] + 24*slot.
constexpr uint32_t kSegmentStride = 24;
constexpr uint32_t kSegmentData = 0;   // -> the packed stream data; passed on as a read cursor
constexpr uint32_t kSegmentReady = 12; // non-zero: this segment can be bound. Zero ends the call.

// rw_xma_stream (docs/rw_audio_structs.h).
constexpr uint32_t kStreamContext = 0;     // +0x00 -> {id, in_buf0, in_buf1, ...}
constexpr uint32_t kStreamSource = 4;      // +0x04  read position, advanced by the chunk fed
constexpr uint32_t kStreamRemaining = 8;   // +0x08  bytes left to feed
constexpr uint32_t kStreamStartBits = 20;  // +0x14  pending read offset in BITS, applied once
constexpr uint32_t kStreamBufferSel = 25;  // +0x19  which input buffer to fill next

// The context record the stream points at.
constexpr uint32_t kContextId = 0;       // the handle every XMA import takes in r3
constexpr uint32_t kContextInBuf0 = 4;
constexpr uint32_t kContextInBuf1 = 8;

constexpr uint32_t kChunkBytes = 2048;       // cmplwi r29,2048 -- 0x800, one XMA packet
constexpr uint32_t kPacketHeaderBits = 32;   // addi r4,r11,32 -- one XMA packet header, in bits
constexpr uint32_t kSampleRateMask = 3;      // clrlwi r4,r11,30 -- XMA's 2-bit sample-rate code

// The seven kernel imports, by the guest address generated/skate3_init.cpp registers for each
// thunk; that address is exactly where the direct `bl` lands, and it keeps this port free of any
// kernel-symbol reference (probe/ports/lint.py check 6). Unlike sub_82B4FAF8.inc these do not
// write ctr first: a `bl` does not write ctr, so leaving it alone is the more faithful of the two
// spellings. kXMAEnableContext and kXMADisableContext agree with sub_82B4FC00.inc.
constexpr uint32_t kXMAIsInputBuffer0Valid = 0x82F9CCD4;
constexpr uint32_t kXMAIsInputBuffer1Valid = 0x82F9CCA4;
constexpr uint32_t kXMAEnableContext = 0x82F9CC54;
constexpr uint32_t kXMADisableContext = 0x82F9CC44;
constexpr uint32_t kXMASetInputBuffer0Valid = 0x82F9CC84;
constexpr uint32_t kXMASetInputBuffer1Valid = 0x82F9CCE4;
constexpr uint32_t kXMASetInputBufferReadOffset = 0x82F9CCF4;

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)

REX_FUNC(Native) {
  // __savegprlr_25 spills r25-r31 and the link register into slots inside the 144 bytes the stwu
  // below claims; the harness compares r13-r31, so the entry values have to survive, but the
  // slots are this call's own frame and are neither windowed nor reproduced.
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  // The frame IS reproduced: both guest callees build their own frames below r1, and
  // sub_82B4FD40 moves 0x800 bytes through vector registers with a 128-byte frame of its own.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-144(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t system = ctx.r3.u32;  // mr r30,r3
  // li r26,0 ; li r25,1 -- the two literals held across the whole function, used only as store
  // sources and as the zero the offsets and counters start from. Written as literals below.

  for (;;) {
    // loc_82B4FE58 -- PASS A. cmplwi cr6,r11,0 ; ble cr6,0x82b4febc is an UNSIGNED compare
    // against zero, so `ble` can only mean equal: any non-zero count enters the loop.
    uint32_t count = REX_LOAD_U32(system + kStreamCount);  // lwz r11,68(r30)
    if (count != 0) {
      uint64_t index = 0;   // mr r27,r26 -- the iteration counter
      uint64_t offset = 0;  // mr r28,r26 -- the byte offset into the stream array
      do {
        // loc_82B4FE6C
        const uint32_t array = REX_LOAD_U32(system + kStreamArray);  // lwz r11,52(r30)
        // add r31,r28,r11 in 64 bits; lwzx r11,r28,r11 addresses with the low word. Only the low
        // word is ever used to address, exactly as the lifted form does.
        const uint32_t stream = uint32_t(offset + array);

        // lwzx r11,r28,r11 ; lwz r3,0(r11) ; bl 0x82f9ccd4
        ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream) + kContextId);
        ctx.lr = 0x82B4FE80;
        REX_CALL_INDIRECT_FUNC(kXMAIsInputBuffer0Valid);
        // cmpwi cr6,r3,0 ; beq cr6,0x82b4ff98 -- either buffer already free sends this stream to
        // the feed loop; only when BOTH are still valid is there nothing to do but re-enable.
        bool feed = ctx.r3.s32 == 0;
        if (!feed) {
          // lwz r11,0(r31) ; lwz r3,0(r11) -- the context pointer is RELOADED after every import,
          // which clobbers the volatile registers. Every reload below is the original's.
          ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream) + kContextId);
          ctx.lr = 0x82B4FE94;
          REX_CALL_INDIRECT_FUNC(kXMAIsInputBuffer1Valid);
          feed = ctx.r3.s32 == 0;  // beq cr6,0x82b4ff98
          if (!feed) {
            ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream) + kContextId);  // lwz r11,0(r31) ; lwz r3,0(r11)
            ctx.lr = 0x82B4FEA8;
            REX_CALL_INDIRECT_FUNC(kXMAEnableContext);  // bl 0x82f9cc54
          }
        }

        if (feed) {
          // loc_82B4FF98 -- the feed loop. loc_82B4FF94 stores the advanced source and FALLS
          // THROUGH to this test, so one visit to a stream can fill both input buffers in turn,
          // and keeps going until the source runs out or the next buffer is still busy.
          for (;;) {
            // lwz r11,8(r31) ; cmplwi cr6,r11,0 ; beq cr6,0x82b4fea8
            if (REX_LOAD_U32(stream + kStreamRemaining) == 0) break;

            // lbz r11,25(r31) ; cmplwi cr6,r11,0 -- the compare is issued BEFORE the two loads
            // below, which both happen on either arm of the branch. Order kept; no store between
            // them, so the compare's position is not observable.
            const uint32_t select = REX_LOAD_U8(stream + kStreamBufferSel);
            const uint32_t context = REX_LOAD_U32(stream + kStreamContext);  // lwz r11,0(r31)
            ctx.r3.u64 = REX_LOAD_U32(context + kContextId);                 // lwz r3,0(r11)

            uint64_t chunk;
            uint64_t advanced;
            // bne cr6,0x82b50058 -- a non-zero select byte means input buffer 1 is next.
            if (select == 0) {
              ctx.lr = 0x82B4FFBC;
              REX_CALL_INDIRECT_FUNC(kXMAIsInputBuffer0Valid);  // bl 0x82f9ccd4
              // cmpwi cr6,r3,0 ; bne cr6,0x82b4fea8 -- still valid, so nothing can be pushed.
              if (ctx.r3.s32 != 0) break;

              // lwz r29,8(r31) ; cmplwi cr6,r29,2048 ; blt -- at most one packet per pass.
              chunk = REX_LOAD_U32(stream + kStreamRemaining);
              if (uint32_t(chunk) >= kChunkBytes) chunk = kChunkBytes;  // li r29,2048

              // bl 0x82b4fd40 -- the vector copy: dest = context->in_buf0, src = stream->source.
              // The context pointer is reloaded first (lwz r11,0(r31)).
              {
                const uint32_t reloaded = REX_LOAD_U32(stream + kStreamContext);
                skate3::audio::GuestCall(ctx, base, sub_82B4FD40,
                                         REX_LOAD_U32(reloaded + kContextInBuf0),   // lwz r3,4(r11)
                                         REX_LOAD_U32(stream + kStreamSource),      // lwz r4,4(r31)
                                         chunk);                                    // mr r5,r29
              }

              // bl 0x82f9cc44 -- XMADisableContext(id, wait = 1)
              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.r4.s64 = 1;  // li r4,1
              ctx.lr = 0x82B4FFF8;
              REX_CALL_INDIRECT_FUNC(kXMADisableContext);

              // bl 0x82f9cc84 -- the buffer just filled becomes valid.
              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.lr = 0x82B50004;
              REX_CALL_INDIRECT_FUNC(kXMASetInputBuffer0Valid);

              // lwz r11,20(r31) ; cmpwi cr6,r11,0 ; ble cr6,0x82b50024 -- SIGNED, so only a
              // strictly positive pending offset is applied, and it is applied once.
              const uint32_t start_bits = REX_LOAD_U32(stream + kStreamStartBits);
              if (int32_t(start_bits) > 0) {
                ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
                // addi r4,r11,32 in 64 bits on a zero-extended word: an offset near INT_MAX
                // carries into the upper half rather than wrapping. Kept 64-bit.
                ctx.r4.s64 = int64_t(start_bits) + kPacketHeaderBits;
                ctx.lr = 0x82B50020;
                REX_CALL_INDIRECT_FUNC(kXMASetInputBufferReadOffset);  // bl 0x82f9ccf4
                REX_STORE_U32(stream + kStreamStartBits, 0);           // stw r26,20(r31)
              }

              // loc_82B50024
              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.lr = 0x82B50030;
              REX_CALL_INDIRECT_FUNC(kXMAEnableContext);  // bl 0x82f9cc54

              // subf r9,r29,r10 ; stw r9,8(r31) -- 64-bit, truncated at the store. remaining is
              // at least the chunk here, so this one cannot borrow.
              const uint64_t remaining = REX_LOAD_U32(stream + kStreamRemaining);  // lwz r10,8(r31)
              REX_STORE_U32(stream + kStreamRemaining, uint32_t(remaining - chunk));
              // subf r7,r29,r8 ; stw r7,60(r30) -- this one CAN borrow: nothing checks that the
              // object's queued total covers the chunk. Reproduced, borrow and all.
              const uint64_t queued = REX_LOAD_U32(system + kBytesQueued);  // lwz r8,60(r30)
              REX_STORE_U32(system + kBytesQueued, uint32_t(queued - chunk));
              // add r6,r11,r29 -- the advanced source, stored at loc_82B4FF94 below.
              advanced = REX_LOAD_U32(stream + kStreamSource) + chunk;  // lwz r11,4(r31)
              REX_STORE_U8(stream + kStreamBufferSel, 1);               // stb r25,25(r31)
            } else {
              // loc_82B50058 -- the same sequence against input buffer 1. r3 still holds the id
              // loaded before the branch; this arm does not reload it first.
              ctx.lr = 0x82B5005C;
              REX_CALL_INDIRECT_FUNC(kXMAIsInputBuffer1Valid);  // bl 0x82f9cca4
              if (ctx.r3.s32 != 0) break;  // cmpwi cr6,r3,0 ; bne cr6,0x82b4fea8

              chunk = REX_LOAD_U32(stream + kStreamRemaining);          // lwz r29,8(r31)
              if (uint32_t(chunk) >= kChunkBytes) chunk = kChunkBytes;  // li r29,2048

              // loc_82B50074 -- bl 0x82b4fd40 with dest = context->in_buf1.
              {
                const uint32_t reloaded = REX_LOAD_U32(stream + kStreamContext);  // lwz r11,0(r31)
                skate3::audio::GuestCall(ctx, base, sub_82B4FD40,
                                         REX_LOAD_U32(reloaded + kContextInBuf1),  // lwz r3,8(r11)
                                         REX_LOAD_U32(stream + kStreamSource),     // lwz r4,4(r31)
                                         chunk);                                   // mr r5,r29
              }

              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.r4.s64 = 1;  // li r4,1
              ctx.lr = 0x82B50098;
              REX_CALL_INDIRECT_FUNC(kXMADisableContext);  // bl 0x82f9cc44

              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.lr = 0x82B500A4;
              REX_CALL_INDIRECT_FUNC(kXMASetInputBuffer1Valid);  // bl 0x82f9cce4

              // lwz r11,20(r31) ; cmpwi cr6,r11,0 ; ble cr6,0x82b500c4 -- SIGNED, as above.
              const uint32_t start_bits = REX_LOAD_U32(stream + kStreamStartBits);
              if (int32_t(start_bits) > 0) {
                ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
                ctx.r4.s64 = int64_t(start_bits) + kPacketHeaderBits;  // addi r4,r11,32
                ctx.lr = 0x82B500C0;
                REX_CALL_INDIRECT_FUNC(kXMASetInputBufferReadOffset);  // bl 0x82f9ccf4
                REX_STORE_U32(stream + kStreamStartBits, 0);           // stw r26,20(r31)
              }

              // loc_82B500C4
              ctx.r3.u64 = REX_LOAD_U32(REX_LOAD_U32(stream + kStreamContext) + kContextId);
              ctx.lr = 0x82B500D0;
              REX_CALL_INDIRECT_FUNC(kXMAEnableContext);  // bl 0x82f9cc54

              const uint64_t remaining = REX_LOAD_U32(stream + kStreamRemaining);  // lwz r10,8(r31)
              REX_STORE_U32(stream + kStreamRemaining, uint32_t(remaining - chunk));  // stw r9,8(r31)
              const uint64_t queued = REX_LOAD_U32(system + kBytesQueued);  // lwz r8,60(r30)
              REX_STORE_U32(system + kBytesQueued, uint32_t(queued - chunk));  // stw r7,60(r30)
              advanced = REX_LOAD_U32(stream + kStreamSource) + chunk;  // lwz r11,4(r31) ; add r6,r11,r29
              REX_STORE_U8(stream + kStreamBufferSel, 0);               // stb r26,25(r31)
            }

            // loc_82B4FF94 -- the one store both arms share, then straight back to the test.
            REX_STORE_U32(stream + kStreamSource, uint32_t(advanced));  // stw r6,4(r31)
          }
        }

        // loc_82B4FEA8 -- the bound is RE-READ every pass and compared UNSIGNED.
        count = REX_LOAD_U32(system + kStreamCount);  // lwz r11,68(r30)
        index += 1;                                   // addi r27,r27,1
        offset += kStreamStride;                      // addi r28,r28,28
      } while (uint32_t(index) < count);              // cmplw cr6,r27,r11 ; blt cr6,0x82b4fe6c
    }

    // loc_82B4FEBC -- anything still queued and the call is over: the streams have work, so there
    // is no reason to bind another segment. cmplwi cr6,r11,0 ; bne cr6,0x82b500f8.
    if (REX_LOAD_U32(system + kBytesQueued) != 0) break;

    const uint32_t slot = REX_LOAD_U8(system + kSegmentSlot);            // lbz r11,48(r30)
    const uint64_t table = REX_LOAD_U32(system + kSegmentTableOffset);   // lwz r10,36(r30)
    // rotlwi r9,r11,1 ; add r11,r11,r9 ; rlwinm r11,r11,3,0,28 -- 3*slot, then rotated left 3 and
    // masked with 0xFFFFFFF8, i.e. 24*slot. `slot` came from an lbz so it is at most 255: 3*slot
    // is under 2^10, the rotate cannot wrap into the low bits, and 24*slot is already a multiple
    // of 8 so the mask drops nothing. The stride is therefore exactly 24, not a 24 with caveats.
    // add r11,r11,r10 ; add r27,r11,r30 -- both 64-bit sums of zero-extended halves.
    const uint64_t segment64 = kSegmentStride * uint64_t(slot) + table + system;
    const uint32_t segment = uint32_t(segment64);
    // lwz r10,12(r27) ; cmpwi cr6,r10,0 ; beq cr6,0x82b500f8 -- no next segment, so return.
    if (REX_LOAD_U32(segment + kSegmentReady) == 0) break;

    // lbz r11,48(r30) -- RELOADED, not the `slot` above; addi r11,r11,1 ; clrlwi r10,r11,24.
    const uint32_t next_slot = uint32_t(int64_t(REX_LOAD_U8(system + kSegmentSlot)) + 1) & 0xFFu;
    REX_STORE_U8(system + kSegmentSlot, uint8_t(next_slot));  // stb r10,48(r30)
    // lbz r9,50(r30) ; cmplw cr6,r10,r9 ; blt cr6,0x82b4ff10 -- UNSIGNED; at the bound it wraps.
    if (next_slot >= REX_LOAD_U8(system + kSegmentSlotCount)) {
      REX_STORE_U8(system + kSegmentSlot, 0);  // stb r26,48(r30)
    }

    // loc_82B4FF10 -- the flag is read and the segment's data pointer loaded on both arms.
    const uint32_t reinit = REX_LOAD_U8(system + kReinitFlag);       // lbz r11,85(r30)
    uint64_t cursor = REX_LOAD_U32(segment + kSegmentData);          // lwz r28,0(r27)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b4ff34
    if (reinit != 0) {
      // lwz r11,0(r28) ; clrlwi r4,r11,30 -- the low two bits of the segment's first word, which
      // is what sub_82B4FC00 parks at +0x50 and writes into XMA_CONTEXT_INIT.sample_rate: XMA's
      // sample rate is a 2-bit code, not a rate in Hz.
      const uint32_t header = REX_LOAD_U32(uint32_t(cursor));
      // bl 0x82b4fc00 -- reprograms every context of this object.
      skate3::audio::GuestCall(ctx, base, sub_82B4FC00, system, header & kSampleRateMask);
      REX_STORE_U8(system + kReinitFlag, 0);  // stb r26,85(r30)
    }

    // loc_82B4FF34 -- PASS B. Same unsigned-against-zero test as pass A; an empty array goes
    // straight back to the top, which is a spin until pass A or a callee changes something.
    uint32_t count_b = REX_LOAD_U32(system + kStreamCount);  // lwz r11,68(r30)
    if (count_b == 0) continue;                             // ble cr6,0x82b4fe58
    // lwz r11,52(r30) -- the array base is loaded ONCE here and then RELOADED at the bottom of
    // every iteration, so it is loop-carried: the value used at the top of pass N+1 is the one
    // read after pass N's callee ran.
    uint32_t array_b = REX_LOAD_U32(system + kStreamArray);
    uint64_t index_b = 0;   // mr r29,r26
    uint64_t offset_b = 0;  // mr r31,r26
    do {
      // loc_82B4FF4C -- bl 0x82b50100 (stream, index, segment, cursor). r3 is the 64-bit sum
      // add r3,r31,r11, not a truncated address, so it is passed as 64 bits.
      skate3::audio::GuestCall(ctx, base, sub_82B50100, offset_b + array_b, index_b, segment64,
                               cursor);

      array_b = REX_LOAD_U32(system + kStreamArray);                 // lwz r11,52(r30)
      const uint64_t queued = REX_LOAD_U32(system + kBytesQueued);   // lwz r9,60(r30)
      index_b += 1;                                                  // addi r29,r29,1
      // add r10,r31,r11 -- the OLD offset against the JUST-RELOADED array base.
      const uint32_t stream_b = uint32_t(offset_b + array_b);
      cursor += ctx.r3.u64;    // add r28,r3,r28 -- the callee returns bytes consumed
      offset_b += kStreamStride;  // addi r31,r31,28
      // lwz r10,8(r10) ; add r9,r10,r9 ; stw r9,60(r30) -- 64-bit sum, truncated at the store.
      const uint64_t remaining = REX_LOAD_U32(stream_b + kStreamRemaining);
      REX_STORE_U32(system + kBytesQueued, uint32_t(remaining + queued));
      count_b = REX_LOAD_U32(system + kStreamCount);  // lwz r8,68(r30)
      // cmplw cr6,r29,r8 ; blt cr6,0x82b4ff4c -- UNSIGNED, against the reloaded count.
    } while (uint32_t(index_b) < count_b);
    // b 0x82b4fe58
  }

  // loc_82B500F8 -- no `li r3,...` on any path: r3 is whatever the last call left behind.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
  ctx.lr = entry_lr;                      // __restgprlr_25 restores lr and r25-r31
  ctx.r12.u64 = entry_lr;
  ctx.r25 = saved_r25;
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

}  // namespace port_82B4FE40
SKATE3_PORT(82B4FE40, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
