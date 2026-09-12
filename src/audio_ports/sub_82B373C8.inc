// sub_82B373C8  role: per-channel RMS and peak meters over one block, into ring histories   lifted: skate3_recomp.67.cpp:61681 (451 lines)
// STATUS: verified
// calls/session: boot 46  play 20294  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B373C8.md
namespace port_82B373C8 {

// Meter object (r3). Nothing in docs/rw_audio_structs.h names it, so the offsets stay numeric
// and the names below are readings of what the code does with them.
constexpr uint32_t kChannelCount = 0x2A;    // u8   channels metered; 8 is the design maximum
constexpr uint32_t kSlotBase = 0xEC;        // 236, the tail loop's cursor: +4 sum, +36 level
constexpr uint32_t kRmsSum = 0xF0;          // 240  f32[] running sum of means
constexpr uint32_t kLevel = 0x110;          // 272  f32[] level now
constexpr uint32_t kPeak = 0x130;           // 304  f32[] peak this block
constexpr uint32_t kPeakHold = 0x150;       // 336  f32[] peak held
constexpr uint32_t kFrames = 0x178;         // 376  u32  ring length, in blocks
constexpr uint32_t kLevelHistory = 0x180;   // 384  u16  byte offset, in this object, of the
                                            //      per-channel level ring
constexpr uint32_t kPeakHistory = 0x182;    // 386  u16  same, for the peak ring
constexpr uint32_t kCursor = 0x184;         // 388  u16  ring write position

// Channel-buffer descriptor (r4), the same shape sub_82B238A8 reads at its desc +0x1C.
constexpr uint32_t kChanBase = 4;           // u32  float* of channel 0
constexpr uint32_t kChanStride = 14;        // u16  floats between channels

// The two red-zone scratch arrays, at r1-304 (sums) and r1-176 (peaks), 16 bytes per channel.
// 128 bytes each, so the original is built for at most eight channels; past that it walks off
// the top of its own red zone and into the caller's frame.
constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kSumScratch = 304;
constexpr uint32_t kPeakScratch = 176;
// r1-320 is the third scratch slot, the std/lfd int-to-double bridge; see Native.

// 0x82160000 + 23056. Also reached as kZeroSingle in sub_82B1D5D0.inc, where it is read as 0.0f;
// if that is right, the fcmpu against it below can never take the `lt` side.
constexpr uint32_t kZeroSingle = 0x82165A10;
// 0x82300000 - 31232 + 1092: the f32 the summed squares are scaled by before the divide.
constexpr uint32_t kRmsScale = 0x822F8A44;

// One block: 256 floats per channel, sampled 16 bytes out of every 64 (see Native).
constexpr uint32_t kBlockBytes = 1024;

// fadds / fsubs / fmuls / fdivs: the double operation rounded to single, as the lifted lines do.
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: rounded back to a single and stored. The round trip through float is kept rather than
// copying the word, because it is not a no-op on a signalling NaN.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// lvx128: a 16-byte load, address masked, with RexGlue's big-endian lane reversal.
simde__m128i LoadVector(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)),
                               simde_mm_load_si128((simde__m128i*)VectorMaskL));
}

// stvx128: the same masking and the same reversal on the way out.
void StoreVector(uint8_t* base, uint32_t address, simde__m128 value) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(value),
                                             simde_mm_load_si128((simde__m128i*)VectorMaskL)));
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t desc = ctx.r4.u32;
  // No stwu: every scratch address below is r1-relative, in this function's own red zone.
  const uint32_t sp = ctx.r1.u32;
  const uint32_t count = REX_LOAD_U8(object + kChannelCount);  // lbz r28,42(r3)

  // cmplwi cr6,r28,0 ; beq cr6,0x82b374a8
  if (count != 0) {
    // vspltisw128 v63,-1 ; vslw128 v58,v63,v63 -- 0x80000000 in every lane, the sign mask.
    const simde__m128i ones = simde_mm_set1_epi32(int(0xFFFFFFFF));
    const simde__m128i sign =
        simde_mm_sllv_epi32(ones, simde_mm_and_si128(ones, simde_mm_set1_epi32(0x1F)));
    const simde__m128 zero = simde_mm_castsi128_ps(simde_mm_set1_epi32(0x0));  // vspltisw128 v59,0
    const uint32_t stride = REX_LOAD_U16(desc + kChanStride);   // lhz r11,14(r4)
    uint32_t row = REX_LOAD_U32(desc + kChanBase);              // lwz r10,4(r4)
    const uint32_t row_step = __builtin_rotateleft32(stride, 2);  // rotlwi r6,r11,2
    uint32_t scratch = 0;                                       // li r8,0

    // addic. r7,r7,-1 ; bne 0x82b373fc -- once per channel, counting r28 down.
    for (uint32_t remaining = count; remaining != 0; remaining--) {
      // Eight dcbt over the row: prefetch hints, no state, so nothing is emitted for them.

      // Four accumulator pairs, all zeroed from v59 and all fed the identical value below.
      simde__m128 sum_a = zero, sum_b = zero, sum_c = zero, sum_d = zero;  // vor128 v13..v10
      simde__m128 max_a = zero, max_b = zero, max_c = zero, max_d = zero;  // vor128 v63..v60

      // Sixteen passes stepping 64 bytes but loading only 16 of them, so three quarters of the
      // row is never read. Kept as the original has it: the four accumulators are fed the same
      // vector, which is what makes the sum below four times the sum of squares.
      for (uint32_t offset = 0; offset < kBlockBytes; offset += 64) {  // li r9,16 ; mtctr ; bdnz
        const simde__m128i raw = LoadVector(base, row + offset);       // lvx128 v57,r10,r11
        // vandc128 v0,v57,v58 -- clear the sign bit, i.e. the magnitude of each lane.
        const simde__m128 mag = simde_mm_castsi128_ps(simde_mm_andnot_si128(sign, raw));
        ctx.fpscr.enableFlushModeUnconditional();
        // Single-rounding fused multiply-adds, four of them on the same operands.
        sum_a = simde_mm_fmadd_ps(mag, mag, sum_a);                    // vmaddfp v13,v0,v0,v13
        sum_b = simde_mm_fmadd_ps(mag, mag, sum_b);                    // vmaddfp v12,v0,v0,v12
        sum_c = simde_mm_fmadd_ps(mag, mag, sum_c);                    // vmaddfp v11,v0,v0,v11
        sum_d = simde_mm_fmadd_ps(mag, mag, sum_d);                    // vmaddfp v10,v0,v0,v10
        // maxps returns its SECOND operand on a NaN lane, so the order is semantic.
        max_a = simde_mm_max_ps(mag, max_a);                           // vmaxfp128 v63,v0,v63
        max_b = simde_mm_max_ps(mag, max_b);                           // vmaxfp128 v62,v0,v62
        max_c = simde_mm_max_ps(mag, max_c);                           // vmaxfp128 v61,v0,v61
        max_d = simde_mm_max_ps(mag, max_d);                           // vmaxfp128 v60,v0,v60
      }

      // Pairwise reduction, in the original's tree: every vaddfp is its own rounding.
      const simde__m128 sum_cd = simde_mm_add_ps(sum_c, sum_d);        // vaddfp128 v56,v11,v10
      const simde__m128 sum_ab = simde_mm_add_ps(sum_a, sum_b);        // vaddfp128 v55,v13,v12
      const simde__m128 max_cd = simde_mm_max_ps(max_c, max_d);        // vmaxfp128 v54,v61,v60
      const simde__m128 max_ab = simde_mm_max_ps(max_a, max_b);        // vmaxfp128 v53,v63,v62
      const simde__m128 sums = simde_mm_add_ps(sum_ab, sum_cd);        // vaddfp128 v52,v55,v56
      const simde__m128 peaks = simde_mm_max_ps(max_ab, max_cd);       // vmaxfp128 v51,v53,v54
      row += row_step;                                                 // add r10,r6,r10
      // Both stores land in this function's own red zone, 16 bytes per channel. They are kept as
      // guest stores rather than host locals because the read-back below is by lfsx at four
      // byte offsets, so the stvx128 lane reversal is part of the arithmetic.
      StoreVector(base, scratch + (sp - kSumScratch), sums);           // stvx128 v52,r8,r11
      StoreVector(base, scratch + (sp - kPeakScratch), peaks);         // stvx128 v51,r8,r9
      scratch += 16;                                                   // addi r8,r8,16
    }
  }

  // lis r11,-32234 ; lfs f10,23056(r11) -- loaded whatever the channel count is, because the
  // tail below uses it too.
  ctx.fpscr.disableFlushModeUnconditional();
  const double floor_value = LoadSingle(base, kZeroSingle);

  // cmplwi cr6,r28,0 ; beq cr6,0x82b3764c
  if (count != 0) {
    const double scale = LoadSingle(base, kRmsScale);              // lfs f9,1092(r9)
    const uint32_t history_step = (count << 2) & 0xFFFFFFFCu;      // rlwinm r29,r28,2,0,29
    uint32_t slots = object + kLevel;                              // addi r10,r3,272
    uint32_t frame_first = 0;                                      // li r5,0
    uint32_t history_row = 0;                                      // li r30,0

    // cmplw cr6,r31,r28 ; blt cr6,0x82b374d8
    for (uint32_t channel = 0; channel < count; channel++) {
      const uint32_t frames = REX_LOAD_U32(object + kFrames);      // lwz r8,376(r3)
      const uint32_t cursor = REX_LOAD_U16(object + kCursor);      // lhz r9,388(r3)
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, slots + 32, floor_value);                  // stfs f10,32(r10)
      double sum_even = floor_value;                               // fmr f11,f10
      double peak = floor_value;                                   // fmr f0,f10
      double sum_odd = floor_value;                                // fmr f12,f10
      // mullw r8,r8,r31 ; add r7,r8,r9 -- this channel's ring index. mullw keeps only the low
      // 32 bits, so the product is taken unsigned: the signed one can overflow.
      const uint32_t index = frames * channel + cursor;
      uint32_t lane = (channel << 4) & 0xFFFFFFF0u;                 // rlwinm r11,r31,4,0,27

      // Two passes over the scratch vector, reading its four lanes as 0,4 then 8,12: the even
      // offsets accumulate into one sum and the odd into the other, and both feed one peak.
      for (int pass = 0; pass < 2; pass++) {                        // li r11,2 ; mtctr ; bdnz
        ctx.fpscr.disableFlushModeUnconditional();
        double sum_lane = LoadSingle(base, lane + (sp - kSumScratch));       // lfsx f8,r11,r9
        double peak_lane = LoadSingle(base, lane + (sp - kPeakScratch));     // lfsx f13,r11,r8
        sum_even = AddSingle(sum_lane, sum_even);                    // fadds f11,f8,f11
        if (peak < peak_lane) peak = peak_lane;                      // fcmpu ; bge ; fmr f0,f13
        ctx.fpscr.disableFlushModeUnconditional();
        sum_lane = LoadSingle(base, lane + (sp - kSumScratch + 4));          // lfsx f8,r11,r9
        peak_lane = LoadSingle(base, lane + (sp - kPeakScratch + 4));        // lfsx f13,r11,r8
        sum_odd = AddSingle(sum_lane, sum_odd);                      // fadds f12,f8,f12
        if (peak < peak_lane) peak = peak_lane;                      // fcmpu ; bge ; fmr f0,f13
        lane += 8;                                                   // addi r11,r11,8
      }

      const uint32_t frames_now = REX_LOAD_U32(object + kFrames);   // lwz r9,376(r3)
      ctx.fpscr.disableFlushModeUnconditional();
      const double total = AddSingle(sum_odd, sum_even);            // fadds f13,f12,f11
      const uint32_t level_off = REX_LOAD_U16(object + kLevelHistory);  // lhz r11,384(r3)
      const uint32_t slot = (index << 2) & 0xFFFFFFFCu;             // rlwinm r7,r7,2,0,29
      const double level_prev = LoadSingle(base, slots);            // lfs f11,0(r10)
      const uint32_t level_row = object + level_off;                // add r4,r11,r3
      const uint32_t peak_off = REX_LOAD_U16(object + kPeakHistory);    // lhz r11,386(r3)
      const uint32_t peak_row = object + peak_off;                  // add r6,r11,r3
      // extsw r8,r9 ; std r8,-320(r1) ; lfd f8,-320(r1) ; fcfid f7,f8 -- the guest's
      // int-to-double bridge through its own red zone (r1-320); the round trip is bit-preserving,
      // so it is done in registers here.
      const double frames_f = static_cast<double>(int64_t(int32_t(frames_now)));
      const double weighted = MulSingle(total, scale);              // fmuls f6,f13,f9
      const double history = LoadSingle(base, slot + level_row);     // lfsx f5,r7,r4
      const double divisor = static_cast<double>(static_cast<float>(frames_f));  // frsp f4,f7
      const double mean = DivSingle(weighted, divisor);             // fdivs f12,f6,f4
      const double delta = SubSingle(mean, history);                // fsubs f3,f12,f5
      const double level = AddSingle(delta, level_prev);            // fadds f2,f3,f11
      StoreSingle(base, slots, level);                              // stfs f2,0(r10)
      // fcmpu cr6,f2,f10 ; bge cr6,0x82b375ac ; stfs f10,0(r10)
      if (level < floor_value) StoreSingle(base, slots, floor_value);

      ctx.fpscr.disableFlushModeUnconditional();
      const double sum_prev = LoadSingle(base, slots - 32);         // lfs f13,-32(r10)
      const double sum_next = AddSingle(sum_prev, mean);            // fadds f11,f13,f12
      StoreSingle(base, slots - 32, sum_next);                      // stfs f11,-32(r10)

      // fcmpu cr6,f0,f10 (taken before the add above) ; blt cr6,0x82b375d8
      if (peak < floor_value) {
        // Unreachable if the constant really is 0.0f, since every peak here is a magnitude.
        ctx.fpscr.disableFlushModeUnconditional();
        const double slot_peak = LoadSingle(base, slot + peak_row);  // lfsx f13,r7,r6
        // fcmpu cr6,f13,f10 ; blt cr6,0x82b3762c
        if (!(slot_peak < floor_value)) {
          StoreSingle(base, slot + peak_row, peak);                  // stfsx f0,r7,r6
          // lwz r9,376(r3) ; add r9,r9,r5 ; cmplw cr6,r5,r9 ; bge cr6,0x82b3762c
          const uint32_t limit = REX_LOAD_U32(object + kFrames) + frame_first;
          if (frame_first < limit) {
            uint32_t probe = history_row + peak_row;                 // add r9,r30,r6
            uint32_t at = frame_first;                               // mr r11,r5
            uint32_t bound = 0;
            do {
              ctx.fpscr.disableFlushModeUnconditional();
              const double candidate = LoadSingle(base, probe);      // lfs f13,0(r9)
              const double held = LoadSingle(base, slots + 32);      // lfs f11,32(r10)
              // fcmpu cr6,f11,f13 ; bge cr6,0x82b37614 ; stfs f13,32(r10)
              if (held < candidate) StoreSingle(base, slots + 32, candidate);
              bound = REX_LOAD_U32(object + kFrames) + frame_first;  // lwz r8 ; add r8,r8,r5
              at += 1;                                               // addi r11,r11,1
              probe += 4;                                            // addi r9,r9,4
            } while (at < bound);  // cmplw cr6,r11,r8 ; blt cr6,0x82b37600
          }
        }
      } else {
        ctx.fpscr.disableFlushModeUnconditional();
        const double held = LoadSingle(base, slots + 64);            // lfs f13,64(r10)
        StoreSingle(base, slots + 32, peak);                         // stfs f0,32(r10)
        // fcmpu cr6,f0,f13 ; ble cr6,0x82b3762c ; stfs f0,64(r10)
        if (peak > held) StoreSingle(base, slots + 64, peak);
      }

      // loc_82B3762C, reached by every path above.
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, slot + peak_row, peak);                      // stfsx f0,r7,r6
      StoreSingle(base, slot + level_row, mean);                     // stfsx f12,r7,r4
      slots += 4;                                                    // addi r10,r10,4
      frame_first += count;                                          // add r5,r5,r28
      history_row += history_step;                                   // add r30,r29,r30
    }
  }

  // loc_82B3764C: on the last block of the ring, publish each channel's running sum as its
  // level and clear the sum.
  const uint32_t frames = REX_LOAD_U32(object + kFrames);       // lwz r11,376(r3)
  const uint32_t cursor = REX_LOAD_U16(object + kCursor);       // lhz r10,388(r3)
  // addi r9,r11,-1 ; cmpw cr6,r10,r9 ; bne ; cmplwi cr6,r28,0 ; beq
  if (int32_t(cursor) == int32_t(frames - 1u) && count != 0) {
    uint32_t cell = object + kSlotBase;                         // addi r11,r3,236
    for (uint32_t k = 0; k < count; k++) {                      // mtctr r28 ; bdnz
      ctx.fpscr.disableFlushModeUnconditional();
      const double sum = LoadSingle(base, cell + 4);            // lfs f0,4(r11)
      StoreSingle(base, cell + 36, sum);                        // stfs f0,36(r11)
      StoreSingle(base, cell + 4, floor_value);                 // stfsu f10,4(r11)
      cell += 4;
    }
  }

  // loc_82B37680: advance the ring cursor, as `(cursor + 1) mod frames` by an actual divide.
  const uint32_t cursor_now = REX_LOAD_U16(object + kCursor);   // lhz r11,388(r3)
  const uint32_t frames_now = REX_LOAD_U32(object + kFrames);   // lwz r10,376(r3)
  const uint32_t next = uint32_t(cursor_now + 1u);              // addi r9,r11,1
  // twllei r10,0 -- traps on a zero ring length. The lifted line's second clause (u32 < 0) is
  // vacuous and is not reproduced.
  if (int32_t(frames_now) == 0) ppc_trap(ctx, base, 0);
  const uint32_t wrapped = next & 0xFFFFu;                      // clrlwi r8,r9,16
  const uint32_t doubled = __builtin_rotateleft32(wrapped, 1);  // rotlwi r11,r8,1
  const uint32_t quotient = uint32_t(                           // divw r7,r8,r10
      (int32_t(frames_now) && !(int32_t(wrapped) == INT32_MIN && int32_t(frames_now) == -1))
          ? int32_t(wrapped) / int32_t(frames_now)
          : 0);
  const uint32_t guard = doubled - 1u;                          // addi r5,r11,-1
  const uint32_t product = quotient * frames_now;               // mullw r6,r7,r10 (low 32 bits)
  const uint32_t remainder = wrapped - product;                 // subf r4,r6,r8
  const uint32_t overflow = frames_now & ~guard;                // andc r11,r10,r5
  REX_STORE_U16(object + kCursor, uint16_t(remainder));         // sth r4,388(r3)
  // twlgei r11,-1 -- the divide's overflow check; its second clause is vacuous too.
  if (int32_t(overflow) == -1) ppc_trap(ctx, base, 0);
}

// True when [a, a+alen) and [b, b+blen) share a byte.
bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

// The five header fields the channel loop reloads after storing. A declared span covering one of
// them would shift the remaining addresses mid-call and make the enumeration below wrong, so it
// is checked rather than assumed.
bool ClearsControlFields(uint32_t addr, uint32_t object) {
  return !Overlaps(addr, 4, object + kChannelCount, 1) &&
         !Overlaps(addr, 4, object + kFrames, 4) &&
         !Overlaps(addr, 4, object + kLevelHistory, 2) &&
         !Overlaps(addr, 4, object + kPeakHistory, 2) &&
         !Overlaps(addr, 4, object + kCursor, 2);
}

// The write set, from entry state only. Leaf, so there are no callee writes to fold in, and the
// two red-zone scratch arrays plus the int-to-double bridge are this function's own frame.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t desc = ctx.r4.u32;
  const uint32_t count = REX_LOAD_U8(object + kChannelCount);
  if (count > kMaxChannels) return false;  // the original overruns its own red zone past eight

  const uint32_t frames = REX_LOAD_U32(object + kFrames);
  const uint32_t cursor = REX_LOAD_U16(object + kCursor);
  const uint32_t level_row = object + REX_LOAD_U16(object + kLevelHistory);
  const uint32_t peak_row = object + REX_LOAD_U16(object + kPeakHistory);

  // Four parallel per-channel arrays and the cursor. At eight channels the four reach 0x170, so
  // they cannot touch the header fields at 0x178 and above.
  spec.write(object + kRmsSum, 4 * count);
  spec.write(object + kLevel, 4 * count);
  spec.write(object + kPeak, 4 * count);
  spec.write(object + kPeakHold, 4 * count);
  spec.write(object + kCursor, 2);

  // One slot in each ring per channel, at ((frames * channel) + cursor) * 4. The cursor is
  // reloaded every channel but only written after the loop, so it is constant across it.
  for (uint32_t channel = 0; channel < count; channel++) {
    const uint32_t index = frames * channel + cursor;
    const uint32_t slot = (index << 2) & 0xFFFFFFFCu;
    if (!ClearsControlFields(slot + level_row, object)) return false;
    if (!ClearsControlFields(slot + peak_row, object)) return false;
    spec.write(slot + level_row, 4);
    spec.write(slot + peak_row, 4);
  }

  spec.read(object + kChannelCount, 1);
  spec.read(object + kRmsSum, kPeakHold + 4 * kMaxChannels - kRmsSum);
  spec.read(object + kFrames, 4);
  spec.read(object + kLevelHistory, 6);  // 384, 386 and 388
  spec.read(desc + kChanBase, 12);       // +4 base and +14 stride
  spec.read(kZeroSingle, 4);
  spec.read(kRmsScale, 4);
  const uint32_t first = REX_LOAD_U32(desc + kChanBase);
  const uint32_t step = __builtin_rotateleft32(REX_LOAD_U16(desc + kChanStride), 2);
  // Recorded, never compared, so a nonsense ring length is capped rather than rejected.
  const uint32_t scan = frames <= 4096 ? 4 * frames : 4 * 4096;
  for (uint32_t channel = 0; channel < count; channel++) {
    spec.read(first + step * channel, kBlockBytes);          // the metered block
    spec.read(peak_row + 4 * count * channel, scan);         // the peak ring scan
  }
  return true;
}

}  // namespace port_82B373C8
SKATE3_PORT(82B373C8, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
