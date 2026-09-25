// sub_82B29288  role: PacketPlayer output step -- hand off to the ramp (sub_82B29118) once a hold is written, else report a rate/channel change, or walk the Player's +0x54 packet table to the next ready entry, decode from Player+0x150 into the output descriptor's spare buffer (sub_82B3CA60), retire the entries that drained, and hold each channel's last sample   lifted: skate3_recomp.67.cpp:27834 (626 lines)
// STATUS: gate-1 (sub_82B3CA60 indirect call: two bctrl at 0x82B3CB80 / 0x82B3CCA0, on the path where the entry at the cursor is in state 1)
// calls/session: boot 1683  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B29288.md
namespace port_82B29288 {

// Named from docs/rw_audio_structs.h (rw_player).
constexpr uint32_t kSystem = 8;          // +0x08  rw_system*
constexpr uint32_t kTable = 84;          // +0x54  20 entries, stride 12
constexpr uint32_t kDecoder = 336;       // +0x150 active decoder
constexpr uint32_t kSampleRate = 340;    // +0x154 read with lfs: a float here
constexpr uint32_t kState = 350;         // +0x15E 1 = playing, 4 = stopped
constexpr uint32_t kChannelCount = 351;  // +0x15F

// Table entry, relative to its own start (Player + 84 + 12*i). +9 is the documented
// discriminator; the other two are readings from this function.
constexpr uint32_t kEntryStride = 12;
constexpr uint32_t kEntryConsumed = 4;   // u32, accumulates what the decoder produced
constexpr uint32_t kEntryStream = 8;     // u8, the stream index sub_82B23C10 takes
constexpr uint32_t kEntryState = 9;      // u8, 0 empty / 1 ready / 2 drained (as used here)
constexpr uint32_t kTableEntries = 20;   // the cursor wraps at 20

// Undocumented Player fields; names are readings from this function only.
constexpr uint32_t kReportedChannels = 42;  // u8, last channel count put in the descriptor
constexpr uint32_t kLiveDecoder = 324;      // u32, the decoder while an entry is committed, else 0
constexpr uint32_t kReserve = 348;          // u16, rounded up to 128 and taken from the pool
constexpr uint32_t kTotalConsumed = 356;    // u32
constexpr uint32_t kReportedRate = 360;     // f32, last rate put in the descriptor
constexpr uint32_t kRequestCap = 364;       // u16, most the decoder is asked for per call
constexpr uint32_t kHoldOffset = 366;       // u16, byte offset FROM THE PLAYER of a float array
constexpr uint32_t kHoldChannels = 368;     // u8, most channels held
constexpr uint32_t kHoldWritten = 369;      // u8, set to 1 once the hold array is filled
constexpr uint32_t kRampFrames = 370;       // u8, sub_82B29118 divides by it
constexpr uint32_t kCursorStop = 371;       // u8, the walk stops at this index
constexpr uint32_t kCursor = 372;           // u8, current index into the table

// The output descriptor (r4). +28/+32 are two buffer pointers swapped after every fill.
constexpr uint32_t kOutBufferA = 28;
constexpr uint32_t kOutBufferB = 32;   // the one handed to sub_82B3CA60
constexpr uint32_t kOutFrames = 48;    // u32, what the decoder produced (0 when none)
constexpr uint32_t kOutRate = 52;      // f32
constexpr uint32_t kOutChannels = 60;  // u8

// A buffer object: +4 the sample base (floats), +14 the channel stride in samples.
constexpr uint32_t kBufSamples = 4;
constexpr uint32_t kBufStride = 14;

// u32[Player.system + 0] -> an object whose +32 is a budget this function reserves from.
constexpr uint32_t kPoolBudget = 32;

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)

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

// rotlwi ; add ; rlwinm r,r,2,0,29 ; add r,r,r31 -- exactly 12*index for a byte index; only the
// low word ever addresses.
uint32_t EntryAddr(uint32_t player, uint32_t index) {
  return player + kTable + kEntryStride * index;
}

// addi +1 ; clrlwi 24 ; addi -20 ; subfic 0 ; subfe r7,r8,r8 ; and -- CA is set only when the
// incremented byte is exactly 20, making the mask 0; otherwise the mask is all ones.
uint64_t NextIndex(uint32_t index) {
  const uint64_t next = uint64_t((index & 0xFF) + 1) & 0xFF;
  const uint64_t mask = (next == kTableEntries) ? 0 : ~uint64_t(0);
  return mask & next;
}

// The address of channel `channel`'s last consumed sample: lhz 14 ; lwz 4 ; mullw ; add r30 ;
// rlwinm 2,0,29 ; add ; lfs -4. mullw is the 64-bit signed product, the add is 64-bit on the
// result of sub_82B3CA60, and only the low word survives the rlwinm.
uint32_t LastSampleAddr(uint8_t* base, uint32_t buffer, uint64_t channel, uint64_t consumed) {
  const uint64_t stride = REX_LOAD_U16(buffer + kBufStride);
  const uint64_t samples = REX_LOAD_U32(buffer + kBufSamples);
  const uint64_t product = uint64_t(int64_t(int32_t(uint32_t(stride))) *
                                    int64_t(int32_t(uint32_t(channel))));
  const uint64_t index = product + consumed;
  const uint64_t offset = uint32_t(uint32_t(index) << 2);
  return uint32_t(offset + samples) - 4u;
}

// __restgprlr_24: r24-r31 were never touched, so only r1, r12 and lr are rebuilt.
void Epilogue(PPCContext& ctx, uint64_t entry_lr) {
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
  ctx.r12.u64 = entry_lr;                 // lwz r12,-8(r1)
  ctx.lr = entry_lr;                      // mtlr r12
}

REX_FUNC(Native) {
  // mflr r12 ; bl __savegprlr_24 ; stwu r1,-160(r1). The spills are inside the frame and are not
  // reproduced; the frame is, because every callee runs on it.
  const uint64_t entry_lr = ctx.lr;
  ctx.r12.u64 = entry_lr;
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);  // stwu r1,-160(r1)
  ctx.r1.u32 = frame;

  const uint32_t player = ctx.r3.u32;  // mr r31,r3
  const uint32_t out = ctx.r4.u32;     // mr r28,r4

  // lbz r11,370(r3) ; cmplwi ; beq -- lbz r11,369(r3) ; cmplwi ; beq
  if (REX_LOAD_U8(player + kRampFrames) != 0 && REX_LOAD_U8(player + kHoldWritten) != 0) {
    // bl 0x82b29118 with r3/r4 as they arrived; r3 is whatever it returns.
    skate3::audio::GuestCall(ctx, base, sub_82B29118, ctx.r3.u64, ctx.r4.u64);
    Epilogue(ctx, entry_lr);
    return;
  }

  // loc_82B292C0 -- the state byte is read before the store below.
  const uint32_t state = REX_LOAD_U8(player + kState);  // lbz r11,350(r31)
  REX_STORE_U32(player + kLiveDecoder, 0);              // stw r24,324(r31)
  uint64_t reserved = 0;       // r26
  uint64_t budget_before = 0;  // r27
  uint64_t consumed = 0;       // r30, from sub_82B3CA60 once it runs

  // cmpwi 4 ; beq -> 0 ; cmpwi 0 ; li r11,1 ; bne -> 1: playing means neither 4 nor 0.
  if (state != 4 && state != 0) {
    ctx.fpscr.disableFlushModeUnconditional();
    const double rate = LoadSingle(base, player + kSampleRate);        // lfs f0,340(r31)
    const double reported = LoadSingle(base, player + kReportedRate);  // lfs f13,360(r31)
    // fcmpu cr6,f0,f13 ; bne -- unordered is "not equal", so a NaN rate takes the reset too.
    bool changed = !(rate == reported);
    if (!changed) {
      const uint32_t seen = REX_LOAD_U8(player + kReportedChannels);    // lbz r11,42(r31)
      const uint32_t channels = REX_LOAD_U8(player + kChannelCount);    // lbz r10,351(r31)
      changed = channels != seen;                                       // cmplw ; bne
    }
    if (changed) {
      // loc_82B296A8 -- report the new format and remember it. The two Player fields are
      // RELOADED after the stores through r28, which could alias them.
      REX_STORE_U32(out + kOutFrames, 0);                                     // stw r24,48(r28)
      ctx.r3.s64 = 1;                                                         // li r3,1
      REX_STORE_U8(out + kOutChannels, REX_LOAD_U8(player + kChannelCount));  // lbz ; stb r11,60(r28)
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, out + kOutRate, LoadSingle(base, player + kSampleRate));  // lfs ; stfs f0,52(r28)
      const uint8_t channels = REX_LOAD_U8(player + kChannelCount);    // lbz r10,351(r31)
      const double new_rate = LoadSingle(base, player + kSampleRate);  // lfs f13,340(r31)
      REX_STORE_U8(player + kReportedChannels, channels);              // stb r10,42(r31)
      StoreSingle(base, player + kReportedRate, new_rate);             // stfs f13,360(r31)
      Epilogue(ctx, entry_lr);
      return;
    }

    // lbz r11,372 ; ... ; lbz r9,93(r10) ; cmplwi ; bne 0x82b29394 -- an empty entry at the
    // cursor starts the walk.
    if (REX_LOAD_U8(EntryAddr(player, REX_LOAD_U8(player + kCursor)) + kEntryState) == 0) {
      const uint32_t stop = REX_LOAD_U8(player + kCursorStop);  // lbz r9,371(r31), read once
      // loc_82B29344 -- unbounded in the original when nothing is ready and the stop index is
      // unreachable; reproduced as is.
      for (;;) {
        const uint32_t cursor = REX_LOAD_U8(player + kCursor);  // lbz r11,372(r31)
        if (cursor == stop) break;                              // cmplw ; beq 0x82b29394
        const uint64_t next = NextIndex(cursor);
        REX_STORE_U8(player + kCursor, uint8_t(next));          // stb r6,372(r31)
        // lbz r3,93(r4) ; cmplwi ; beq 0x82b29344 -- the computed index, not a reload
        if (REX_LOAD_U8(EntryAddr(player, uint32_t(next & 0xFF)) + kEntryState) != 0) break;
      }
    }

    // loc_82B29394 -- the cursor is reloaded.
    const uint32_t entry = EntryAddr(player, REX_LOAD_U8(player + kCursor));
    if (REX_LOAD_U8(entry + kEntryState) == 1) {  // cmplwi cr6,r9,1 ; bne 0x82b29508
      const uint32_t system = REX_LOAD_U32(player + kSystem);        // lwz r10,8(r31)
      const uint64_t reserve = REX_LOAD_U16(player + kReserve);      // lhz r11,348(r31)
      const uint32_t pool = REX_LOAD_U32(system + 0);                // lwz r8,0(r10)
      const uint64_t rounded = uint32_t(reserve + 127) & 0xFFFFFF80u;  // addi ; rlwinm 0,0,24
      budget_before = REX_LOAD_U32(pool + kPoolBudget);             // lwz r27,32(r8)
      reserved = budget_before - rounded;                           // subf r11,r7,r27 (64-bit)
      REX_STORE_U32(pool + kPoolBudget, uint32_t(reserved));        // stw r11,32(r8)

      // Both loads follow the store above, so both are genuine reloads.
      const uint32_t decoder = REX_LOAD_U32(player + kDecoder);     // lwz r30,336(r31)
      const uint32_t cursor = REX_LOAD_U8(player + kCursor);        // lbz r11,372(r31)
      REX_STORE_U32(player + kLiveDecoder, decoder);                // stw r30,324(r31)
      const uint32_t stream = REX_LOAD_U8(EntryAddr(player, cursor) + kEntryStream);  // lbz r4,92(r5)
      skate3::audio::GuestCall(ctx, base, sub_82B23C10, decoder, stream);  // bl 0x82b23c10
      const uint64_t available = ctx.r3.u64;                        // mr r29,r3

      const uint64_t cap = REX_LOAD_U16(player + kRequestCap);      // lhz r11,364(r31)
      // cmpw cr6,r11,r3 ; blt -- SIGNED low words; r5 takes the whole chosen register.
      const uint64_t request =
          (int32_t(uint32_t(cap)) < int32_t(uint32_t(available))) ? cap : available;
      const uint32_t target = REX_LOAD_U32(out + kOutBufferB);      // lwz r4,32(r28)
      skate3::audio::GuestCall(ctx, base, sub_82B3CA60, decoder, target, request);  // bl 0x82b3ca60

      const uint32_t buffer_a = REX_LOAD_U32(out + kOutBufferA);    // lwz r10,28(r28)
      const uint32_t buffer_b = REX_LOAD_U32(out + kOutBufferB);    // lwz r11,32(r28)
      consumed = ctx.r3.u64;                                        // mr r30,r3
      REX_STORE_U32(out + kOutFrames, ctx.r3.u32);                  // stw r3,48(r28)
      const uint64_t left = available - consumed;                   // subf. r3,r3,r29
      REX_STORE_U32(out + kOutBufferB, buffer_a);                   // stw r10,32(r28)
      REX_STORE_U32(out + kOutBufferA, buffer_b);                   // stw r11,28(r28)
      REX_STORE_U8(out + kOutChannels, REX_LOAD_U8(player + kChannelCount));  // lbz ; stb r9,60(r28)
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, out + kOutRate, LoadSingle(base, player + kSampleRate));  // lfs ; stfs f0,52(r28)
      const uint64_t total = consumed + REX_LOAD_U32(player + kTotalConsumed);  // lwz r9,356 ; add
      REX_STORE_U32(player + kTotalConsumed, uint32_t(total));      // stw r7,356(r31)
      const uint32_t current = EntryAddr(player, REX_LOAD_U8(player + kCursor));  // lbz 372, reloaded
      const uint64_t entry_total = consumed + REX_LOAD_U32(current + kEntryConsumed);  // lwz r10,88 ; add
      REX_STORE_U32(current + kEntryConsumed, uint32_t(entry_total));  // stw r6,88(r11)

      // bne 0x82b29508 on cr0 from subf.: the LOW word of available - consumed.
      if (uint32_t(left) == 0) {
        uint64_t last = left;  // r3 across the loop
        // loc_82B29484 -- retire drained entries while each one ahead has nothing left.
        for (;;) {
          const uint32_t at = EntryAddr(player, REX_LOAD_U8(player + kCursor));  // lbz r11,372
          if (REX_LOAD_U8(at + kEntryState) != 1) break;           // cmplwi 1 ; bne 0x82b29508
          REX_STORE_U8(at + kEntryState, 2);                       // stb r25,93(r11)
          const uint32_t live = REX_LOAD_U32(player + kLiveDecoder);  // lwz r9,324(r31)
          const uint64_t next = NextIndex(REX_LOAD_U8(player + kCursor));  // lbz 372, reloaded
          REX_STORE_U8(player + kCursor, uint8_t(next));           // stb r11,372(r31)
          if (live != 0) {                                         // cmplwi r9,0 ; beq 0x82b29500
            const uint32_t ahead = EntryAddr(player, uint32_t(next & 0xFF));
            if (REX_LOAD_U8(ahead + kEntryState) == 1) {           // cmplwi 1 ; bne 0x82b29500
              const uint32_t ahead_stream = REX_LOAD_U8(ahead + kEntryStream);  // lbz r4,92(r11)
              skate3::audio::GuestCall(ctx, base, sub_82B23C10, live, ahead_stream);  // bl 0x82b23c10
              last = ctx.r3.u64;
            }
          }
          // loc_82B29500: cmpwi cr6,r3,0 ; beq 0x82b29484 -- signed low word
          if (int32_t(uint32_t(last)) != 0) break;
        }
      }
    }
  }

  // loc_82B29508 -- reloaded, not carried.
  if (REX_LOAD_U32(player + kLiveDecoder) != 0) {  // lwz r11,324 ; cmplwi ; beq 0x82b2952c
    REX_STORE_U32(player + kLiveDecoder, 0);       // stw r24,324(r31)
    if (uint32_t(reserved) != 0) {                 // cmplwi cr6,r26,0 ; beq 0x82b2952c
      const uint32_t system = REX_LOAD_U32(player + kSystem);  // lwz r11,8(r31)
      const uint32_t pool = REX_LOAD_U32(system + 0);          // lwz r10,0(r11)
      REX_STORE_U32(pool + kPoolBudget, uint32_t(budget_before));  // stw r27,32(r10)
    }
  }

  // loc_82B2952C
  REX_STORE_U8(out + kOutChannels, REX_LOAD_U8(player + kReportedChannels));  // lbz ; stb r11,60(r28)
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, out + kOutRate, LoadSingle(base, player + kReportedRate));  // lfs ; stfs f0,52(r28)

  // cmpwi cr6,r30,0 ; bne 0x82b2955c -- signed low word
  if (int32_t(uint32_t(consumed)) == 0) {
    // lhz r11,364(r31) ; cmplwi ; beq 0x82b29698
    if (REX_LOAD_U16(player + kRequestCap) != 0) {
      ctx.r3.s64 = 0;  // li r3,0
      Epilogue(ctx, entry_lr);
      return;
    }
    // loc_82B29698
    REX_STORE_U32(out + kOutFrames, 0);  // stw r24,48(r28)
    ctx.r3.s64 = 1;                      // li r3,1
    Epilogue(ctx, entry_lr);
    return;
  }

  // loc_82B2955C -- hold each channel's last decoded sample. +60 is RELOADED from the
  // descriptor (it was just stored from +42); +28 is the buffer the decoder just filled.
  const uint32_t hold_max = REX_LOAD_U8(player + kHoldChannels);  // lbz r10,368(r31)
  const uint32_t out_channels = REX_LOAD_U8(out + kOutChannels);  // lbz r9,60(r28)
  const uint32_t buffer = REX_LOAD_U32(out + kOutBufferA);        // lwz r11,28(r28)
  // cmplw cr6,r9,r10 ; blt -- unsigned min; clrlwi r5,r9,24 is a no-op on a byte
  const uint32_t count = out_channels < hold_max ? out_channels : hold_max;
  const uint32_t hold = player + REX_LOAD_U16(player + kHoldOffset);  // lhz r10,366 ; add r4,r10,r31

  uint64_t channel = 0;  // mr r8,r24
  // cmpwi cr6,r5,4 ; blt 0x82b29640
  if (int32_t(count) >= 4) {
    uint64_t groups = uint64_t(uint32_t(count - 4) >> 2) + 1;  // rlwinm 30,2,31 ; addi 1 ; mtctr
    do {
      // loc_82B295AC -- four channels per pass, each: reload the buffer header, lfs, stfs.
      ctx.fpscr.disableFlushModeUnconditional();
      for (uint64_t k = 0; k < 4; k++) {
        const double sample = LoadSingle(base, LastSampleAddr(base, buffer, channel + k, consumed));
        // stfs f0,4(r9) / stfs f13,8(r9) / stfs f12,12(r9) / stfsu f11,16(r9)
        StoreSingle(base, hold + uint32_t(4 * (channel + k)), sample);
      }
      channel += 4;  // addi r8,r8,4 ; addi r10,r10,4
    } while (--groups != 0);  // bdnz 0x82b295ac
  }

  // loc_82B29640: cmplw cr6,r8,r5 ; bge 0x82b29684
  if (uint32_t(channel) < count) {
    uint64_t remaining = count - channel;  // subf r9,r8,r5 ; mtctr r9
    do {
      // loc_82B2965C
      const uint32_t src = LastSampleAddr(base, buffer, channel, consumed);
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, hold + uint32_t(4 * channel), LoadSingle(base, src));  // lfs ; stfsu f0,4(r10)
      channel += 1;  // addi r8,r8,1
    } while (--remaining != 0);  // bdnz 0x82b2965c
  }

  // loc_82B29684
  REX_STORE_U8(player + kHoldWritten, 1);  // stb r11,369(r31)
  ctx.r3.s64 = 1;                          // li r3,1
  Epilogue(ctx, entry_lr);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. The write set this
// WOULD declare, and the paths on which it is declarable, are in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B29288
SKATE3_PORT(82B29288, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
