// sub_82B29BE0  role: republish a source's spatial mix -- when any of the ten cached placement parameters moved, recompute the panner entries and the 8x8 gain matrix, then run the channel mix (hard, or ramped out of the previous matrix), and swap the buffer pair   lifted: skate3_recomp.67.cpp:29260 (388 lines)
// STATUS: verified
// calls/session: boot 194135  play 292653  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B29BE0.md
namespace port_82B29BE0 {

// r3 = the mixer-side source object. Nothing in docs/rw_audio_structs.h names it, so the offsets
// stay plain. The three regions this call touches are contiguous and exactly account for
// [+316, +744):
constexpr uint32_t kConfig = 128;       // the listener/format config sub_82B460A0 takes in r3
constexpr uint32_t kEntries = 316;      // 8 panner entries of 16 bytes: {x, y, len2, gain}
constexpr uint32_t kMatrix = 444;       // f32[8][8] gain matrix, row = source, column = dest
constexpr uint32_t kCompared = 700;     // f32  the value stfs'd from +100 on the recompute path
constexpr uint32_t kCached = 704;       // f32[10] the cached copies of the ten live parameters
constexpr uint32_t kTail = 744;         // f32  handed to sub_82B460A0 in f4
constexpr uint32_t kSourceCount = 748;  // u32  source channels; the row count everywhere
constexpr uint32_t kDestCount = 752;    // u32  destination channels; the column count
// 316 .. 744: the entries, the whole matrix, +700 and the ten cached singles, in one span.
constexpr uint32_t kObjectSpan = kTail - kEntries;  // 428

// r4 = the two-entry buffer-descriptor pair, the same shape sub_82B34E08 swaps.
constexpr uint32_t kPairBack = 28;   // rw_ptr  the source descriptor this call mixes FROM
constexpr uint32_t kPairFront = 32;  // rw_ptr  the destination descriptor it mixes INTO

// On a descriptor -- sub_82B29AF0's kBufferBase/kChannelStride, same names, same arithmetic.
constexpr uint32_t kBufferBase = 4;        // u32  channel 0's float array
constexpr uint32_t kChannelStride = 14;    // u16  singles between one channel and the next
constexpr uint32_t kDescriptorBytes = 16;

// The four channel-index words sub_82B454B8 and sub_82B45788 use to place a gain inside a matrix
// row: lwz rN,172(r3) .. 184(r3) with r3 = self + kConfig, so self+300 .. self+312.
constexpr uint32_t kIndexWords = kConfig + 172;  // 300

constexpr uint32_t kFrameBytes = 496;   // stwu r1,-496(r1)
constexpr uint32_t kFrameCopy = 96;     // addi r6,r1,96 -- the saved-matrix buffer's first word
constexpr uint32_t kRowBytes = 32;      // eight singles per source row
constexpr uint32_t kMaxRows = 8;        // the matrix is 8x8; a larger count writes outside it
constexpr uint32_t kBytesPerChannel = 1024;  // 256 singles, both mix kernels
constexpr uint32_t kLiveCount = 10;

// The ten live parameters, and the cached copy each is compared against: parameter i is the single
// at self + 52 + 8i (f31, f29, f28, f27, f26, f25, f30, f24, f23, f22 in the lifted body) and its
// cache is the single at self + 704 + 4i.
constexpr uint32_t kLiveStride = 8;
constexpr uint32_t kLiveFirst = 52;

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

// mullw r10,r10,r31 ; rlwinm r10,r9,2,0,29 ; add r3,r10,r11 -- one channel's float array, the same
// three instructions in sub_82B29AF0 and in sub_82B298E0. mullw is the 64-bit product of two
// SIGN-extended words; the rlwinm is a plain 32-bit shift left by two; the add is 64-bit on two
// zero-extended words, so the sum can carry into bit 32 and only the store truncates it.
inline uint64_t ChannelAddress(uint64_t stride, uint32_t index, uint64_t buffer_base) {
  const int64_t elements = int64_t(int32_t(uint32_t(stride))) * int64_t(int32_t(index));
  const uint64_t byte_offset = static_cast<uint32_t>(static_cast<uint32_t>(elements) << 2);
  return byte_offset + buffer_base;
}

// Is [a, a+alen) disjoint from [b, b+blen)? 64-bit, so a span reaching 2^32 cannot fold around.
inline bool Disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  return a + alen <= b || b + blen <= a;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;       // mr r31,r3
  const uint32_t pair = ctx.r4.u32;       // mr r26,r4
  const uint64_t flag = ctx.r5.u64;       // mr r29,r5 -- only the low byte is ever tested

  // __savegprlr_26 and __savefpr_22 spill r26-r31 and f22-f31 into the red zone and the tail
  // restores them; the four guest calls are why they are spilled at all. The harness compares both
  // ranges, so the entry values have to survive -- locals do the same job without the frame.
  const PPCRegister saved_gpr[6] = {ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const PPCRegister saved_fpr[10] = {ctx.f22, ctx.f23, ctx.f24, ctx.f25, ctx.f26,
                                     ctx.f27, ctx.f28, ctx.f29, ctx.f30, ctx.f31};

  // stwu r1,-496(r1). Reproduced -- most ports skip their own frame -- because the recompute path
  // hands sub_82B298E0 a pointer at r1+96 and that callee's own 432-byte frame would sit on top of
  // the buffer if r1 were left where the caller had it.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;

  ctx.fpscr.disableFlushModeUnconditional();
  // The ten live parameters, held across the whole call in f31/f29/f28/f27/f26/f25/f30/f24/f23/f22.
  double live[kLiveCount];
  for (uint32_t i = 0; i < kLiveCount; i++) {  // lfs f31,52(r3) .. lfs f22,124(r3)
    live[i] = LoadSingle(base, self + kLiveFirst + kLiveStride * i);
  }
  const uint32_t back = REX_LOAD_U32(pair + kPairBack);    // lwz r28,28(r4)
  const uint32_t front = REX_LOAD_U32(pair + kPairFront);  // lwz r27,32(r4)

  // Ten fcmpu/bne pairs against the cache, short-circuiting on the first inequality. An unordered
  // compare (a NaN parameter) clears cr6.eq and so takes the recompute path, which is what C++ `!=`
  // does with a NaN operand.
  bool unchanged = true;
  for (uint32_t i = 0; i < kLiveCount; i++) {  // lfs f0,704+4i(r3) ; fcmpu cr6,fN,f0 ; bne cr6
    if (live[i] != LoadSingle(base, self + kCached + 4 * i)) {
      unchanged = false;
      break;
    }
  }

  const uint32_t entries = self + kEntries;  // addi r30,r3,316
  const uint32_t config = self + kConfig;    // addi r3,r31,128
  const uint32_t matrix = self + kMatrix;    // addi r10,r31,444

  if (unchanged) {
    // clrlwi r11,r5,24 ; beq cr6,0x82b29d04 -- nothing moved, so the panner and the matrix are
    // recomputed only when the flag byte asks for it, and nothing is cached or published.
    if ((flag & 0xFF) != 0) {
      const uint32_t count = REX_LOAD_U32(self + kSourceCount);  // lwz r4,748(r3)
      ctx.f7.f64 = live[9];  // fmr f7,f22
      ctx.f6.f64 = live[8];  // fmr f6,f23
      ctx.f5.f64 = live[7];  // fmr f5,f24
      ctx.f4.f64 = live[3];  // fmr f4,f27
      ctx.f3.f64 = live[2];  // fmr f3,f28
      ctx.f2.f64 = live[1];  // fmr f2,f29
      ctx.f1.f64 = live[0];  // fmr f1,f31
      skate3::audio::GuestCall(ctx, base, sub_82B45C50, entries, count);  // bl 0x82b45c50

      ctx.fpscr.disableFlushModeUnconditional();
      ctx.f4.f64 = LoadSingle(base, self + kTail);  // lfs f4,744(r31)
      ctx.f3.f64 = live[6];                         // fmr f3,f30
      ctx.f2.f64 = live[4];                         // fmr f2,f26
      ctx.f1.f64 = live[5];                         // fmr f1,f25
      // sub_82B460A0 takes the matrix in r10 (mr r27,r10 at its entry), which GuestCall's
      // positional arguments stop short of; set it here rather than clobber r6-r9, which the
      // callee never reads.
      ctx.r10.u64 = matrix;  // addi r10,r31,444
      skate3::audio::GuestCall(ctx, base, sub_82B460A0, config, entries,
                               uint64_t(REX_LOAD_U32(self + kSourceCount)));  // lwz r5,748(r31)
    }
    // loc_82B29D04 -- the hard mix, always, with the descriptors read at entry.
    skate3::audio::GuestCall(ctx, base, sub_82B29AF0, self, front, back);  // bl 0x82b29af0
  } else {
    // loc_82B29D18 -- something moved.
    const uint32_t count = REX_LOAD_U32(self + kSourceCount);  // lwz r4,748(r31)
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, self + kCompared, live[6]);  // stfs f30,700(r31)

    // cmpwi cr6,r4,0 ; ble cr6 -- a signed test, so a negative count skips the copy entirely.
    if (int32_t(count) > 0) {
      // loc_82B29D38 -- save the matrix as it stands into the frame, 32 bytes per source row, for
      // sub_82B298E0 to ramp out of. Seven lfs at +4..+28 then one lfsu at +32, and the stores in
      // the same shape; kept in that order because the two runs could in principle alias.
      uint32_t src = self + kMatrix - 4;  // addi r11,r31,440
      uint32_t dst = frame + kFrameCopy - 4;  // addi r10,r1,92
      uint32_t trips = count;                 // rotlwi r9,r4,0 ; mtctr r9
      do {
        ctx.fpscr.disableFlushModeUnconditional();
        double row[8];
        for (uint32_t k = 0; k < 7; k++) {  // lfs f13,4(r11) .. lfs f7,28(r11)
          row[k] = LoadSingle(base, src + 4 + 4 * k);
        }
        src += kRowBytes;                       // lfsu f0,32(r11)
        row[7] = LoadSingle(base, src);
        for (uint32_t k = 0; k < 7; k++) {      // stfs f13,4(r10) .. stfs f7,28(r10)
          StoreSingle(base, dst + 4 + 4 * k, row[k]);
        }
        dst += kRowBytes;                       // stfsu f0,32(r10)
        StoreSingle(base, dst, row[7]);
      } while (--trips != 0);                   // bdnz 0x82b29d38
    }

    // loc_82B29D7C -- the same two recomputes as the fast path, with r4 still holding the count
    // loaded at the top of this block.
    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f7.f64 = live[9];  // fmr f7,f22
    ctx.f6.f64 = live[8];  // fmr f6,f23
    ctx.f5.f64 = live[7];  // fmr f5,f24
    ctx.f4.f64 = live[3];  // fmr f4,f27
    ctx.f3.f64 = live[2];  // fmr f3,f28
    ctx.f2.f64 = live[1];  // fmr f2,f29
    ctx.f1.f64 = live[0];  // fmr f1,f31
    skate3::audio::GuestCall(ctx, base, sub_82B45C50, entries, count);  // bl 0x82b45c50

    ctx.fpscr.disableFlushModeUnconditional();
    ctx.f4.f64 = LoadSingle(base, self + kTail);  // lfs f4,744(r31)
    ctx.f3.f64 = live[6];                         // fmr f3,f30
    ctx.f2.f64 = live[4];                         // fmr f2,f26
    ctx.f1.f64 = live[5];                         // fmr f1,f25
    ctx.r10.u64 = matrix;                         // addi r10,r31,444
    skate3::audio::GuestCall(ctx, base, sub_82B460A0, config, entries,
                             uint64_t(REX_LOAD_U32(self + kSourceCount)));  // lwz r5,748(r31)

    // clrlwi r11,r29,24 ; beq cr6,0x82b29de8 -- the flag picks the hard mix or the ramped one.
    if ((flag & 0xFF) != 0) {
      skate3::audio::GuestCall(ctx, base, sub_82B29AF0, self, front, back);  // bl 0x82b29af0
    } else {
      // loc_82B29DE8: addi r6,r1,96 -- the saved matrix is the ramp's starting point.
      skate3::audio::GuestCall(ctx, base, sub_82B298E0, self, front, back,
                               uint64_t(frame) + kFrameCopy);  // bl 0x82b298e0
    }

    // loc_82B29DF0 -- publish the ten parameters this recompute was built on.
    ctx.fpscr.disableFlushModeUnconditional();
    for (uint32_t i = 0; i < kLiveCount; i++) {  // stfs f31,704(r31) .. stfs f22,740(r31)
      StoreSingle(base, self + kCached + 4 * i, live[i]);
    }
  }

  // loc_82B29E18 -- swap the pair. Both words are RELOADED here rather than reusing r27/r28.
  const uint32_t swap_front = REX_LOAD_U32(pair + kPairFront);  // lwz r11,32(r26)
  const uint32_t swap_back = REX_LOAD_U32(pair + kPairBack);    // lwz r10,28(r26)
  REX_STORE_U32(pair + kPairBack, swap_front);                  // stw r11,28(r26)
  REX_STORE_U32(pair + kPairFront, swap_back);                  // stw r10,32(r26)
  ctx.r3.s64 = 1;                                               // li r3,1

  ctx.r1.u32 = entry_sp;  // addi r1,r1,496
  ctx.r26 = saved_gpr[0];  // __restgprlr_26
  ctx.r27 = saved_gpr[1];
  ctx.r28 = saved_gpr[2];
  ctx.r29 = saved_gpr[3];
  ctx.r30 = saved_gpr[4];
  ctx.r31 = saved_gpr[5];
  ctx.f22 = saved_fpr[0];  // __restfpr_22
  ctx.f23 = saved_fpr[1];
  ctx.f24 = saved_fpr[2];
  ctx.f25 = saved_fpr[3];
  ctx.f26 = saved_fpr[4];
  ctx.f27 = saved_fpr[5];
  ctx.f28 = saved_fpr[6];
  ctx.f29 = saved_fpr[7];
  ctx.f30 = saved_fpr[8];
  ctx.f31 = saved_fpr[9];
}

// The write set, as the union of every path. Three parts:
//
//   * one 428-byte span on the object, [+316, +744): sub_82B45C50 writes at most panner entries
//     0..6 (16 bytes each, so [+316, +428) -- entry 7 is never reached on any of its seven live
//     count cases); sub_82B460A0 writes only inside the 8x8 matrix [+444, +700); this function
//     writes +700 and the ten cached singles [+704, +744).
//   * the two pair words, [+28, +36).
//   * the destination channels. sub_82B29AF0 goes through sub_82B3BED8/sub_82B44B20, which write
//     exactly [dst, dst+1024); sub_82B298E0 goes through sub_82B3C098/sub_82B44D18, whose dcbzl
//     and stvx128 forms cover [dst & ~127, (dst & ~15) + 1024). The wider of the two is declared,
//     and both consumers walk the same channels of the same descriptor.
//
// Not declared, because it is this function's own frame: the 32-bytes-per-row copy of the matrix at
// r1+96, and sub_82B298E0's per-row deltas at its own r1+80.
//
// Returns false when the counts or the four channel-index words could put a callee's store outside
// the 8x8 matrix, when a channel run would overlap a control word re-read during the call, or when
// the budget cannot hold the channels.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  if (self == 0 || pair == 0) return false;  // the original would store through null

  const uint32_t source_count = REX_LOAD_U32(self + kSourceCount);
  const uint32_t dest_count = REX_LOAD_U32(self + kDestCount);
  const uint32_t dst_desc = REX_LOAD_U32(pair + kPairFront);
  const uint32_t src_desc = REX_LOAD_U32(pair + kPairBack);
  if (dst_desc == 0 || src_desc == 0) return false;  // addressed off null by the mix kernels

  // The matrix has eight rows of eight singles and every callee indexes it by one of these: a row
  // count above 8 (sub_82B460A0's stereo path writes one row per source) or a channel index above 8
  // (sub_82B454B8/sub_82B45788 store at row + 4*index) puts a store outside [+444, +700), where
  // this window set does not follow it.
  if (source_count > kMaxRows || dest_count > kMaxRows) return false;
  for (uint32_t k = 0; k < 4; k++) {
    if (REX_LOAD_U32(self + kIndexWords + 4 * k) >= kMaxRows) return false;
  }

  // Every address and trip count above comes from these words, and each one is re-read while the
  // call runs: the two counts, the four channel-index words, the tail single, and both descriptors'
  // base and stride. If a declared write covered any of them the addresses would stop deriving from
  // entry state -- that is the gate-2 exit, so it is checked rather than assumed.
  const uint32_t control_addr[4] = {self + kIndexWords, self + kTail, dst_desc + kBufferBase,
                                    src_desc + kBufferBase};
  const uint32_t control_len[4] = {16, 12, kDescriptorBytes - kBufferBase,
                                   kDescriptorBytes - kBufferBase};
  for (uint32_t k = 0; k < 4; k++) {
    if (!Disjoint(self + kEntries, kObjectSpan, control_addr[k], control_len[k])) return false;
    if (!Disjoint(pair + kPairBack, 8, control_addr[k], control_len[k])) return false;
  }

  spec.write(self + kEntries, kObjectSpan);  // the entries, the matrix, +700 and the ten singles
  spec.write(pair + kPairBack, 8);           // stw r11,28(r26) ; stw r10,32(r26)

  // The destination channels, merged where they abut so a many-channel destination stays inside the
  // span budget. dest_count is capped at 8 above, so this loop is bounded by construction.
  const uint64_t stride = REX_LOAD_U16(dst_desc + kChannelStride);
  const uint64_t buffer = REX_LOAD_U32(dst_desc + kBufferBase);
  bool open = false;
  uint32_t span_begin = 0;
  uint32_t span_end = 0;
  for (uint32_t dest = 0; dest < dest_count && dest < kMaxRows; dest++) {
    const uint64_t channel = ChannelAddress(stride, dest, buffer);
    if (channel + kBytesPerChannel > 0x100000000ull) return false;  // would wrap 2^32
    const uint32_t begin = uint32_t(channel) & ~127u;               // dcbzl's block
    const uint32_t end = (uint32_t(channel) & ~0xFu) + kBytesPerChannel;
    for (uint32_t k = 0; k < 4; k++) {
      if (!Disjoint(begin, end - begin, control_addr[k], control_len[k])) return false;
    }
    if (!Disjoint(begin, end - begin, self + kEntries, kObjectSpan)) return false;
    if (!Disjoint(begin, end - begin, pair + kPairBack, 8)) return false;
    if (open && begin >= span_begin && begin <= span_end) {
      if (end > span_end) span_end = end;
      continue;
    }
    if (open) spec.write(span_begin, span_end - span_begin);
    span_begin = begin;
    span_end = end;
    open = true;
  }
  if (open) spec.write(span_begin, span_end - span_begin);

  // Reads, recorded for replay only, never compared. The object span from the four channel-index
  // words to the two counts covers everything read off the object outside the write window; the
  // source channels are summarised by one span over their extremes rather than one each.
  spec.read(self + kConfig, kEntries - kConfig);  // the config sub_82B454B8 reads, +172..+184 in it
  spec.read(self + kTail, 12);                    // the tail single and the two counts
  spec.read(dst_desc, kDescriptorBytes);
  spec.read(src_desc, kDescriptorBytes);
  const uint64_t src_stride = REX_LOAD_U16(src_desc + kChannelStride);
  const uint64_t src_buffer = REX_LOAD_U32(src_desc + kBufferBase);
  uint32_t src_lo = uint32_t(src_buffer);
  uint64_t src_hi = uint64_t(src_lo) + kBytesPerChannel;
  for (uint32_t src = 1; src < source_count && src < kMaxRows; src++) {
    const uint32_t begin = uint32_t(ChannelAddress(src_stride, src, src_buffer));
    if (begin < src_lo) src_lo = begin;
    if (uint64_t(begin) + kBytesPerChannel > src_hi) src_hi = uint64_t(begin) + kBytesPerChannel;
  }
  if (src_hi > 0x100000000ull) src_hi = 0x100000000ull;  // a recorded read never runs off the end
  spec.read(src_lo, uint32_t(src_hi - src_lo));
  return true;
}

}  // namespace port_82B29BE0
SKATE3_PORT(82B29BE0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
