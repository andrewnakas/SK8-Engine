// sub_82B50100  role: seek a packetised bit stream to a frame -- divide the descriptor's sample position into 512-sample frames, walk the per-substream seek table's frame counts packet by packet until the target packet is found, skip the frames still ahead of it one length field at a time, then record the packet pointer, the bytes left in the chunk and the bit offset into the caller's cursor record   lifted: skate3_recomp.68.cpp:55550 (214 lines)
// STATUS: verified
// calls/session: boot 3817  play 4653  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B50100.md
namespace port_82B50100 {

// r3 -- the cursor record this call fills in. Nothing in docs/rw_audio_structs.h names it, so the
// offsets stay numbers. These three words are the whole write set.
constexpr uint32_t kCursorPacket = 4;     // u32  the 2048-byte packet the target frame starts in
constexpr uint32_t kCursorRemaining = 8;  // u32  bytes from that packet to the end of the chunk, -4
constexpr uint32_t kCursorBits = 20;      // u32  the bit offset inside that packet, or -1

// r4 -- the substream index, used only to pick a seek-table entry.
// r5 -- the stream descriptor.
constexpr uint32_t kDescTable = 4;    // u32  base of the seek table; 0 means "no table"
constexpr uint32_t kDescSamples = 8;  // s32  the sample position to seek to
constexpr uint32_t kDescFlag = 20;    // u8   clear adds the 384-sample bias before the divide

// r6 -- the chunk. `+0` is the packed length field whose two low bits are the flag
// docs/rust-port.md records as unexplained; `+4` is the first 2048-byte packet.
constexpr uint32_t kChunkLength = 0;
constexpr uint32_t kChunkPayload = 4;

constexpr uint32_t kFrameBytes = 160;   // stwu r1,-160(r1)
constexpr uint32_t kBitsCell = 80;      // the word at r1+80: the bit cursor, and the varint scratch
constexpr uint32_t kPointerCell = 88;   // the word at r1+88: the packet pointer sub_82B50270 walks
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kFrameShift = 9;     // srawi r8,r11,9 ; addze -- a signed divide by 512
constexpr uint32_t kSampleBias = 384;
constexpr uint32_t kBitShift = 11;      // srawi r6,r7,11
constexpr uint32_t kBitMask = 0x7FFF;   // clrlwi r10,r6,17
constexpr int32_t kPageBits = 16352;    // at or past this the position is poisoned to -1

// Caps on the RECORDED READ set only. Reads are never compared; these keep a large chunk or a
// runaway length field from bloating a recorded vector.
constexpr uint32_t kReadCap = 32 * 1024;
constexpr uint32_t kTableReadCap = 256;

// srawi rX,rS,n followed by addze: a signed division of the low 32 bits by 2^n that truncates
// toward zero. The carry addze consumes is set only for a negative dividend with a remainder.
inline int64_t DivideTowardZero(uint32_t value, uint32_t shift) {
  const int32_t signed_value = static_cast<int32_t>(value);
  const bool carry = signed_value < 0 && (value & ((1u << shift) - 1u)) != 0;
  return static_cast<int64_t>(signed_value >> shift) + (carry ? 1 : 0);
}

// lwz r7,80(r1) ; srawi r6,r7,11 ; clrlwi r10,r6,17 ; cmpwi cr6,r10,16352 ; blt ; li r10,-1.
// The four header bytes were just stored into the cell in stream order, so the load reads them
// back as one big-endian word.
inline int64_t BitPositionFromCell(uint8_t* base, uint32_t cell) {
  const uint32_t word = REX_LOAD_U32(cell);
  const int32_t shifted = static_cast<int32_t>(word) >> kBitShift;
  const int32_t value = static_cast<int32_t>(static_cast<uint32_t>(shifted) & kBitMask);
  return value < kPageBits ? static_cast<int64_t>(value) : static_cast<int64_t>(-1);
}

REX_FUNC(Native) {
  const uint32_t cursor = ctx.r3.u32;     // mr r26,r3
  const uint32_t substream = ctx.r4.u32;
  const uint32_t desc = ctx.r5.u32;
  const uint32_t chunk = ctx.r6.u32;

  // stwu r1,-160(r1). Reproduced -- most ports skip their own frame -- because the two cells at
  // +80 and +88 are handed to sub_82B4F5F8 and sub_82B50270 as pointers, so they have to be real
  // guest memory, and sub_82B50270 spills r28-r31 below whatever r1 holds when it is entered.
  // r25-r31 are never touched here, so __savegprlr_25 has nothing to reproduce.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;

  const uint32_t samples = REX_LOAD_U32(desc + kDescSamples);        // lwz r11,8(r5)
  const uint32_t length_field = REX_LOAD_U32(chunk + kChunkLength);  // lwz r10,0(r6)
  const uint32_t flag = REX_LOAD_U8(desc + kDescFlag);               // lbz r9,20(r5)

  // rlwinm r25,r10,30,2,31 -- the length field with its two low flag bits shifted out. This is
  // also what the function returns: the caller adds it to the chunk pointer to reach the next one.
  const uint64_t chunk_bytes = length_field >> 2;

  // srawi r8,r11,9 ; addze r28,r8 -- frames of 512 samples.
  int64_t frames = DivideTowardZero(samples, kFrameShift);
  if (flag == 0) {  // cmplwi cr6,r9,0 ; bne cr6,0x82b5013c
    // addi r11,r11,384 ; srawi r10,r11,9 ; addze r28,r10 -- the bias is added to the low 32 bits,
    // and every later use of r11 is a .s32/.u32 access, so a carry out of bit 31 is discarded.
    frames = DivideTowardZero(samples + kSampleBias, kFrameShift);
  }

  // loc_82B5013C -- the chunk's first packet, and the four header bytes of it.
  const uint64_t payload = ctx.r6.u64 + kChunkPayload;  // addi r27,r6,4
  uint64_t packet = payload;                            // mr r29,r27
  const uint32_t h1 = REX_LOAD_U8(chunk + 5);           // lbz r10,5(r6)
  const uint32_t h0 = REX_LOAD_U8(chunk + 4);           // lbz r11,4(r6)
  const uint32_t h2 = REX_LOAD_U8(chunk + 6);           // lbz r9,6(r6)
  const uint32_t h3 = REX_LOAD_U8(chunk + 7);           // lbz r8,7(r6)
  REX_STORE_U32(frame + kPointerCell, static_cast<uint32_t>(packet));  // stw r27,88(r1)
  REX_STORE_U8(frame + kBitsCell + 1, static_cast<uint8_t>(h1));       // stb r10,81(r1)
  REX_STORE_U8(frame + kBitsCell + 0, static_cast<uint8_t>(h0));       // stb r11,80(r1)
  REX_STORE_U8(frame + kBitsCell + 2, static_cast<uint8_t>(h2));       // stb r9,82(r1)
  REX_STORE_U8(frame + kBitsCell + 3, static_cast<uint8_t>(h3));       // stb r8,83(r1)
  int64_t position = BitPositionFromCell(base, frame + kBitsCell);
  REX_STORE_U32(frame + kBitsCell, static_cast<uint32_t>(position));   // stw r10,80(r1)

  const uint32_t table = REX_LOAD_U32(desc + kDescTable);  // lwz r11,4(r5)
  uint64_t consumed = 0;                                   // r31, frames left behind
  // cmplwi cr6,r11,0 ; beq cr6,0x82b50228 -- with no seek table the walk is skipped entirely and
  // the target frame is counted from the first packet.
  if (table != 0) {
    // rlwinm r10,r4,2,0,29 ; lwzx r10,r10,r11 ; add r30,r10,r11. The rotate's wrapped bits are
    // exactly the ones the mask clears, so this is a plain 32-bit index * 4; the add that follows
    // is 64-bit on two zero-extended words, and only the byte loads inside the callee truncate it.
    const uint32_t index_offset = static_cast<uint32_t>(substream << 2);
    const uint32_t entry = REX_LOAD_U32(index_offset + table);
    uint64_t reader = static_cast<uint64_t>(entry) + static_cast<uint64_t>(table);

    // bl 0x82b4f5f8 -- one unsigned varint, the frame count of the packet the reader is on. It
    // writes the value into the +80 cell, which is why the bit position stored above is scratch
    // until loc_82B501E8 recomputes it.
    skate3::audio::GuestCall(ctx, base, sub_82B4F5F8, reader,
                             static_cast<uint64_t>(frame + kBitsCell));
    uint64_t counted = REX_LOAD_U32(frame + kBitsCell);  // lwz r11,80(r1)
    reader = ctx.r3.u64 + reader;                        // add r30,r3,r30

    // cmpw cr6,r28,r11 ; blt cr6,0x82b501e8 -- signed, on the low 32 bits of both.
    if (static_cast<int32_t>(static_cast<uint32_t>(frames)) >=
        static_cast<int32_t>(static_cast<uint32_t>(counted))) {
      do {  // loc_82B501BC
        packet += kPacketBytes;  // addi r29,r29,2048
        consumed += counted;     // add r31,r11,r31 -- 64-bit, as the lifted add is
        skate3::audio::GuestCall(ctx, base, sub_82B4F5F8, reader,
                                 static_cast<uint64_t>(frame + kBitsCell));  // bl 0x82b4f5f8
        counted = REX_LOAD_U32(frame + kBitsCell);  // lwz r11,80(r1)
        reader = ctx.r3.u64 + reader;               // add r30,r3,r30
        // add r10,r11,r31 ; cmpw cr6,r28,r10 ; bge cr6,0x82b501bc
      } while (static_cast<int32_t>(static_cast<uint32_t>(frames)) >=
               static_cast<int32_t>(static_cast<uint32_t>(counted + consumed)));
      REX_STORE_U32(frame + kPointerCell, static_cast<uint32_t>(packet));  // stw r29,88(r1)
    }

    // loc_82B501E8 -- the header of the packet the walk settled on, same four bytes, same word.
    const uint32_t p0 = REX_LOAD_U8(static_cast<uint32_t>(packet) + 0);  // lbz r11,0(r29)
    const uint32_t p2 = REX_LOAD_U8(static_cast<uint32_t>(packet) + 2);  // lbz r10,2(r29)
    const uint32_t p1 = REX_LOAD_U8(static_cast<uint32_t>(packet) + 1);  // lbz r9,1(r29)
    const uint32_t p3 = REX_LOAD_U8(static_cast<uint32_t>(packet) + 3);  // lbz r8,3(r29)
    frames = static_cast<int64_t>(static_cast<uint64_t>(frames) - consumed);  // subf r28,r31,r28
    REX_STORE_U8(frame + kBitsCell + 0, static_cast<uint8_t>(p0));  // stb r11,80(r1)
    REX_STORE_U8(frame + kBitsCell + 2, static_cast<uint8_t>(p2));  // stb r10,82(r1)
    REX_STORE_U8(frame + kBitsCell + 1, static_cast<uint8_t>(p1));  // stb r9,81(r1)
    REX_STORE_U8(frame + kBitsCell + 3, static_cast<uint8_t>(p3));  // stb r8,83(r1)
    position = BitPositionFromCell(base, frame + kBitsCell);
    REX_STORE_U32(frame + kBitsCell, static_cast<uint32_t>(position));  // stw r11,80(r1)
  }

  // loc_82B50228 -- cmpwi cr6,r28,0 ; ble cr6,0x82b50248. One length field per frame still ahead
  // of the target; sub_82B50270 advances the cursor and steps the packet pointer on a wrap.
  if (static_cast<int32_t>(static_cast<uint32_t>(frames)) > 0) {
    do {  // loc_82B50230
      skate3::audio::GuestCall(ctx, base, sub_82B50270, static_cast<uint64_t>(frame + kPointerCell),
                               static_cast<uint64_t>(frame + kBitsCell));  // bl 0x82b50270
      frames -= 1;  // addic. r28,r28,-1 -- the branch tests the low 32 bits
    } while (static_cast<int32_t>(static_cast<uint32_t>(frames)) != 0);
    packet = REX_LOAD_U32(frame + kPointerCell);  // lwz r29,88(r1)
  }

  // loc_82B50248 -- publish. r11 = r27 - r29 is negative (one packet per step backwards from the
  // payload), so remaining = chunk_bytes - 4 - 2048 * packets walked.
  const uint64_t behind = payload - packet;               // subf r11,r29,r27
  const uint32_t bits = REX_LOAD_U32(frame + kBitsCell);  // lwz r10,80(r1)
  REX_STORE_U32(cursor + kCursorPacket, static_cast<uint32_t>(packet));  // stw r29,4(r26)
  ctx.r3.u64 = chunk_bytes;                                              // mr r3,r25
  const uint64_t remaining = behind + chunk_bytes;                       // add r11,r11,r25
  REX_STORE_U32(cursor + kCursorBits, bits);                             // stw r10,20(r26)
  REX_STORE_U32(cursor + kCursorRemaining,
                static_cast<uint32_t>(remaining - 4));  // addi r9,r11,-4 ; stw r9,8(r26)

  ctx.r1.u32 = entry_sp;  // addi r1,r1,160
}

// The write set is three words on the cursor record and nothing else. Both callees are ported and
// verified, and both write only through the pointers they are handed:
//
//   * sub_82B4F5F8 (recomp/src/audio_ports/sub_82B4F5F8.inc): one `stw` at r4, always r1+80 here.
//   * sub_82B50270 (recomp/src/audio_ports/sub_82B50270.inc): `stw` at r4 and, on the page-wrap
//     path, `stw` at r3 -- r1+80 and r1+88 here.
//
// Both cells are this function's own frame, so neither is declared. A change to either of those
// two files' window sets has to be reflected here.
//
// Nothing about the three addresses depends on the call: they are fixed offsets from the entry r3,
// so the walk's trip count -- which is not bounded from entry state -- cannot move a byte.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t cursor = ctx.r3.u32;
  const uint32_t desc = ctx.r5.u32;
  const uint32_t chunk = ctx.r6.u32;
  if (cursor == 0 || desc == 0 || chunk == 0) return false;  // the original would store through null

  spec.write(cursor + kCursorPacket, 8);  // +4 and +8, adjacent
  spec.write(cursor + kCursorBits, 4);    // +20

  // Reads, recorded for replay only, never compared, and deliberately a summary: the seek-table
  // walk consumes one to five bytes per packet with no bound available before the call, so a fixed
  // window over the reader's start stands in for it.
  spec.read(desc + kDescTable, 20);  // +4 table, +8 samples, +20 flag
  spec.read(chunk + kChunkLength, 4);
  const uint32_t chunk_bytes = REX_LOAD_U32(chunk + kChunkLength) >> 2;
  const uint32_t span = chunk_bytes > kReadCap ? kReadCap : chunk_bytes;
  if (span > kChunkPayload && uint64_t(chunk) + span <= 0x100000000ull) {
    spec.read(chunk + kChunkPayload, span - kChunkPayload);
  }
  const uint32_t table = REX_LOAD_U32(desc + kDescTable);
  if (table != 0) {
    const uint32_t index_offset = static_cast<uint32_t>(ctx.r4.u32 << 2);
    spec.read(index_offset + table, 4);
    const uint32_t reader = REX_LOAD_U32(index_offset + table) + table;
    if (uint64_t(reader) + kTableReadCap <= 0x100000000ull) spec.read(reader, kTableReadCap);
  }
  return true;
}

}  // namespace port_82B50100
SKATE3_PORT(82B50100, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
