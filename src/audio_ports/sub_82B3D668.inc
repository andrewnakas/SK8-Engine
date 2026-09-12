// sub_82B3D668  role: grow the stream's float ring to hold a requested span -- with no ring yet, hand the whole job to sub_82B3D590; otherwise round the request up to a multiple of 32, add the lag at +24, and when that exceeds the span at +20 allocate channels(+48) * span words through the System allocator's vtable slot +4, move each channel's live words into the new ring with three memcpys, free the old ring through slot +12, and publish the new base, span and position -- returns 1, or 0 when the allocation failed   lifted: skate3_recomp.68.cpp:9982 (284 lines)
// STATUS: gate-1 (two of its own bctrl: the allocation at 0x82B3D6F8 through the System allocator's vtable slot +4, and the free of the old ring at 0x82B3D838 through slot +12 -- sub_82B3D668 indirect call)
// calls/session: boot 18  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3D668.md
namespace port_82B3D668 {

// The stream object (r3), the same one sub_82B3DB90 / sub_82B3DC48 / sub_82B3DD90 / sub_82B3DEA8
// / sub_82B3DF90 take; nothing in docs/rw_audio_structs.h names it. This function is where its
// ring is created and resized, which is what fixes the units: the ring holds `channels` (+48)
// consecutive per-channel spans of `span` (+20) words each.
constexpr uint32_t kObjRing = 0;        // the float ring's byte base -- written here
constexpr uint32_t kObjRequest = 12;    // u32, the last requested span; written on every success
constexpr uint32_t kObjBlock = 16;      // sub_82B3DD90's kObjBlock; only passed on to sub_82B3D590
constexpr uint32_t kObjSpanHigh = 20;   // per-channel span in words, lag included -- written here
constexpr uint32_t kObjSpanLow = 24;    // the lag; the span is rounded request PLUS this
constexpr uint32_t kObjCursor = 36;     // sub_82B3DF90's kCursor: the words carried over
constexpr uint32_t kObjChannels = 48;   // the channel count: the loop bound and the multiplier
constexpr uint32_t kObjPosition = 52;   // the running word position -- reset to the lag here

// lis r20,-31993 ; lwz r11,30252(r20) -> 0x83070000 + 0x762C, the global System pointer that
// port_82B219E8 names kStatsOwner and port_82B1E458 names kSystemPtr.
constexpr uint32_t kSystemPtr = 0x83070000u + 30252u;  // 0x8307762C
constexpr uint32_t kAllocator = 36;  // System+36 -> the allocator object
constexpr uint32_t kVptr = 0;        // allocator+0 -> its vtable
constexpr uint32_t kAllocSlot = 4;   // that vtable +4:  allocate(bytes, tag, 0, align)
constexpr uint32_t kFreeSlot = 12;   // that vtable +12: free(block, 0) -- the same entry
                                     // sub_82B32870 and sub_82B48F28 use

// lis r11,-32241 ; addi r5,r11,-10732 -- computed as ((-32241 & 0xFFFF) << 16) - 10732, i.e.
// 0x820F0000 - 0x29EC. Almost certainly the allocation's name/tag pointer (rodata, and the only
// argument that is a constant address); its contents were not read.
constexpr uint32_t kAllocTag = 0x820F0000u - 10732u;  // 0x820ED614
constexpr uint32_t kAllocAlign = 128;                 // li r7,128

constexpr uint32_t kFrameBytes = 208;  // stwu r1,-208(r1)

// bctrl return addresses, reproduced so a callee that reads lr sees what it would see.
constexpr uint32_t kAllocReturn = 0x82B3D6FCu;
constexpr uint32_t kFreeReturn = 0x82B3D83Cu;

// rlwinm rN,rX,2,0,29 -- the low word scaled by four, zero-extended back to 64 bits. Shared with
// the rest of the family; identical to port_82B3DEA8's WordsToBytes.
inline uint64_t WordsToBytes(uint64_t value) {
  return (uint64_t(uint32_t(value)) << 2) & 0xFFFFFFFCull;
}

// divw rD,rA,rB, lifted verbatim: a zero divisor or the INT32_MIN/-1 overflow yields 0, and the
// quotient is zero-extended from 32 bits.
inline uint64_t DivW(uint64_t dividend, uint64_t divisor) {
  const int32_t a = int32_t(uint32_t(dividend));
  const int32_t b = int32_t(uint32_t(divisor));
  return uint32_t((b && !(a == INT32_MIN && b == -1)) ? a / b : 0);
}

// mullw rD,rA,rB -- the full 64-bit product of the sign-extended low words, so a carry into
// bit 32 survives into the address arithmetic.
inline uint64_t MulLw(uint64_t a, uint64_t b) {
  return uint64_t(int64_t(int32_t(uint32_t(a))) * int64_t(int32_t(uint32_t(b))));
}

REX_FUNC(Native) {
  // The frame is reproduced because all five callees run on it. __savegprlr_18's spills of
  // r18..r31 and lr land inside these 208 bytes, so they are this call's own frame and are not
  // windowed; they are not reproduced either, because this body keeps those values in locals and
  // never writes the guest registers, so __restgprlr_18 would have nothing to undo.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f520f0
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-208(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;      // mr r31,r3
  const uint64_t request = ctx.r4.u64;   // mr r19,r4

  // lwz r11,0(r3) ; cmplwi cr6,r11,0 ; bne cr6,0x82b3d6a0
  if (REX_LOAD_U32(self + kObjRing) == 0) {
    // No ring yet: sub_82B3D590(self, channels, request, block) does the first allocation, and
    // its r3 is this function's result -- the lifted form falls straight into the epilogue.
    // The two loads are done in the lifted order (+16 then +48) before the call is set up.
    const uint64_t block = REX_LOAD_U32(self + kObjBlock);         // lwz r6,16(r3)
    const uint64_t channels = REX_LOAD_U32(self + kObjChannels);   // lwz r4,48(r3)
    skate3::audio::GuestCall(ctx, base, sub_82B3D590, uint64_t(self), channels, request, block);
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,208
    ctx.lr = entry_lr;                      // b 0x82f52140 (__restgprlr_18)
    return;
  }

  // loc_82B3D6A0 -- addi r10,r19,32 ; rlwinm r10,r10,0,0,26. The request rounded up to a multiple
  // of 32; a request already on a multiple gets a whole extra 32.
  const uint64_t rounded = uint64_t(uint32_t(uint32_t(request) + 32u) & 0xFFFFFFE0u);
  const uint64_t lag = REX_LOAD_U32(self + kObjSpanLow);         // lwz r11,24(r31)
  const uint64_t channels = REX_LOAD_U32(self + kObjChannels);   // lwz r9,48(r31)
  const uint64_t old_span = REX_LOAD_U32(self + kObjSpanHigh);   // lwz r8,20(r31)
  const uint64_t new_span = rounded + lag;                       // add r23,r10,r11
  // mullw r7,r9,r23 ; rlwinm r4,r7,2,0,29 -- the whole ring, every channel, in bytes.
  const uint64_t total_bytes = WordsToBytes(MulLw(channels, new_span));

  // cmpw cr6,r8,r23 ; bge cr6,0x82b3d84c -- SIGNED: the ring only ever grows.
  if (int32_t(uint32_t(old_span)) < int32_t(uint32_t(new_span))) {
    // lwz r11,30252(r20) ; lwz r3,36(r11) ; lwz r10,0(r3) ; lwz r9,4(r10) ; mtctr r9 ; bctrl
    // allocate(bytes, tag, 0, 128). This is the first gate-1 call: an allocation can never be
    // replayed on rewound memory.
    const uint32_t system = REX_LOAD_U32(kSystemPtr);
    const uint32_t allocator = REX_LOAD_U32(system + kAllocator);
    const uint32_t alloc_vtable = REX_LOAD_U32(allocator + kVptr);
    ctx.ctr.u64 = REX_LOAD_U32(alloc_vtable + kAllocSlot);
    ctx.r3.u64 = allocator;
    ctx.r4.u64 = total_bytes;
    ctx.r5.s64 = int32_t(kAllocTag);  // lis ; addi -- sign-extended into the high word, as lifted
    ctx.r6.s64 = 0;                   // li r6,0
    ctx.r7.s64 = kAllocAlign;         // li r7,128
    ctx.lr = kAllocReturn;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

    const uint64_t buffer = ctx.r3.u64;  // mr r21,r3
    // cmplwi cr6,r3,0 ; beq cr6,0x82b3d85c -- a failed allocation writes NOTHING, not even +12.
    if (uint32_t(buffer) == 0) {
      ctx.r3.s64 = 0;                         // li r18,0 ; mr r3,r18
      ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,208
      ctx.lr = entry_lr;                      // b 0x82f52140
      return;
    }

    // lwz r11,48(r31) ; cmpwi cr6,r11,0 ; ble cr6,0x82b3d814 -- SIGNED, and reloaded: this is not
    // the `channels` read above.
    if (int32_t(REX_LOAD_U32(self + kObjChannels)) > 0) {
      const uint64_t new_stride = WordsToBytes(new_span);  // rlwinm r22,r23,2,0,29
      uint64_t new_base = buffer;                          // mr r24,r3
      for (int32_t c = 0;;) {
        // loc_82B3D720 -- every field is reloaded every iteration; three calls run in between,
        // so nothing may be hoisted out of the loop.
        const uint64_t span = REX_LOAD_U32(self + kObjSpanHigh);    // lwz r11,20(r31)
        const uint64_t position = REX_LOAD_U32(self + kObjPosition);// lwz r6,52(r31)
        const uint64_t channel_words = MulLw(span, uint64_t(uint32_t(c)));  // mullw r5,r11,r26
        const uint64_t ring = REX_LOAD_U32(self + kObjRing);        // lwz r7,0(r31)
        const uint64_t lag_now = REX_LOAD_U32(self + kObjSpanLow);  // lwz r10,24(r31)
        const uint64_t filled = REX_LOAD_U32(self + kObjCursor);    // lwz r9,36(r31)
        const uint64_t quotient = DivW(position, span);             // divw r4,r6,r11
        // rlwinm r8,r5,2,0,29 ; add r30,r8,r7 -- this channel's base in the old ring.
        const uint64_t channel_base = WordsToBytes(channel_words) + ring;
        const uint64_t whole = MulLw(quotient, span);               // mullw r3,r4,r11
        const uint64_t remainder = position - whole;                // subf r8,r3,r6
        const uint64_t span_bytes = WordsToBytes(span);             // rlwinm r7,r11,2,0,29
        const uint64_t offset = remainder + lag_now;                // add r5,r8,r10
        const uint64_t doubled = __builtin_rotateleft32(uint32_t(position), 1);  // rotlwi r8,r6,1
        const uint64_t offset_bytes = WordsToBytes(offset);         // rlwinm r6,r5,2,0,29
        const uint64_t doubled_less_one = uint64_t(int64_t(doubled) - 1);  // addi r3,r8,-1
        const uint64_t read_ptr = offset_bytes + channel_base;      // add r6,r6,r30
        const uint64_t filled_bytes = WordsToBytes(filled);         // rlwinm r8,r9,2,0,29
        const uint64_t lag_bytes = WordsToBytes(lag_now);           // rlwinm r5,r10,2,0,29
        uint64_t source = read_ptr - filled_bytes;                  // subf r4,r8,r6
        const uint64_t guard = span & ~doubled_less_one;            // andc r3,r11,r3
        const uint64_t channel_end = span_bytes + channel_base;     // add r7,r7,r30
        // twllei r11,0 -- a zero span. ppc_trap(type 0) only warns and returns, so the body
        // carries on with DivW's zero quotient, exactly as the lifted form does.
        if (int32_t(uint32_t(span)) == 0) ppc_trap(ctx, base, 0);
        // twlgei r3,-1 -- the divide-overflow guard.
        if (int32_t(uint32_t(guard)) == -1) ppc_trap(ctx, base, 0);
        const uint64_t lag_boundary = channel_end - lag_bytes;      // subf r6,r5,r7

        // cmplw cr6,r4,r30 ; blt cr6,0x82b3d794 ; cmplw cr6,r7,r4 ; bgt cr6,0x82b3d7a0
        // A source below this channel's base, or at or past its end, is wrapped forward by the
        // usable length. Both compares are unsigned on the low words.
        if (uint32_t(source) < uint32_t(channel_base) ||
            !(uint32_t(channel_end) > uint32_t(source))) {
          // loc_82B3D794 -- subf r11,r10,r11 ; rlwinm r11,r11,2,0,29 ; add r4,r11,r4
          source = WordsToBytes(span - lag_now) + source;
        }

        // loc_82B3D7A0 -- where the carried-over words land in the NEW channel: the usable end,
        // counted back by the same word count.
        const uint64_t new_usable_end = WordsToBytes(new_span - lag_now) + new_base;  // add r28
        const uint64_t destination = new_usable_end - filled_bytes;  // subf r27,r8,r28
        // subf r10,r4,r6 ; srawi r25,r10,2 -- words from the source to the lag boundary. (srawi
        // also sets CA; xer is not compared and the value is not read again.)
        uint64_t run = uint64_t(int64_t(int32_t(uint32_t(lag_boundary - source)) >> 2));
        // cmpw cr6,r9,r25 ; bge cr6,0x82b3d7c4 ; mr r25,r9 -- SIGNED min with the word count.
        if (int32_t(uint32_t(filled)) < int32_t(uint32_t(run))) run = filled;

        // loc_82B3D7C4 -- rlwinm r29,r25,2,0,29 ; bl 0x82edf460: the run up to the boundary.
        const uint64_t run_bytes = WordsToBytes(run);
        skate3::audio::GuestCall(ctx, base, sub_82EDF460, destination, source, run_bytes);
        // lwz r11,36(r31) -- the word count is RELOADED after the call; the rest of it comes
        // from this channel's base. bl 0x82edf460.
        const uint64_t rest = REX_LOAD_U32(self + kObjCursor) - run;      // subf r10,r25,r11
        skate3::audio::GuestCall(ctx, base, sub_82EDF460, run_bytes + destination, channel_base,
                                 WordsToBytes(rest));
        // lwz r9,24(r31) -- the lag is reloaded too. bl 0x82edf460: the lag window is mirrored
        // from the new channel's usable end down to its base.
        const uint64_t mirror_bytes = WordsToBytes(REX_LOAD_U32(self + kObjSpanLow));
        skate3::audio::GuestCall(ctx, base, sub_82EDF460, new_base, new_usable_end, mirror_bytes);

        // lwz r8,48(r31) -- the channel count is reloaded every iteration as well.
        const int32_t count = int32_t(REX_LOAD_U32(self + kObjChannels));
        c++;                        // addi r26,r26,1
        new_base += new_stride;     // add r24,r22,r24
        // cmpw cr6,r26,r8 ; blt cr6,0x82b3d720 -- SIGNED
        if (c >= count) break;
      }
    }

    // loc_82B3D814 -- lwz r4,0(r31) ; cmplwi cr6,r4,0 ; beq cr6,0x82b3d83c. The old ring goes
    // back through the allocator's free entry: the second gate-1 call. The System pointer is
    // reloaded from the global.
    const uint32_t old_ring = REX_LOAD_U32(self + kObjRing);
    if (old_ring != 0) {
      const uint32_t system_now = REX_LOAD_U32(kSystemPtr);
      const uint32_t allocator_now = REX_LOAD_U32(system_now + kAllocator);
      const uint32_t free_vtable = REX_LOAD_U32(allocator_now + kVptr);
      ctx.ctr.u64 = REX_LOAD_U32(free_vtable + kFreeSlot);
      ctx.r3.u64 = allocator_now;
      ctx.r4.u64 = old_ring;
      ctx.r5.s64 = 0;  // li r5,0
      ctx.lr = kFreeReturn;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    }

    // loc_82B3D83C -- publish the new ring. The position is reset to the lag, read back out of
    // the object rather than reused from the `lag` above.
    REX_STORE_U32(self + kObjRing, uint32_t(buffer));        // stw r21,0(r31)
    REX_STORE_U32(self + kObjSpanHigh, uint32_t(new_span));  // stw r23,20(r31)
    REX_STORE_U32(self + kObjPosition,
                  REX_LOAD_U32(self + kObjSpanLow));         // lwz r11,24(r31) ; stw r11,52(r31)
  }

  // loc_82B3D84C -- reached both by the bge (no growth needed) and by the growth path.
  REX_STORE_U32(self + kObjRequest, uint32_t(request));  // stw r19,12(r31)
  ctx.r3.s64 = 1;                                        // li r18,1 ; mr r3,r18
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;                 // addi r1,r1,208
  ctx.lr = entry_lr;                                     // b 0x82f52140 (__restgprlr_18)
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3D668
SKATE3_PORT(82B3D668, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
