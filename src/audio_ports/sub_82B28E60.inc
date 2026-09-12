// sub_82B28E60  role: the PacketPlayer decode pump -- bail out when the record at +12 is marked dead (+71 == 2), retire every one of the twenty stride-12 table entries at +0x54 whose discriminator reads 2 and whose decoder slot has been fully consumed, republish the playback position at +0x30 as [+0x164] / [+0x154] (or a pool constant when the player is not active), then spin: while the state at +0x15E is neither 0 nor 4 and the entry the cursor at +0x173 names is free, start the codec with sub_82B29018 and latch state 2, and once in state 2 hand a decode request to sub_82B28D08, repeating for as long as it keeps returning non-zero   lifted: skate3_recomp.67.cpp:27205 (247 lines)
// STATUS: gate-1 (no bctrl of its own, but its callees are not replayable: sub_82B3C870's virtual notify at census depth 2, `bctrl` 0x82B3C8E4, and sub_82B29018 one level down takes a critical section and makes two indirect calls of its own -- docs/PLAN.md calls it a closure parameter rather than a port for exactly that reason)
// calls/session: boot 1681  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B28E60.md
namespace port_82B28E60 {

// The object is rw_player (docs/rw_audio_structs.h); decimal offsets here, hex in the comments.
constexpr uint32_t kRecord = 12;    // lwz r11,12(r3) -- the registration record
constexpr uint32_t kRecordKind = 71;  // lbz r10,71(r11) -- 2 means dead; sub_82B49280 writes it
constexpr uint32_t kPosition = 48;  // +0x30  stfd -- a DOUBLE, not in rw_audio_structs.h yet
constexpr uint32_t kTable = 84;     // +0x54  rw_player::_table54, 20 entries of 12 bytes
constexpr uint32_t kTableEntries = 20;
constexpr uint32_t kEntryBytes = 12;
constexpr uint32_t kEntryWord = 0;   // stw r29,-9(r7) -- the entry's first word, cleared with it
constexpr uint32_t kEntrySlot = 8;   // lbz r9,-1(r7)  -- which decoder slot this entry holds
constexpr uint32_t kEntryKind = 9;   // +0x09 the discriminator sub_82B28A00 also reads
constexpr uint32_t kEntryPending = 2;  // cmplwi cr6,r10,2 -- the only value this retires

constexpr uint32_t kDecoder = 336;      // +0x150 rw_player::decoder
constexpr uint32_t kSampleRate = 340;   // +0x154 rw_player::sample_rate, read here as an f32
constexpr uint32_t kState = 350;        // +0x15E rw_player::state
constexpr uint32_t kSampleCount = 356;  // +0x164 s32, the numerator of the position
constexpr uint32_t kCursor = 371;       // +0x173 rw_player::stop_field173 -- the entry index

// The decoder's slot ring. Nothing names these; the shapes are what this function reads.
constexpr uint32_t kSlotTableOffset = 36;  // lwz r10,36(r11) -- byte offset of the ring, from the
                                           // decoder base, so the ring is inside the decoder
constexpr uint32_t kSlotBytes = 24;        // rlwinm r8,r8,3,0,28 on slot*3
constexpr uint32_t kSlotStart = 8;         // lwz r11,8(r8)
constexpr uint32_t kSlotEnd = 12;          // lwz r10,12(r8)
constexpr uint32_t kDecoderCursor = 28;    // lwz r11,28(r11) -- used instead of kSlotStart when
constexpr uint32_t kDecoderCurrent = 49;   // lbz r6,49(r11) names this slot as the current one

constexpr uint32_t kStateStopped = 4;  // cmpwi cr6,r11,4
constexpr uint32_t kStatePlaying = 1;  // cmplwi cr6,r9,1
constexpr uint32_t kStateDecoding = 2; // li r30,2 ; stb r30,350(r31)

// lis r11,-32208 ; addi r10,r11,-31232 ; lfd f0,256(r10): ((-32208 & 0xFFFF) << 16) - 31232 + 256.
// The same 0x822F8xxx double pool sub_82B1D5D0 reads 1/4096 from.
constexpr uint32_t kIdlePosition = 0x822F8700;

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
constexpr uint32_t kConvertSlot = 80;  // std r10,80(r1) ; lfd f13,80(r1) -- inside the own frame
// __savegprlr_29 / __restgprlr_29 slots, negative offsets from the ENTRY sp, all inside the 128
// bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

// rotlwi rX,rY,1 ; add rX,rY,rX ; rlwinm rX,rX,N,0,31-N: index * 3 then * 2^N, each rotate masked
// to 32 bits but the add a full 64-bit add.
uint64_t TimesThree(uint64_t index) {
  const uint64_t twice = __builtin_rotateleft32(static_cast<uint32_t>(index), 1);
  return index + twice;
}
uint64_t SlotOffset(uint64_t slot) {  // *24
  return (static_cast<uint64_t>(static_cast<uint32_t>(TimesThree(slot))) << 3) & 0xFFFFFFF8ull;
}
uint64_t EntryOffset(uint64_t index) {  // *12
  return (static_cast<uint64_t>(static_cast<uint32_t>(TimesThree(index))) << 2) & 0xFFFFFFFCull;
}

// cmpwi 4 / beq ; cmpwi 0 / li 1 / bne ; clrlwi 24 / cmplwi 0 -- the byte is 0 or 1 before the
// mask, so the mask is a no-op and the predicate is just "neither stopped nor zero".
bool Active(uint32_t state) { return state != kStateStopped && state != 0; }

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3

  // The frame is reproduced because both callees run on it, and because the fcfid round trip
  // below goes through frame + 80.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);   // bl 0x82f5211c -> std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);   //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);   //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  //                  stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  // lwz r11,12(r3) ; lbz r10,71(r11) ; cmplwi cr6,r10,2 ; beq cr6,0x82b2900c -- a dead
  // registration ends the call before anything is touched.
  const uint32_t record = REX_LOAD_U32(self + kRecord);
  if (REX_LOAD_U8(record + kRecordKind) != 2) {
    // li r11,20 ; mtctr r11 ; addi r7,r3,93 ; li r29,0 -- r29 stays zero for the whole function
    // and is the value both stores below write.
    uint64_t kind_byte = static_cast<uint64_t>(self) + kTable + kEntryKind;  // r7
    for (uint32_t i = 0; i < kTableEntries; i++) {  // bdnz 0x82b28e90, exactly twenty passes
      // loc_82B28E90: lbz r10,0(r7) ; cmplwi cr6,r10,2 ; bne cr6,0x82b28ef4
      if (REX_LOAD_U8(static_cast<uint32_t>(kind_byte)) == kEntryPending) {
        // lbz r9,-1(r7) ; lwz r11,336(r31) -- the decoder is NOT null-checked; a null one reads
        // its table offset from address 36. Kept as lifted.
        const uint64_t slot = REX_LOAD_U8(static_cast<uint32_t>(kind_byte) - 1);
        const uint32_t decoder = REX_LOAD_U32(self + kDecoder);
        const uint64_t table = REX_LOAD_U32(decoder + kSlotTableOffset);  // lwz r10,36(r11)
        // add r10,r8,r10 ; add r8,r10,r11 -- both 64-bit.
        const uint64_t entry = SlotOffset(slot) + table + static_cast<uint64_t>(decoder);
        const uint32_t end = REX_LOAD_U32(static_cast<uint32_t>(entry) + kSlotEnd);  // lwz r10,12
        bool retire;
        // cmpwi cr6,r10,0 ; beq cr6,0x82b28eec -- SIGNED against zero.
        if (static_cast<int32_t>(end) == 0) {
          retire = true;
        } else {
          // lbz r6,49(r11) ; cmplw cr6,r9,r6 ; bne cr6,0x82b28edc -- UNSIGNED.
          const uint32_t current = REX_LOAD_U8(decoder + kDecoderCurrent);
          const uint64_t start = (static_cast<uint32_t>(slot) == current)
                                     ? REX_LOAD_U32(decoder + kDecoderCursor)   // lwz r11,28(r11)
                                     : REX_LOAD_U32(static_cast<uint32_t>(entry) + kSlotStart);
          // subf r11,r11,r10 -- 64-bit, and cr6 sees only the low word.
          const uint64_t remaining = static_cast<uint64_t>(end) - start;
          retire = static_cast<int32_t>(static_cast<uint32_t>(remaining)) == 0;
        }
        if (retire) {
          // loc_82B28EEC: stb r29,0(r7) ; stw r29,-9(r7) -- the discriminator first, then the
          // entry's own first word nine bytes back.
          REX_STORE_U8(static_cast<uint32_t>(kind_byte), 0);
          REX_STORE_U32(static_cast<uint32_t>(kind_byte) - kEntryKind + kEntryWord, 0);
        }
      }
      kind_byte = kind_byte + kEntryBytes;  // addi r7,r7,12
    }

    // lbz r11,350(r31) -- reloaded because the retire loop's stores could alias it.
    if (!Active(REX_LOAD_U8(self + kState))) {
      // Not active: the position becomes a pool constant and the call ends.
      ctx.fpscr.disableFlushModeUnconditional();
      REX_STORE_U64(self + kPosition, REX_LOAD_U64(kIdlePosition));  // lfd f0 ; stfd f0,48(r31)
    } else {
      // loc_82B28F3C: position = double(s32 [+0x164]) / double(f32 [+0x154]). `fdiv`, not
      // `fdivs`, so this is a full double divide and the result is stored as a double.
      const uint32_t count = REX_LOAD_U32(self + kSampleCount);  // lwz r11,356(r31)
      ctx.fpscr.disableFlushModeUnconditional();
      PPCRegister rate{};
      rate.u32 = REX_LOAD_U32(self + kSampleRate);  // lfs f0,340(r31)
      const double divisor = double(rate.f32);
      // extsw r10,r11 ; std r10,80(r1) ; lfd f13,80(r1) ; fcfid f12,f13 -- the round trip goes
      // through the own frame, on the SIGN-extended word.
      REX_STORE_U64(frame + kConvertSlot,
                    static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(count))));
      const int64_t spilled = static_cast<int64_t>(REX_LOAD_U64(frame + kConvertSlot));
      PPCRegister position{};
      position.f64 = double(spilled) / divisor;       // fcfid f12,f13 ; fdiv f11,f12,f0
      REX_STORE_U64(self + kPosition, position.u64);  // stfd f11,48(r31)

      for (;;) {
        // loc_82B28F60
        const uint32_t state = REX_LOAD_U8(self + kState);  // lbz r9,350(r31)
        if (!Active(state)) break;                         // beq cr6,0x82b2900c
        // lbz r11,371(r31) ; *12 ; add r10,r11,r31 ; lbz r8,93(r10) -- the entry the cursor
        // names must be free before anything is started.
        const uint64_t entry =
            EntryOffset(REX_LOAD_U8(self + kCursor)) + static_cast<uint64_t>(self);
        if (REX_LOAD_U8(static_cast<uint32_t>(entry) + kTable + kEntryKind) != 0) break;

        // cmplwi cr6,r9,1 ; bne cr6,0x82b28fc8
        if (state == kStatePlaying) {
          // bl 0x82b29018 -- the codec start. Takes a critical section and two indirect calls.
          skate3::audio::GuestCall(ctx, base, sub_82B29018, static_cast<uint64_t>(self));
          // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; beq cr6,0x82b2900c -- only the LOW BYTE.
          if ((ctx.r3.u32 & 0xFF) == 0) break;
          REX_STORE_U8(self + kState, kStateDecoding);  // stb r30,350(r31)
        }

        // loc_82B28FC8: the state is reloaded, so sub_82B29018 can have moved it on.
        if (REX_LOAD_U8(self + kState) != kStateDecoding) continue;  // bne cr6,0x82b28f60
        // The cursor and its entry are read a SECOND time here, on purpose: the call above may
        // have advanced either.
        const uint64_t ready =
            EntryOffset(REX_LOAD_U8(self + kCursor)) + static_cast<uint64_t>(self);
        if (REX_LOAD_U8(static_cast<uint32_t>(ready) + kTable + kEntryKind) != 0) continue;
        // li r4,0 ; bl 0x82b28d08 -- queue one decode request.
        skate3::audio::GuestCall(ctx, base, sub_82B28D08, static_cast<uint64_t>(self), 0ull);
        // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; bne cr6,0x82b28f60 -- keep pumping while it
        // reports progress; a zero low byte ends the call.
        if ((ctx.r3.u32 & 0xFF) == 0) break;
      }
    }
  }

  // loc_82B2900C, and the early returns: addi r1,r1,128 ; b __restgprlr_29. No `li r3,...` on any
  // path -- r3 is the last callee's result, or the caller's own r3 when nothing was called.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.r29.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR29);   // ld r29,-32(r1)
  ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR30);   // ld r30,-24(r1)
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR31);   // ld r31,-16(r1)
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
}

// Never called: the SKATE3_PORT macro skips the shadow branch for a gate-labelled port. Kept so
// the file has the same shape as every other port; the write set it would declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B28E60
SKATE3_PORT(82B28E60, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
