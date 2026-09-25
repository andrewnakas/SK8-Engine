// sub_82B34E08  role: flush the mix accumulator -- swap the row pair, copy or clear each channel row, fold the pending deltas, then zero the accumulator   lifted: skate3_recomp.67.cpp:55999 (176 lines)
// STATUS: verified
// calls/session: boot 722892  play 845754  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34E08.md
namespace port_82B34E08 {

// r3 = the mixer-side object, r4 = the two-entry row-owner pair. Nothing in
// docs/rw_audio_structs.h names either, so the offsets stay plain.
constexpr uint32_t kChannels = 42;       // u8      channel count; RELOADED at every use
constexpr uint32_t kAccum = 60;          // rw_ptr  accumulator, kChannels * 1024 bytes
constexpr uint32_t kDeltas = 64;         // float[] sub_82B3C668's delta array (r4 = self + 64)
constexpr uint32_t kPendingOffset = 96;  // u16     byte offset into the accumulator of a flag word
constexpr uint32_t kDeltaFlag = 98;      // u8      non-zero: fold the deltas, then clear this
constexpr uint32_t k165 = 165;           // u8      0xFF forces the flush with nothing else pending

// The pair at r4: +32 is the owner used by THIS call (read before the swap), +28 the other one.
constexpr uint32_t kPairBack = 28;   // rw_ptr
constexpr uint32_t kPairFront = 32;  // rw_ptr

// On the row owner -- the same two fields sub_82B3C668 reads, same names.
constexpr uint32_t kRowBase = 4;      // rw_ptr  row base
constexpr uint32_t kRowStride = 14;   // u16     row pitch in SINGLES; the byte pitch is 4x
constexpr uint32_t kRowBytes = 1024;  // li r5,1024 -- the memcpy/memset length per channel

// A hard bound on the loop trip count, so the window builder's arrays are visibly bounded. The
// count is a u8, and the byte and span budgets cut it further; this only keeps the array honest.
constexpr uint32_t kMaxChannels = 32;

// mullw r8,r11,r30 ; rlwinm r11,r8,2,0,29 -- the 64-bit product of the sign-extended words, then
// the LOW word shifted left two with the bottom two bits cleared. Identical to sub_82B3C668's own
// row arithmetic, which is what makes its rows a subset of the spans declared below.
inline uint32_t RowOffset(uint32_t stride, uint32_t index) {
  const int64_t product = int64_t(int32_t(stride)) * int64_t(int32_t(index));
  return (uint32_t(uint64_t(product)) << 2) & 0xFFFFFFFCu;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  const uint32_t pending_off = REX_LOAD_U16(self + kPendingOffset);  // lhz r27,96(r3)
  const uint32_t accum = REX_LOAD_U32(self + kAccum);                // lwz r26,60(r3)
  const uint32_t pending_addr = pending_off + accum;                 // the lwzx/stwx address

  // lwzx r11,r27,r26 ; bne -- nothing pending, no deltas, and +165 short of 0xFF: return 0
  // without a single store. Every input to this test is entry state.
  if (REX_LOAD_U32(pending_addr) == 0 &&                    // cmplwi cr6,r11,0
      REX_LOAD_U8(self + kDeltaFlag) == 0 &&                // lbz r11,98(r3) ; cmplwi ; bne
      REX_LOAD_U8(self + k165) < 255) {                     // cmplwi cr6,r11,255 ; bge
    ctx.r3.s64 = 0;                                         // li r3,0
    return;
  }

  // loc_82B34E50 -- swap the pair. The FRONT pointer is read before the swap and is the owner the
  // rest of the call uses; the swap then moves it to the back.
  const uint32_t back = REX_LOAD_U32(pair + kPairBack);    // lwz r11,28(r4)
  const uint32_t owner = REX_LOAD_U32(pair + kPairFront);  // lwz r29,32(r4)
  REX_STORE_U32(pair + kPairFront, back);                  // stw r11,32(r4)
  REX_STORE_U32(pair + kPairBack, owner);                  // stw r29,28(r4)

  // Both reloaded after the swap, in this order, before the branch.
  const uint32_t pending = REX_LOAD_U32(pending_addr);       // lwzx r10,r27,r26
  const uint32_t channels = REX_LOAD_U8(self + kChannels);   // lbz r11,42(r31)

  uint32_t channel = 0;  // li r30,0 -- one counter, shared by the two mutually exclusive loops
  if (pending != 0) {    // beq cr6,0x82b34ec4
    if (channels != 0) { // beq cr6,0x82b34eb8
      uint32_t source = 0;  // li r28,0
      do {                  // loc_82B34E80
        const uint32_t stride = REX_LOAD_U16(owner + kRowStride);  // lhz r11,14(r29)
        const uint32_t accum_now = REX_LOAD_U32(self + kAccum);    // lwz r9,60(r31)
        const uint32_t row_base = REX_LOAD_U32(owner + kRowBase);  // lwz r10,4(r29)
        // add r4,r9,r28 and add r3,r11,r10 are 64-bit; the callee addresses with the low words.
        skate3::audio::GuestCall(ctx, base, sub_82EDF460,          // bl 0x82edf460 -- memcpy
                                 uint64_t(RowOffset(stride, channel)) + uint64_t(row_base),
                                 uint64_t(accum_now) + uint64_t(source),
                                 uint64_t(kRowBytes));             // li r5,1024
        channel++;                                                 // addi r30,r30,1
        source += kRowBytes;                                       // addi r28,r28,1024
        // lbz r7,42(r31) ; cmplw cr6,r30,r7 ; blt -- the count is reloaded every iteration
      } while (channel < REX_LOAD_U8(self + kChannels));
    }
    REX_STORE_U32(pending_addr, 0);  // loc_82B34EB8: li r11,0 ; stwx r11,r27,r26
  } else if (channels != 0) {        // loc_82B34EC4: cmplwi cr6,r11,0 ; beq
    do {                             // loc_82B34ECC
      const uint32_t stride = REX_LOAD_U16(owner + kRowStride);  // lhz r11,14(r29)
      const uint32_t row_base = REX_LOAD_U32(owner + kRowBase);  // lwz r10,4(r29)
      skate3::audio::GuestCall(ctx, base, sub_82EE5E80,          // bl 0x82ee5e80 -- memset
                               uint64_t(RowOffset(stride, channel)) + uint64_t(row_base),
                               uint64_t(0),           // li r4,0
                               uint64_t(kRowBytes));  // li r5,1024
      channel++;                                      // addi r30,r30,1
      // lbz r8,42(r31) ; cmplw cr6,r30,r8 ; blt
    } while (channel < REX_LOAD_U8(self + kChannels));
  }

  // loc_82B34EFC -- the flag is reloaded here, after both loops
  if (REX_LOAD_U8(self + kDeltaFlag) != 0) {  // lbz r11,98(r31) ; beq
    // mr r3,r29 ; addi r4,r31,64 ; lbz r5,42(r31) -- sub_82B3C668 is itself a verified port, so it
    // goes through GuestCall on the live context and is exercised by its own hook.
    skate3::audio::GuestCall(ctx, base, sub_82B3C668, uint64_t(owner), uint64_t(self) + kDeltas,
                             uint64_t(REX_LOAD_U8(self + kChannels)));
    REX_STORE_U8(self + kDeltaFlag, 0);  // li r11,0 ; stb r11,98(r31)
  }

  // loc_82B34F20 -- clear the whole accumulator. rotlwi r5,r11,10 on a byte is r11 * 1024: the
  // byte cannot rotate a bit past 32, so the rotate and the shift agree. Both fields reloaded.
  const uint32_t channels_now = REX_LOAD_U8(self + kChannels);  // lbz r11,42(r31)
  const uint32_t accum_now = REX_LOAD_U32(self + kAccum);       // lwz r3,60(r31)
  skate3::audio::GuestCall(ctx, base, sub_82EE5E80, uint64_t(accum_now),
                           uint64_t(0),  // li r4,0
                           uint64_t(__builtin_rotateleft32(channels_now, 10)));
  ctx.r3.s64 = 1;  // li r3,1
}

// Every byte the call writes, from entry state. sub_82EDF460 (memcpy) and sub_82EE5E80 (memset)
// write exactly [dst, dst+n): their dcbz/stvlx/stvrx forms were read through and each one lands
// inside that interval. sub_82B3C668 writes the first 64 bytes of each row plus its delta array,
// which its own verified port already enumerates the same way.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  const uint32_t pending_off = REX_LOAD_U16(self + kPendingOffset);
  const uint32_t accum = REX_LOAD_U32(self + kAccum);
  const uint32_t pending_addr = pending_off + accum;

  spec.read(self + kChannels, 1);
  spec.read(self + kAccum, 4);
  spec.read(self + kPendingOffset, 4);  // the u16 at +96 and the flag at +98
  spec.read(self + k165, 1);
  spec.read(pair + kPairBack, 8);
  spec.read(pending_addr, 4);

  // The early return is decided entirely on entry state -- the pending word is loaded before the
  // swap and both bytes are plain object fields -- so it is predicted rather than papered over
  // with a union. It writes nothing at all.
  if (REX_LOAD_U32(pending_addr) == 0 && REX_LOAD_U8(self + kDeltaFlag) == 0 &&
      REX_LOAD_U8(self + k165) < 255) {
    return true;
  }

  const uint32_t channels = REX_LOAD_U8(self + kChannels);
  const uint32_t owner = REX_LOAD_U32(pair + kPairFront);
  spec.read(owner + kRowBase, 4);
  spec.read(owner + kRowStride, 2);
  spec.read(accum, 1024u * channels);  // the memcpy source, and the closing memset's target

  // Which per-channel loop runs turns on the pending word RELOADED after the swap, and whether
  // sub_82B3C668 runs turns on the flag reloaded after the loops. Neither is predicted here: the
  // union of every path is declared, and a byte this call leaves alone compares equal anyway.
  struct Span {
    uint32_t addr;
    uint32_t len;
  };
  Span spans[skate3::audio::kPortMaxSpans];
  uint32_t span_count = 0;
  uint64_t total = 0;
  auto add = [&](uint32_t addr, uint64_t len) {
    if (len == 0) return true;
    if (len > 0xFFFFFFFFull || uint64_t(addr) + len > 0x100000000ull) return false;  // wraps 2^32
    if (span_count >= skate3::audio::kPortMaxSpans) return false;
    spans[span_count].addr = addr;
    spans[span_count].len = uint32_t(len);
    span_count++;
    total += len;
    return true;
  };

  if (!add(pair + kPairBack, 8)) return false;   // stw r11,32(r4) ; stw r29,28(r4)
  if (!add(pending_addr, 4)) return false;       // stwx r11,r27,r26
  if (!add(self + kDeltaFlag, 1)) return false;  // stb r11,98(r31)

  if (channels != 0) {
    if (channels > kMaxChannels) return false;  // keeps the row loop below visibly bounded
    if (owner == 0) return false;  // the original would address its rows off null; not comparable
    if (!add(self + kDeltas, 4ull * channels)) return false;  // sub_82B3C668 clears one per channel

    // Each channel's row: 1024 bytes on the memcpy path, only the leading 64 on sub_82B3C668's,
    // so the 1 KB span covers both. At the live byte pitch of 1024 the rows abut and merge.
    const uint32_t stride = REX_LOAD_U16(owner + kRowStride);
    const uint32_t row_base = REX_LOAD_U32(owner + kRowBase);
    const uint32_t pitch = RowOffset(stride, 1);  // (stride * 4) & ~3; the u16 cannot overflow it
    if (pitch > kRowBytes) {
      for (uint32_t i = 0; i < channels && i < kMaxChannels; i++) {
        const uint32_t row = uint32_t(uint64_t(RowOffset(stride, i)) + uint64_t(row_base));
        if (!add(row, kRowBytes)) return false;
      }
    } else if (pitch == 0) {
      if (!add(row_base, kRowBytes)) return false;  // every iteration lands on the same 1 KB
    } else {
      const uint64_t last = uint64_t(pitch) * (channels - 1);
      // Guard the monotone assumption: if the index math wrapped, the union is not this interval.
      if (RowOffset(stride, channels - 1) != uint32_t(last)) return false;
      if (!add(row_base, last + kRowBytes)) return false;
    }

    if (!add(accum, 1024ull * channels)) return false;  // the closing memset
  }

  // The four fields re-read inside (and after) the loops must stay outside every write span, or
  // neither the row addresses nor the trip counts derive from entry state: that is the gate-2 exit.
  const Span reread[4] = {{self + kChannels, 1},
                          {self + kAccum, 4},
                          {owner + kRowBase, 4},
                          {owner + kRowStride, 2}};
  for (uint32_t i = 0; i < span_count; i++) {
    for (uint32_t k = 0; k < 4; k++) {
      const Span& f = reread[k];
      if (uint64_t(f.addr) < uint64_t(spans[i].addr) + spans[i].len &&
          uint64_t(spans[i].addr) < uint64_t(f.addr) + f.len) {
        return false;
      }
    }
  }

  if (total > skate3::audio::kPortWatchCap) return false;
  for (uint32_t i = 0; i < span_count; i++) spec.write(spans[i].addr, spans[i].len);
  return true;
}

}  // namespace port_82B34E08
SKATE3_PORT(82B34E08, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
