// sub_82B298E0  role: apply the channel gain matrix with a ramp -- per source row, the per-destination delta (live gain - the caller's saved gain) x a rodata step goes into the frame, then each source channel is mixed into each destination channel starting at the SAVED gain and ramping by that delta; source 0 overwrites (sub_82B3C098), sources 1.. accumulate (sub_82B44D18)   lifted: skate3_recomp.67.cpp:28796 (319 lines)
// STATUS: verified
// calls/session: boot 142508  play 180524  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B298E0.md
namespace port_82B298E0 {

// r3 = the mixer-side source object, the same one sub_82B29AF0 and sub_82B29BE0 take. Nothing in
// docs/rw_audio_structs.h names it, so the offsets stay plain.
constexpr uint32_t kGainMatrix = 444;    // f32[source][dest]: row s at +444 + 32*s, column d at +4*d
constexpr uint32_t kGainRowStride = 32;  // eight singles per source row
constexpr uint32_t kSourceCount = 748;   // u32  source channels
constexpr uint32_t kDestCount = 752;     // u32  destination channels; RELOADED every iteration

// r4 = the destination buffer descriptor, r5 = the source one; the same two fields in both.
constexpr uint32_t kBufferBase = 4;        // u32  channel 0's float array
constexpr uint32_t kChannelStride = 14;    // u16  singles between one channel and the next
constexpr uint32_t kDescriptorBytes = 16;  // the span the alias check in Windows() has to cover

// r6 = the caller's saved copy of the matrix, same layout, 32 bytes per source row. sub_82B29BE0
// hands its own r1+96; it is read here and never written.

constexpr uint32_t kFrameBytes = 432;   // stwu r1,-432(r1)
constexpr uint32_t kDeltaOffset = 80;   // the per-destination deltas, 32 bytes per source row
// The delta block is this function's own frame, so a source count past this overruns the frame into
// the caller's -- the matrix is 8x8 long before that, which is where Windows() draws the line.
constexpr uint32_t kFrameRows = (kFrameBytes - kDeltaOffset) / kGainRowStride;  // 11
constexpr uint32_t kMaxRows = 8;
static_assert(kMaxRows <= kFrameRows, "the delta rows must fit this function's own frame");

constexpr uint32_t kSamples = 256;
constexpr uint32_t kBytesPerChannel = kSamples * 4;
constexpr uint32_t kBlockBytes = 128;  // sub_82B3C098's dcbzl block, which rounds its start down
constexpr uint32_t kMaxChannels = skate3::audio::kPortWatchCap / kBytesPerChannel;

// One rodata single: lis r31,-32208 ; addi r31,r31,-31232 ; lfs f0,480(r31), so
// ((0x8230) << 16) - 31232 + 480. Computed, never read off by eye. The value is not needed: the
// body loads it through the guest map. (Inference, unverified: 1/64, the ramp length both kernels
// use -- see the note.)
constexpr uint32_t kRampStep = 0x822F87E0;

// The rodata sub_82B3C098 and sub_82B44D18 read, copied from their ported Windows() so the recorded
// vector replays without the image: recomp/src/audio_ports/sub_82B3C098.inc (same TU, but a HIGHER
// address, so the constants cannot be referenced from here -- the aggregator includes in address
// order). If either kernel's rodata set changes, this list has to change with it.
constexpr uint32_t kKernelStepScale = 0x82257308;
constexpr uint32_t kKernelRampSpan = 0x820ED958;
constexpr uint32_t kKernelLane2 = 0x82060C50;
constexpr uint32_t kKernelLane3 = 0x82063B08;
constexpr uint32_t kKernelPoolLow = 0x8231BA20;   // four 16-byte multipliers
constexpr uint32_t kKernelPoolHigh = 0x8231BA60;  // four more

// lfs / lfsu / lfsx: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs / stfsu / stfsx: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fsubs / fmuls: the double operation rounded to single, as the lifted line does it.
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }

// mullw r9,r10,r31 ; rlwinm r10,r9,2,0,29 ; add r3,r10,r11 -- one channel's float array, the same
// three instructions as in sub_82B29AF0. mullw is a 64-bit product of two SIGN-extended words; the
// rlwinm is a plain 32-bit shift left by two; the add is 64-bit on two zero-extended words, so the
// sum can carry into bit 32 and only the callee's address truncation drops it.
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
  const uint32_t mixer = ctx.r3.u32;     // mr r27,r3
  const uint32_t dst_desc = ctx.r4.u32;  // mr r26,r4
  const uint32_t src_desc = ctx.r5.u32;  // mr r22,r5
  const uint32_t saved_base = ctx.r6.u32;  // mr r25,r6

  // __savegprlr_22 spills r22-r31 and the tail restores them; the harness compares that range, so
  // the entry values have to survive. Locals do the same job without the spill slots.
  const PPCRegister saved_gpr[10] = {ctx.r22, ctx.r23, ctx.r24, ctx.r25, ctx.r26,
                                     ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};

  // stwu r1,-432(r1). Reproduced -- most ports skip their own frame -- because the deltas live in
  // it at +80 and both mix kernels spill below the LIVE r1 and read that scratch back.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;
  const uint32_t deltas = frame + kDeltaOffset;  // addi r11,r1,84 .. addi r3,r1,100

  const uint32_t source_first = REX_LOAD_U32(mixer + kSourceCount);  // lwz r10,748(r3)

  // cmpwi cr6,r10,0 ; ble cr6,0x82b29a00 -- a SIGNED test here, unlike the two unsigned loop
  // bounds below: a negative count computes no deltas at all.
  if (static_cast<int32_t>(source_first) > 0) {
    ctx.fpscr.disableFlushModeUnconditional();
    const double step = LoadSingle(base, kRampStep);  // lfs f0,480(r31)
    uint32_t live = mixer + kGainMatrix - 4;          // addi r10,r27,440
    uint32_t saved = saved_base + 8;                  // addi r11,r25,8
    uint32_t delta = deltas - kGainRowStride + 4;     // addi r9,r11,-32 (r11 was r1+84)
    uint32_t trips = source_first;                    // mtctr r10
    // loc_82B29958. Every one of the sixteen loads precedes every one of the eight stores in the
    // lifted body, so grouping them here does not move a load across a store. The columns are
    // differenced in the order 2, 3, 0, 5, 4, 6, 7, 1 and stored in the order 0, 2, 3, 5, 4, 6, 7,
    // 1; both orders are kept.
    do {
      ctx.fpscr.disableFlushModeUnconditional();
      const double live2 = LoadSingle(base, live + 12);   // lfs f11,12(r10)
      const double saved2 = LoadSingle(base, saved + 0);  // lfs f5,0(r11)
      const double live3 = LoadSingle(base, live + 16);   // lfs f10,16(r10)
      const double d2 = SubSingle(live2, saved2);         // fsubs f2,f11,f5
      const double saved3 = LoadSingle(base, saved + 4);  // lfs f3,4(r11)
      const double saved0 = LoadSingle(base, saved - 8);  // lfs f6,-8(r11)
      const double d3 = SubSingle(live3, saved3);         // fsubs f11,f10,f3
      const double live0 = LoadSingle(base, live + 4);    // lfs f13,4(r10)
      const double live5 = LoadSingle(base, live + 24);   // lfs f8,24(r10)
      const double d0 = SubSingle(live0, saved0);         // fsubs f4,f13,f6
      const double saved5 = LoadSingle(base, saved + 12);  // lfs f10,12(r11)
      const double live1 = LoadSingle(base, live + 8);    // lfs f12,8(r10)
      const double d5 = SubSingle(live5, saved5);         // fsubs f5,f8,f10
      const double live6 = LoadSingle(base, live + 28);   // lfs f7,28(r10)
      const double saved4 = LoadSingle(base, saved + 8);  // lfs f1,8(r11)
      const double live4 = LoadSingle(base, live + 20);   // lfs f9,20(r10)
      const double saved6 = LoadSingle(base, saved + 16);  // lfs f6,16(r11)
      const double d4 = SubSingle(live4, saved4);         // fsubs f9,f9,f1
      const double saved7 = LoadSingle(base, saved + 20);  // lfs f3,20(r11)
      const double d6 = SubSingle(live6, saved6);         // fsubs f1,f7,f6
      live += kGainRowStride;                             // lfsu f13,32(r10)
      const double live7 = LoadSingle(base, live);
      const double saved1 = LoadSingle(base, saved - 4);  // lfs f10,-4(r11)
      const double d7 = SubSingle(live7, saved7);         // fsubs f8,f13,f3
      const double d1 = SubSingle(live1, saved1);         // fsubs f7,f12,f10

      // The six stfsx addresses are `saved + (frame + 80 + 4c - saved_base)`, built by the six
      // `subf rN,r25,rN` above. Both the subtract and the add are 64-bit on zero-extended words and
      // only the store's u32 truncation is observed, so the frame-relative form below is exact.
      StoreSingle(base, delta + 28, MulSingle(d0, step));  // fmuls f6,f4,f0  ; stfs f6,28(r9)
      StoreSingle(base, delta + 36, MulSingle(d2, step));  // fmuls f4,f2,f0  ; stfsx f4,r11,r8
      StoreSingle(base, delta + 40, MulSingle(d3, step));  // fmuls f3,f11,f0 ; stfsx f3,r11,r7
      StoreSingle(base, delta + 48, MulSingle(d5, step));  // fmuls f13,f5,f0 ; stfsx f13,r11,r5
      StoreSingle(base, delta + 44, MulSingle(d4, step));  // fmuls f2,f9,f0  ; stfsx f2,r11,r6
      StoreSingle(base, delta + 52, MulSingle(d6, step));  // fmuls f12,f1,f0 ; stfsx f12,r11,r4
      StoreSingle(base, delta + 56, MulSingle(d7, step));  // fmuls f11,f8,f0 ; stfsx f11,r11,r3
      delta += kGainRowStride;                             // fmuls f10,f7,f0 ; stfsu f10,32(r9)
      StoreSingle(base, delta, MulSingle(d1, step));
      saved += kGainRowStride;                             // addi r11,r11,32
    } while (--trips != 0);                                // bdnz 0x82b29958
  }

  // loc_82B29A00. r11: read here and reloaded after every kernel call. The second pass tests
  // whatever the LAST reload left in it, which is why this is one variable carried across both
  // passes and not two -- the same shape as sub_82B29AF0.
  uint32_t dest_count = REX_LOAD_U32(mixer + kDestCount);        // lwz r11,752(r27)
  uint64_t source = REX_LOAD_U32(src_desc + kBufferBase);        // lwz r28,4(r22)

  // cmplwi cr6,r11,0 ; beq cr6,0x82b29a58 -- first pass: source channel 0 over every destination,
  // at the saved gain, ramping by row 0's deltas. Note the source address is channel 0's base with
  // no stride multiply, and it is not reloaded inside the loop.
  if (dest_count != 0) {
    uint32_t gain = saved_base;   // mr r30,r25
    uint32_t dest = 0;            // li r31,0
    do {                          // loc_82B29A20
      const uint64_t stride = REX_LOAD_U16(dst_desc + kChannelStride);  // lhz r10,14(r26)
      const uint64_t buffer = REX_LOAD_U32(dst_desc + kBufferBase);     // lwz r11,4(r26)
      ctx.fpscr.disableFlushModeUnconditional();
      // lfsx f2,r30,r29 with r29 = (r1+80) - r25, so the delta slot matching this gain slot
      ctx.f2.f64 = LoadSingle(base, gain + (deltas - saved_base));
      ctx.f1.f64 = LoadSingle(base, gain);                              // lfs f1,0(r30)
      const uint64_t destination = ChannelAddress(stride, dest, buffer);
      // bl 0x82b3c098 -- dst[k] = src[k] * gain over 256 singles, gain ramping by f2
      skate3::audio::GuestCall(ctx, base, sub_82B3C098, destination, source);
      dest_count = REX_LOAD_U32(mixer + kDestCount);  // lwz r11,752(r27)
      dest += 1;                                      // addi r31,r31,1
      gain += 4;                                      // addi r30,r30,4
    } while (dest < dest_count);                      // cmplw cr6,r31,r11 ; blt cr6
  }

  // loc_82B29A58
  uint32_t source_count = REX_LOAD_U32(mixer + kSourceCount);  // lwz r10,748(r27)
  // cmplwi cr6,r10,1 ; ble cr6,0x82b29ae4 -- one source channel means the first pass was all of it
  if (source_count > 1) {
    uint32_t row = saved_base + kGainRowStride;  // addi r23,r25,32
    uint32_t src = 1;                            // li r24,1
    do {                                         // loc_82B29A6C
      const uint64_t stride = REX_LOAD_U16(src_desc + kChannelStride);  // lhz r9,14(r22)
      const uint64_t buffer = REX_LOAD_U32(src_desc + kBufferBase);     // lwz r10,4(r22)
      source = ChannelAddress(stride, src, buffer);                     // add r28,r9,r10
      // cmplwi cr6,r11,0 ; beq cr6,0x82b29ad0 -- the carried destination count, not a fresh read
      if (dest_count != 0) {
        uint32_t gain = row;  // mr r30,r23
        uint32_t dest = 0;    // li r31,0
        do {                  // loc_82B29A98
          const uint64_t dst_stride = REX_LOAD_U16(dst_desc + kChannelStride);  // lhz r10,14(r26)
          const uint64_t dst_buffer = REX_LOAD_U32(dst_desc + kBufferBase);     // lwz r11,4(r26)
          ctx.fpscr.disableFlushModeUnconditional();
          ctx.f2.f64 = LoadSingle(base, gain + (deltas - saved_base));  // lfsx f2,r30,r29
          ctx.f1.f64 = LoadSingle(base, gain);                          // lfs f1,0(r30)
          const uint64_t destination = ChannelAddress(dst_stride, dest, dst_buffer);
          // bl 0x82b44d18 -- dst[k] += src[k] * gain over 256 singles, gain ramping by f2
          skate3::audio::GuestCall(ctx, base, sub_82B44D18, destination, source);
          dest_count = REX_LOAD_U32(mixer + kDestCount);  // lwz r11,752(r27)
          dest += 1;                                      // addi r31,r31,1
          gain += 4;                                      // addi r30,r30,4
        } while (dest < dest_count);                      // cmplw cr6,r31,r11 ; blt cr6
      }
      // loc_82B29AD0
      source_count = REX_LOAD_U32(mixer + kSourceCount);  // lwz r10,748(r27)
      src += 1;                                           // addi r24,r24,1
      row += kGainRowStride;                              // addi r23,r23,32
    } while (src < source_count);                         // cmplw cr6,r24,r10 ; blt cr6
  }

  // loc_82B29AE4: addi r1,r1,432 ; b 0x82f52150 (__restgprlr_22)
  ctx.r1.u32 = entry_sp;
  ctx.r22 = saved_gpr[0];
  ctx.r23 = saved_gpr[1];
  ctx.r24 = saved_gpr[2];
  ctx.r25 = saved_gpr[3];
  ctx.r26 = saved_gpr[4];
  ctx.r27 = saved_gpr[5];
  ctx.r28 = saved_gpr[6];
  ctx.r29 = saved_gpr[7];
  ctx.r30 = saved_gpr[8];
  ctx.r31 = saved_gpr[9];
}

// The write set is the destination channels, and only those: the eight deltas per source row go into
// this function's own 432-byte frame (at +80, which is why the frame IS reproduced), and neither
// kernel writes anything off its own frame. Per destination channel the two kernels together cover
//
//   sub_82B3C098:  [dst & ~127, (dst & ~15) + 1024)   -- dcbzl rounds the start down to its block
//   sub_82B44D18:  [dst & ~15,  (dst & ~15) + 1024)
//
// exactly as their own ported Windows() declare (recomp/src/audio_ports/sub_82B3C098.inc and
// sub_82B44D18.inc -- if either widens, this must widen too). The wider form is declared, since the
// first pass always runs the dcbzl form when there is any destination at all.
//
// Returns false when the source count would overrun the 8x8 matrix (the frame holds only 11 rows of
// deltas in any case), when a channel run would wrap 2^32, when it would cover one of the words the
// loops re-read for their bounds and addresses (+748, +752, both descriptors' base and stride), or
// when it would cover this function's own frame, where the deltas it is about to read live.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t mixer = ctx.r3.u32;
  const uint32_t dst_desc = ctx.r4.u32;
  const uint32_t src_desc = ctx.r5.u32;
  const uint32_t saved_base = ctx.r6.u32;
  const uint32_t frame = ctx.r1.u32 - kFrameBytes;

  const uint32_t dest_count = REX_LOAD_U32(mixer + kDestCount);
  const uint32_t source_count = REX_LOAD_U32(mixer + kSourceCount);
  const uint64_t dst_stride = REX_LOAD_U16(dst_desc + kChannelStride);
  const uint64_t dst_buffer = REX_LOAD_U32(dst_desc + kBufferBase);

  if (source_count > kMaxRows) return false;    // past the matrix, and then past the frame
  if (dest_count > kMaxChannels) return false;  // could not fit the byte budget in any case

  // The words that drive the loop counts and the channel addresses, every one RE-READ during the
  // call. A kernel's output covering any of them would stop the addresses deriving from entry
  // state, and a later iteration could then write outside the declared set -- gate 2 for that call.
  const uint32_t control_addr[4] = {mixer + kSourceCount, dst_desc + kBufferBase,
                                    src_desc + kBufferBase, frame};
  const uint32_t control_len[4] = {8, kDescriptorBytes - kBufferBase,
                                   kDescriptorBytes - kBufferBase, kFrameBytes};

  // Merge contiguous or overlapping channels: an interleaved destination coalesces to one span,
  // which is what keeps a many-channel destination inside the 32-span budget.
  bool open = false;
  uint32_t span_begin = 0;
  uint32_t span_end = 0;
  for (uint32_t dest = 0; dest < dest_count && dest < kMaxChannels; dest++) {
    const uint64_t channel = ChannelAddress(dst_stride, dest, dst_buffer);
    if (channel + kBytesPerChannel > 0x100000000ull) return false;  // would wrap 2^32
    const uint32_t begin = static_cast<uint32_t>(channel) & ~(kBlockBytes - 1);
    const uint32_t end = (static_cast<uint32_t>(channel) & ~0xFu) + kBytesPerChannel;
    for (uint32_t k = 0; k < 4; k++) {
      if (!Disjoint(begin, end - begin, control_addr[k], control_len[k])) return false;
    }
    if (open && begin >= span_begin && begin <= span_end) {
      if (end > span_end) span_end = end;
      continue;
    }
    if (open) {
      spec.write(span_begin, span_end - span_begin);
      spec.read(span_begin, span_end - span_begin);  // sub_82B44D18 reads its accumulator
    }
    span_begin = begin;
    span_end = end;
    open = true;
  }
  if (open) {
    spec.write(span_begin, span_end - span_begin);
    spec.read(span_begin, span_end - span_begin);
  }

  // Reads, recorded for replay only. The live matrix and the caller's saved copy are read row by
  // row; the source channels are summarised by one span over their extremes rather than one each.
  spec.read(mixer + kSourceCount, 8);
  const uint32_t rows = source_count > kMaxRows ? kMaxRows : source_count;
  if (rows > 0) spec.read(mixer + kGainMatrix, kGainRowStride * rows);
  // The gains the mix passes read: `rows` whole rows for the deltas, and 4 bytes per destination
  // from row 0 even when the delta loop did not run (a destination count past eight columns walks
  // into the next row, so the wider of the two lengths is recorded).
  const uint32_t gain_rows = kGainRowStride * rows;
  const uint32_t gain_cols = 4 * dest_count;
  spec.read(saved_base, gain_rows > gain_cols ? gain_rows : gain_cols);
  spec.read(dst_desc, kDescriptorBytes);
  spec.read(src_desc, kDescriptorBytes);
  spec.read(kRampStep, 4);
  spec.read(kKernelStepScale, 4);
  spec.read(kKernelRampSpan, 4);
  spec.read(kKernelLane2, 4);
  spec.read(kKernelLane3, 4);
  spec.read(kKernelPoolLow, 16 * 4);
  spec.read(kKernelPoolHigh, 16 * 4);
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

}  // namespace port_82B298E0
SKATE3_PORT(82B298E0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
