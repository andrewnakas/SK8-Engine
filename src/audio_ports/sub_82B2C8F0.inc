// sub_82B2C8F0  role: re-fold the mix rows when the channel count changes -- build the back owner's row pointers and the front owner's row pointers in frame scratch, remap one into the other through sub_82B468C0, then swap the owner pair and record the new count   lifted: skate3_recomp.67.cpp:36226 (143 lines)
// STATUS: verified
// calls/session: boot 48658  play 147982  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2C8F0.md
namespace port_82B2C8F0 {

// r3 = the mixer-side object, r4 = the two-entry row-owner pair. The same two structures
// port_82B34E08 works on, with its names.
constexpr uint32_t kMirroredCount = 41;  // u8   set from the pair's old count when +48 is zero
constexpr uint32_t kChannels = 42;       // u8   the new channel count; RELOADED at every use

constexpr uint32_t kPairBack = 28;    // rw_ptr  the owner the rows come FROM
constexpr uint32_t kPairFront = 32;   // rw_ptr  the owner the rows go TO
constexpr uint32_t kPairLength = 48;  // u32     row length in floats; zero skips the remap
constexpr uint32_t kPairCount = 60;   // u8      the count the pair was last folded for

// On a row owner -- the same two fields port_82B34E08 and sub_82B3C668 read.
constexpr uint32_t kRowBase = 4;     // rw_ptr
constexpr uint32_t kRowStride = 14;  // u16  row pitch in SINGLES; the byte pitch is 4x

// The 176-byte frame: eight source pointers, then the destination pointers.
constexpr uint32_t kFrameBytes = 176;
constexpr uint32_t kSourceArray = 80;
constexpr uint32_t kDestArray = 112;
constexpr uint32_t kArrayWords = 8;

// sub_82B426D0's channel-mapping tables, reached when both counts are in {1,2,4,6,8}. Both computed
// from lis -32241 (0x820F0000) plus -10496 / -10368, not read by eye.
// CORRECTED 2026-09-13: these read 0x820CD700 / 0x820CD780 -- a digit wrong, by eye. 0x820F0000 -
// 10496 is 0x820ED700, which is what sub_82B426D0 and sub_82B468C0 use and what the Rust gather
// replays clean against. Until the fix, MappingStaysInBounds below checked the wrong table.
constexpr uint32_t kMatrixIndex = 0x820ED700u;  // 64 (start,end) byte pairs
constexpr uint32_t kMatrixBytes = 0x820ED780u;  // one byte per mapping: dst = b & 7

// A cap on the declared row length so a corrupt +48 cannot run the span arithmetic away. 8 KB of
// floats is already far past the 1 KB rows the mixer actually uses.
constexpr uint32_t kMaxRowFloats = 2048;

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

// rotlwi rX,rY,2 on a zero-extended u16: the byte pitch, and it cannot rotate a bit past 32.
inline uint32_t BytePitch(uint32_t stride) { return __builtin_rotateleft32(stride, 2); }

inline bool IsLegalCount(uint32_t n) {
  return n == 1 || n == 2 || n == 4 || n == 6 || n == 8;
}

REX_FUNC(Native) {
  const uint32_t mixer = ctx.r3.u32;  // mr r30,r3
  const uint32_t pair = ctx.r4.u32;   // mr r31,r4

  // stwu r1,-176(r1) -- reproduced: the two pointer arrays live at r1+80 and r1+112 and are read by
  // sub_82B468C0, which spills r25-r31 and allocates 144 bytes of its own below r1.
  const uint32_t sp = ctx.r1.u32 - kFrameBytes;
  REX_STORE_U32(sp, ctx.r1.u32);  // the back chain; inside this function's own frame
  ctx.r1.u32 = sp;

  const uint32_t length = REX_LOAD_U32(pair + kPairLength);  // lwz r7,48(r4)
  const uint32_t old_count = REX_LOAD_U8(pair + kPairCount);  // lbz r6,60(r4)

  if (length == 0) {  // cmplwi cr6,r7,0 ; bne
    REX_STORE_U8(mixer + kMirroredCount, uint8_t(old_count));  // stb r6,41(r3)
  }

  // loc_82B2C920
  const uint32_t new_count = REX_LOAD_U8(mixer + kChannels);  // lbz r5,42(r30)
  if (old_count != new_count) {  // cmplw cr6,r6,r5 ; beq -- nothing to do when they agree
    if (length != 0) {           // cmplwi cr6,r7,0 ; beq -- skip straight to the swap
      const uint32_t back = REX_LOAD_U32(pair + kPairBack);    // lwz r11,28(r31)
      const uint32_t front = REX_LOAD_U32(pair + kPairFront);  // lwz r4,32(r31)
      // cmplwi cr6,r6,8 ; ble ; li r10,8 -- UNSIGNED, so the source list is clamped to eight
      const uint32_t sources = old_count > 8 ? 8u : old_count;

      if (sources != 0) {  // cmplwi cr6,r10,0 ; beq -- mtctr with 0 would run 2^32 times
        const uint64_t pitch = BytePitch(REX_LOAD_U16(back + kRowStride));  // lhz r9,14(r11)
        uint64_t row = REX_LOAD_U32(back + kRowBase);                       // lwz r11,4(r11)
        for (uint32_t i = 0; i < sources; i++) {  // loc_82B2C96C, bdnz
          REX_STORE_U32(sp + kSourceArray + 4 * i, uint32_t(row));  // stwu r11,4(r10)
          row += pitch;  // add r11,r9,r11 -- 64-bit; only the store truncates
        }
      }

      // loc_82B2C978 -- guarded on the count read before the loops, counted by the one read here
      if (new_count != 0) {  // cmplwi cr6,r5,0 ; beq
        const uint32_t trips = REX_LOAD_U8(mixer + kChannels);                // lbz r8,42(r30)
        const uint64_t pitch = BytePitch(REX_LOAD_U16(front + kRowStride));   // lhz r3,14(r4)
        uint64_t row = REX_LOAD_U32(front + kRowBase);                        // lwz r11,4(r4)
        for (uint32_t i = 0; i < trips; i++) {  // loc_82B2C998, bdnz
          REX_STORE_U32(sp + kDestArray + 4 * i, uint32_t(row));  // stwu r11,4(r10)
          row += pitch;                                           // add r11,r9,r11
        }
      }

      // loc_82B2C9A4: addi r4,r1,80 ; addi r3,r1,112 ; bl 0x82b468c0. r5, r6 and r7 still hold the
      // new count, the old count and the row length from entry.
      skate3::audio::GuestCall(ctx, base, sub_82B468C0, ctx.r1.u64 + kDestArray,
                               ctx.r1.u64 + kSourceArray, uint64_t(new_count),
                               uint64_t(old_count), uint64_t(length));
    }

    // loc_82B2C9B0 -- both owners reloaded, then exchanged.
    const uint32_t front_now = REX_LOAD_U32(pair + kPairFront);  // lwz r11,32(r31)
    const uint32_t back_now = REX_LOAD_U32(pair + kPairBack);    // lwz r10,28(r31)
    REX_STORE_U32(pair + kPairBack, front_now);                  // stw r11,28(r31)
    REX_STORE_U32(pair + kPairFront, back_now);                  // stw r10,32(r31)
    // lbz r9,42(r30) -- reloaded a third time
    REX_STORE_U8(pair + kPairCount, uint8_t(REX_LOAD_U8(mixer + kChannels)));  // stb r9,60(r31)
  }

  // loc_82B2C9C8
  ctx.r3.s64 = 1;                         // li r3,1
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,176 -- a 64-bit add, as in the lifted form
}

// sub_82B426D0 picks its destination as `byte & 7` out of the mapping table for (old -> new), so the
// only thing that could put a write outside the declared rows is a table byte naming a destination
// the frame array never filled. The range is at most 256 bytes; walk it and refuse if one does.
bool MappingStaysInBounds(uint8_t* base, uint32_t old_count, uint32_t new_count) {
  const uint32_t slot = kMatrixIndex + 2u * ((8u * old_count + new_count) - 9u);
  const uint32_t first = REX_LOAD_U8(slot + 0);
  const uint32_t last = REX_LOAD_U8(slot + 1);
  for (uint32_t i = first; i <= last && i < 256u; i++) {
    if ((REX_LOAD_U8(kMatrixBytes + i) & 7u) >= new_count) return false;
  }
  return true;
}

// Every byte the call writes, from entry state. Both arms of the early tests are predicted, since
// the pair's length and count and the mixer's count are all plain fields read before any store.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t mixer = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  if (mixer == 0 || pair == 0) return false;  // the original would load through null

  const uint32_t length = REX_LOAD_U32(pair + kPairLength);
  const uint32_t old_count = REX_LOAD_U8(pair + kPairCount);
  const uint32_t new_count = REX_LOAD_U8(mixer + kChannels);
  spec.read(pair + kPairBack, 8);
  spec.read(pair + kPairLength, 4);
  spec.read(pair + kPairCount, 1);
  spec.read(mixer + kChannels, 1);

  if (length == 0) spec.write(mixer + kMirroredCount, 1);  // stb r6,41(r3)
  if (old_count == new_count) return true;                 // beq cr6,0x82b2c9c8 -- nothing else
  spec.write(pair + kPairBack, 8);  // stw r11,28(r31) ; stw r10,32(r31)
  spec.write(pair + kPairCount, 1); // stb r9,60(r31)
  if (length == 0) return true;     // beq cr6,0x82b2c9b0 -- no rows are touched

  // Past eight entries the arrays run off the frame scratch the loops filled, and the addresses
  // stored through would not be entry state.
  if (new_count > kArrayWords || old_count > kArrayWords) return false;
  if (new_count == 0) return true;  // nothing was written into the destination array
  if (length > kMaxRowFloats) return false;
  if (IsLegalCount(new_count) && IsLegalCount(old_count) &&
      !MappingStaysInBounds(base, old_count, new_count)) {
    return false;
  }

  const uint32_t front = REX_LOAD_U32(pair + kPairFront);
  const uint32_t back = REX_LOAD_U32(pair + kPairBack);
  if (front == 0) return false;  // the row loop would build its pointers off null
  spec.read(front + kRowBase, 4);
  spec.read(front + kRowStride, 2);
  if (back != 0) {
    spec.read(back + kRowBase, 4);
    spec.read(back + kRowStride, 2);
  }

  // sub_82B3BED8, sub_82B44B20 and sub_82EE5E80 each write exactly [dst, dst + 4*length) -- the
  // vector paths need length % 64 == 0 AND a 128-byte-aligned destination, and then their dcbzl and
  // stvx128 stores land inside that same interval. One span per destination row.
  const uint32_t row_bytes = 4u * length;
  const uint64_t pitch = BytePitch(REX_LOAD_U16(front + kRowStride));
  uint64_t row = REX_LOAD_U32(front + kRowBase);
  uint32_t rows[kArrayWords];
  for (uint32_t i = 0; i < new_count; i++) {
    rows[i] = uint32_t(row);
    row += pitch;
  }

  // The owner pair and the mixer's count are re-read after the remap. If a row landed on either,
  // neither the swap's values nor the recorded count would derive from entry state -- gate 2.
  for (uint32_t i = 0; i < new_count; i++) {
    if (Overlaps(rows[i], row_bytes, pair + kPairBack, 8)) return false;
    if (Overlaps(rows[i], row_bytes, mixer + kChannels, 1)) return false;
    spec.write(rows[i], row_bytes);
  }
  return true;
}

}  // namespace port_82B2C8F0
SKATE3_PORT(82B2C8F0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
