// sub_82B4FC00  role: (re)program every hardware XMA context of a System -- park the sample-rate argument at +0x50, then for each of the u32[+0x44] streams in the 0x1C-stride array at +0x34 build a 56-byte XMA_CONTEXT_INIT on the stack from the stream's context record and call XMADisableContext, XMAInitializeContext, XMASetOutputBufferValid, XMASetInputBuffer0/1 and XMAEnableContext on it   lifted: skate3_recomp.68.cpp:54812 (180 lines)
// STATUS: gate-1 (XMA import, depth 0: XMADisableContext, XMAInitializeContext, XMASetOutputBufferValid, XMASetInputBuffer0, XMASetInputBuffer1, XMAEnableContext -- each one reprograms the decoder hardware, so none of it can be replayed on rewound memory)
// calls/session: boot 1907  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B4FC00.md
namespace port_82B4FC00 {

// The System object (r3). docs/rw_audio_structs.h documents this array and its count on
// rw_xma_stream; the field at +0x50 is not documented and is named for what happens to it.
constexpr uint32_t kStreamArray = 52;      // +0x34 -> the rw_xma_stream array
constexpr uint32_t kStreamCount = 68;      // +0x44  u32 element count, reloaded every iteration
constexpr uint32_t kSampleRateField = 80;  // +0x50  the r4 argument is parked here
constexpr uint32_t kStreamStride = 28;     // 0x1C, the stride sub_82B50B80 also uses

// rw_xma_stream (docs/rw_audio_structs.h).
constexpr uint32_t kStreamContext = 0;    // +0x00 -> {id, in_buf0, in_buf1, out_buf, work_buf}
constexpr uint32_t kStreamChannels = 24;  // +0x18  u8, 1 or 2

// The context record the stream points at.
constexpr uint32_t kContextId = 0;       // the handle every XMA import takes in r3
constexpr uint32_t kContextInBuf0 = 4;
constexpr uint32_t kContextInBuf1 = 8;
constexpr uint32_t kContextOutBuf = 12;
constexpr uint32_t kContextWorkBuf = 16;

// XMA_CONTEXT_INIT, the 56-byte block XMAInitializeContext reads. Field names and the size are
// the kernel's own (third_party/rexglue-sdk xboxkrnl_audio_xma.cpp), not a guess.
constexpr uint32_t kInitBytes = 56;
constexpr uint32_t kInitInBuf0 = 0;
constexpr uint32_t kInitInBuf0Packets = 4;
constexpr uint32_t kInitInBuf1 = 8;
constexpr uint32_t kInitInBuf1Packets = 12;
constexpr uint32_t kInitReadOffset = 16;
constexpr uint32_t kInitOutBuf = 20;
constexpr uint32_t kInitOutBlocks = 24;
constexpr uint32_t kInitWorkBuf = 28;
constexpr uint32_t kInitSubframes = 32;
constexpr uint32_t kInitChannelCount = 36;
constexpr uint32_t kInitSampleRate = 40;
// +44..+55 is XMA_LOOP_DATA, left at the zero the clearing loop wrote.

// The literals the original programs, held in r24-r28 across the whole loop.
constexpr uint32_t kPacketCount = 1;     // li r28,1  -- one 0x800 packet per input buffer
constexpr uint32_t kReadOffsetBits = 32; // li r25,32 -- BITS, i.e. one XMA packet header
constexpr uint32_t kOutBlocks = 24;      // li r26,24
constexpr uint32_t kSubframes = 4;       // li r24,4

// The six kernel imports, by the guest address generated/skate3_init.cpp registers for each
// thunk. Calling them through the registered address lands on exactly the function the direct
// `bl` would, and keeps this port free of any direct kernel-symbol reference.
constexpr uint32_t kXMADisableContext = 0x82F9CC44;
constexpr uint32_t kXMAInitializeContext = 0x82F9CC24;
constexpr uint32_t kXMASetOutputBufferValid = 0x82F9CCC4;
constexpr uint32_t kXMASetInputBuffer0 = 0x82F9CC34;
constexpr uint32_t kXMASetInputBuffer1 = 0x82F9CC04;
constexpr uint32_t kXMAEnableContext = 0x82F9CC54;

constexpr uint32_t kFrameBytes = 208;  // stwu r1,-208(r1)
constexpr uint32_t kInitSlot = 80;     // the XMA_CONTEXT_INIT block lives at r1+80

REX_FUNC(Native) {
  // __savegprlr_24 spills r24-r31 and the link register; the harness compares r13-r31, so the
  // entry values have to survive here too. The spill slots are this call's own, not windowed.
  const PPCRegister saved_r24 = ctx.r24;
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  ctx.r12.u64 = ctx.lr;  // mflr r12 -- the value __restgprlr_24 puts back in lr on the way out

  // The frame IS reproduced: the XMA_CONTEXT_INIT block at r1+80 is passed by address to
  // XMAInitializeContext, so it has to land inside the 208 bytes this call allocates.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-208(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t system = ctx.r3.u32;      // mr r30,r3
  const uint64_t sample_rate = ctx.r4.u64;  // mr r27,r4

  // lwz r11,68(r3) BEFORE stw r4,80(r3); the two fields cannot alias, but the order is kept.
  uint32_t count = REX_LOAD_U32(system + kStreamCount);
  REX_STORE_U32(system + kSampleRateField, ctx.r4.u32);  // stw r4,80(r3)

  // cmplwi cr6,r11,0 ; ble cr6,0x82b4fd38 -- an UNSIGNED compare against 0, so `ble` is `== 0`:
  // no stream means the whole loop is skipped and nothing else is written.
  uint32_t index = 0;   // li r29,0
  uint64_t offset = 0;  // li r31,0 -- the byte offset, 0x1C per stream
  // A guarded do-while, which is the shape the original has: the entry test uses the count loaded
  // before the store, the loop test uses the count reloaded at the bottom of each iteration.
  if (count != 0) do {
    // loc_82B4FC3C -- clear the 56 bytes of XMA_CONTEXT_INIT, every iteration.
    // li r10,7 ; addi r11,r1,72 ; mtctr r10 ; loop of stdu r9,8(r11)
    uint32_t clear = frame + kInitSlot - 8;
    for (uint32_t n = 0; n < kInitBytes / 8; n++) {
      clear += 8;
      REX_STORE_U64(clear, 0);  // stdu r9,8(r11)
    }

    const uint32_t array = REX_LOAD_U32(system + kStreamArray);  // lwz r11,52(r30)
    // add r11,r11,r31 -- 64-bit add of two zero-extended halves.
    const uint32_t stream = uint32_t(uint64_t(array) + offset);
    const uint32_t init = frame + kInitSlot;

    // The field stores, in the original's order. The context pointer at stream+0 is RELOADED
    // before each of the five fields that come out of it; those reloads are reproduced.
    REX_STORE_U32(init + kInitSampleRate, uint32_t(sample_rate));  // stw r27,120(r1)
    REX_STORE_U32(init + kInitSubframes, kSubframes);              // stw r24,112(r1)
    REX_STORE_U32(init + kInitReadOffset, kReadOffsetBits);        // stw r25,96(r1)

    // lbz r10,24(r11) ; addi r10,r10,-1 -- a 64-bit signed decrement of a zero-extended byte, so
    // a stream claiming 0 channels stores 0xFFFFFFFF rather than saturating. Reproduced.
    const uint32_t channels = REX_LOAD_U8(stream + kStreamChannels);
    REX_STORE_U32(init + kInitChannelCount,
                  uint32_t(static_cast<int64_t>(channels) - 1));  // stw r10,116(r1)

    const uint32_t ctx_for_out = REX_LOAD_U32(stream + kStreamContext);  // lwz r9,0(r11)
    REX_STORE_U32(init + kInitOutBuf,
                  REX_LOAD_U32(ctx_for_out + kContextOutBuf));  // lwz r8,12(r9) ; stw r8,100(r1)
    REX_STORE_U32(init + kInitOutBlocks, kOutBlocks);           // stw r26,104(r1)

    const uint32_t ctx_for_work = REX_LOAD_U32(stream + kStreamContext);  // lwz r7,0(r11)
    REX_STORE_U32(init + kInitWorkBuf,
                  REX_LOAD_U32(ctx_for_work + kContextWorkBuf));  // lwz r6,16(r7) ; stw r6,108(r1)

    REX_STORE_U32(init + kInitInBuf0Packets, kPacketCount);  // stw r28,84(r1)
    REX_STORE_U32(init + kInitInBuf1Packets, kPacketCount);  // stw r28,92(r1)

    const uint32_t ctx_for_in0 = REX_LOAD_U32(stream + kStreamContext);  // lwz r5,0(r11)
    REX_STORE_U32(init + kInitInBuf0,
                  REX_LOAD_U32(ctx_for_in0 + kContextInBuf0));  // lwz r3,4(r5) ; stw r3,80(r1)

    const uint32_t ctx_for_in1 = REX_LOAD_U32(stream + kStreamContext);  // lwz r10,0(r11)
    REX_STORE_U32(init + kInitInBuf1,
                  REX_LOAD_U32(ctx_for_in1 + kContextInBuf1));  // lwz r9,8(r10) ; stw r9,88(r1)

    // bl 0x82f9cc44 -- XMADisableContext(id, wait = 1). r4 was set to 1 up with the stores.
    const uint32_t ctx_for_call = REX_LOAD_U32(stream + kStreamContext);  // lwz r8,0(r11)
    ctx.r3.u64 = REX_LOAD_U32(ctx_for_call + kContextId);                 // lwz r3,0(r8)
    ctx.r4.s64 = 1;                                                       // li r4,1
    ctx.lr = 0x82B4FCC0;
    REX_CALL_INDIRECT_FUNC(kXMADisableContext);

    // Every call below re-reads the array pointer out of the System and the context pointer out
    // of the stream, because the import clobbers the volatile registers. `lwzx rN,rArray,r31`
    // loads the word AT stream+0, i.e. the context pointer itself.
    {
      const uint32_t array2 = REX_LOAD_U32(system + kStreamArray);  // lwz r7,52(r30)
      const uint32_t context = REX_LOAD_U32(uint32_t(array2) + uint32_t(offset));  // lwzx r6,r7,r31
      ctx.r3.u64 = REX_LOAD_U32(context + kContextId);  // lwz r3,0(r6)
      ctx.r4.u64 = init;                                // addi r4,r1,80
      ctx.lr = 0x82B4FCD4;
      REX_CALL_INDIRECT_FUNC(kXMAInitializeContext);  // bl 0x82f9cc24
    }
    {
      const uint32_t array2 = REX_LOAD_U32(system + kStreamArray);  // lwz r5,52(r30)
      const uint32_t context = REX_LOAD_U32(uint32_t(array2) + uint32_t(offset));  // lwzx r4,r5,r31
      ctx.r3.u64 = REX_LOAD_U32(context + kContextId);  // lwz r3,0(r4)
      ctx.lr = 0x82B4FCE4;
      REX_CALL_INDIRECT_FUNC(kXMASetOutputBufferValid);  // bl 0x82f9ccc4
    }
    {
      const uint32_t array2 = REX_LOAD_U32(system + kStreamArray);  // lwz r3,52(r30)
      const uint32_t context = REX_LOAD_U32(uint32_t(array2) + uint32_t(offset));  // lwzx r11,r3,r31
      ctx.r5.s64 = kPacketCount;                               // li r5,1
      ctx.r4.u64 = REX_LOAD_U32(context + kContextInBuf0);     // lwz r4,4(r11)
      ctx.r3.u64 = REX_LOAD_U32(context + kContextId);         // lwz r3,0(r11)
      ctx.lr = 0x82B4FCFC;
      REX_CALL_INDIRECT_FUNC(kXMASetInputBuffer0);  // bl 0x82f9cc34
    }
    {
      const uint32_t array2 = REX_LOAD_U32(system + kStreamArray);  // lwz r10,52(r30)
      const uint32_t context = REX_LOAD_U32(uint32_t(array2) + uint32_t(offset));  // lwzx r9,r10,r31
      ctx.r5.s64 = kPacketCount;                               // li r5,1
      ctx.r4.u64 = REX_LOAD_U32(context + kContextInBuf1);     // lwz r4,8(r9)
      ctx.r3.u64 = REX_LOAD_U32(context + kContextId);         // lwz r3,0(r9)
      ctx.lr = 0x82B4FD14;
      REX_CALL_INDIRECT_FUNC(kXMASetInputBuffer1);  // bl 0x82f9cc04
    }
    {
      const uint32_t array2 = REX_LOAD_U32(system + kStreamArray);  // lwz r8,52(r30)
      const uint32_t context = REX_LOAD_U32(uint32_t(array2) + uint32_t(offset));  // lwzx r7,r8,r31
      ctx.r3.u64 = REX_LOAD_U32(context + kContextId);  // lwz r3,0(r7)
      ctx.lr = 0x82B4FD24;
      REX_CALL_INDIRECT_FUNC(kXMAEnableContext);  // bl 0x82f9cc54
    }

    count = REX_LOAD_U32(system + kStreamCount);  // lwz r6,68(r30) -- reloaded every iteration
    index += 1;                                   // addi r29,r29,1
    offset += kStreamStride;                      // addi r31,r31,28
    // cmplw cr6,r29,r6 ; blt cr6,0x82b4fc3c -- UNSIGNED, against the RELOADED count.
  } while (index < count);

  // loc_82B4FD38
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,208
  ctx.lr = ctx.r12.u64;                   // __restgprlr_24 restores lr and r24-r31
  ctx.r24 = saved_r24;
  ctx.r25 = saved_r25;
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. Only one guest store is outside this call's own frame --
// u32[r3+80] -- and everything else it does lands in decoder hardware state the harness has no
// way to rewind. The full reasoning is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B4FC00
SKATE3_PORT(82B4FC00, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
