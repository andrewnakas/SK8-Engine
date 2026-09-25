// sub_82B33D40  role: start voice `index` of the system at r3 -- when the voice is in state 1 and the slot's type byte is still 0, hand the pending stream to sub_82B33C08/sub_82B335A8 and push two timestamps into the 0x82EC subsystem; then look up the format handler whose tag matches the voice's type byte in the table at 0x82119870, publish it in the voice's 48-byte control slot, and start the voice either through sub_82B332D0 or through sub_82B33418   lifted: skate3_recomp.67.cpp:53540 (333 lines)
// STATUS: gate-1 (a lock and three function-pointer calls, on every path: sub_82B33C08 -> sub_82EC1290 RtlEnterCriticalSection/RtlLeaveCriticalSection, sub_82B3CD38 indirect call (four bctrl), sub_82B33418 -> sub_82B3C870 indirect call, sub_82B3C930 indirect call)
// calls/session: boot 1932  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33D40.md
namespace port_82B33D40 {

// Nothing in docs/rw_audio_structs.h names either record. The names below are read off the uses
// in this one function and are a reading, not a certainty; the offsets are not.

// The system object (r3/r30).
constexpr uint32_t kSubObject = 8;    // -> the object sub_82B48840 turns into the handler registry
constexpr uint32_t kVoiceArray = 96;  // -> base of the 80-byte voice records
constexpr uint32_t kSlotTable = 464;  // u16: byte offset, inside the object, of the 48-byte slots

// A voice record: 80 bytes, at *(object + 96) + 80 * index (r31).
constexpr uint32_t kVoiceBaseTime = 0;      // double, added to the converted delta at +12
constexpr uint32_t kVoiceDataBase = 8;      // added to +64 to form sub_82B33418's second argument
constexpr uint32_t kVoiceDelta = 12;        // s32, sign-extended and converted to double
constexpr uint32_t kVoiceSinkArg = 28;      // second argument to both 0x82EC helpers
constexpr uint32_t kVoiceSink = 40;         // the 0x82EC subsystem object; null means "no sink"
constexpr uint32_t kVoiceOpenResult = 48;   // written: sub_82B33418's return value
constexpr uint32_t kVoiceStreamFlag = 60;   // s32, nonzero forces the flag byte to 1
constexpr uint32_t kVoiceDataOffset = 64;   // added to +8
constexpr uint32_t kVoiceFormatIndex = 72;  // u8, indexes the format-tag table at 0x82119870
constexpr uint32_t kVoiceMode = 73;         // u8, 0 or 2 selects the sub_82B33418 path
constexpr uint32_t kVoiceStartFlag = 74;    // u8 written from the word sub_82B32550 fills
constexpr uint32_t kVoiceState = 76;        // u8, exactly 1 enables the drain preamble
constexpr uint32_t kVoiceChannelByte = 77;  // u8, forwarded to whichever starter runs

// A control slot: 48 bytes, at object + u16(object + 464) + 48 * index (r27).
constexpr uint32_t kSlotHandler = 8;   // written: the handler sub_82B3CD38 resolved, or 0
constexpr uint32_t kSlotCursor = 24;   // s32, negative skips the two timestamp pushes (sub_82B33970
                                       // reads the same field as "the stream is spent")
constexpr uint32_t kSlotOverrideB = 32;  // s32, nonzero forces the flag byte to 1
constexpr uint32_t kSlotOverrideA = 36;  // s32, nonzero forces the flag byte to 1
constexpr uint32_t kSlotStatus = 40;   // written 0, then 255 when no handler matched
constexpr uint32_t kSlotTag = 44;      // u16 written from handler + 32
constexpr uint32_t kSlotType = 47;     // u8 forwarded to sub_82B3CD38

// lis r9,-32238 ; addi r8,r9,-26512 -- ((-32238 & 0xFFFF) << 16) - 26512 = 0x82119870. A table of
// 32-bit format tags indexed by the voice's type byte.
constexpr uint32_t kFormatTagTable = 0x82120000u - 26512u;

// lis r10,-32077 ; addi r7,r10,7824 -- ((-32077 & 0xFFFF) << 16) + 7824 = 0x82B31E90, handed to
// sub_82EC0E08 as its fifth argument. What it points at is not read here. `lis` sign-extends to 64
// bits on this machine and the lifted form keeps that, so the argument is the negative value.
constexpr int64_t kSinkTag = static_cast<int32_t>(0x82B31E90u);

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)
// Three slots in the frame, each reused for more than one purpose across the body.
constexpr uint32_t kFrame80 = 80;
constexpr uint32_t kFrame88 = 88;
constexpr uint32_t kFrame96 = 96;

// The epilogue every return path shares: addi r1,r1,160 ; b __restgprlr_26 ; with the li r3 that
// preceded it. __restgprlr_26 reloads r26..r31 and lr from the frame; this body never writes those
// registers, so restoring lr from the entry value is the same result.
inline void Leave(PPCContext& ctx, uint64_t entry_lr, int64_t result) {
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.lr = entry_lr;
  ctx.r3.s64 = result;
}

REX_FUNC(Native) {
  const uint64_t entry_lr = ctx.lr;  // mflr r12, spilled by __savegprlr_26

  // stwu r1,-160(r1). The frame IS reproduced: sub_82B33C08 takes three pointers into it, and the
  // timestamp loop and sub_82B32550 use the same slots as scratch. __savegprlr_26's spills of
  // r26..r31 land in this frame too; they are the function's own frame and are not reproduced,
  // because this body keeps those values in locals and leaves the registers alone.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;       // r30
  const uint64_t index = ctx.r4.u64;        // r29
  const uint64_t caller_arg = ctx.r5.u64;   // r26, forwarded untouched to whichever starter runs

  // rlwinm r10,r4,2 ; add r10,r4,r10 ; rlwinm r9,r10,4 -- index * 80, and the same shape for
  // index * 48. The lifted adds are 64-bit, but every rlwinm after them keeps only the low 32
  // bits and the sums are used only as addresses, so 32-bit arithmetic is the same value.
  const uint32_t voice_offset = static_cast<uint32_t>(ctx.r4.u32 * 5u) << 4;
  const uint32_t slot_offset = static_cast<uint32_t>(ctx.r4.u32 * 3u) << 4;
  const uint32_t voice = REX_LOAD_U32(object + kVoiceArray) + voice_offset;          // r31
  const uint32_t slot = object + REX_LOAD_U16(object + kSlotTable) + slot_offset;    // r27

  // lbz r6,76(r31) ; cmplwi cr6,r6,1 ; bne -- exactly 1, not nonzero.
  // lbz r11,47(r27) ; cmplwi cr6,r11,0 ; bne -- loaded only when the first test passed.
  if (REX_LOAD_U8(voice + kVoiceState) == 1 && REX_LOAD_U8(slot + kSlotType) == 0) {
    // bl 0x82b33c08 -- r3 and r4 are still the entry arguments; r5/r6/r7 are three frame slots it
    // fills. It takes the 0x82EC subsystem's critical section through sub_82EC1290: gate 1.
    skate3::audio::GuestCall(ctx, base, sub_82B33C08, object, index, frame + kFrame80,
                             frame + kFrame96, frame + kFrame88);

    // lwz r11,80(r1) ; cmplwi cr6,r11,0 ; beq -> loc_82B33F3C
    const uint32_t produced = REX_LOAD_U32(frame + kFrame80);
    if (produced == 0) {
      Leave(ctx, entry_lr, 0);
      return;
    }
    // addi r5,r11,4 ; bl 0x82b335a8
    skate3::audio::GuestCall(ctx, base, sub_82B335A8, object, index, uint64_t(produced) + 4);

    // lwz r11,24(r27) ; cmpwi cr6,r11,0 ; blt -> loc_82B33E24 (signed)
    if (static_cast<int32_t>(REX_LOAD_U32(slot + kSlotCursor)) >= 0) {
      // li r28,2 ; addic. r28,r28,-1 ; bne -- exactly two iterations, and every field the body
      // reads is reloaded each time because sub_82EC0E08 could have changed it.
      for (int32_t remaining = 2; remaining != 0; remaining--) {
        // lwz r11,12(r31)
        const int32_t delta = static_cast<int32_t>(REX_LOAD_U32(voice + kVoiceDelta));
        // lfd f0,0(r31)
        ctx.fpscr.disableFlushModeUnconditional();
        PPCRegister base_time{};
        base_time.u64 = REX_LOAD_U64(voice + kVoiceBaseTime);
        // lwz r4,28(r31) ; lwz r3,40(r31)
        const uint32_t sink_arg = REX_LOAD_U32(voice + kVoiceSinkArg);
        const uint32_t sink = REX_LOAD_U32(voice + kVoiceSink);

        // extsw r9,r11 ; std r9,96(r1) ; lfd f13,96(r1) ; fcfid f12,f13 -- the sign-extended word
        // goes out to memory and comes back as the float register it is converted from. Kept as a
        // round trip through the frame, as lifted.
        REX_STORE_U64(frame + kFrame96, static_cast<uint64_t>(static_cast<int64_t>(delta)));
        PPCRegister spilled{};
        spilled.u64 = REX_LOAD_U64(frame + kFrame96);
        const double converted = double(spilled.s64);

        // fadd f11,f12,f0
        PPCRegister stamp{};
        stamp.f64 = converted + base_time.f64;
        // fctidz f10,f11 -- NaN and anything above 2^63 handled exactly as the lifted line does;
        // note the test is `>`, so 2^63 itself goes through cvttsd2si.
        const int64_t truncated = std::isnan(stamp.f64) ? int64_t(0x8000000000000000ULL)
                                  : (stamp.f64 > double(LLONG_MAX))
                                      ? LLONG_MAX
                                      : simde_mm_cvttsd_si64(simde_mm_load_sd(&stamp.f64));
        // stfd f10,80(r1) ; ld r6,80(r1) -- the integer is passed as the 64-bit word it spilled to
        REX_STORE_U64(frame + kFrame80, static_cast<uint64_t>(truncated));
        const uint64_t stamp_arg = REX_LOAD_U64(frame + kFrame80);

        // bl 0x82ec0e08 -- li r5,0 supplies the third argument
        skate3::audio::GuestCall(ctx, base, sub_82EC0E08, sink, sink_arg, 0, stamp_arg, kSinkTag,
                                 object);
      }
    }

    // loc_82B33E24: lwz r3,40(r31) ; cmplwi cr6,r3,0 ; beq -> loc_82B33E38
    const uint32_t sink = REX_LOAD_U32(voice + kVoiceSink);
    if (sink != 0) {
      // lwz r4,88(r1) ; bl 0x82ec1378 -- the handle sub_82B33C08 left in the third out-slot
      skate3::audio::GuestCall(ctx, base, sub_82EC1378, sink, REX_LOAD_U32(frame + kFrame88));
    }
  }

  // loc_82B33E38: lwz r3,8(r30) ; bl 0x82b48840 -- outside the audio corpus; it returns the
  // registry whose intrusive list of format handlers the walk below searches.
  skate3::audio::GuestCall(ctx, base, sub_82B48840, REX_LOAD_U32(object + kSubObject));
  const uint32_t registry = ctx.r3.u32;

  // lwz r11,0(r3) ; li r28,0 ; cmplwi cr6,r11,0 ; beq -> loc_82B33E80 (r10 = r28 = 0)
  uint32_t link = REX_LOAD_U32(registry + 0);
  uint32_t found = 0;
  if (link != 0) {
    // lbz r10,72(r31) ; rotlwi r7,r10,2 ; lwzx r9,r7,r8 -- the type byte is at most 255, so the
    // 32-bit rotate cannot wrap and the index is a plain * 4.
    const uint32_t want = REX_LOAD_U32(kFormatTagTable + 4u * REX_LOAD_U8(voice + kVoiceFormatIndex));
    do {
      // loc_82B33E64: addi r10,r11,-16 -- the link sits at container + 16
      const uint32_t container = link - 16;
      // lwz r11,0(r11) -- the walk advances BEFORE the match is tested
      link = REX_LOAD_U32(link + 0);
      // lwz r8,20(r10) ; cmplw cr6,r8,r9 ; beq -> loc_82B33E84
      if (REX_LOAD_U32(container + 20) == want) {
        found = container;
        break;
      }
      // cmplwi cr6,r11,0 ; bne -> loc_82B33E64
    } while (link != 0);
  }

  // loc_82B33E84: bl 0x82b3cd38 -- it calls through the handler's function pointers (four bctrl):
  // gate 1.
  const uint32_t sub_object = REX_LOAD_U32(object + kSubObject);     // lwz r5,8(r30)
  const uint32_t slot_type = REX_LOAD_U8(slot + kSlotType);          // lbz r4,47(r27)
  skate3::audio::GuestCall(ctx, base, sub_82B3CD38, found, slot_type, sub_object);
  const uint32_t handler = ctx.r3.u32;

  REX_STORE_U32(slot + kSlotStatus, 0);        // stw r28,40(r27)
  REX_STORE_U32(slot + kSlotHandler, handler); // stw r3,8(r27)
  if (handler == 0) {
    // li r11,255 ; stw r11,40(r27) -- +40 is written twice on this path, in this order
    REX_STORE_U32(slot + kSlotStatus, 255);
    Leave(ctx, entry_lr, 0);
    return;
  }

  // loc_82B33EB8
  const uint32_t tag = REX_LOAD_U32(handler + 32);                             // lwz r11,32(r3)
  const int32_t override_a = static_cast<int32_t>(REX_LOAD_U32(slot + kSlotOverrideA));
  REX_STORE_U16(slot + kSlotTag, static_cast<uint16_t>(tag));                   // sth r11,44(r27)
  // The flag is 1 unless all three of +36, +32 and the voice's +60 are zero.
  uint32_t flag = 1;  // loc_82B33EE8: li r10,1
  if (override_a == 0 && static_cast<int32_t>(REX_LOAD_U32(slot + kSlotOverrideB)) == 0 &&
      static_cast<int32_t>(REX_LOAD_U32(voice + kVoiceStreamFlag)) == 0) {
    flag = 0;  // mr r10,r28
  }

  // loc_82B33EEC: lbz r11,73(r31) ; clrlwi r5,r10,24 -- r5 is set on BOTH paths here and is still
  // read after the sub_82B32550 call below, so it is assigned on the context, not just locally.
  const uint32_t mode = REX_LOAD_U8(voice + kVoiceMode);
  ctx.r5.u64 = flag & 0xFFu;
  const uint64_t flag_byte = ctx.r5.u64;
  // cmplwi cr6,r11,0 ; beq -> loc_82B33F48 ; cmplwi cr6,r11,2 ; beq -> loc_82B33F48
  if (mode != 0 && mode != 2) {
    // mr r7,r5 ; lbz r6,77(r31) ; mr r5,r26 ; mr r4,r29 ; mr r3,r30 ; bl 0x82b332d0
    const uint32_t channel = REX_LOAD_U8(voice + kVoiceChannelByte);
    skate3::audio::GuestCall(ctx, base, sub_82B332D0, object, index, caller_arg, channel,
                             flag_byte);
    // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; bne -> loc_82B33F88
    if ((ctx.r3.u32 & 0xFF) != 0) {
      Leave(ctx, entry_lr, 1);
      return;
    }
    // lwz r3,8(r27) ; cmplwi cr6,r3,0 ; beq -> loc_82B33F3C
    const uint32_t live = REX_LOAD_U32(slot + kSlotHandler);
    if (live != 0) {
      // bl 0x82b3c930 -- releases the handler through a function pointer: gate 1
      skate3::audio::GuestCall(ctx, base, sub_82B3C930, live);
      REX_STORE_U32(slot + kSlotHandler, 0);  // stw r28,8(r27)
    }
    // loc_82B33F3C
    Leave(ctx, entry_lr, 0);
    return;
  }

  // loc_82B33F48: stw r28,80(r1) ; addi r4,r1,80 ; bl 0x82b32550 -- the slot is zeroed first and
  // sub_82B32550 writes the chosen index into it.
  REX_STORE_U32(frame + kFrame80, 0);
  skate3::audio::GuestCall(ctx, base, sub_82B32550, object, frame + kFrame80);
  // mr r8,r5 -- r5 is VOLATILE and this reads it back after a call. sub_82B32550 happens never to
  // write r5, so the flag byte set at loc_82B33EEC survives; reproduced as the lifted form does
  // it, off the context, rather than reusing the local.
  const uint64_t preserved_flag = ctx.r5.u64;
  // lwz r11,64(r31) ; lwz r10,8(r31) ; add r4,r11,r10 -- a 64-bit add of two zero-extended words
  const uint64_t data_offset = REX_LOAD_U32(voice + kVoiceDataOffset);
  const uint64_t data = data_offset + uint64_t(REX_LOAD_U32(voice + kVoiceDataBase));
  const uint32_t channel = REX_LOAD_U8(voice + kVoiceChannelByte);  // lbz r7,77(r31)
  // lwz r11,80(r1) ; stb r11,74(r31) -- the low byte of what sub_82B32550 just stored
  REX_STORE_U8(voice + kVoiceStartFlag, static_cast<uint8_t>(REX_LOAD_U32(frame + kFrame80)));
  // bl 0x82b33418 -- reaches sub_82B3C870's bctrl: gate 1
  skate3::audio::GuestCall(ctx, base, sub_82B33418, object, data, index, caller_arg, channel,
                           preserved_flag);
  REX_STORE_U32(voice + kVoiceOpenResult, ctx.r3.u32);  // stw r3,48(r31)

  // loc_82B33F88
  Leave(ctx, entry_lr, 1);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note; it is not
// the reason for the label -- the label is gate 1.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B33D40
SKATE3_PORT(82B33D40, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
