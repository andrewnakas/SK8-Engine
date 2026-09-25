// sub_82B33970  role: advance voice `index` of the system at r3 to its next buffer -- the voice's mode byte at +73 picks one of three shapes: 0 opens the next buffer straight away, 1 pushes a timestamp to the 0x82EC subsystem and hands the voice to sub_82B332D0, anything else opens the next buffer only while the slot cursor is still short of the voice's watermark and then decides between "keep going" and sub_82B332D0. Writes 1 through the r6 byte pointer when the slot cursor has already gone negative, 0 otherwise   lifted: skate3_recomp.67.cpp:52996 (368 lines)
// STATUS: partial -- compared 4569, skipped 2170, zero divergence (session g1_splits, 150 s scripted); thin -- the compared path writes one byte and returns 1. NEVER promoted: the skipped path has a native body but no comparison. (comparable only when the slot cursor at slot+24 is already negative -- that path calls nothing at all, writes one byte through r6 and returns 1; every other cursor value reaches sub_82B33418, sub_82B332D0 or sub_82EC0E08, i.e. two function pointers and a critical section)
// calls/session: boot 2070  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33970.md
namespace port_82B33970 {

// The same two records sub_82B33D40.inc works on, at the same offsets; nothing in
// docs/rw_audio_structs.h names either, and the names below are read off the uses.

// The system object (r3/r30).
constexpr uint32_t kVoiceArray = 96;  // -> base of the 80-byte voice records
constexpr uint32_t kSlotTable = 464;  // u16: byte offset, inside the object, of the 48-byte slots

// A voice record: 80 bytes, at *(object + 96) + 80 * index (r31).
constexpr uint32_t kVoiceBaseTime = 0;      // double, added to the converted delta at +12
constexpr uint32_t kVoiceDataBase = 8;      // copied to +52 when the cursor is exactly 0
constexpr uint32_t kVoiceDelta = 12;        // s32, sign-extended and converted to double
constexpr uint32_t kVoiceWatermark = 16;    // s32, compared against both slot+24 and slot+20
constexpr uint32_t kVoicePublishedCursor = 20;  // written: a copy of the slot's cursor
constexpr uint32_t kVoiceSinkArg = 28;      // second argument to sub_82EC0E08; also tested for 0
constexpr uint32_t kVoiceSink = 40;         // the 0x82EC subsystem object
constexpr uint32_t kVoiceOpenResult = 48;   // written: sub_82B33418's return value
constexpr uint32_t kVoiceStartPtr = 52;     // written: sub_82B33418's second argument
constexpr uint32_t kVoiceMode = 73;         // u8, selects the shape: 0, 1, or anything else
constexpr uint32_t kVoiceStartFlag = 74;    // u8 written from the word sub_82B32550 fills
constexpr uint32_t kVoiceState = 76;        // u8, picks the tag and gates the timestamp push

// A control slot: 48 bytes, at object + u16(object + 464) + 48 * index (r28).
constexpr uint32_t kSlotTotal = 20;   // s32
constexpr uint32_t kSlotCursor = 24;  // s32; negative means the stream is spent

// lis r11,-32077 ; addi r7,r11,7728 / 7824 -- ((-32077 & 0xFFFF) << 16) + the displacement, i.e.
// 0x82B31E30 and 0x82B31E90. Two fixed pointers into the image handed to sub_82EC0E08 as its fifth
// argument; what they point at is not read here. `lis` sign-extends to 64 bits and the lifted form
// keeps that, so the argument is the negative value.
constexpr int64_t kTagIdle = static_cast<int32_t>(0x82B31E30u);
constexpr int64_t kTagStarted = static_cast<int32_t>(0x82B31E90u);

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)
constexpr uint32_t kFrame80 = 80;      // the one frame slot this body uses, for three purposes

// The epilogue every return path shares: li r3,<n> ; addi r1,r1,144 ; b __restgprlr_27. That stub
// reloads r27..r31 and lr from the frame; this body keeps those registers in locals and never
// writes them, so restoring lr from the entry value is the same result.
inline void Leave(PPCContext& ctx, uint64_t entry_lr, int64_t result) {
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.lr = entry_lr;
  ctx.r3.s64 = result;
}

// The timestamp push at 0x82B33A6C and 0x82B33B70, identical but for the tag: the voice's +12 word,
// sign-extended and converted to a double, added to the double at +0, truncated back to a 64-bit
// integer, and handed to sub_82EC0E08. Both conversions round-trip through frame+80, as lifted.
void PushTimestamp(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t voice, uint32_t object,
                   int64_t tag) {
  const int32_t delta = static_cast<int32_t>(REX_LOAD_U32(voice + kVoiceDelta));  // lwz r11,12(r31)
  // lfd f0,0(r31)
  ctx.fpscr.disableFlushModeUnconditional();
  PPCRegister base_time{};
  base_time.u64 = REX_LOAD_U64(voice + kVoiceBaseTime);
  const uint32_t sink_arg = REX_LOAD_U32(voice + kVoiceSinkArg);  // lwz r4,28(r31)
  const uint32_t sink = REX_LOAD_U32(voice + kVoiceSink);         // lwz r3,40(r31)

  // extsw r10,r11 ; std r10,80(r1) ; lfd f13,80(r1) ; fcfid f12,f13
  REX_STORE_U64(frame + kFrame80, static_cast<uint64_t>(static_cast<int64_t>(delta)));
  PPCRegister spilled{};
  spilled.u64 = REX_LOAD_U64(frame + kFrame80);
  // fadd f11,f12,f0
  PPCRegister stamp{};
  stamp.f64 = double(spilled.s64) + base_time.f64;
  // fctidz f10,f11 -- NaN and anything above 2^63 handled exactly as the lifted line does; the test
  // is `>`, so 2^63 itself goes through cvttsd2si.
  const int64_t truncated = std::isnan(stamp.f64) ? int64_t(0x8000000000000000ULL)
                            : (stamp.f64 > double(LLONG_MAX))
                                ? LLONG_MAX
                                : simde_mm_cvttsd_si64(simde_mm_load_sd(&stamp.f64));
  // stfd f10,80(r1) ; ld r6,80(r1) -- passed as the 64-bit word it spilled to
  REX_STORE_U64(frame + kFrame80, static_cast<uint64_t>(truncated));
  const uint64_t stamp_arg = REX_LOAD_U64(frame + kFrame80);
  // bl 0x82ec0e08 -- li r5,0 supplies the third argument. It reaches sub_82EC1290's critical
  // section: gate 1.
  skate3::audio::GuestCall(ctx, base, sub_82EC0E08, sink, sink_arg, 0, stamp_arg, tag, object);
}

// loc_82B339E4 and loc_82B33B0C, identical but for one store: zero a frame word, let sub_82B32550
// choose the next buffer index into it, publish the low byte at voice+74, and open the voice
// through sub_82B33418. The first site also republishes the slot cursor into voice+20, in the
// middle of the sequence; the second already did that at loc_82B33AE0.
void OpenNextBuffer(PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t object, uint32_t voice,
                    uint32_t slot, uint64_t index, uint64_t user, bool publish_cursor) {
  // stw r9,80(r1) -- r9 is the li r9,0 from the top of the function on both paths
  REX_STORE_U32(frame + kFrame80, 0);
  // addi r4,r1,80 ; bl 0x82b32550
  skate3::audio::GuestCall(ctx, base, sub_82B32550, object, frame + kFrame80);
  // lwz r11,80(r1) ; stb r11,74(r31)
  REX_STORE_U8(voice + kVoiceStartFlag, static_cast<uint8_t>(REX_LOAD_U32(frame + kFrame80)));
  if (publish_cursor) {
    // lwz r9,24(r28) ; stw r9,20(r31)
    REX_STORE_U32(voice + kVoicePublishedCursor, REX_LOAD_U32(slot + kSlotCursor));
  }
  // lwz r4,52(r31) ; li r7,1 ; li r8,0 ; bl 0x82b33418 -- reaches sub_82B3C870's bctrl: gate 1
  const uint32_t start = REX_LOAD_U32(voice + kVoiceStartPtr);
  skate3::audio::GuestCall(ctx, base, sub_82B33418, object, start, index, user, 1, 0);
  REX_STORE_U32(voice + kVoiceOpenResult, ctx.r3.u32);  // stw r3,48(r31)
}

REX_FUNC(Native) {
  const uint64_t entry_lr = ctx.lr;  // mflr r12, spilled by __savegprlr_27

  // stwu r1,-144(r1). The frame is reproduced: sub_82B32550 is handed a pointer into it and both
  // float conversions spill there. __savegprlr_27's spills of r27..r31 land in it as well; they are
  // the function's own frame and are not reproduced, because this body keeps those values in
  // locals and leaves the registers alone.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;      // r30
  const uint64_t index = ctx.r4.u64;       // r29
  const uint64_t user = ctx.r5.u64;        // r27, forwarded untouched
  const uint32_t out_flag = ctx.r6.u32;    // a byte out-parameter, written before any call runs

  // rlwinm r11,r4,1 ; add r11,r4,r11 ; rlwinm r8,r11,4 -- index * 48; and the same shape for
  // index * 80. The lifted adds are 64-bit, but each rlwinm keeps only the low word and the sums
  // are used only as addresses, so 32-bit arithmetic is the same value.
  const uint32_t slot_base = REX_LOAD_U16(object + kSlotTable);   // lhz r9,464(r3)
  const uint32_t voice_array = REX_LOAD_U32(object + kVoiceArray);  // lwz r10,96(r3)
  const uint32_t voice = voice_array + (static_cast<uint32_t>(ctx.r4.u32 * 5u) << 4);  // r31
  const uint32_t slot = (slot_base + (static_cast<uint32_t>(ctx.r4.u32 * 3u) << 4)) + object;  // r28

  // lwz r11,24(r28) ; cmpwi cr6,r11,0 ; blt -> loc_82B33BF4
  const int32_t cursor = static_cast<int32_t>(REX_LOAD_U32(slot + kSlotCursor));
  if (cursor < 0) {
    // loc_82B33BF4: li r11,1 ; stb r11,0(r6). This is the one path that calls nothing at all.
    REX_STORE_U8(out_flag, 1);
    Leave(ctx, entry_lr, 1);
    return;
  }

  // lbz r10,73(r31) ; li r9,0 ; stb r9,0(r6)
  const uint32_t mode = REX_LOAD_U8(voice + kVoiceMode);
  REX_STORE_U8(out_flag, 0);

  if (mode == 0) {
    // cmpwi cr6,r11,0 ; bne -> loc_82B339E4 -- the same r11 as the test above, not reloaded
    if (cursor == 0) {
      // lwz r11,8(r31) ; stw r11,52(r31)
      REX_STORE_U32(voice + kVoiceStartPtr, REX_LOAD_U32(voice + kVoiceDataBase));
    }
    // loc_82B339E4
    OpenNextBuffer(ctx, base, frame, object, voice, slot, index, user, /*publish_cursor=*/true);
    Leave(ctx, entry_lr, 1);
    return;
  }

  // loc_82B33A30: cmplwi cr6,r10,1 ; bne -> loc_82B33AE0
  if (mode == 1) {
    // lbz r10,76(r31) -- nonzero picks the second tag (loc_82B33A50), zero the first
    const uint32_t state = REX_LOAD_U8(voice + kVoiceState);
    const int64_t tag = state != 0 ? kTagStarted : kTagIdle;
    // loc_82B33A58: cmplwi cr6,r10,1 ; bne -> loc_82B33A6C ; lwz r11,28(r31) ; beq -> loc_82B33AA8
    if (state != 1 || REX_LOAD_U32(voice + kVoiceSinkArg) != 0) {
      PushTimestamp(ctx, base, frame, voice, object, tag);
    }
    // loc_82B33AA8: lwz r11,24(r28) ; stw r11,20(r31) -- the cursor is reloaded here, after a call
    // that could have moved it
    REX_STORE_U32(voice + kVoicePublishedCursor, REX_LOAD_U32(slot + kSlotCursor));
    // li r7,0 ; li r6,1 ; bl 0x82b332d0 -- reaches sub_82B3C870's bctrl through sub_82B33418
    skate3::audio::GuestCall(ctx, base, sub_82B332D0, object, index, user, 1, 0);
    // clrlwi r10,r3,24 ; cmplwi cr6,r10,0 ; bne -> loc_82B33BFC
    Leave(ctx, entry_lr, (ctx.r3.u32 & 0xFF) != 0 ? 1 : 0);
    return;
  }

  // loc_82B33AE0
  const uint32_t published = REX_LOAD_U32(slot + kSlotCursor);          // lwz r11,24(r28)
  const uint32_t watermark = REX_LOAD_U32(voice + kVoiceWatermark);     // lwz r10,16(r31)
  REX_STORE_U32(voice + kVoicePublishedCursor, published);              // stw r11,20(r31)
  // lwz r8,24(r28) -- a genuine reload after the store above, which could alias it
  const uint32_t cursor_now = REX_LOAD_U32(slot + kSlotCursor);
  // cmpw cr6,r8,r10 ; bge -> loc_82B33B44 (signed)
  if (static_cast<int32_t>(cursor_now) < static_cast<int32_t>(watermark)) {
    // rotlwi r11,r8,0 ; cmpwi cr6,r11,0 ; bne -> loc_82B33B0C -- the rotate is a no-op copy
    if (static_cast<int32_t>(cursor_now) == 0) {
      REX_STORE_U32(voice + kVoiceStartPtr, REX_LOAD_U32(voice + kVoiceDataBase));
    }
    // loc_82B33B0C -- no cursor republish here: loc_82B33AE0 already did it
    OpenNextBuffer(ctx, base, frame, object, voice, slot, index, user, /*publish_cursor=*/false);
  }

  // loc_82B33B44: lwz r10,16(r31) ; lwz r9,20(r28) ; rlwinm/subfc/subfe/and/clrlwi -- the
  // carry-based lowering of a SIGNED less-than, byte-extended and tested against zero. Both
  // operands are reloaded here.
  const uint32_t reached = REX_LOAD_U32(voice + kVoiceWatermark);
  const uint32_t total = REX_LOAD_U32(slot + kSlotTotal);
  // beq cr6,0x82b33bfc -- not less than: nothing more to do, return 1
  if (!(static_cast<int32_t>(reached) < static_cast<int32_t>(total))) {
    Leave(ctx, entry_lr, 1);
    return;
  }

  // The tag here is the first one unconditionally (addi r7,r10,7728).
  PushTimestamp(ctx, base, frame, voice, object, kTagIdle);

  // lwz r8,24(r28) ; lwz r7,16(r31) ; cmpw cr6,r8,r7 ; blt -> loc_82B33BFC
  const uint32_t cursor_after = REX_LOAD_U32(slot + kSlotCursor);
  const uint32_t watermark_after = REX_LOAD_U32(voice + kVoiceWatermark);
  if (static_cast<int32_t>(cursor_after) < static_cast<int32_t>(watermark_after)) {
    Leave(ctx, entry_lr, 1);
    return;
  }

  // li r7,0 ; li r6,1 ; bl 0x82b332d0
  skate3::audio::GuestCall(ctx, base, sub_82B332D0, object, index, user, 1, 0);
  // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; bne -> loc_82B33BFC
  Leave(ctx, entry_lr, (ctx.r3.u32 & 0xFF) != 0 ? 1 : 0);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. One path here is in fact comparable -- see the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  // The same r28 arithmetic Native does, from the same entry bytes: object + u16(+464) + 48*index.
  const uint32_t slot_base = REX_LOAD_U16(object + kSlotTable);
  const uint32_t slot = (slot_base + (static_cast<uint32_t>(ctx.r4.u32 * 3u) << 4)) + object;
  spec.read(object + kSlotTable, 2);
  spec.read(slot + kSlotCursor, 4);

  // Both bodies store the frame link word at r1-144 before reading the cursor. If that word were
  // the cursor the entry read would not be the value the body tests, so decline rather than assume
  // the stack and the system object are disjoint.
  const uint32_t frame_link = ctx.r1.u32 - kFrameBytes;
  if (static_cast<uint64_t>(frame_link) + 4 > slot + kSlotCursor &&
      static_cast<uint64_t>(slot + kSlotCursor) + 4 > frame_link) {
    return false;
  }

  // lwz r11,24(r28) ; cmpwi cr6,r11,0 ; blt -> loc_82B33BF4. A spent stream is the one path that
  // calls nothing: `li r11,1 ; stb r11,0(r6) ; li r3,1`. Every other value reaches sub_82B33418 or
  // sub_82B332D0 (both -> sub_82B3C870's `bctrl`) or sub_82EC0E08 (-> sub_82EC1290's critical
  // section), none of which the harness can replay on rewound memory.
  if (static_cast<int32_t>(REX_LOAD_U32(slot + kSlotCursor)) >= 0) return false;

  spec.write(ctx.r6.u32, 1);  // stb r11,0(r6) -- the only byte this path writes
  return true;
}

}  // namespace port_82B33970
SKATE3_PORT(82B33970, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
