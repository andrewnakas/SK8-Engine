// sub_82B21AC0  role: the audio worker thread's pump -- hold the queue's critical section, wait on the handle at self+96, drain the command queue with sub_82B48530, fill the current one of two 6144-byte PCM buffers with sub_82B219E8 (zeroing it when that reports no audio), then hand every filled buffer to the device through two of its vtable entries while the device reports two or fewer queued. Around all of it, reconcile the global state mirror at 0x830845C8 with the request at 0x830845CC, moving the lock with the transition, and accumulate the block's elapsed time into 0x830BDEC0   lifted: skate3_recomp.67.cpp:9365 (378 lines)
// STATUS: gate-1 (it takes the lock itself and calls through the device vtable: RtlEnterCriticalSection / RtlLeaveCriticalSection at census depth 0, four call sites, plus sub_82B21AC0 indirect call, `bctrl` at 0x82B21C14 and 0x82B21C60 -- and it would be gate-3 as well, since sub_82B1F7E8 reads mftb)
// calls/session: boot 1  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B21AC0.md
namespace port_82B21AC0 {

// Four globals, all `lis`-derived, computed as ((imm & 0xFFFF) << 16) + displacement:
//   lis r26/r28,-31992 -> 0x83080000   lis r25,-31994 -> 0x83060000
//   lis r11,-31988     -> 0x830C0000 ; addi r29,r11,-8516 -> 0x830BDEBC
// probe/ports/notes/sub_82B21D70.md records this function's global as 0x82044588. That is wrong
// twice over: 0x82040000 is not what `lis -31992` loads, and 0x82040000 + 17864 would not be
// 0x82044588 anyway. sub_82B218E8.inc and sub_82B219E8.inc agree with the values below.
//
// sub_82B218E8 names 0x830845CC kRunState and publishes 1 or 2 into it to start this pump; 0 means
// stopped. 0x830845C8 is this loop's own copy of it -- what it last acted on.
constexpr uint32_t kRunStateMirror = 0x83080000u + 17864u;  // 0x830845C8
constexpr uint32_t kRunState = 0x83080000u + 17868u;        // 0x830845CC
// u8; zero ends the pump. Eight bytes past sub_82B218E8's kArmedByte (0x83067057), not the same.
constexpr uint32_t kRunning = 0x83060000u + 28767u;         // 0x8306705F
// sub_82B219E8 names 0x830BDEBC kProfile; this call uses two of its words as a stopwatch.
constexpr uint32_t kProfileTotal = 0x830C0000u - 8516u + 4u;  // 0x830BDEC0: accumulated block time
constexpr uint32_t kProfileStart = 0x830C0000u - 8516u + 8u;  // 0x830BDEC4: this block's start time

// The two kernel imports the `bl`s reach. probe/ports/lint.py check 6 forbids naming an import
// thunk directly from a port body -- that rule exists so a guest callee goes through GuestCall and
// is exercised by its own hook, which a kernel import has no way to be -- so both are reached
// through their guest addresses in the dispatch table, which is the same code the `bl` enters.
// The one difference from the original is that this writes ctr, which the `bl` does not; ctr is
// volatile and outside the compared set, and this body never runs anyway (gate-1).
constexpr uint32_t kRtlEnterCriticalSection = 0x82F9CB44;  // bl 0x82f9cb44
constexpr uint32_t kRtlLeaveCriticalSection = 0x82F9CB54;  // bl 0x82f9cb54

// The worker object (r3/r31). Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kQueue = 8;        // -> the system object sub_82B48530 drains; its +92 is the lock
constexpr uint32_t kDevice = 76;      // -> the output device; its +0 is the vtable
constexpr uint32_t kPcmBase = 84;     // -> the two PCM buffers, 6144 bytes each
constexpr uint32_t kSubmitIndex = 88; // s32, 0 or 1: which buffer is next to hand to the device
constexpr uint32_t kFillIndex = 92;   // s32, 0 or 1: which buffer is next to fill
constexpr uint32_t kWaitHandle = 96;  // -> the handle sub_82EDFEC0 waits on
constexpr uint32_t kDescriptors = 100;  // one 36-byte descriptor per buffer
constexpr uint32_t kDescStride = 36;
constexpr uint32_t kSlots = 43;       // the slot array, addressed as (index + 43) * 4, i.e. self+172
constexpr uint32_t kBufferBytes = 6144;  // li r5,6144; also 3 << 11, which is how the offset is built

constexpr uint32_t kQueueLock = 92;   // the queue object's RTL_CRITICAL_SECTION*
constexpr uint32_t kVtable = 0;       // device +0
constexpr uint32_t kVtStatus = 100;   // vtable +100: (device, &status)
constexpr uint32_t kVtSubmit = 84;    // vtable +84:  (device, &descriptor, 0)

constexpr uint32_t kFrameBytes = 192;   // stwu r1,-192(r1)
constexpr uint32_t kStatus = 80;        // addi r4,r1,80 -- the status block the device fills
constexpr uint32_t kStatusQueued = 84;  // lwz r9,84(r1) -- its second word: buffers still queued

// cmpw / cmpwi: a SIGNED compare of the low words, whatever the 64-bit registers hold above them.
inline int32_t Word(uint64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }

// lwz rN,88/92(r31) ; addi rN,rN,43 ; rlwinm rM,rN,2,0,29 ; lwzx/stwx ...,rM,r31 -- the per-buffer
// state word: 0 free, 1 filled, 2 handed to the device.
inline uint32_t SlotAddr(uint32_t self, uint64_t index) {
  return self + (static_cast<uint32_t>(index + kSlots) << 2);
}

// lwz r11,88(r31) ; rlwinm r10,r11,3,0,28 ; add r9,r11,r10 ; rlwinm r11,r9,2,0,29 ; add r11,r11,r31
// ; addi r4,r11,100 -- index * 9 * 4 = index * 36.
inline uint32_t DescAddr(uint32_t self, uint64_t index) {
  return (self + (static_cast<uint32_t>(static_cast<uint32_t>(index) * 9u) << 2)) + kDescriptors;
}

inline void CallImport(PPCContext& ctx, uint8_t* base, uint32_t import, uint64_t lock, uint32_t link) {
  ctx.r3.u64 = lock;
  ctx.ctr.u64 = import;
  ctx.lr = link;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

REX_FUNC(Native) {
  const uint64_t entry_lr = ctx.lr;  // mflr r12, spilled by __savegprlr_22

  // stwu r1,-192(r1). The frame is reproduced because the device's status method is handed a
  // pointer into it (r1+80). __savegprlr_22's spills of r22..r31 land in it as well; they are the
  // function's own frame and are not reproduced, because this body keeps those values in locals
  // and leaves the registers alone.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;                    // mr r31,r3
  const uint32_t queue = REX_LOAD_U32(self + kQueue);  // lwz r27,8(r3)

  // lwz r11,17868(r26) ; lwz r3,92(r27) ; stw r11,17864(r28) ; bl 0x82f9cb44 -- the mirror starts
  // at whatever is currently requested, and the lock is taken before anything else. The lock
  // pointer is loaded before the store, and reloaded at every later use.
  const uint64_t requested = REX_LOAD_U32(kRunState);
  const uint32_t lock = REX_LOAD_U32(queue + kQueueLock);
  REX_STORE_U32(kRunStateMirror, static_cast<uint32_t>(requested));
  CallImport(ctx, base, kRtlEnterCriticalSection, lock, 0x82B21AEC);

  // lbz r11,28767(r25) ; cmplwi cr6,r11,0 ; beq -> loc_82B21D50
  bool stop = false;  // a path that jumps straight to loc_82B21D50, skipping the running re-test
  if (REX_LOAD_U8(kRunning) != 0) {
    for (;;) {
      // loc_82B21B14: bl 0x82edfec0 -- wait on the handle, r4 = -1 (li r22,-1, sign-extended)
      skate3::audio::GuestCall(ctx, base, sub_82EDFEC0, REX_LOAD_U32(self + kWaitHandle),
                               int64_t(-1));

      // lwzx r9,r10,r31 ; cmpwi cr6,r9,0 ; bne -> loc_82B21D44 -- nothing to do unless the buffer
      // this pass would fill is free.
      if (Word(REX_LOAD_U32(SlotAddr(self, REX_LOAD_U32(self + kFillIndex)))) == 0) {
        for (;;) {
          // loc_82B21B3C: lbz r11,28767(r10) ; beq -> loc_82B21D44
          if (REX_LOAD_U8(kRunning) == 0) break;

          // lwz r11,17864(r28) ; cmpwi cr6,r11,0 ; bne -> loc_82B21B6C
          if (Word(REX_LOAD_U32(kRunStateMirror)) == 0) {
            // Stopped: drain the command queue and nothing else.
            skate3::audio::GuestCall(ctx, base, sub_82B48530, queue);
            if (REX_LOAD_U8(kRunning) == 0) {  // -> loc_82B21D50
              stop = true;
              break;
            }
            // b 0x82b21cac
          } else if (Word(REX_LOAD_U32(kRunStateMirror)) == 1) {  // loc_82B21B6C -- mirror RELOADED
            // bl 0x82b1f7e8 ; stw r11,8(r29) -- the block's start time
            skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
            REX_STORE_U32(kProfileStart, ctx.r3.u32);
            skate3::audio::GuestCall(ctx, base, sub_82B48530, queue);
            if (REX_LOAD_U8(kRunning) == 0) {  // -> loc_82B21D50
              stop = true;
              break;
            }
            // lwz r11,17868(r26) ; cmpwi cr6,r11,1 ; bne -> loc_82B21CAC -- the REQUEST, not the
            // mirror: a stop asked for during the drain skips the block.
            if (Word(REX_LOAD_U32(kRunState)) == 1) {
              // bl 0x82b219e8 -- fill the current buffer; anything but 1 means it produced nothing
              skate3::audio::GuestCall(ctx, base, sub_82B219E8, self);
              if (Word(ctx.r3.u64) != 1) {
                // rlwinm r10,r11,1 ; add r11,r11,r10 ; rlwinm r11,r11,11 -- index * 3 * 2048
                const uint64_t fill = REX_LOAD_U32(self + kFillIndex);   // lwz r11,92(r31)
                const uint64_t pcm = REX_LOAD_U32(self + kPcmBase);      // lwz r9,84(r31)
                const uint32_t offset = static_cast<uint32_t>(fill * 3u) << 11;
                // bl 0x82ee5e80 -- (buffer, 0, 6144)
                skate3::audio::GuestCall(ctx, base, sub_82EE5E80, uint64_t(offset) + pcm, 0,
                                         kBufferBytes);
              }

              // loc_82B21BD8: stwx r23,r10,r31 -- the buffer is filled (1)
              REX_STORE_U32(SlotAddr(self, REX_LOAD_U32(self + kFillIndex)), 1);

              // lwzx r7,r8,r31 ; cmpwi cr6,r7,1 ; bne -> loc_82B21C94, and the same test again at
              // loc_82B21C7C: hand over every filled buffer the device will still take.
              while (Word(REX_LOAD_U32(SlotAddr(self, REX_LOAD_U32(self + kSubmitIndex)))) == 1) {
                // loc_82B21C00: lwz r3,76(r31) ; lwz r11,0(r3) ; lwz r10,100(r11) ; bctrl
                const uint32_t device = REX_LOAD_U32(self + kDevice);
                ctx.r3.u64 = device;
                ctx.r4.u64 = frame + kStatus;  // addi r4,r1,80
                ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(device + kVtable) + kVtStatus);
                ctx.lr = 0x82B21C18;
                REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

                // lwz r9,84(r1) ; cmplwi cr6,r9,2 ; bgt -> loc_82B21C94 -- UNSIGNED
                if (REX_LOAD_U32(frame + kStatusQueued) > 2u) break;

                // lwz r11,88(r31) ; stwx r30,r10,r31 -- the buffer is the device's now (2)
                REX_STORE_U32(SlotAddr(self, REX_LOAD_U32(self + kSubmitIndex)), 2);
                // lwz r11,88(r31) -- reloaded after that store
                const uint64_t submit = REX_LOAD_U32(self + kSubmitIndex);
                const uint32_t device_now = REX_LOAD_U32(self + kDevice);  // lwz r3,76(r31)
                ctx.r3.u64 = device_now;
                ctx.r4.u64 = DescAddr(self, submit);
                ctx.r5.s64 = 0;  // li r5,0
                ctx.ctr.u64 = REX_LOAD_U32(REX_LOAD_U32(device_now + kVtable) + kVtSubmit);
                ctx.lr = 0x82B21C64;
                REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

                // lwz r11,88(r31) ; addi r11,r11,1 ; stw ; cmpwi cr6,r11,2 ; bne ; stw r24,88(r31)
                // -- the index is written, then written again as zero when it reached two.
                const uint64_t bumped = REX_LOAD_U32(self + kSubmitIndex) + 1;
                REX_STORE_U32(self + kSubmitIndex, static_cast<uint32_t>(bumped));
                if (Word(bumped) == 2) REX_STORE_U32(self + kSubmitIndex, 0);
              }

              // loc_82B21C94: the same two-store wrap for the fill index
              const uint64_t next_fill = REX_LOAD_U32(self + kFillIndex) + 1;
              REX_STORE_U32(self + kFillIndex, static_cast<uint32_t>(next_fill));
              if (Word(next_fill) == 2) REX_STORE_U32(self + kFillIndex, 0);
            }
          }

          // loc_82B21CAC: charge the elapsed time, unless nothing is requested or the mirror
          // already reads 2.
          if (Word(REX_LOAD_U32(kRunState)) != 0 && Word(REX_LOAD_U32(kRunStateMirror)) != 2) {
            skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
            const uint64_t now = ctx.r3.u64;
            const uint64_t total = REX_LOAD_U32(kProfileTotal);    // lwz r10,4(r29)
            const uint64_t started = REX_LOAD_U32(kProfileStart);  // lwz r11,8(r29)
            // subf r11,r11,r10 ; add r11,r3,r11 -- 64-bit, stored as a word
            REX_STORE_U32(kProfileTotal, static_cast<uint32_t>(now + (total - started)));
          }

          // loc_82B21CDC: reconcile the mirror with the request, moving the lock with it. 2 is the
          // state in which the lock is NOT held: leaving it re-acquires, entering it releases.
          const uint64_t mirror = REX_LOAD_U32(kRunStateMirror);
          const uint64_t request = REX_LOAD_U32(kRunState);
          if (Word(mirror) != Word(request)) {
            // rotlwi r11,r11,0 -- a no-op copy ; cmpwi cr6,r11,2 ; bne -> loc_82B21D00
            if (Word(mirror) == 2) {
              CallImport(ctx, base, kRtlEnterCriticalSection, REX_LOAD_U32(queue + kQueueLock),
                         0x82B21D00);
            }
            // loc_82B21D00: lwz r11,17868(r26) -- the request is reloaded here
            if (Word(REX_LOAD_U32(kRunState)) == 2) {
              CallImport(ctx, base, kRtlLeaveCriticalSection, REX_LOAD_U32(queue + kQueueLock),
                         0x82B21D14);
            }
            // loc_82B21D14: reloaded a third time, and that is what the mirror records
            REX_STORE_U32(kRunStateMirror, REX_LOAD_U32(kRunState));
          }

          // loc_82B21D1C: go round the inner loop only while running is still asked for and the
          // next buffer to fill is free; otherwise -> loc_82B21D44.
          if (Word(REX_LOAD_U32(kRunState)) != 1) break;
          if (Word(REX_LOAD_U32(SlotAddr(self, REX_LOAD_U32(self + kFillIndex)))) != 0) break;
        }
      }

      if (stop) break;
      // loc_82B21D44: lbz r11,28767(r25) ; bne -> loc_82B21B14
      if (REX_LOAD_U8(kRunning) == 0) break;
    }
  }

  // loc_82B21D50: release the lock unless the mirror says it was already given up.
  if (Word(REX_LOAD_U32(kRunStateMirror)) != 2) {
    CallImport(ctx, base, kRtlLeaveCriticalSection, REX_LOAD_U32(queue + kQueueLock), 0x82B21D64);
  }

  // addi r1,r1,192 ; b __restgprlr_22. That stub reloads r22..r31 and lr from the frame; this body
  // keeps those registers in locals and never writes them. No `li r3` on any path: r3 is left as
  // the last call put it.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.lr = entry_lr;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. Why no window set exists at all is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B21AC0
SKATE3_PORT(82B21AC0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
