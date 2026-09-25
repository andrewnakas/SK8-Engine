// sub_82B3CA60  role: deliver `count` frames from a stream object into a caller's buffer descriptor -- drain whatever is already pending in the object's own scratch descriptor, then refill it through the fill function at +20 and copy channel by channel with sub_82EDF460, crediting sub_82B3C9D8 each time; when the object has no scratch buffer (+51 == 0) the fill function writes straight into the caller's descriptor. Returns the number of frames delivered   lifted: skate3_recomp.68.cpp:8181 (338 lines)
// STATUS: gate-1 (its own two bctrl through the fill pointer at +20, plus a callee with four more: sub_82B3CA60 indirect call, `bctrl` at 0x82B3CB80 and 0x82B3CCA0; sub_82B3C9D8 indirect call)
// calls/session: boot 134290  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3CA60.md
namespace port_82B3CA60 {

// Nothing in docs/rw_audio_structs.h names either record. The names are read off the uses here.

// The stream object (r3/r31).
constexpr uint32_t kFillFn = 20;         // u32 function pointer: fill(self, descriptor, frames)
constexpr uint32_t kConsumed = 28;       // u32 subtracted from the entry's total to cap a refill
constexpr uint32_t kEntryTable = 36;     // u32 byte offset, inside the object, of the entry table
constexpr uint32_t kScratchOffset = 40;  // u32 byte offset, inside the object, of the scratch desc
constexpr uint32_t kPending = 44;        // u16 frames sitting unconsumed in the scratch descriptor
constexpr uint32_t kChannels = 46;       // u8 channel count: the copy loop's trip count
constexpr uint32_t kEntryIndex = 49;     // u8 index of the active 24-byte entry
constexpr uint32_t kUsesScratch = 51;    // u8 nonzero: refill into the scratch desc, then copy

// A buffer descriptor. The object's scratch one (r29) and the caller's (r4/r26) share the layout.
constexpr uint32_t kBufData = 4;    // u32 -> the samples, four bytes each
constexpr uint32_t kBufFilled = 12; // u16 frames the last refill produced
constexpr uint32_t kBufStride = 14; // u16 frames between one channel's data and the next

// An entry in the table at object + u32(object + 36).
constexpr uint32_t kEntryStride = 24;  // lbz ; rotlwi 1 ; add (x3) ; rlwinm 3 (x8)
constexpr uint32_t kEntryTotal = 12;   // u32 total frames available; zero ends the delivery

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)

// cmpw / cmpwi: a SIGNED compare of the low words, whatever the 64-bit registers hold above them.
inline int32_t Word(uint64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }

// The epilogue both return paths share: mr r3,r27 ; addi r1,r1,160 ; b __restgprlr_24. That stub
// reloads r24..r31 and lr from the frame; this body keeps those registers in locals and never
// writes them, so restoring lr from the entry value is the same result.
inline void Leave(PPCContext& ctx, uint64_t entry_lr, uint64_t delivered) {
  ctx.r3.u64 = delivered;
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.lr = entry_lr;
}

REX_FUNC(Native) {
  const uint64_t entry_lr = ctx.lr;  // mflr r12, spilled by __savegprlr_24

  // stwu r1,-160(r1). Nothing in this body reads the frame -- it exists for the three callees and
  // for __savegprlr_24's spills, which are the function's own frame and are not reproduced.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;        // r31
  const uint32_t dest = ctx.r4.u32;        // r26, the caller's descriptor
  const uint64_t requested = ctx.r5.u64;   // r24, compared only as a signed low word
  uint64_t delivered = 0;                  // r27, and the return value: all 64 bits of it

  // lbz r11,51(r3) ; cmplwi cr6,r11,0 ; beq -> loc_82B3CC44
  if (REX_LOAD_U8(self + kUsesScratch) != 0) {
    // lwz r11,40(r3) ; add r29,r11,r3
    const uint32_t scratch = REX_LOAD_U32(self + kScratchOffset) + self;  // r29

    // lhz r11,44(r3) ; cmplwi cr6,r11,0 ; beq -> loc_82B3CB28
    const uint64_t pending = REX_LOAD_U16(self + kPending);
    if (pending != 0) {
      // cmpw cr6,r11,r5 ; mr r27,r11 ; blt -> loc_82B3CAAC ; mr r27,r5 -- min(pending, requested),
      // signed, and when requested wins it is r5's full 64 bits that land in r27.
      delivered = Word(pending) < Word(requested) ? pending : requested;

      // loc_82B3CAAC: lbz r11,46(r31) ; li r30,0 ; cmplwi ; beq -> loc_82B3CB10
      if (REX_LOAD_U8(self + kChannels) != 0) {
        // rlwinm r28,r27,2,0,29 -- frames to bytes, from the low 32 bits of r27
        const uint64_t bytes = static_cast<uint32_t>(delivered) << 2;
        uint64_t channel = 0;  // r30
        for (;;) {
          // loc_82B3CAC0. Every field is reloaded each iteration: sub_82EDF460 writes into the
          // caller's buffer, which may alias any of them.
          const uint64_t src_stride = REX_LOAD_U16(scratch + kBufStride);   // lhz r11,14(r29)
          const uint64_t pending_now = REX_LOAD_U16(self + kPending);       // lhz r9,44(r31)
          // mullw r7,r11,r30
          const int64_t src_channel = int64_t(Word(src_stride)) * int64_t(Word(channel));
          const uint64_t filled = REX_LOAD_U16(scratch + kBufFilled);       // lhz r11,12(r29)
          const uint64_t dst_stride = REX_LOAD_U16(dest + kBufStride);      // lhz r6,14(r26)
          const uint64_t src_base = REX_LOAD_U32(scratch + kBufData);       // lwz r10,4(r29)
          const uint64_t dst_base = REX_LOAD_U32(dest + kBufData);          // lwz r8,4(r26)
          // subf r9,r9,r7 ; add r3,r9,r11 -- the pending run starts `pending` frames back from
          // what the last refill filled. 64-bit, but the rlwinm below keeps only the low word.
          const uint64_t src_index = (static_cast<uint64_t>(src_channel) - pending_now) + filled;
          // mullw r4,r6,r30 ; rlwinm r9,r4,2 ; add r3,r9,r8
          const int64_t dst_channel = int64_t(Word(dst_stride)) * int64_t(Word(channel));
          const uint64_t dst_ptr = (static_cast<uint32_t>(dst_channel) << 2) + dst_base;
          // rlwinm r11,r3,2 ; add r4,r11,r10
          const uint64_t src_ptr = (static_cast<uint32_t>(src_index) << 2) + src_base;
          // bl 0x82edf460 -- (dest, source, bytes)
          skate3::audio::GuestCall(ctx, base, sub_82EDF460, dst_ptr, src_ptr, bytes);
          // lbz r11,46(r31) ; addi r30,r30,1 ; cmplw cr6,r30,r11 ; blt (unsigned, reloaded)
          const uint32_t channels = REX_LOAD_U8(self + kChannels);
          channel = channel + 1;
          if (!(static_cast<uint32_t>(channel) < channels)) break;
        }
      }

      // loc_82B3CB10: lhz r11,44(r31) ; subf r10,r27,r11 ; sth r10,44(r31)
      const uint64_t left = REX_LOAD_U16(self + kPending) - delivered;
      REX_STORE_U16(self + kPending, static_cast<uint16_t>(left));
      // bl 0x82b3c9d8 -- credits the frames just handed over; it has four bctrl of its own
      skate3::audio::GuestCall(ctx, base, sub_82B3C9D8, self, delivered);
    }

    // loc_82B3CB28: cmpw cr6,r27,r24 ; bge -> loc_82B3CCBC
    while (Word(delivered) < Word(requested)) {
      // loc_82B3CB30: lbz r11,49(r31) ; rotlwi/add/rlwinm -- 24 bytes per entry
      const uint32_t entry =
          (kEntryStride * REX_LOAD_U8(self + kEntryIndex) + self) + REX_LOAD_U32(self + kEntryTable);
      // lwz r10,12(r28) ; cmpwi cr6,r10,0 ; beq -> loc_82B3CCBC
      if (Word(REX_LOAD_U32(entry + kEntryTotal)) == 0) break;

      // lhz r11,14(r29) ; subf r30,r27,r24 ; cmpw cr6,r30,r11 ; blt -- one refill is capped by the
      // scratch descriptor's stride, i.e. one channel's worth.
      const uint64_t src_stride = REX_LOAD_U16(scratch + kBufStride);
      const uint64_t remaining = requested - delivered;  // r30
      const uint64_t ask = Word(remaining) < Word(src_stride) ? remaining : src_stride;

      // lwz r11,20(r31) ; mtctr r11 ; bctrl -- fill(self, scratch, ask)
      ctx.r3.u64 = self;
      ctx.r4.u64 = scratch;
      ctx.r5.u64 = ask;
      ctx.ctr.u64 = REX_LOAD_U32(self + kFillFn);
      ctx.lr = 0x82B3CB84;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      const uint64_t produced = ctx.r3.u64;

      // lwz r10,12(r28) ; lwz r9,28(r31) ; subf r11,r9,r10 ; cmpw cr6,r3,r11 ; bge ; mr r11,r3
      const uint64_t total = REX_LOAD_U32(entry + kEntryTotal);
      const uint64_t consumed = REX_LOAD_U32(self + kConsumed);
      uint64_t accepted = total - consumed;
      if (Word(produced) < Word(accepted)) accepted = produced;
      // clrlwi r11,r11,16 ; sth r11,44(r31) ; sth r11,12(r29)
      const uint16_t stored = static_cast<uint16_t>(accepted);
      REX_STORE_U16(self + kPending, stored);
      REX_STORE_U16(scratch + kBufFilled, stored);
      // lhz r11,44(r31) -- a genuine reload: scratch+12 is self + u32(self+40) + 12, which is
      // self+44 whenever that offset is 32, and then the second store just overwrote the first.
      const uint64_t pending_now = REX_LOAD_U16(self + kPending);
      // cmpw cr6,r11,r30 ; mr r25,r11 ; blt ; mr r25,r30
      const uint64_t chunk = Word(pending_now) < Word(remaining) ? pending_now : remaining;  // r25

      // loc_82B3CBBC: lbz r11,46(r31) ; li r30,0 ; cmplwi ; beq -> loc_82B3CC14
      if (REX_LOAD_U8(self + kChannels) != 0) {
        // rlwinm r28,r25,2,0,29
        const uint64_t bytes = static_cast<uint32_t>(chunk) << 2;
        uint64_t channel = 0;  // r30, reused
        for (;;) {
          // loc_82B3CBD0. This copy starts at the head of the scratch buffer and lands `delivered`
          // frames into the caller's, the mirror of the drain loop above.
          const uint64_t dst_stride = REX_LOAD_U16(dest + kBufStride);     // lhz r11,14(r26)
          const uint64_t stride = REX_LOAD_U16(scratch + kBufStride);      // lhz r9,14(r29)
          // mullw r11,r11,r30
          const int64_t dst_channel = int64_t(Word(dst_stride)) * int64_t(Word(channel));
          const uint64_t src_base = REX_LOAD_U32(scratch + kBufData);      // lwz r8,4(r29)
          const uint64_t dst_base = REX_LOAD_U32(dest + kBufData);         // lwz r10,4(r26)
          // add r7,r11,r27
          const uint64_t dst_index = static_cast<uint64_t>(dst_channel) + delivered;
          // mullw r6,r9,r30 ; rlwinm r9,r6,2 ; add r4,r9,r8
          const int64_t src_channel = int64_t(Word(stride)) * int64_t(Word(channel));
          const uint64_t src_ptr = (static_cast<uint32_t>(src_channel) << 2) + src_base;
          // rlwinm r11,r7,2 ; add r3,r11,r10
          const uint64_t dst_ptr = (static_cast<uint32_t>(dst_index) << 2) + dst_base;
          // bl 0x82edf460
          skate3::audio::GuestCall(ctx, base, sub_82EDF460, dst_ptr, src_ptr, bytes);
          // lbz r5,46(r31) ; addi r30,r30,1 ; cmplw cr6,r30,r5 ; blt (unsigned, reloaded)
          const uint32_t channels = REX_LOAD_U8(self + kChannels);
          channel = channel + 1;
          if (!(static_cast<uint32_t>(channel) < channels)) break;
        }
      }

      // loc_82B3CC14: lhz r11,44(r31) ; subf r10,r25,r11 ; add r27,r25,r27 ; sth r10,44(r31)
      const uint64_t left = REX_LOAD_U16(self + kPending) - chunk;
      delivered = chunk + delivered;
      REX_STORE_U16(self + kPending, static_cast<uint16_t>(left));
      // bl 0x82b3c9d8
      skate3::audio::GuestCall(ctx, base, sub_82B3C9D8, self, chunk);
      // cmpw cr6,r27,r24 ; blt -> loc_82B3CB30
    }
    Leave(ctx, entry_lr, delivered);
    return;
  }

  // loc_82B3CC44: no scratch descriptor -- the fill function writes into the caller's buffer and
  // the frames it claims to have produced are ignored; the request size is credited instead.
  // cmpwi cr6,r24,0 ; ble -> loc_82B3CCBC
  if (Word(requested) > 0) {
    do {
      // loc_82B3CC4C -- the same entry address, recomputed each pass
      const uint32_t entry =
          (kEntryStride * REX_LOAD_U8(self + kEntryIndex) + self) + REX_LOAD_U32(self + kEntryTable);
      // lwz r11,12(r11) ; cmpwi cr6,r11,0 ; beq -> loc_82B3CCBC
      const uint64_t total = REX_LOAD_U32(entry + kEntryTotal);
      if (Word(total) == 0) break;

      // lwz r9,28(r31) ; subf r10,r27,r24 ; subf r30,r9,r11 ; cmpw cr6,r10,r30 ; bge ; mr r30,r10
      const uint64_t consumed = REX_LOAD_U32(self + kConsumed);
      const uint64_t remaining = requested - delivered;
      uint64_t chunk = total - consumed;  // r30
      if (Word(remaining) < Word(chunk)) chunk = remaining;

      // loc_82B3CC8C: lwz r11,20(r31) ; mtctr r11 ; bctrl -- fill(self, caller's descriptor, chunk)
      ctx.r3.u64 = self;
      ctx.r4.u64 = dest;
      ctx.r5.u64 = chunk;
      ctx.ctr.u64 = REX_LOAD_U32(self + kFillFn);
      ctx.lr = 0x82B3CCA4;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      // r3 is NOT read back here: add r27,r30,r27 credits what was asked for, not what came back.
      delivered = chunk + delivered;
      // bl 0x82b3c9d8
      skate3::audio::GuestCall(ctx, base, sub_82B3C9D8, self, chunk);
      // cmpw cr6,r27,r24 ; blt -> loc_82B3CC4C
    } while (Word(delivered) < Word(requested));
  }

  // loc_82B3CCBC
  Leave(ctx, entry_lr, delivered);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3CA60
SKATE3_PORT(82B3CA60, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
