// sub_82B335A8  role: parse a stream's packed bit header into voice `index`'s 80-byte record and 48-byte control slot (or, with no stream, write the defaults)   lifted: skate3_recomp.67.cpp:52452 (267 lines)
// STATUS: verified
// calls/session: boot 1943  play 2175  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B335A8.md
namespace port_82B335A8 {

// Nothing in docs/rw_audio_structs.h names either record. The offset names are the ones
// sub_82B33D40.inc (this function's only caller) gives them -- a reading, not a certainty.

// The system object (r3).
constexpr uint32_t kVoiceArray = 96;  // -> base of the 80-byte voice records
constexpr uint32_t kSlotTable = 464;  // u16: byte offset, inside the object, of the 48-byte slots

// A voice record: 80 bytes at *(object + 96) + 80 * index (r31).
constexpr uint32_t kVoiceDataBase = 8;      // written: the first byte after the header
constexpr uint32_t kVoiceDelta = 12;        // written: the tenth field (32 bits) or 0
constexpr uint32_t kVoice16 = 16;           // written: the ninth field (32 bits), mode 2 only
constexpr uint32_t kVoiceFormatIndex = 72;  // u8 written: the second field (4 bits)
constexpr uint32_t kVoiceMode = 73;         // u8 written: the fifth field (2 bits), then reloaded
constexpr uint32_t kVoiceState = 76;        // u8 written: the first field (4 bits)

// A control slot: 48 bytes at object + u16(object + 464) + 48 * index (r30).
constexpr uint32_t kSlot16 = 16;       // f32 written: the fourth field (18 bits) as a single
constexpr uint32_t kSlot20 = 20;       // u32 written: the seventh field (29 bits)
constexpr uint32_t kSlotCursor = 24;   // s32 written: the eighth field (32 bits), or -1
constexpr uint32_t kSlotType = 47;     // u8 written: the third field (6 bits) plus one

// lis r10,-32206 ; lfs f0,-22460(r10) -- ((-32206 & 0xFFFF) << 16) - 22460 = 0x8231A844.
constexpr uint32_t kDefaultSingle = 0x82320000u - 22460u;

constexpr uint8_t kHeaderTag = 72;     // lbz r11,0(r29) ; cmplwi cr6,r11,72 -- 'H', skipped by 4

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)
constexpr uint32_t kReader = 80;       // r1+80: {u32 data pointer, u32 bit cursor} for sub_82B26B70
constexpr uint32_t kReaderCursor = 84;
constexpr uint32_t kConvert = 88;      // r1+88: the std/lfd round trip before fcfid

// Every field the stream can supply: 4+4+6+18+2+1+29 bits, then up to three 32-bit words.
constexpr uint32_t kMaxHeaderBits = 4 + 4 + 6 + 18 + 2 + 1 + 29 + 32 * 3;

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

// rlwinm r11,r4,1 ; add r11,r4,r11 ; rlwinm r8,r11,4 -- index * 48. The add is 64-bit, but the
// rlwinm keeps only its low 32 bits, so a 32-bit product is the same value.
inline uint32_t SlotOffset(uint32_t index) { return static_cast<uint32_t>(index * 3u) << 4; }
// rlwinm r8,r4,2 ; add r7,r4,r8 ; rlwinm r11,r7,4 -- index * 80, the same shape.
inline uint32_t VoiceOffset(uint32_t index) { return static_cast<uint32_t>(index * 5u) << 4; }

// One call to the bit reader over the frame's {pointer, cursor} pair: li r4,N ; addi r3,r1,80 ;
// bl 0x82b26b70. The result is left in ctx.r3, as the lifted form reads it.
inline void ReadBits(PPCContext& ctx, uint8_t* base, uint32_t reader, uint32_t bits) {
  skate3::audio::GuestCall(ctx, base, sub_82B26B70, reader, bits);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;

  // lhz r9,464(r3) ; add r9,r9,r8 ; add r30,r9,r3 and lwz r10,96(r3) ; add r31,r11,r10 -- both
  // sums are 64-bit in the lifted form but used only through .u32 as store addresses.
  const uint32_t slot = static_cast<uint32_t>(uint64_t(REX_LOAD_U16(object + kSlotTable)) +
                                              SlotOffset(index) + ctx.r3.u64);        // r30
  const uint32_t voice = static_cast<uint32_t>(uint64_t(VoiceOffset(index)) +
                                               REX_LOAD_U32(object + kVoiceArray));   // r31
  uint64_t stream = ctx.r5.u64;  // mr r29,r5

  // __savegprlr_26 spills r26-r31; this body keeps every value in locals and never writes
  // those registers, so there is nothing to restore.

  // stwu r1,-160(r1). Reproduced: sub_82B26B70 takes a pointer to the {data, cursor} pair at
  // r1+80, and its own `std r31,-8(r1)` must not land on that pair.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  // cmplwi cr6,r5,0 ; bne cr6,0x82b3363c -- no stream: write the defaults and leave.
  if (static_cast<uint32_t>(stream) == 0) {
    REX_STORE_U8(slot + kSlotType, 0);                           // stb r27,47(r30)
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, slot + kSlot16, LoadSingle(base, kDefaultSingle));  // lfs ; stfs f0,16(r30)
    REX_STORE_U32(slot + kSlot20, 0x7FFFFFFFu);                  // stw r8,20(r30) (lis ; ori)
    REX_STORE_U32(slot + kSlotCursor, 0xFFFFFFFFu);              // stw r7,24(r30) (li r7,-1)
    REX_STORE_U8(voice + kVoiceFormatIndex, 255);                // stb r6,72(r31)
    REX_STORE_U8(voice + kVoiceMode, 1);                         // stb r11,73(r31)
    REX_STORE_U32(voice + kVoice16, 0);                          // stw r27,16(r31)
    REX_STORE_U32(voice + kVoiceDelta, 0);                       // stw r27,12(r31)
    REX_STORE_U32(voice + kVoiceDataBase, 0);                    // stw r27,8(r31)
    REX_STORE_U8(voice + kVoiceState, 1);                        // stb r11,76(r31)
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;                       // addi r1,r1,160
    return;
  }

  // loc_82B3363C: lbz r11,0(r29) ; cmplwi cr6,r11,72 ; bne -- an 'H' tag word is skipped.
  if (REX_LOAD_U8(static_cast<uint32_t>(stream)) == kHeaderTag) {
    stream = stream + 4;  // addi r29,r29,4 (64-bit)
  }

  // loc_82B3364C: the reader is {stream, 0} in the frame.
  const uint32_t reader = frame + kReader;
  REX_STORE_U32(reader, static_cast<uint32_t>(stream));  // stw r29,80(r1)
  REX_STORE_U32(frame + kReaderCursor, 0);               // stw r27,84(r1)

  ReadBits(ctx, base, reader, 4);
  REX_STORE_U8(voice + kVoiceState, ctx.r3.u8);                  // stb r3,76(r31)
  ReadBits(ctx, base, reader, 4);
  REX_STORE_U8(voice + kVoiceFormatIndex, ctx.r3.u8);            // stb r3,72(r31)
  ReadBits(ctx, base, reader, 6);
  REX_STORE_U8(slot + kSlotType, static_cast<uint8_t>(ctx.r3.s64 + 1));  // addi r9,r3,1 ; stb r9,47(r30)

  ReadBits(ctx, base, reader, 18);
  // clrldi r7,r3,32 ; std r7,88(r1) ; lfd f0,88(r1) ; fcfid f13,f0 ; frsp f12,f13 -- kept as the
  // round trip through the frame the lifted form makes.
  REX_STORE_U64(frame + kConvert, ctx.r3.u64 & 0xFFFFFFFFull);
  ctx.fpscr.disableFlushModeUnconditional();
  PPCRegister spilled{};
  spilled.u64 = REX_LOAD_U64(frame + kConvert);
  const double converted = double(float(double(spilled.s64)));
  StoreSingle(base, slot + kSlot16, converted);                  // stfs f12,16(r30)

  ReadBits(ctx, base, reader, 2);
  REX_STORE_U8(voice + kVoiceMode, ctx.r3.u8);                   // stb r3,73(r31)
  ReadBits(ctx, base, reader, 1);
  const uint32_t has_cursor = ctx.r3.u32 & 0xFF;                 // clrlwi r28,r3,24 (r26 below)
  ReadBits(ctx, base, reader, 29);
  REX_STORE_U32(slot + kSlot20, ctx.r3.u32);                     // stw r3,20(r30)

  // cmplwi cr6,r28,0 ; beq cr6,0x82b33700
  if (has_cursor != 0) {
    ReadBits(ctx, base, reader, 32);
    REX_STORE_U32(slot + kSlotCursor, ctx.r3.u32);               // stw r3,24(r30)
  } else {
    REX_STORE_U32(slot + kSlotCursor, 0xFFFFFFFFu);              // loc_82B33700: stw r11,24(r30)
  }

  // loc_82B33708: lbz r28,73(r31) -- RELOADED from the record rather than reusing the field.
  const uint32_t mode = REX_LOAD_U8(voice + kVoiceMode);
  // cmplwi cr6,r28,2 ; bne cr6,0x82b33724
  if (mode == 2) {
    ReadBits(ctx, base, reader, 32);
    REX_STORE_U32(voice + kVoice16, ctx.r3.u32);                 // stw r3,16(r31)
  }

  // loc_82B33724: cmplwi cr6,r26,0 ; beq cr6,0x82b33764
  if (has_cursor != 0) {
    bool read_delta;
    if (mode == 1) {  // cmplwi cr6,r28,1 ; beq cr6,0x82b33754
      read_delta = true;
    } else if (mode != 2) {  // cmplwi cr6,r28,2 ; bne cr6,0x82b3374c
      read_delta = false;
    } else {
      // lwz r11,24(r30) ; lwz r10,16(r31) ; cmpw cr6,r11,r10 ; bge cr6,0x82b33754 -- signed, and
      // both words reloaded after this call stored them.
      const int32_t cursor = static_cast<int32_t>(REX_LOAD_U32(slot + kSlotCursor));
      const int32_t limit = static_cast<int32_t>(REX_LOAD_U32(voice + kVoice16));
      read_delta = cursor >= limit;
    }
    if (read_delta) {
      ReadBits(ctx, base, reader, 32);                           // loc_82B33754
      REX_STORE_U32(voice + kVoiceDelta, ctx.r3.u32);            // stw r3,12(r31)
    } else {
      REX_STORE_U32(voice + kVoiceDelta, 0);                     // loc_82B3374C: stw r27,12(r31)
    }
  }

  // loc_82B33764: lwz r11,84(r1) ; rlwinm r11,r11,29,3,31 ; add r10,r11,r29 -- the header's byte
  // length added to the (possibly tag-skipped) stream pointer, 64-bit, truncated by the store.
  const uint64_t header_bytes = REX_LOAD_U32(frame + kReaderCursor) >> 3;
  REX_STORE_U32(voice + kVoiceDataBase, static_cast<uint32_t>(header_bytes + stream));  // stw r10,8(r31)
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
}

// The write set, entry state only. Both records are addressed once at entry (object+96 and
// object+464 are loaded before the first store and never again), so every address is known now.
// sub_82B26B70's only store is the cursor word at r1+84 -- this function's own frame, not declared.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;  // the original would load its two bases off null
  const uint32_t index = ctx.r4.u32;
  const uint32_t slot = static_cast<uint32_t>(uint64_t(REX_LOAD_U16(object + kSlotTable)) +
                                              SlotOffset(index) + ctx.r3.u64);
  const uint32_t voice = static_cast<uint32_t>(uint64_t(VoiceOffset(index)) +
                                               REX_LOAD_U32(object + kVoiceArray));
  if (uint64_t(slot) + 48 > 0x100000000ull || uint64_t(voice) + 80 > 0x100000000ull) return false;

  spec.write(slot + kSlot16, 12);              // +16 f32, +20 u32, +24 s32
  spec.write(slot + kSlotType, 1);             // +47 u8
  spec.write(voice + kVoiceDataBase, 12);      // +8, +12, +16
  spec.write(voice + kVoiceFormatIndex, 2);    // +72, +73
  spec.write(voice + kVoiceState, 1);          // +76

  spec.read(object + kVoiceArray, 4);
  spec.read(object + kSlotTable, 2);
  const uint32_t stream = ctx.r5.u32;
  if (stream == 0) {
    spec.read(kDefaultSingle, 4);
  } else {
    // The tag byte, the optional 4-byte skip, and at most kMaxHeaderBits of payload.
    const uint64_t end = uint64_t(stream) + 4 + (kMaxHeaderBits + 7) / 8 + 1;
    spec.read(stream, static_cast<uint32_t>((end > 0x100000000ull ? 0x100000000ull : end) - stream));
  }
  return true;
}

}  // namespace port_82B335A8
SKATE3_PORT(82B335A8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
