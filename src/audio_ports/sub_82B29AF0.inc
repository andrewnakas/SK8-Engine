// sub_82B29AF0  role: apply the channel gain matrix -- mix 256 floats of each source channel into each destination channel at gain[source][dest]; source 0 overwrites (sub_82B3BED8), sources 1.. accumulate (sub_82B44B20)   lifted: skate3_recomp.67.cpp:29116 (143 lines)
// STATUS: verified
// calls/session: boot 245762  play 404782  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B29AF0.md
namespace port_82B29AF0 {

// r3 = the mixer. Nothing in docs/rw_audio_structs.h names it, so the offsets stay plain.
constexpr uint32_t kGainMatrix = 444;      // f32[source][dest]: row `s` at +444 + 32*s, column d
                                           //   at +4*d. addi r29,r3,440 with lfsu's +4 pre-
                                           //   increment is what puts the first gain at +444.
constexpr uint32_t kGainRowStride = 32;    // addi r24,r24,32 -- eight floats per source row
constexpr uint32_t kSourceCount = 748;     // u32  source channels; only > 1 reaches the 2nd pass
constexpr uint32_t kDestCount = 752;       // u32  destination channels; RELOADED every iteration

// r4 = the destination buffer descriptor, r5 = the source one; the same two fields in both.
constexpr uint32_t kBufferBase = 4;        // u32  channel 0's float array
constexpr uint32_t kChannelStride = 14;    // u16  floats between one channel and the next
constexpr uint32_t kDescriptorBytes = 16;  // the span the alias check in Windows() has to cover

constexpr uint32_t kFrameBytes = 160;      // stwu r1,-160(r1)
constexpr uint32_t kSamples = 256;         // li r6,256 at both call sites
constexpr uint32_t kBytesPerChannel = kSamples * 4;
// 32 channels of 1024 bytes is exactly kPortWatchCap, so nothing wider could be compared anyway.
constexpr uint32_t kMaxChannels = skate3::audio::kPortWatchCap / kBytesPerChannel;

// lfsu f1,4(rN): pre-increment the cursor, load one single-precision gain into f1, hand back the
// updated cursor. The fpscr call sits where the lifted line puts it.
inline uint32_t LoadGainUpdate(PPCContext& ctx, uint8_t* base, uint32_t cursor) {
  ctx.fpscr.disableFlushModeUnconditional();
  const uint32_t ea = cursor + 4;
  PPCRegister gain{};
  gain.u32 = REX_LOAD_U32(ea);
  ctx.f1.f64 = double(gain.f32);
  return ea;
}

// mullw r9,r11,r31 ; rlwinm r11,r9,2,0,29 ; add r3,r11,r10 -- one channel's float array.
// mullw is a 64-bit product of two SIGN-extended words. The rlwinm duplicates the product's low
// word into both halves, rotates left 2 and masks 0xFFFFFFFC, which is a plain 32-bit shift left
// by two (the bits it rotates out are masked off). The add is 64-bit on two zero-extended words,
// so the sum can carry into bit 32 and only the callee's address truncation drops it.
inline uint64_t ChannelAddress(uint64_t stride, uint32_t index, uint64_t buffer_base) {
  const int64_t elements = int64_t(int32_t(uint32_t(stride))) * int64_t(int32_t(index));
  const uint64_t byte_offset = static_cast<uint32_t>(static_cast<uint32_t>(elements) << 2);
  return byte_offset + buffer_base;
}

// Is [a, a+alen) disjoint from [b, b+blen)? 64-bit so a span reaching 2^32 cannot fold around.
inline bool Disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  return a + alen <= b || b + blen <= a;
}

REX_FUNC(Native) {
  const uint32_t mixer = ctx.r3.u32;     // mr r28,r3
  const uint32_t dst_desc = ctx.r4.u32;  // mr r30,r4
  const uint32_t src_desc = ctx.r5.u32;  // mr r25,r5

  // stwu r1,-160(r1). Reproduced -- most ports skip their own frame -- because both kernels spill
  // f1 and their callee-saved registers below the LIVE r1 and read that scratch back. Without the
  // frame it would land 160 bytes higher than the original put it.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // r11. Read here and reloaded after every kernel call. The second pass tests whatever the LAST
  // reload left in it, which is why this is one variable carried across both passes and not two.
  uint32_t dest_count = REX_LOAD_U32(mixer + kDestCount);  // lwz r11,752(r3)
  uint64_t source = REX_LOAD_U32(src_desc + kBufferBase);  // lwz r27,4(r5)

  // cmplwi cr6,r11,0 ; beq cr6,0x82b29b54 -- first pass: source channel 0 over every destination.
  if (dest_count != 0) {
    uint32_t gain = mixer + kGainMatrix - 4;  // addi r29,r3,440
    uint32_t dest = 0;                        // li r31,0
    do {                                      // loc_82B29B20
      const uint64_t stride = REX_LOAD_U16(dst_desc + kChannelStride);  // lhz r11,14(r30)
      const uint64_t buffer = REX_LOAD_U32(dst_desc + kBufferBase);     // lwz r10,4(r30)
      gain = LoadGainUpdate(ctx, base, gain);                           // lfsu f1,4(r29)
      const uint64_t destination = ChannelAddress(stride, dest, buffer);
      // bl 0x82b3bed8 -- dst[k] = src[k] * f1 over 256 floats. r5 is handed whatever the previous
      // call left in it, exactly as in the original; the kernel writes r5 before reading it.
      skate3::audio::GuestCall(ctx, base, sub_82B3BED8, destination, source, ctx.r5.u64,
                               uint64_t(kSamples));
      dest_count = REX_LOAD_U32(mixer + kDestCount);  // lwz r11,752(r28)
      dest += 1;                                      // addi r31,r31,1
    } while (dest < dest_count);                      // cmplw cr6,r31,r11 ; blt cr6
  }

  // loc_82B29B54
  uint32_t source_count = REX_LOAD_U32(mixer + kSourceCount);  // lwz r10,748(r28)
  // cmplwi cr6,r10,1 ; ble cr6,0x82b29bd4 -- one source channel means the first pass was all of it
  if (source_count > 1) {
    uint32_t row = mixer + kGainMatrix + kGainRowStride;  // addi r24,r28,476
    uint32_t src = 1;                                     // li r26,1
    do {                                                  // loc_82B29B68
      const uint64_t stride = REX_LOAD_U16(src_desc + kChannelStride);  // lhz r10,14(r25)
      const uint64_t buffer = REX_LOAD_U32(src_desc + kBufferBase);     // lwz r9,4(r25)
      source = ChannelAddress(stride, src, buffer);                     // add r27,r10,r9
      // cmplwi cr6,r11,0 ; beq cr6,0x82b29bc0
      if (dest_count != 0) {
        uint32_t gain = row - 4;  // addi r29,r24,-4
        uint32_t dest = 0;        // li r31,0
        do {                      // loc_82B29B8C
          const uint64_t dst_stride = REX_LOAD_U16(dst_desc + kChannelStride);  // lhz r11,14(r30)
          const uint64_t dst_buffer = REX_LOAD_U32(dst_desc + kBufferBase);     // lwz r10,4(r30)
          gain = LoadGainUpdate(ctx, base, gain);                               // lfsu f1,4(r29)
          const uint64_t destination = ChannelAddress(dst_stride, dest, dst_buffer);
          // bl 0x82b44b20 -- dst[k] += src[k] * f1 over 256 floats.
          skate3::audio::GuestCall(ctx, base, sub_82B44B20, destination, source, ctx.r5.u64,
                                   uint64_t(kSamples));
          dest_count = REX_LOAD_U32(mixer + kDestCount);  // lwz r11,752(r28)
          dest += 1;                                      // addi r31,r31,1
        } while (dest < dest_count);                      // cmplw cr6,r31,r11 ; blt cr6
      }
      // loc_82B29BC0
      source_count = REX_LOAD_U32(mixer + kSourceCount);  // lwz r10,748(r28)
      src += 1;                                           // addi r26,r26,1
      row += kGainRowStride;                              // addi r24,r24,32
    } while (src < source_count);                         // cmplw cr6,r26,r10 ; blt cr6
  }

  ctx.r1.u32 = entry_sp;  // addi r1,r1,160 ; b 0x82f52158 (__restgprlr_24)
}

// The write set is the destination channels, and only those: both kernels write exactly
// [r3, r3 + 4*r6) and nothing else off their own frames (see their own ported Windows()). The
// second pass reuses the same destinations the first pass walked, so one sweep names all of it.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t mixer = ctx.r3.u32;
  const uint32_t dst_desc = ctx.r4.u32;
  const uint32_t src_desc = ctx.r5.u32;

  const uint32_t dest_count = REX_LOAD_U32(mixer + kDestCount);
  const uint32_t source_count = REX_LOAD_U32(mixer + kSourceCount);
  const uint64_t dst_stride = REX_LOAD_U16(dst_desc + kChannelStride);
  const uint64_t dst_buffer = REX_LOAD_U32(dst_desc + kBufferBase);

  // Four words drive the iteration counts and the addresses, and every one of them is RE-READ
  // during the call: +748, +752, and both descriptors' base and stride. If a kernel's output
  // covered any of them, the loop bounds and the channel addresses would not derive from entry
  // state and a later iteration could write outside the declared set -- gate 2 for that call.
  const uint32_t control_addr[3] = {mixer + kSourceCount, dst_desc + kBufferBase,
                                    src_desc + kBufferBase};
  const uint32_t control_len[3] = {8, kDescriptorBytes - kBufferBase,
                                   kDescriptorBytes - kBufferBase};

  if (dest_count > kMaxChannels) return false;  // could not fit the byte budget in any case

  // Merge contiguous or overlapping channels: interleaved buffers coalesce to one span, which is
  // what keeps a many-channel destination inside the 32-span budget.
  bool open = false;
  uint32_t span_begin = 0;
  uint32_t span_end = 0;
  for (uint32_t dest = 0; dest < dest_count; dest++) {
    const uint32_t begin = static_cast<uint32_t>(ChannelAddress(dst_stride, dest, dst_buffer));
    if (uint64_t(begin) + kBytesPerChannel > 0x100000000ull) return false;  // would wrap 2^32
    const uint32_t end = begin + kBytesPerChannel;
    for (int k = 0; k < 3; k++) {
      if (!Disjoint(begin, kBytesPerChannel, control_addr[k], control_len[k])) return false;
    }
    if (open && begin >= span_begin && begin <= span_end) {
      if (end > span_end) span_end = end;
      continue;
    }
    if (open) {
      spec.write(span_begin, span_end - span_begin);
      spec.read(span_begin, span_end - span_begin);  // sub_82B44B20 reads its accumulator
    }
    span_begin = begin;
    span_end = end;
    open = true;
  }
  if (open) {
    spec.write(span_begin, span_end - span_begin);
    spec.read(span_begin, span_end - span_begin);
  }

  // Reads, recorded for replay only. The gain matrix spans row 0 column 0 through the last row's
  // last column; +748/+752 are the two counts. The source channels are summarised by one span
  // over the extremes rather than one per channel, to stay inside the 32-span input table.
  spec.read(mixer + kSourceCount, 8);
  const uint32_t rows = source_count > kMaxChannels ? kMaxChannels : source_count;
  const uint32_t gain_rows = rows > 0 ? rows - 1 : 0;
  spec.read(mixer + kGainMatrix, kGainRowStride * gain_rows + 4 * dest_count);
  spec.read(dst_desc, kDescriptorBytes);
  spec.read(src_desc, kDescriptorBytes);
  const uint64_t src_stride = REX_LOAD_U16(src_desc + kChannelStride);
  const uint64_t src_buffer = REX_LOAD_U32(src_desc + kBufferBase);
  uint32_t src_lo = static_cast<uint32_t>(src_buffer);
  uint64_t src_hi = uint64_t(src_lo) + kBytesPerChannel;
  for (uint32_t src = 1; src < rows; src++) {
    const uint32_t begin = static_cast<uint32_t>(ChannelAddress(src_stride, src, src_buffer));
    if (begin < src_lo) src_lo = begin;
    if (uint64_t(begin) + kBytesPerChannel > src_hi) src_hi = uint64_t(begin) + kBytesPerChannel;
  }
  if (src_hi > 0x100000000ull) src_hi = 0x100000000ull;  // a recorded read never runs off the end
  spec.read(src_lo, static_cast<uint32_t>(src_hi - src_lo));
  return true;
}

}  // namespace port_82B29AF0
SKATE3_PORT(82B29AF0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
