// sub_82B472C0  role: walk a table of fixed 16-byte seek records {offset, base, length, mode} until the record containing a target position is found, and publish it into the object -- the fixed-record twin of sub_82B474B8, which walks four run-length streams instead   lifted: skate3_recomp.68.cpp:33901 (269 lines)
// STATUS: pending -- written 2026-09-13, not yet compared under the shadow harness
// calls/session: boot ?  play ?  map ?          thread: not in docs/audio-executed-set.txt; reached from sub_82B471D8
// note: outside the 216 -- ported because sub_82B471D8 dispatches to it, and sub_82B470D0 and sub_82B33780 above that
namespace port_82B472C0 {

// The object's fields are port_82B474B8's publish set, the same offsets on the same structure.
constexpr uint32_t kPublishBase = 4;
constexpr uint32_t kPublishPosition = 8;
constexpr uint32_t kPublishSkip = 12;
constexpr uint32_t kPublishLength = 16;
constexpr uint32_t kPublishOffset = 20;
constexpr uint32_t kMinLength = 28;   // read ONCE here, where sub_82B474B8 reloads it
constexpr uint32_t kPublishExact = 32;
constexpr uint32_t kRecordBytes = 16;
// A bound for the window builder's walk only; the body walks as far as the original does.
constexpr uint32_t kMaxRecords = 2048;

// Four lbz into the red zone at r1-144.. and one lwz back: a big-endian word read byte by byte, so
// an unaligned record reads the same as an aligned one. The red-zone stores are this function's own
// scratch and are not reproduced.
inline uint32_t RecordWord(uint8_t* base, uint32_t at) {
  const uint32_t b0 = REX_LOAD_U8(at + 0);
  const uint32_t b1 = REX_LOAD_U8(at + 1);
  const uint32_t b2 = REX_LOAD_U8(at + 2);
  const uint32_t b3 = REX_LOAD_U8(at + 3);
  return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

struct Record {
  uint64_t offset;  // +0, summed into the published +20
  uint64_t base;    // +4, summed into the published +4 when non-zero
  uint64_t length;  // +8, the step; negative ends the walk
  uint64_t mode;    // +12, 1 publishes every record
};

inline Record LoadRecord(uint8_t* base, uint32_t at) {
  return {RecordWord(base, at), RecordWord(base, at + 4), RecordWord(base, at + 8),
          RecordWord(base, at + 12)};
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r9,r3
  const uint32_t records = ctx.r4.u32;
  const uint64_t target = ctx.r5.u64;
  // __savegprlr_17 spills r17-r31; the body keeps everything in locals and never writes them.
  const uint64_t min_length = REX_LOAD_U32(object + kMinLength);  // lwz r30,28(r9)
  // subf ; subfic ; rlwinm ; addme ; and -- the span when it is a positive int32, else zero.
  const uint64_t span = target - min_length;
  const uint64_t carry = static_cast<uint32_t>(span) == 0 ? 1u : 0u;
  const uint64_t sign = (static_cast<uint32_t>(span) >> 31) & 1u;
  const uint64_t limit = (sign + carry - 1) & span;
  const uint64_t base_entry = REX_LOAD_U32(object + kPublishBase);  // lwz r27,4(r9)
  Record rec = LoadRecord(base, records);
  int64_t result = 1;  // li r3,1
  // cmpwi cr6,r31,0 ; blt cr6,0x82b474b0 -- a negative first length returns 1 at once.
  if (static_cast<int32_t>(static_cast<uint32_t>(rec.length)) >= 0) {
    uint64_t position = 0, sum_a = 0, sum_b = 0;  // r10, r7, r6
    uint32_t cursor = records;
    for (;;) {  // loc_82B4738C
      const bool in_range =
          static_cast<int32_t>(static_cast<uint32_t>(position)) <=
              static_cast<int32_t>(static_cast<uint32_t>(limit)) &&
          static_cast<int32_t>(static_cast<uint32_t>(limit)) <
              static_cast<int32_t>(static_cast<uint32_t>(rec.length + position));
      if (in_range || static_cast<int32_t>(static_cast<uint32_t>(rec.mode)) == 1) {
        // loc_82B473B0
        if (static_cast<int32_t>(static_cast<uint32_t>(rec.base)) == 0) {
          REX_STORE_U32(object + kPublishBase, 0);                                   // stw r26,4(r9)
        } else {
          REX_STORE_U32(object + kPublishBase, static_cast<uint32_t>(sum_b + base_entry));  // add ; stw
        }
        const uint64_t remaining = target - position;                               // subf r8,r10,r5
        REX_STORE_U32(object + kPublishPosition, static_cast<uint32_t>(position));  // stw r10,8(r9)
        const uint64_t length = static_cast<int32_t>(static_cast<uint32_t>(remaining)) <
                                        static_cast<int32_t>(static_cast<uint32_t>(min_length))
                                    ? remaining
                                    : min_length;                                   // cmpw ; blt ; mr
        const uint32_t mode_less_one = static_cast<uint32_t>(rec.mode - 1);         // addi r4,r4,-1
        REX_STORE_U32(object + kPublishLength, static_cast<uint32_t>(length));      // stw r8,16(r9)
        const uint64_t tail = target - length;                                      // subf r8,r8,r5
        REX_STORE_U32(object + kPublishOffset, static_cast<uint32_t>(sum_a));       // stw r7,20(r9)
        const uint32_t leading = mode_less_one == 0 ? 32u : uint32_t(__builtin_clz(mode_less_one));
        REX_STORE_U32(object + kPublishSkip, static_cast<uint32_t>(tail - position));  // stw r8,12(r9)
        REX_STORE_U8(object + kPublishExact, static_cast<uint8_t>((leading >> 5) & 1u));  // stb r4,32(r9)
      }
      // loc_82B47400
      position = rec.length + position;  // add r10,r31,r10
      if (static_cast<int32_t>(static_cast<uint32_t>(target)) <
          static_cast<int32_t>(static_cast<uint32_t>(position))) {
        result = 0;  // loc_82B474AC: mr r3,r26
        break;
      }
      sum_b = rec.base + sum_b;    // add r6,r29,r6
      sum_a = rec.offset + sum_a;  // lwz r8,-144(r1) ; add r7,r8,r7 -- the current record's word
      cursor += kRecordBytes;      // addi r11,r11,16
      rec = LoadRecord(base, cursor);
      if (static_cast<int32_t>(static_cast<uint32_t>(rec.length)) < 0) break;  // bge loops, else 1
    }
  }
  ctx.r3.s64 = result;
}

// The walk's length depends only on each record's length word and the target, so it can be run
// here from entry state -- unless the records overlap the words this call publishes, which the body
// stores before it reads the next record. Those calls are refused.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t records = ctx.r4.u32;
  const uint64_t target = ctx.r5.u64;
  if (object == 0 || records == 0) return false;
  uint32_t count = 1;
  uint64_t step = RecordWord(base, records + 8);
  if (static_cast<int32_t>(static_cast<uint32_t>(step)) >= 0) {
    uint64_t position = 0;
    for (;;) {
      position = step + position;
      if (static_cast<int32_t>(static_cast<uint32_t>(target)) <
          static_cast<int32_t>(static_cast<uint32_t>(position))) {
        break;
      }
      if (++count > kMaxRecords) return false;
      step = RecordWord(base, records + kRecordBytes * (count - 1) + 8);
      if (static_cast<int32_t>(static_cast<uint32_t>(step)) < 0) break;
    }
  }
  const uint64_t read_bytes = uint64_t(kRecordBytes) * count;
  if (uint64_t(records) + read_bytes > 0x100000000ull) return false;
  const uint64_t lo = records, hi = uint64_t(records) + read_bytes;
  if (lo < uint64_t(object) + kPublishExact + 1 && uint64_t(object) + kPublishBase < hi) return false;
  spec.write(object + kPublishBase, kPublishOffset + 4 - kPublishBase);  // +4 .. +23
  spec.write(object + kPublishExact, 1);
  spec.read(object + kPublishBase, 4);
  spec.read(object + kMinLength, 4);
  spec.read(records, static_cast<uint32_t>(read_bytes));
  return true;
}

}  // namespace port_82B472C0
SKATE3_PORT(82B472C0, skate3::audio::kPortPending, skate3::audio::kReturnR3)
