// sub_82B471D8  role: parse a seek header -- a kind nibble to +24, a 16-bit minimum length to +28, a data pointer (or 0) to +4 and the header's end to +0 -- then hand the walk to sub_82B472C0 (mode 0: fixed records) or sub_82B474B8 (mode 1: run-length streams), or return 0   lifted: skate3_recomp.68.cpp:33768 (132 lines)
// STATUS: pending -- written 2026-09-13, not yet compared under the shadow harness
// calls/session: boot ?  play ?  map ?          thread: not in docs/audio-executed-set.txt; reached from sub_82B470D0
// note: outside the 216 -- ported because sub_82B470D0 and sub_82B33780 cannot be ported to Rust without it
namespace port_82B471D8 {

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
constexpr uint32_t kHeaderEnd = 0;     // stw r7,0(r3): the header address plus 12
constexpr uint32_t kData = 4;          // stw r11,4(r3): header + the third word, or 0
constexpr uint32_t kKind = 24;         // stw r7,24(r3): the low nibble of byte 1
constexpr uint32_t kMinLength = 28;    // stw r6,28(r3): bytes 2..3, big-endian
constexpr uint32_t kObjectSpan = 36;   // +0 .. +35: this body's four words and both callees' sets
// sub_82B474B8.inc's own read summary for its streams, mirrored.
constexpr uint32_t kStreamReadSummary = 1024;
// COPIED from sub_82B472C0.inc's window builder (this file is compiled first, so it cannot name
// that port): the fixed-record walk, run on entry state. A change there must be made here too.
constexpr uint32_t kRecordBytes = 16;
constexpr uint32_t kMaxRecords = 2048;

inline uint32_t BigEndianWord(uint8_t* base, uint32_t at) {
  const uint32_t b0 = REX_LOAD_U8(at + 0);
  const uint32_t b1 = REX_LOAD_U8(at + 1);
  const uint32_t b2 = REX_LOAD_U8(at + 2);
  const uint32_t b3 = REX_LOAD_U8(at + 3);
  return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint64_t header = ctx.r4.u64;  // mr r10,r4
  const uint32_t at = ctx.r4.u32;
  // stwu r1,-128(r1). Reproduced: sub_82B474B8 builds its 272-byte frame below this one. The
  // original also assembles its three header words through bytes stored at r1+80/88/96; that is
  // its own frame, so the words are assembled in registers here.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t byte1 = REX_LOAD_U8(at + 1);        // lbz r8,1(r4)
  const uint32_t kind = byte1 & 0xFu;                // clrlwi r7,r8,28
  const uint32_t mode = byte1 >> 4;                  // rlwinm r8,r8,28,4,31
  REX_STORE_U32(object + kKind, kind);               // stw r7,24(r3)
  const uint32_t b2 = REX_LOAD_U8(at + 2);
  const uint32_t b3 = REX_LOAD_U8(at + 3);
  const uint32_t min_length = (b2 << 8) | b3;        // stb 80,81 ; lhz r6,80(r1)
  const uint32_t stream_offset = BigEndianWord(base, at + 4);  // bytes 4..7, lwz r11,88(r1)
  const uint32_t data_offset = BigEndianWord(base, at + 8);    // bytes 8..11, lwz r9,96(r1)
  const uint64_t stream = uint64_t(stream_offset) + header;    // add r4,r11,r10 -- 64-bit
  REX_STORE_U32(object + kMinLength, min_length);              // stw r6,28(r3)
  // add r11,r9,r10 ; cmplwi cr6,r9,0 ; bne ; li r11,0
  const uint64_t data = data_offset == 0 ? 0 : uint64_t(data_offset) + header;
  REX_STORE_U32(object + kData, static_cast<uint32_t>(data));  // stw r11,4(r3)
  REX_STORE_U32(object + kHeaderEnd, static_cast<uint32_t>(header + 12));  // stw r7,0(r3)
  // cmplwi cr6,r8,1 ; blt -> sub_82B472C0 ; beq -> sub_82B474B8 ; else li r3,0. r3 is still the
  // entry object and r5 the entry target; both callees read only r3, r4 and r5.
  if (mode < 1) {
    skate3::audio::GuestCall(ctx, base, sub_82B472C0, ctx.r3.u64, stream, ctx.r5.u64);
  } else if (mode == 1) {
    skate3::audio::GuestCall(ctx, base, sub_82B474B8, ctx.r3.u64, stream, ctx.r5.u64);
  } else {
    ctx.r3.s64 = 0;
  }
  ctx.r1.u32 = entry_sp;  // addi r1,r1,128
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t at = ctx.r4.u32;
  if (object == 0 || at == 0) return false;
  // The header is read once, before the first store, so an overlap with the object cannot change it.
  spec.read(at + 1, 11);
  spec.write(object, kObjectSpan);
  const uint32_t mode = REX_LOAD_U8(at + 1) >> 4;
  const uint32_t stream = static_cast<uint32_t>(uint64_t(BigEndianWord(base, at + 4)) + ctx.r4.u64);
  if (mode == 1) {
    // sub_82B474B8's reads, as its own window builder declares them: a fixed stream summary.
    const uint64_t end = uint64_t(stream) + kStreamReadSummary;
    spec.read(stream, static_cast<uint32_t>((end > 0x100000000ull ? 0x100000000ull : end) - stream));
    return true;
  }
  if (mode != 0) return true;  // li r3,0: nothing past this body's own four words
  const uint64_t target = ctx.r5.u64;
  uint32_t count = 1;
  uint64_t step = BigEndianWord(base, stream + 8);
  if (static_cast<int32_t>(static_cast<uint32_t>(step)) >= 0) {
    uint64_t position = 0;
    for (;;) {
      position = step + position;
      if (static_cast<int32_t>(static_cast<uint32_t>(target)) <
          static_cast<int32_t>(static_cast<uint32_t>(position))) {
        break;
      }
      if (++count > kMaxRecords) return false;
      step = BigEndianWord(base, stream + kRecordBytes * (count - 1) + 8);
      if (static_cast<int32_t>(static_cast<uint32_t>(step)) < 0) break;
    }
  }
  const uint64_t read_bytes = uint64_t(kRecordBytes) * count;
  if (uint64_t(stream) + read_bytes > 0x100000000ull) return false;
  // The records are read after this call's stores and the callee's publishes; refuse an overlap.
  if (uint64_t(stream) < uint64_t(object) + kObjectSpan && uint64_t(object) < uint64_t(stream) + read_bytes) {
    return false;
  }
  spec.read(stream, static_cast<uint32_t>(read_bytes));
  return true;
}

}  // namespace port_82B471D8
SKATE3_PORT(82B471D8, skate3::audio::kPortPending, skate3::audio::kReturnR3)
