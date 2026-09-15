// sub_82B30C50  role: run one output frame of the DAC -- gather the source channels into a scratch bank when the channel counts differ (sub_82B468C0), resample each channel through its history tail (sub_82B43FB8) at the ratio between the stream's rate and the device's, convert the result to interleaved little-endian 16-bit, copy it into the device buffer, and give every scratch block back to the bump allocator it came from   lifted: skate3_recomp.67.cpp:46372 (958 lines)
// STATUS: partial -- compared 13240, skipped 0, zero divergence (session g1_splits, 150 s scripted); NEARLY VACUOUS -- every call in this session took the early exit, so the 958-line working path never ran and zero skips means the game's behaviour, not coverage. NEVER promoted: the skipped path has a native body but no comparison. (comparable only when u32[device+92] == 0, the early exit, which writes nothing outside its own frame and returns 1. The working path is still gate-2 and is skipped: four sequential bump allocations sized from a 16.16 ratio this call itself computes and rewrites, callees addressed through two pointer tables this call fills in its own frame, and ~20 KB of the 32 KB budget at six channels -- 98 KB on the zero-ratio fallback; see the note)
// calls/session: boot 18684  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B30C50.md
namespace port_82B30C50 {

// r3 = the device object. Nothing in docs/rw_audio_structs.h names it; these are the fields used.
constexpr uint32_t kAllocatorOwner = 8;    // u32 -> u32 -> the bump allocator, whose +32 is the top
constexpr uint32_t kAllocatorTop = 32;     // u32 on that allocator: decremented to allocate
constexpr uint32_t kSourceChannels = 42;   // u8  channels already in the source descriptor
constexpr uint32_t kDrained = 88;          // u32 non-zero skips the whole conversion pass
constexpr uint32_t kDestChannels = 84;     // u32 channels the device wants
constexpr uint32_t kActive = 92;           // u32 zero means there is nothing to do at all
constexpr uint32_t kDeviceRate = 80;       // f32 equal to the stream's rate means no resampling
constexpr uint32_t kRatioSingle = 100;     // f32 the cached rate ratio
constexpr uint32_t kRatioFixed = 108;      // s32 the same ratio in 16.16, clamped to 0x40000
constexpr uint32_t kFramesPerChannel = 104;  // u32 singles per channel in the resampled bank
constexpr uint32_t kPhase = 112;           // u32 the resampler's leftover 16-bit phase
constexpr uint32_t kProduced = 120;        // u32 bytes handed to the device, clamped by +116
constexpr uint32_t kCapacity = 116;        // u32 the device buffer's size
constexpr uint32_t kHistoryOffset = 128;   // u16 offset INTO this object of the per-channel history
constexpr uint32_t kBytesPerFrame = 130;   // u16 bytes per interleaved output frame
constexpr uint32_t kOutputBuffer = 132;    // u32 the device buffer the finished frame is copied to
constexpr uint32_t kHistoryCount = 136;    // u8  singles of history carried into the next frame
constexpr uint32_t kHistoryWanted = 137;   // u8  subtracted from the history budget
constexpr uint32_t kHistoryStride = 24;    // six singles of history per channel
constexpr uint32_t kHistorySlots = 6;

// r4 = the stream: +28 the source buffer descriptor (+4 base, +14 u16 channel stride, as in
// sub_82B29AF0), +40 a format record whose +12 is the source sample rate.
constexpr uint32_t kStreamDescriptor = 28;
constexpr uint32_t kStreamFormat = 40;
constexpr uint32_t kFormatRate = 12;
constexpr uint32_t kBufferBase = 4;
constexpr uint32_t kChannelStride = 14;

constexpr uint32_t kFrameBytes = 352;      // stwu r1,-352(r1)
constexpr uint32_t kSavedTop = 176;        // the allocator top saved before the gather block
constexpr uint32_t kIndexSlot = 80;        // sub_82B43FB8's index out-parameter, and fctiwz scratch
constexpr uint32_t kPhaseSlot = 88;        // its phase out-parameter, and the halfword scratch
constexpr uint32_t kSourceTable = 96;      // u32[8] the channels the resampler reads from
constexpr uint32_t kDestTable = 128;       // u32[8] the channels it writes to
constexpr uint32_t kGatherBytes = 1024;    // 256 singles per channel, both sub_82B468C0 arguments
constexpr uint32_t kGatherFrames = 256;
constexpr uint32_t kHistoryBlock = 1152;   // the second allocation, a fixed size
constexpr uint32_t kRatioCeiling = 0x40000;  // lis r10,4 ; cmpw ; the 16.16 ratio's clamp
constexpr uint32_t kNoRatioFrames = 8192;  // li r23,8192 when the fixed ratio reads zero

// Five rodata singles, each computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye:
//   0x820B4114  lis -32245 ; lfs 16660   -- scales the ratio into 16.16 (65536.0f would fit)
//   0x82165A10  lis -32234 ; lfs 23056   -- 0.0f, the sign test before the round-half
//   0x8209975C  lis -32246 ; lfs -26788  -- 0.5f, added or subtracted before fctiwz
//   0x821747FC  lis -32233 ; addi 18428  -- the full-scale multiplier for the 16-bit conversion
//   0x8216DEE0  that address minus 26908 -- the lower clamp; 0x8231A844 (1.0f) is the upper
// probe/harness/out/image is empty on this tree, so no value was read; every one is loaded through
// the guest map, so none needs to be known here.
constexpr uint32_t kFixedScale = 0x820B4114;
constexpr uint32_t kZeroSingle = 0x82165A10;
constexpr uint32_t kHalfSingle = 0x8209975C;
constexpr uint32_t kFullScale = 0x821747FC;
constexpr uint32_t kClampLow = 0x8216DEE0;
constexpr uint32_t kClampHigh = 0x8231A844;

// lfs / lfsu: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs / stfsu: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fadds / fsubs / fmuls / fdivs: the double operation rounded to single, as the lifted line does it.
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }

// fctiwz, branch for branch as the lifted line spells it: a NaN gives 0x80000000, anything past
// INT_MAX gives INT_MAX, everything else truncates toward zero.
inline int64_t Fctiwz(double value) {
  double v = value;
  return std::isnan(v) ? int64_t(0x80000000U)
         : (v > double(INT_MAX)) ? int64_t(INT_MAX)
                                 : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&v)));
}

// fctiwz ; stfd f,80(r1) ; lwz 92(r1) -- the low word of the converted value, read back out of the
// frame. Modelled as arithmetic: the round trip is through this function's own frame.
inline int32_t FctiwzWord(double value) {
  return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(Fctiwz(value))));
}

// fctiwz ; stfd 80(r1) ; lhz 86(r1) ; sth 88(r1) ; lbz 88/89(r1) ; two stb -- the sample conversion.
// The two bytes come out LOW first, so the device buffer is LITTLE-endian 16-bit. Some of the five
// copies of this in the lifted body store +1 before +0; they are two distinct bytes of the same
// halfword with no read between, so the order is not reproduced.
inline void StoreSample(uint8_t* base, uint32_t addr, double scaled) {
  const uint32_t word = static_cast<uint32_t>(static_cast<uint64_t>(Fctiwz(scaled))) & 0xFFFFu;
  REX_STORE_U8(addr + 0, static_cast<uint8_t>(word & 0xFFu));
  REX_STORE_U8(addr + 1, static_cast<uint8_t>(word >> 8));
}

// fcmpu/ble then fcmpu/bge: clamped to [lower, upper], and a NaN falls through both compares
// unchanged -- which fctiwz then turns into 0x80000000, so the stored sample is zero.
inline double Clamp(double value, double lower, double upper) {
  if (value > upper) return upper;
  if (value < lower) return lower;
  return value;
}

// addi rN,r?,127 ; rlwinm rN,rN,0,0,24 -- round a byte count up to the allocator's 128-byte grain.
inline uint32_t Align128(uint32_t bytes) { return (bytes + 127u) & ~127u; }

// The bump allocator: *(*(device + 8)) + 32, re-read before every allocation and every release.
inline uint32_t AllocatorWord(uint8_t* base, uint32_t device) {
  return REX_LOAD_U32(REX_LOAD_U32(device + kAllocatorOwner)) + kAllocatorTop;
}

REX_FUNC(Native) {
  const uint32_t device = ctx.r3.u32;  // mr r25,r3
  const uint32_t stream = ctx.r4.u32;

  // __savegprlr_14 spills r14-r31 and `stfd f31,-160(r1)` spills f31; both come back at the tail and
  // the harness compares those ranges, so the entry values have to survive.
  const PPCRegister saved_gpr[18] = {ctx.r14, ctx.r15, ctx.r16, ctx.r17, ctx.r18, ctx.r19,
                                     ctx.r20, ctx.r21, ctx.r22, ctx.r23, ctx.r24, ctx.r25,
                                     ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const PPCRegister saved_f31 = ctx.f31;

  // stwu r1,-352(r1). Reproduced -- most ports skip their own frame -- because both pointer tables,
  // sub_82B43FB8's two out-parameters and the conversion scratch all live in it, and because the
  // memcpy helpers spill below the live r1.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;

  // lwz r11,92(r3) ; cmplwi ; beq cr6,0x82b312c8 -- nothing active, nothing written
  if (REX_LOAD_U32(device + kActive) != 0) {
    const uint32_t format = REX_LOAD_U32(stream + kStreamFormat);        // lwz r10,40(r4)
    const uint32_t source_count = REX_LOAD_U8(device + kSourceChannels);  // lbz r6,42(r3)
    const uint32_t descriptor = REX_LOAD_U32(stream + kStreamDescriptor);  // lwz r11,28(r4)
    const uint32_t dest_count = REX_LOAD_U32(device + kDestChannels);     // lwz r30,84(r3)
    REX_STORE_U32(frame + kSavedTop, 0);                                  // stw r20,176(r1)
    ctx.fpscr.disableFlushModeUnconditional();
    const double source_rate = LoadSingle(base, format + kFormatRate);    // lfs f31,12(r10)

    // The four allocator saves, all zero until their block is taken: r16/r17/r18/r19/r21 in the
    // original. A zero is what the releases at the tail test against.
    uint32_t gather_block = 0;   // r16, and the flag for the +176 release
    uint32_t history_old = 0;    // r17
    uint32_t history_block = 0;  // r21
    uint32_t bank_old = 0;       // r18
    uint32_t bank_block = 0;     // r19

    // cmplwi cr6,r6,0 ; beq cr6,0x82b30ccc -- the source table: one pointer per source channel,
    // straight off the descriptor at its channel stride.
    if (source_count != 0) {
      const uint32_t stride = REX_LOAD_U16(descriptor + kChannelStride) << 2;  // lhz ; rotlwi r9,r9,2
      uint32_t channel = REX_LOAD_U32(descriptor + kBufferBase);               // lwz r11,4(r11)
      uint32_t slot = frame + kSourceTable - 4;                                // addi r10,r1,92
      uint32_t trips = source_count;                                           // mtctr r6
      do {                                                                     // loc_82B30CC0
        slot += 4;                                                             // stwu r11,4(r10)
        REX_STORE_U32(slot, channel);
        channel += stride;                                                     // add r11,r9,r11
      } while (--trips != 0);                                                  // bdnz
    }

    // loc_82B30CCC: cmplw cr6,r6,r30 ; beq cr6,0x82b30d48 -- when the counts already agree the
    // gather is skipped entirely and the resampler reads the descriptor's channels directly.
    if (source_count != dest_count) {
      const uint32_t allocator = AllocatorWord(base, device);        // lwz r10,8(r25) ; lwz r7,0(r10)
      const uint32_t before = REX_LOAD_U32(allocator);               // lwz r11,32(r7)
      const uint32_t size = Align128(dest_count << 10);              // rlwinm r11,r30,10 ; +127 ; mask
      gather_block = before - size;                                  // subf r10,r8,r11
      REX_STORE_U32(allocator, gather_block);                        // stw r10,32(r7)
      REX_STORE_U32(frame + kSavedTop, before);                      // stw r11,176(r1)

      // cmplwi cr6,r30,0 ; beq cr6,0x82b30d1c
      if (dest_count != 0) {
        uint32_t slot = frame + kDestTable - 4;  // addi r10,r1,124
        uint32_t channel = gather_block;         // mr r11,r16
        uint32_t trips = dest_count;             // mtctr r30
        do {                                     // loc_82B30D10
          slot += 4;                             // stwu r11,4(r10)
          REX_STORE_U32(slot, channel);
          channel += kGatherBytes;               // addi r11,r11,1024
        } while (--trips != 0);
      }
      // loc_82B30D1C: bl 0x82b468c0 -- r6 is NOT set here; it still holds the source count from the
      // top, which is exactly the argument that callee reads.
      skate3::audio::GuestCall(ctx, base, sub_82B468C0, frame + kDestTable, frame + kSourceTable,
                               dest_count, source_count, uint64_t(kGatherFrames));
      // cmplwi cr6,r30,0 ; beq cr6,0x82b30d48 ; bl 0x82f52fb8 -- the gathered pointers replace the
      // source table, so everything downstream reads the gather block.
      if (dest_count != 0) {
        skate3::audio::GuestCall(ctx, base, sub_82F52FB8, frame + kSourceTable, frame + kDestTable,
                                 dest_count << 2);
      }
    }

    // loc_82B30D48
    ctx.fpscr.disableFlushModeUnconditional();
    const double device_rate = LoadSingle(base, device + kDeviceRate);  // lfs f0,80(r25)
    uint32_t out_frames = kGatherFrames;                                // li r29,256
    // fcmpu cr6,f31,f0 ; beq cr6,0x82b30fe0 -- equal rates skip the whole resampling pass
    if (!(source_rate == device_rate)) {
      const double ratio = DivSingle(source_rate, device_rate);  // fdivs f12,f31,f0
      // lfs f0,100(r25) ; fcmpu cr6,f0,f12 ; beq cr6,0x82b30dc4
      if (!(LoadSingle(base, device + kRatioSingle) == ratio)) {
        const double scale = LoadSingle(base, kFixedScale);   // lfs f0,16660(r11)
        const double product = MulSingle(ratio, scale);       // fmuls f0,f12,f0
        const double zero = LoadSingle(base, kZeroSingle);    // lfs f13,23056(r10)
        const double half = LoadSingle(base, kHalfSingle);    // lfs f13,-26788(r11)
        // fcmpu cr6,f0,f13 ; blt cr6,0x82b30d9c -- round half away from zero
        int32_t fixed = product < zero ? FctiwzWord(SubSingle(product, half))
                                       : FctiwzWord(AddSingle(product, half));
        // lis r10,4 ; cmpw cr6,r11,r10 ; ble cr6 ; lis r11,4 -- a SIGNED clamp to 4.0 in 16.16
        if (fixed > static_cast<int32_t>(kRatioCeiling)) fixed = static_cast<int32_t>(kRatioCeiling);
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, device + kRatioSingle, ratio);                        // stfs f12,100(r25)
        REX_STORE_U32(device + kRatioFixed, static_cast<uint32_t>(fixed));      // stw r11,108(r25)
      }

      // loc_82B30DC4: the history block, a fixed 1152 bytes.
      {
        const uint32_t allocator = AllocatorWord(base, device);
        const uint32_t before = REX_LOAD_U32(allocator);
        history_old = before;                    // mr r17,r11
        history_block = before - kHistoryBlock;  // addi r10,r11,-1152
        REX_STORE_U32(allocator, history_block);  // stw r10,32(r9)
      }
      const uint32_t history_budget = REX_LOAD_U8(device + kHistoryCount) + kGatherFrames;  // r22
      // The resampled bank: one row of +104 singles per +84 channels.
      {
        const uint32_t allocator = AllocatorWord(base, device);  // lwz r8,8(r25) ; lwz r4,0(r8)
        const uint32_t rows = REX_LOAD_U32(device + kDestChannels);        // lwz r7,84(r25)
        const uint32_t columns = REX_LOAD_U32(device + kFramesPerChannel);  // lwz r6,104(r25)
        // mullw r5,r7,r6 is the 64-bit product of two sign-extended words; only the low word
        // survives the shift and the mask below.
        const uint32_t size = Align128(static_cast<uint32_t>(
            static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(rows)) *
                                  static_cast<int64_t>(static_cast<int32_t>(columns)))
            << 2));
        const uint32_t before = REX_LOAD_U32(allocator);  // lwz r11,32(r4)
        bank_old = before;                                // mr r18,r11
        bank_block = before - size;                       // subf r11,r10,r11
        REX_STORE_U32(allocator, bank_block);             // stw r11,32(r4)
      }
      // cmplwi cr6,r30,0 ; beq cr6,0x82b30e40 -- the destination table now points into the bank
      if (dest_count != 0) {
        const uint32_t stride = REX_LOAD_U32(device + kFramesPerChannel) << 2;  // lwz r9,104 ; rlwinm
        uint32_t slot = frame + kDestTable - 4;  // addi r10,r1,124
        uint32_t channel = bank_block;           // r11 still holds it
        uint32_t trips = dest_count;             // mtctr r30
        do {                                     // loc_82B30E34
          slot += 4;
          REX_STORE_U32(slot, channel);
          channel += stride;
        } while (--trips != 0);
      }

      // loc_82B30E40: how many output frames this pass can make.
      const uint32_t history = REX_LOAD_U16(device + kHistoryOffset) + device;  // lhz ; add r24,r11,r25
      const int32_t need =
          static_cast<int32_t>(history_budget - REX_LOAD_U8(device + kHistoryWanted)) + 1;
      uint32_t frames = 0;  // r23
      // addic. r11,r9,1 ; bgt 0x82b30e60
      if (need > 0) {
        const uint32_t step = REX_LOAD_U32(device + kRatioFixed);  // lwz r10,108(r25)
        // cmplwi cr6,r10,0 ; bne cr6,0x82b30e74
        if (step == 0) {
          frames = kNoRatioFrames;  // li r23,8192
        } else {
          const uint32_t phase = REX_LOAD_U32(device + kPhase);       // lwz r9,112(r25)
          const uint32_t whole = static_cast<uint32_t>(need) << 16;   // rlwinm r8,r11,16,0,15
          // twllei r10,0 -- the divide-by-zero trap, unreachable behind the test above
          frames = (whole - phase - 1) / step;                        // subf ; addi -1 ; divwu
        }
      }

      // loc_82B30E8C: one pass per destination channel.
      uint32_t phase_word = 0;    // r11 = r20, the value loc_82B30FB8 reads when the loop is skipped
      uint32_t history_kept = 0;  // r31, likewise
      // cmplwi cr6,r30,0 ; beq cr6,0x82b30fb8
      if (dest_count != 0) {
        uint32_t table_offset = 0;       // r29, +4 per channel
        uint32_t history_row = history;  // r28, +24 per channel
        uint32_t history_index = 0;      // r27, +6 per channel
        uint32_t remaining = dest_count;  // r26
        do {                             // loc_82B30EAC
          history_kept = REX_LOAD_U8(device + kHistoryCount);  // lbz r31,136(r25)
          // cmplwi cr6,r31,0 ; beq cr6,0x82b30ecc -- last frame's tail goes in front
          if (history_kept != 0) {
            skate3::audio::GuestCall(ctx, base, sub_82F52FB8, history_block, history_row,
                                     (history_kept & 0xFFu) << 2);
          }
          // loc_82B30ECC: 1024 bytes of this channel behind it
          const uint32_t source = REX_LOAD_U32(frame + kSourceTable + table_offset);  // lwzx r4,r29,r10
          const uint32_t bank = REX_LOAD_U32(frame + kDestTable + table_offset);      // lwzx r31,r29,r9
          skate3::audio::GuestCall(ctx, base, sub_82EDF460, history_block + (history_kept << 2),
                                   source, uint64_t(kGatherBytes));

          // bl 0x82b43fb8 -- resample `frames` singles out of the history block into the bank row,
          // with the index at r1+80 and the 16.16 phase at r1+88 as out-parameters.
          const uint32_t phase = REX_LOAD_U32(device + kPhase);  // lwz r11,112(r25)
          REX_STORE_U32(frame + kIndexSlot, 0);                  // stw r20,80(r1)
          REX_STORE_U32(frame + kPhaseSlot, phase << 16);        // rlwinm r10,r11,16,0,15 ; stw
          const uint32_t step = REX_LOAD_U32(device + kRatioFixed);  // lwz r8,108(r25)
          skate3::audio::GuestCall(ctx, base, sub_82B43FB8, frames, history_block, bank,
                                   frame + kIndexSlot, frame + kPhaseSlot, step);

          // The tail the next frame starts from: everything the resampler did not consume.
          const uint32_t consumed = REX_LOAD_U32(frame + kIndexSlot);  // lwz r7,80(r1)
          const int32_t left = static_cast<int32_t>(history_budget - consumed);  // subf r31,r7,r22
          uint32_t copied = 0;  // r11, zero when the unrolled loop is skipped
          // cmpwi cr6,r31,4 ; blt cr6,0x82b30f74
          if (left >= 4) {
            uint32_t src = history_block + (consumed << 2) - 4;  // rlwinm ; add r11,r11,r21 ; -4
            uint32_t dst = history_row - 4;                      // addi r8,r28,-4
            uint32_t trips = ((static_cast<uint32_t>(left) - 4) >> 2) + 1;  // rlwinm r10,r10,30,2,31
            copied = trips << 2;                                            // rlwinm r11,r10,2,0,29
            do {                                                            // loc_82B30F50
              ctx.fpscr.disableFlushModeUnconditional();
              const double a = LoadSingle(base, src + 4);   // lfs f13,4(r9)
              const double b = LoadSingle(base, src + 8);   // lfs f12,8(r9)
              const double c = LoadSingle(base, src + 12);  // lfs f11,12(r9)
              src += 16;                                    // lfsu f0,16(r9)
              const double d = LoadSingle(base, src);
              StoreSingle(base, dst + 4, a);                // stfs f13,4(r8)
              StoreSingle(base, dst + 8, b);                // stfs f12,8(r8)
              StoreSingle(base, dst + 12, c);               // stfs f11,12(r8)
              dst += 16;                                    // stfsu f0,16(r8)
              StoreSingle(base, dst, d);
            } while (--trips != 0);
          }
          // loc_82B30F74: cmplw cr6,r11,r31 ; bge cr6,0x82b30fa0 -- the odd samples the unrolled
          // loop could not take, by memcpy
          if (copied < static_cast<uint32_t>(left)) {
            const uint32_t dst = ((history_index + copied) << 2) + history;
            const uint32_t src = ((copied + consumed) << 2) + history_block;
            skate3::audio::GuestCall(ctx, base, sub_82F52FB8, dst, src,
                                     (static_cast<uint32_t>(left) - copied) << 2);
          }
          // loc_82B30FA0
          table_offset += 4;                  // addi r29,r29,4
          history_row += kHistoryStride;      // addi r28,r28,24
          history_index += kHistorySlots;     // addi r27,r27,6
        } while (--remaining != 0);           // addic. r26,r26,-1 ; bne
        phase_word = REX_LOAD_U32(frame + kPhaseSlot);  // lwz r11,88(r1)
      }

      // loc_82B30FB8: publish the tail length and the leftover phase.
      REX_STORE_U8(device + kHistoryCount, static_cast<uint8_t>(history_kept));  // stb r31,136(r25)
      REX_STORE_U32(device + kPhase, (phase_word >> 16) & 0xFFFFu);  // rlwinm r9,r11,16,16,31 ; stw
      // cmplwi cr6,r30,0 ; beq cr6,0x82b30fdc -- the bank pointers become the conversion's input
      if (dest_count != 0) {
        skate3::audio::GuestCall(ctx, base, sub_82F52FB8, frame + kSourceTable, frame + kDestTable,
                                 dest_count << 2);
      }
      out_frames = frames;  // mr r29,r23
    }

    // loc_82B30FE0: the interleaved 16-bit block, sized from the frame size, the channel count and
    // the frames this pass produced.
    const uint32_t allocator = AllocatorWord(base, device);  // lwz r10,8(r25) ; lwz r8,0(r10)
    const uint32_t output_old = REX_LOAD_U32(allocator);     // lwz r22,32(r8)
    const uint32_t frame_bytes = REX_LOAD_U16(device + kBytesPerFrame);  // lhz r11,130(r25)
    // mullw r9,r11,r30 then mullw r23,r9,r29: 64-bit products of sign-extended words, truncated by
    // the align below.
    const uint32_t produced = static_cast<uint32_t>(
        static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(
            static_cast<int64_t>(static_cast<int32_t>(frame_bytes)) *
            static_cast<int64_t>(static_cast<int32_t>(dest_count))))) *
        static_cast<int64_t>(static_cast<int32_t>(out_frames)));
    const uint32_t output_block = output_old - Align128(produced);  // +127 ; mask ; subf r4,r6,r22
    REX_STORE_U32(allocator, output_block);                        // stw r4,32(r8)

    // cmplwi cr6,r4,0 ; beq cr6,0x82b31288 -- the allocator ran dry: no conversion, no submit, and
    // the release below is skipped for this block only.
    if (output_block != 0) {
      // lwz r11,88(r25) ; cmplwi ; bne cr6,0x82b3125c
      if (REX_LOAD_U32(device + kDrained) == 0 && dest_count != 0) {
        ctx.fpscr.disableFlushModeUnconditional();
        const double full_scale = LoadSingle(base, kFullScale);  // lfs f11,18428(r11)
        const double upper = LoadSingle(base, kClampHigh);       // lfs f13,-22460(r10)
        const double lower = LoadSingle(base, kClampLow);        // lfs f12,-26908(r9)
        const uint32_t pair_stride = dest_count << 1;            // rlwinm r26,r30,1,0,30
        uint32_t channel = 0;                   // r3, the channel index
        uint32_t table = frame + kSourceTable;  // r24
        uint32_t slot0 = output_block;                // r28: frame 0 of this channel
        uint32_t slot1 = output_block + pair_stride;  // r27: frame 1
        do {                                          // loc_82B31050
          const uint32_t row = REX_LOAD_U32(table);   // lwz r31,0(r24)
          uint32_t done = 0;                          // r11 = r20
          // cmpwi cr6,r29,4 ; blt cr6,0x82b311cc -- four frames at a time
          if (static_cast<int32_t>(out_frames) >= 4) {
            const uint32_t trips = ((out_frames - 4) >> 2) + 1;  // rlwinm r10,r10,30,2,31 ; +1
            done = trips << 2;                                   // rlwinm r11,r11,2,0,29
            const uint32_t advance = dest_count << 3;            // rlwinm r8,r30,3,0,28
            uint32_t sample = row + 8;                           // addi r10,r31,8
            uint32_t out0 = slot0;                               // mr r7,r28
            uint32_t out1 = slot1;                               // mr r6,r27
            uint32_t out2 = ((pair_stride + channel) << 1) + output_block;   // frame 2
            uint32_t out3 = (((dest_count + pair_stride) + channel) << 1) + output_block;  // frame 3
            uint32_t count = trips;                              // mtctr r11
            do {                                                 // loc_82B310A4
              ctx.fpscr.disableFlushModeUnconditional();
              const double s0 = Clamp(LoadSingle(base, sample - 8), lower, upper);
              StoreSample(base, out0, MulSingle(s0, full_scale));
              const double s1 = Clamp(LoadSingle(base, sample - 4), lower, upper);
              StoreSample(base, out1, MulSingle(s1, full_scale));
              const double s2 = Clamp(LoadSingle(base, sample + 0), lower, upper);
              StoreSample(base, out2, MulSingle(s2, full_scale));
              const double s3 = Clamp(LoadSingle(base, sample + 4), lower, upper);
              StoreSample(base, out3, MulSingle(s3, full_scale));
              out0 += advance;  // add r7,r8,r7
              out1 += advance;  // add r6,r8,r6
              out2 += advance;  // add r5,r8,r5
              sample += 16;     // addi r10,r10,16
              out3 += advance;  // add r9,r8,r9
            } while (--count != 0);
          }
          // loc_82B311CC: cmplw cr6,r11,r29 ; bge cr6,0x82b31244 -- the frames left over
          if (done < out_frames) {
            uint32_t sample = (done << 2) + row;  // rlwinm r11,r11,2 ; add r10,r11,r31
            // mullw r10,r11,r30 ; add r7,r10,r3 ; rlwinm r9,r7,1 ; add r11,r9,r4
            uint32_t out = ((static_cast<uint32_t>(
                                 static_cast<int64_t>(static_cast<int32_t>(done)) *
                                 static_cast<int64_t>(static_cast<int32_t>(dest_count))) +
                             channel)
                            << 1) +
                           output_block;
            uint32_t count = out_frames - done;  // subf r8,r11,r29 ; mtctr r8
            do {                                 // loc_82B311F4
              ctx.fpscr.disableFlushModeUnconditional();
              const double s = Clamp(LoadSingle(base, sample), lower, upper);
              sample += 4;  // addi r10,r10,4
              StoreSample(base, out, MulSingle(s, full_scale));
              out += pair_stride;  // add r11,r26,r11
            } while (--count != 0);
          }
          // loc_82B31244
          channel += 1;             // addi r3,r3,1
          table += 4;               // addi r24,r24,4
          slot0 += 2;               // addi r28,r28,2
          slot1 += 2;               // addi r27,r27,2
        } while (channel < dest_count);  // cmplw cr6,r3,r30 ; blt cr6
      }

      // loc_82B3125C: publish the byte count, clamped by the device buffer's capacity, and copy.
      const uint32_t capacity = REX_LOAD_U32(device + kCapacity);  // lwz r11,116(r25)
      REX_STORE_U32(device + kProduced, produced);                 // stw r23,120(r25)
      // cmplw cr6,r23,r11 ; ble cr6,0x82b31270
      if (produced > capacity) REX_STORE_U32(device + kProduced, capacity);
      skate3::audio::GuestCall(ctx, base, sub_82EDF460, REX_LOAD_U32(device + kOutputBuffer),
                               output_block, REX_LOAD_U32(device + kProduced));
      REX_STORE_U32(AllocatorWord(base, device), output_old);  // stw r22,32(r10)
    }

    // loc_82B31288 .. loc_82B312B0: give the other three blocks back, newest first, each only if it
    // was taken. The gather block's saved top comes out of the frame rather than a register.
    if (bank_block != 0) REX_STORE_U32(AllocatorWord(base, device), bank_old);      // stw r18,32(r10)
    if (history_block != 0) REX_STORE_U32(AllocatorWord(base, device), history_old);  // stw r17
    if (gather_block != 0) {
      REX_STORE_U32(AllocatorWord(base, device), REX_LOAD_U32(frame + kSavedTop));  // lwz r10,176(r1)
    }
  }

  // loc_82B312C8: li r3,1 ; addi r1,r1,352 ; lfd f31,-160(r1) ; __restgprlr_14
  ctx.r3.s64 = 1;
  ctx.r1.u32 = entry_sp;
  ctx.r14 = saved_gpr[0];
  ctx.r15 = saved_gpr[1];
  ctx.r16 = saved_gpr[2];
  ctx.r17 = saved_gpr[3];
  ctx.r18 = saved_gpr[4];
  ctx.r19 = saved_gpr[5];
  ctx.r20 = saved_gpr[6];
  ctx.r21 = saved_gpr[7];
  ctx.r22 = saved_gpr[8];
  ctx.r23 = saved_gpr[9];
  ctx.r24 = saved_gpr[10];
  ctx.r25 = saved_gpr[11];
  ctx.r26 = saved_gpr[12];
  ctx.r27 = saved_gpr[13];
  ctx.r28 = saved_gpr[14];
  ctx.r29 = saved_gpr[15];
  ctx.r30 = saved_gpr[16];
  ctx.r31 = saved_gpr[17];
  ctx.f31 = saved_f31;
}

// Gate 2, and this is where the reasoning lives. The write set of the working path is, in outline:
// the allocator word (up to eight times), four scratch blocks taken from it, the object's per-channel
// history rows, six object words, and the device buffer at *(+132). Every one of those is *shaped*
// from entry state, but three things stop it being *named* before the call:
//
//   1. Three of the four block sizes depend on values this call computes. The resampled bank and the
//      output block are sized from `frames`, which comes from a divwu by the 16.16 ratio at +108 --
//      and +108 is itself rewritten earlier in the same call, from a single-precision divide, a
//      rodata multiply, a round-half-away-from-zero and a signed clamp. A window set would have to
//      re-derive all of that; getting it wrong by one puts a store outside the windows, which is the
//      one failure mode that reaches the live game.
//   2. The blocks are addressed through the two pointer tables at r1+96 and r1+128, which this call
//      fills. sub_82B468C0's and sub_82B43FB8's ported Windows() both read their pointer arrays out
//      of memory, and before the call those slots hold the previous frame's values. Mirroring them
//      means predicting the tables, not reading them.
//   3. The budget. For six channels at 256 frames the four blocks come to roughly 6 KB + 1152 B +
//      6 KB + 3 KB, plus about 3 KB again for the device buffer: some 20 KB of the 32 KB cap, before
//      the object rows. Eight channels fits only just, and the `+108 == 0` fallback asks for 8192
//      frames, which is about 98 KB in the output block alone -- three times the cap.
//
// So the split is by path: true for the early exit (`+92 == 0`), which writes nothing outside its
// own frame and returns 1, and false for the working path, where the macro runs the original and
// counts a PortSkip. The earlier draft of this file left the whole port gate-2 to avoid a session
// reporting it "verified" on the strength of the empty path; the STATUS line now carries the
// partiality, and the skip count says on every session how little was compared, so that objection
// is met by labelling rather than by refusing the comparison. What the empty path does check:
// the guard word's offset, the 352-byte frame discipline, r14-r31 and f31 coming back intact
// through __savegprlr_14/__restgprlr_14, and `li r3,1`. What it does not check is everything in
// the three paragraphs above.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  spec.read(ctx.r3.u32 + kActive, 4);
  // lwz r11,92(r3) ; cmplwi cr6,r11,0 ; beq cr6,0x82b312c8 -- read before the frame is even used,
  // so Native and Windows test the same byte of the same word. No writes on that path.
  return REX_LOAD_U32(ctx.r3.u32 + kActive) == 0;
}

}  // namespace port_82B30C50
SKATE3_PORT(82B30C50, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
