// sub_82B3D8E0  role: run one 256-word block through every track of a stream object -- for each track, build the six-word record (input and output channel pointers, plus what sub_82B3DD90 fills in), then repeatedly ask sub_82B3DD90 for a run, hand it to the mixer at self[4] through a function pointer, and write the result back into the ring with sub_82B3DEA8, until 256 words are done; when self[56] is set the first 128 words go through a second pass whose per-sample gain ramp is built on the frame from two rodata constants. Finally advance the object's position and two cursors by 256 and clear the flag   lifted: skate3_recomp.68.cpp:10332 (375 lines)
// STATUS: gate-1 (its own two bctrl through the mixer pointer at self[4]+0: sub_82B3D8E0 indirect call, `bctrl` at 0x82B3DA00 and 0x82B3DA8C)
// calls/session: boot 324500  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3D8E0.md
namespace port_82B3D8E0 {

// The stream object (r3/r29). Names shared with the rest of the family -- sub_82B3DB90,
// sub_82B3DC48, sub_82B3DD90, sub_82B3DEA8, sub_82B3DF90 -- which take the same object; nothing in
// docs/rw_audio_structs.h names it.
constexpr uint32_t kObjMixer = 4;        // -> the mixer object; ITS +0 is the function pointer
constexpr uint32_t kObjSpanHigh = 20;    // the wrap span, and the ceiling both cursors clamp to
constexpr uint32_t kObjSpanLow = 24;     // the floor the wrapped position is held at
constexpr uint32_t kObjSecondCursor = 32;  // advanced by 256 per block, clamped like +36
constexpr uint32_t kObjCursor = 36;      // sub_82B3DF90's kCursor
constexpr uint32_t kObjTracks = 48;      // s32 track count: the outer loop's bound
constexpr uint32_t kObjPosition = 52;    // the running word position, wrapped by +20 here
constexpr uint32_t kObjTwoBuffers = 56;  // u8: sub_82B3DD90 cuts a second segment, and this
                                         // function crossfades the first 128 words. Cleared here.
constexpr uint32_t kMixerEntry = 0;      // lwz r8,0(r3) ; mtctr r8 ; bctrl

// A buffer descriptor (r4 and r5), the layout sub_82B3CA60 also reads.
constexpr uint32_t kBufData = 4;    // u32 -> the samples, four bytes each
constexpr uint32_t kBufStride = 14; // u16 words between one track's data and the next

// The six-word record this call builds on its frame and hands to every callee by address. Words 1,
// 2 and 4 are sub_82B3DD90's output (kOutSeg0, kOutSeg1, kOutEnd in sub_82B3DD90.inc); words 0 and
// 5 are the two descriptors' channel pointers, and which of them is the source is not established
// here, so they are named after the argument they come from. All six advance together.
constexpr uint32_t kRecord = 80;      // frame + 80
constexpr uint32_t kRecBufferA = 0;   // from r4's descriptor
constexpr uint32_t kRecSeg0 = 4;
constexpr uint32_t kRecSeg1 = 8;      // zeroed per track and before the second pass
constexpr uint32_t kRecRamp = 12;     // the gain ramp, or zero for "no ramp"
constexpr uint32_t kRecEnd = 16;
constexpr uint32_t kRecBufferB = 20;  // from r5's descriptor
constexpr uint32_t kRecordWords = 6;

// The gain ramp: 128 floats at frame+112..frame+620, written back to front, and addressed as
// frame + 624 - 4 * (words of ramp left).
constexpr uint32_t kRampCount = 128;
constexpr uint32_t kRampBase = 108;   // addi r11,r1,108 -- stfsu pre-increments, so +112 is first
constexpr uint32_t kRampFirst = 112;  // addi r11,r1,112
constexpr uint32_t kRampEnd = 624;    // addi r10,r1,624

// Two single-precision constants in rodata, loaded rather than assumed:
// lis r11,-32208 ; addi r9,r11,-31232 ; lfs f0,2660(r9)  -> 0x82300000 - 31232 + 2660
// lis r10,-32253 ; lfs f13,212(r10)                      -> 0x82030000 + 212
constexpr uint32_t kRampStartConst = 0x82300000u - 31232u + 2660u;
constexpr uint32_t kRampStepConst = 0x82030000u + 212u;

constexpr uint32_t kFrameBytes = 720;  // stwu r1,-720(r1)

// cmpw / cmpwi: a SIGNED compare of the low words, whatever the 64-bit registers hold above them.
inline int32_t Word(uint64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }

// The record advance after each mixer pass (0x82B3DA04 and 0x82B3DA90): every one of the six words
// moves on by four bytes per word consumed. The original interleaves the six loads, adds and stores
// in a different register order at each site; the six words are distinct and nothing runs between
// them, so one loop is the same six reads and six writes.
void AdvanceRecord(PPCContext& ctx, uint8_t* base, uint32_t record, uint64_t words) {
  (void)ctx;
  const uint32_t step = static_cast<uint32_t>(words) << 2;  // rlwinm r11,r31,2,0,29
  for (uint32_t word = 0; word < kRecordWords; word++) {
    const uint32_t at = record + 4 * word;
    REX_STORE_U32(at, REX_LOAD_U32(at) + step);
  }
}

// Both bctrl sites: mixer(mixer, words, filter_arg, track, record), through the pointer at the
// mixer object's +0. Reproduced as lifted, which is why this function can never be shadowed.
void CallMixer(PPCContext& ctx, uint8_t* base, uint32_t self, uint64_t words, uint64_t filter_arg,
               uint64_t track, uint32_t record, uint32_t link) {
  const uint32_t mixer = REX_LOAD_U32(self + kObjMixer);  // lwz r3,4(r29) / lwz r11,4(r29)
  ctx.r3.u64 = mixer;
  ctx.r4.u64 = words;
  ctx.r5.u64 = filter_arg;
  ctx.r6.u64 = track;
  ctx.r7.u64 = record;
  ctx.ctr.u64 = REX_LOAD_U32(mixer + kMixerEntry);  // lwz r8,0(r3) ; mtctr r8
  ctx.lr = link;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

REX_FUNC(Native) {
  const uint64_t entry_lr = ctx.lr;  // mflr r12, spilled by __savegprlr_22

  // stwu r1,-720(r1). The frame carries the record and the ramp, both handed out by address, so it
  // has to be real guest memory in the same place. __savegprlr_22's spills of r22..r31 land in it
  // as well; they are the function's own frame and are not reproduced, because this body keeps
  // those values in locals and leaves the registers alone.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;        // r29
  const uint32_t buffer_a = ctx.r4.u32;    // r23
  const uint32_t buffer_b = ctx.r5.u32;    // r22
  const uint64_t filter_arg = ctx.r6.u64;  // r25, forwarded to the mixer untouched
  const uint32_t record = frame + kRecord;

  // lbz r11,56(r3) ; cmplwi cr6,r11,0 ; beq -> loc_82B3D934
  if (REX_LOAD_U8(self + kObjTwoBuffers) != 0) {
    // lfs f0,2660(r9) ; lfs f13,212(r10)
    ctx.fpscr.disableFlushModeUnconditional();
    PPCRegister loaded{};
    loaded.u32 = REX_LOAD_U32(kRampStartConst);
    double value = double(loaded.f32);
    loaded.u32 = REX_LOAD_U32(kRampStepConst);
    const double step = double(loaded.f32);

    uint32_t at = frame + kRampBase;  // addi r11,r1,108
    for (uint32_t i = 0; i < kRampCount; i++) {  // li r10,128 ; mtctr r10 ; bdnz
      // loc_82B3D928: stfsu f0,4(r11) -- pre-increment, then a single-precision store
      ctx.fpscr.disableFlushModeUnconditional();
      at = at + 4;
      PPCRegister narrowed{};
      narrowed.f32 = float(value);
      REX_STORE_U32(at, narrowed.u32);
      // fsubs f0,f0,f13 -- the subtraction is in double and the result rounded back to single
      value = double(float(value - step));
    }
  }

  // loc_82B3D934: lwz r11,48(r29) ; cmpwi cr6,r11,0 ; ble -> loc_82B3DB0C
  if (Word(REX_LOAD_U32(self + kObjTracks)) > 0) {
    uint64_t track = 0;  // r28; r24 stays a literal zero for the whole function
    for (;;) {
      // loc_82B3D948
      const uint64_t stride_a = REX_LOAD_U16(buffer_a + kBufStride);  // lhz r11,14(r23)
      const uint64_t stride_b = REX_LOAD_U16(buffer_b + kBufStride);  // lhz r10,14(r22)
      // mullw r8,r11,r28 -- the full 64-bit product; only the low word survives the rlwinm below
      const int64_t off_a = int64_t(Word(stride_a)) * int64_t(Word(track));
      const uint64_t data_a = REX_LOAD_U32(buffer_a + kBufData);      // lwz r11,4(r23)
      const uint64_t data_b = REX_LOAD_U32(buffer_b + kBufData);      // lwz r9,4(r22)
      const uint32_t fading = REX_LOAD_U8(self + kObjTwoBuffers);     // lbz r7,56(r29), reloaded
      REX_STORE_U32(record + kRecSeg1, 0);                            // stw r24,88(r1)
      REX_STORE_U32(record + kRecRamp, 0);                            // stw r24,92(r1)
      // mullw r6,r10,r28 ; rlwinm r10,r8,2 ; rlwinm r8,r6,2 ; add r5,r10,r11 ; add r4,r8,r9
      const int64_t off_b = int64_t(Word(stride_b)) * int64_t(Word(track));
      const uint32_t chan_a = (static_cast<uint32_t>(off_a) << 2) + static_cast<uint32_t>(data_a);
      const uint32_t chan_b = (static_cast<uint32_t>(off_b) << 2) + static_cast<uint32_t>(data_b);
      REX_STORE_U32(record + kRecBufferA, chan_a);   // stw r5,80(r1)
      REX_STORE_U32(record + kRecBufferB, chan_b);   // stw r4,100(r1)
      // cmplwi cr6,r7,0 ; beq -> loc_82B3D99C
      uint64_t ramp_left = 0;  // mr r27,r24
      if (fading != 0) {
        ramp_left = kRampCount;                                    // li r27,128
        REX_STORE_U32(record + kRecRamp, frame + kRampFirst);       // stw r11,92(r1)
      }

      // loc_82B3D99C: mr r30,r24
      uint64_t done = 0;  // r30
      do {
        // loc_82B3D9A0: bl 0x82b3dd90 -- (self, track, done, record). It returns a literal 256.
        skate3::audio::GuestCall(ctx, base, sub_82B3DD90, self, track, done, record);
        const uint64_t offered = ctx.r3.u64;  // r26
        uint64_t chunk = offered;             // mr r31,r3

        // cmpwi cr6,r27,0 ; beq -> loc_82B3DA5C -- the ramped pass, while any ramp is left
        if (Word(ramp_left) != 0) {
          // cmpw cr6,r27,r3 ; mr r31,r27 ; blt ; mr r31,r3 -- min, signed, keeping 64 bits
          chunk = Word(ramp_left) < Word(offered) ? ramp_left : offered;
          // rlwinm r11,r27,2 ; addi r10,r1,624 ; subf r9,r11,r10 ; stw r9,92(r1) -- the ramp
          // pointer walks forward as the remaining ramp shrinks; the first pass is frame+112,
          // which is the same value the setup above already stored.
          REX_STORE_U32(record + kRecRamp,
                        (frame + kRampEnd) - (static_cast<uint32_t>(ramp_left) << 2));
          CallMixer(ctx, base, self, chunk, filter_arg, track, record, 0x82B3DA04);
          // The advance uses the chunk as it was at the bctrl (rlwinm r11,r31,2 is computed before
          // subf r31,r31,r26), so it runs before the three register updates. Those are registers
          // only, so the original's interleaving of them with the stores makes no difference.
          AdvanceRecord(ctx, base, record, chunk);
          done = chunk + done;               // add r30,r31,r30
          ramp_left = ramp_left - chunk;      // subf r27,r31,r27
          chunk = offered - chunk;            // subf r31,r31,r26 -- what the flat pass still owes
        }

        // loc_82B3DA5C: cmpwi cr6,r31,0 ; beq -> loc_82B3DAE0
        if (Word(chunk) != 0) {
          REX_STORE_U32(record + kRecSeg1, 0);  // stw r24,88(r1) -- no second segment, no ramp
          REX_STORE_U32(record + kRecRamp, 0);  // stw r24,92(r1)
          CallMixer(ctx, base, self, chunk, filter_arg, track, record, 0x82B3DA90);
          AdvanceRecord(ctx, base, record, chunk);
          done = chunk + done;                  // add r30,r31,r30
        }

        // loc_82B3DAE0: bl 0x82b3dea8 -- writes the block back into the wrapping ring
        skate3::audio::GuestCall(ctx, base, sub_82B3DEA8, self, track, done, record);
        // cmpwi cr6,r30,256 ; blt -> loc_82B3D9A0
      } while (Word(done) < 256);

      // lwz r11,48(r29) ; addi r28,r28,1 ; cmpw cr6,r28,r11 ; blt -> loc_82B3D948 (reloaded)
      const int32_t tracks = Word(REX_LOAD_U32(self + kObjTracks));
      track = track + 1;
      if (!(Word(track) < tracks)) break;
    }
  }

  // loc_82B3DB0C: the position advances 256 words and wraps modulo the span, then is held at the
  // floor; both cursors advance 256 and stop at the span.
  const uint64_t position = REX_LOAD_U32(self + kObjPosition);  // lwz r10,52(r29)
  const uint64_t span = REX_LOAD_U32(self + kObjSpanHigh);      // lwz r11,20(r29)
  const uint64_t reach = position + 256;                        // addi r8,r10,256
  const uint64_t floor_at = REX_LOAD_U32(self + kObjSpanLow);   // lwz r9,24(r29)
  // twllei r11,0 -- ppc_trap(type 0) only warns and returns, so the body carries on with the
  // divw's zero quotient, exactly as the lifted form does.
  if (Word(span) == 0) ppc_trap(ctx, base, 0);
  // divw r7,r8,r11
  const uint64_t quotient = static_cast<uint32_t>(
      (Word(span) && !(Word(reach) == INT32_MIN && Word(span) == -1)) ? Word(reach) / Word(span)
                                                                     : 0);
  // rotlwi r10,r8,1 ; addi r6,r10,-1 ; andc r4,r11,r6 -- the divide-overflow guard operand
  const uint64_t doubled_less_one =
      static_cast<uint64_t>(static_cast<int64_t>(
          static_cast<uint64_t>(__builtin_rotateleft32(static_cast<uint32_t>(reach), 1))) - 1);
  const uint64_t guard = span & ~doubled_less_one;
  // mullw r5,r7,r11 ; subf r10,r5,r8 -- reach modulo span, truncated toward zero
  const uint64_t whole = static_cast<uint64_t>(int64_t(Word(quotient)) * int64_t(Word(span)));
  const uint64_t remainder = reach - whole;
  REX_STORE_U32(self + kObjPosition, static_cast<uint32_t>(remainder));  // stw r10,52(r29)
  // twlgei r4,-1
  if (Word(guard) == -1) ppc_trap(ctx, base, 0);
  // cmpw cr6,r10,r9 ; bgt -> loc_82B3DB4C ; mr r10,r9 -- +52 is written twice, in this order
  const uint64_t floored = Word(remainder) > Word(floor_at) ? remainder : floor_at;
  REX_STORE_U32(self + kObjPosition, static_cast<uint32_t>(floored));    // stw r10,52(r29)

  // lwz r10,36(r29) ; addi r10,r10,256 ; cmpw cr6,r10,r11 ; blt ; mr r10,r11
  const uint64_t cursor = REX_LOAD_U32(self + kObjCursor) + 256;
  REX_STORE_U32(self + kObjCursor,
                static_cast<uint32_t>(Word(cursor) < Word(span) ? cursor : span));
  // lwz r10,32(r29) ; addi r10,r10,256 ; cmpw cr6,r10,r11 ; bge ; mr r11,r10 -- the same min, with
  // the branch the other way round
  const uint64_t second = REX_LOAD_U32(self + kObjSecondCursor) + 256;
  REX_STORE_U32(self + kObjSecondCursor,
                static_cast<uint32_t>(Word(second) >= Word(span) ? span : second));
  // stb r24,56(r29) -- the crossfade is spent
  REX_STORE_U8(self + kObjTwoBuffers, 0);

  // addi r1,r1,720 ; b __restgprlr_22. That stub reloads r22..r31 and lr from the frame; this body
  // keeps those registers in locals and never writes them. No `li r3`: r3 is left as the last call
  // put it, or as it arrived when the track count was not positive.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.lr = entry_lr;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3D8E0
SKATE3_PORT(82B3D8E0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
