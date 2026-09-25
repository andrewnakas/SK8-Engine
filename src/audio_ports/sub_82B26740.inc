// sub_82B26740  role: run the per-source shelf filter -- derive a corner frequency from the source's own rate and the mixer's, and either rebuild the five biquad coefficients when that corner or the gain moved and filter every channel of the buffer pair through sub_82B43AF8, or, when the corner is past its limit, clear the per-channel histories once and leave the audio alone   lifted: skate3_recomp.67.cpp:21061 (263 lines)
// STATUS: verified
// calls/session: boot 24936  play 29673  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B26740.md
namespace port_82B26740 {

// r3 -- the source object. Nothing in docs/rw_audio_structs.h names it, so the offsets stay plain.
constexpr uint32_t kChannels = 42;       // u8   channel count, RELOADED every loop iteration
constexpr uint32_t kFrequency = 52;      // f32  the source's own corner, before normalising
constexpr uint32_t kGain = 60;           // f32  the shelf gain handed to sub_82B43D78 in f2
constexpr uint32_t kHistory = 64;        // f32[4] per channel: the biquad state sub_82B43AF8 keeps
constexpr uint32_t kHistoryBytes = 16;
constexpr uint32_t kEngaged = 192;       // s32  1 while the filter is running, 0 once cleared
constexpr uint32_t kCoefficients = 196;  // f32[5] sub_82B43D78's [a1, a2, b0, b1, b2] / a0
constexpr uint32_t kCachedCorner = 216;  // f32  the corner the coefficients were built for
constexpr uint32_t kCachedGain = 220;    // f32  the gain they were built for
// [+192, +224): the flag, the five coefficients and the two cached singles, in one span.
constexpr uint32_t kControlSpan = 32;
// The histories end exactly where the flag begins, so eight is the object's own channel limit.
constexpr uint32_t kMaxChannels = 8;

// r4 -- the two-entry buffer-descriptor pair, the same shape sub_82B29BE0 swaps and sub_82B34E08
// swaps. +28 is the side this call reads FROM, +32 the side it writes INTO.
constexpr uint32_t kPairIn = 28;
constexpr uint32_t kPairOut = 32;
constexpr uint32_t kPairFormat = 40;  // u32  the format record whose +12 is the mixer's rate

// On a descriptor -- the same two fields sub_82B29BE0 and sub_82B29AF0 use, same arithmetic.
constexpr uint32_t kBufferBase = 4;     // u32  channel 0's float array
constexpr uint32_t kChannelStride = 14; // u16  singles between one channel and the next
constexpr uint32_t kDescriptorFields = 12;  // [+4, +16) covers both, and both are re-read

constexpr uint32_t kFrameBytes = 144;      // stwu r1,-144(r1)
constexpr uint32_t kSamples = 256;         // li r7,256
constexpr uint32_t kBytesPerChannel = 4 * kSamples;

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset, computed rather than read off
// by eye. The pool base 0x822F8600 is the one sub_82B43AF8.inc and sub_82B2FEA8 name kPool; kOne is
// sub_82B43D78.inc's kOne and kZero is sub_82B269C0.inc's kZeroFloat -- all three derived
// independently here and agreeing.
constexpr uint32_t kRatioScale = 0x820B411C;  // lis -32245 ; lfs 16668
constexpr uint32_t kRatioLimit = 0x822F8E80;  // lis -32208 ; addi -31232 ; lfs 2176
constexpr uint32_t kRatioFloor = 0x822F8E7C;  // the same pool, lfs 2172
constexpr uint32_t kOne = 0x8231A844;         // lis -32206 ; lfs -22460
constexpr uint32_t kZero = 0x82165A10;        // lis -32234 ; lfs 23056

// MIRRORED read-only constants of the two callees, for the recorded read set. sub_82B43AF8.inc is in
// aggregator ports_68a and sub_82B43D78.inc likewise, while this file is in ports_67a -- different
// translation units, so the values are copied rather than referenced.
constexpr uint32_t kDenormBias = 0x822F87B0;  // sub_82B43AF8.inc: kPool + 432
constexpr uint32_t kAlphaScale = 0x822F8E50;  // sub_82B43D78.inc
constexpr uint32_t kTwo = 0x82060C50;         // sub_82B43D78.inc
constexpr uint32_t kMinusTwo = 0x82094178;    // sub_82B43D78.inc

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }

// mullw ; rlwinm 2,0,29 ; add -- one channel's float array. COPIED from
// recomp/src/audio_ports/sub_82B29BE0.inc, which is in this same aggregator (ports_67a) but at a
// HIGHER address, so it is included after this file and cannot be referenced (probe/ports/lint.py
// check 8). mullw is the 64-bit product of two sign-extended words; the rlwinm is a plain 32-bit
// shift left by two; the add is 64-bit on two zero-extended words, so the sum can carry past bit 31
// and only the callee's use of the low half truncates it.
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
  const uint32_t self = ctx.r3.u32;  // mr r30,r3
  const uint32_t pair = ctx.r4.u32;  // mr r26,r4

  // stwu r1,-144(r1). Reproduced -- not because anything here needs the guest stack to have moved,
  // but because both callees spill below whatever r1 they are entered with (sub_82B43D78's own
  // stwu of 128, sub_82B43AF8's of 192), and if r1 sat 144 bytes higher during the replay those
  // spills would land at different addresses than in the original run. f31 and r26-r31 are never
  // written here, so the stfd f31,-64(r1) spill and __savegprlr_26 have nothing to reproduce.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  ctx.fpscr.disableFlushModeUnconditional();  // emitted by the stfd f31,-64(r1)
  const uint32_t format = REX_LOAD_U32(pair + kPairFormat);     // lwz r10,40(r4)
  const double nominal = LoadSingle(base, self + kFrequency);   // lfs f0,52(r3)
  const double rate = LoadSingle(base, format + 12);            // lfs f13,12(r10)
  const double ratio = DivSingle(nominal, rate);                // fdivs f12,f0,f13
  const double limit = LoadSingle(base, kRatioLimit);           // lfs f13,2176(r11)
  // fmuls f31,f12,f0 with f0 = the pool scale.
  double corner = MulSingle(ratio, LoadSingle(base, kRatioScale));

  // fcmpu cr6,f31,f13 ; bge cr6,0x82b26878. An unordered compare (a NaN corner) clears cr6.lt and
  // so takes the clear-and-bypass path, which is what C++ `<` does with a NaN operand. The gain is
  // loaded only on this side of that branch, as the original does.
  double gain = 0.0;
  bool filter = false;
  if (corner < limit) {
    gain = LoadSingle(base, self + kGain);  // lfs f2,60(r3)
    // fcmpu cr6,f2,f0 ; beq cr6,0x82b26878 -- a unity gain is a no-op shelf, so it bypasses too;
    // a NaN gain compares unequal and keeps filtering, as `!=` does.
    filter = gain != LoadSingle(base, kOne);  // lfs f0,-22460(r10)
  }
  if (filter) {
    if (static_cast<int32_t>(REX_LOAD_U32(self + kEngaged)) == 0) {  // lwz r10,192(r3) ; cmpwi
      REX_STORE_U32(self + kEngaged, 1);  // li r10,1 ; stw r10,192(r3)
    }

    // loc_82B267B0 -- floor the corner.
    ctx.fpscr.disableFlushModeUnconditional();
    const double floor_value = LoadSingle(base, kRatioFloor);  // lfs f0,2172(r11)
    if (corner < floor_value) corner = floor_value;            // bge cr6,0x82b267c0 ; fmr f31,f0

    // loc_82B267C0 -- rebuild the coefficients only when the pair (corner, gain) moved.
    ctx.fpscr.disableFlushModeUnconditional();
    if (!(corner == LoadSingle(base, self + kCachedCorner) &&
          gain == LoadSingle(base, self + kCachedGain))) {
      // loc_82B267D8: bl 0x82b43d78 -- the five normalised shelf coefficients, from f1 = the corner
      // and f2 = the gain, which is still in f2 from the compare above.
      ctx.fpscr.disableFlushModeUnconditional();
      ctx.f1.f64 = corner;  // fmr f1,f31
      ctx.f2.f64 = gain;
      skate3::audio::GuestCall(ctx, base, sub_82B43D78, self + kCoefficients);
      ctx.fpscr.disableFlushModeUnconditional();
      // lfs f0,60(r30) -- the gain is RELOADED here rather than kept in f2, and stored as the cache.
      const double published = LoadSingle(base, self + kGain);
      StoreSingle(base, self + kCachedCorner, corner);    // stfs f31,216(r30)
      StoreSingle(base, self + kCachedGain, published);   // stfs f0,220(r30)
    }

    // loc_82B267F0 -- filter every channel. Both descriptors are read once, here.
    uint32_t channels = REX_LOAD_U8(self + kChannels);        // lbz r11,42(r30)
    const uint32_t in_desc = REX_LOAD_U32(pair + kPairIn);    // lwz r29,28(r26)
    const uint32_t out_desc = REX_LOAD_U32(pair + kPairOut);  // lwz r28,32(r26)
    if (channels != 0) {  // cmplwi cr6,r11,0 ; beq cr6,0x82b26858
      const uint32_t coefficients = self + kCoefficients;  // addi r27,r30,196
      uint32_t history = self + kHistory;                  // addi r3,r30,64
      uint32_t index = 0;                                  // li r31,0
      do {  // loc_82B26810
        // Every one of these four fields is re-read on every iteration, not hoisted.
        const uint64_t in_stride = REX_LOAD_U16(in_desc + kChannelStride);    // lhz r11,14(r29)
        const uint64_t out_stride = REX_LOAD_U16(out_desc + kChannelStride);  // lhz r9,14(r28)
        const uint64_t in_base = REX_LOAD_U32(in_desc + kBufferBase);         // lwz r10,4(r29)
        const uint64_t out_base = REX_LOAD_U32(out_desc + kBufferBase);       // lwz r8,4(r28)
        skate3::audio::GuestCall(ctx, base, sub_82B43AF8, uint64_t(history),
                                 ChannelAddress(out_stride, index, out_base),
                                 ChannelAddress(in_stride, index, in_base),
                                 uint64_t(coefficients), uint64_t(kSamples));  // bl 0x82b43af8
        channels = REX_LOAD_U8(self + kChannels);  // lbz r11,42(r30) -- reloaded after the call
        index++;                                   // addi r31,r31,1
        history += kHistoryBytes;                  // addi r3,r3,16
      } while (index < channels);                  // cmplw cr6,r31,r11 ; blt
    }

    // loc_82B26858 -- swap the pair. Both words are RELOADED here rather than reusing r28/r29.
    const uint32_t swap_out = REX_LOAD_U32(pair + kPairOut);  // lwz r11,32(r26)
    const uint32_t swap_in = REX_LOAD_U32(pair + kPairIn);    // lwz r10,28(r26)
    REX_STORE_U32(pair + kPairIn, swap_out);                  // stw r11,28(r26)
    REX_STORE_U32(pair + kPairOut, swap_in);                  // stw r10,32(r26)
    ctx.r3.s64 = 1;                                           // li r3,1
  } else {
    // loc_82B26878 -- the corner is past its limit (or the gain is unity): the filter is bypassed.
    // cmpwi cr6,r11,1 -- a SIGNED compare, so only exactly 1 clears.
    if (static_cast<int32_t>(REX_LOAD_U32(self + kEngaged)) == 1) {
      uint32_t channels = REX_LOAD_U8(self + kChannels);  // lbz r11,42(r30)
      if (channels != 0) {
        ctx.fpscr.disableFlushModeUnconditional();
        const double zero = LoadSingle(base, kZero);  // lfs f0,23056(r9)
        uint32_t cursor = self + kGain;               // addi r11,r30,60
        uint32_t done = 0;                            // li r10,0
        do {  // loc_82B268A0 -- four singles per channel, the fourth through stfsu
          ctx.fpscr.disableFlushModeUnconditional();
          StoreSingle(base, cursor + 4, zero);   // stfs f0,4(r11)
          done++;                                // addi r10,r10,1
          StoreSingle(base, cursor + 8, zero);   // stfs f0,8(r11)
          StoreSingle(base, cursor + 12, zero);  // stfs f0,12(r11)
          cursor += kHistoryBytes;               // stfsu f0,16(r11) updates first
          StoreSingle(base, cursor, zero);
          channels = REX_LOAD_U8(self + kChannels);  // lbz r9,42(r30) -- reloaded
        } while (done < channels);                   // cmplw cr6,r10,r9 ; blt
      }
      // loc_82B268C0
      REX_STORE_U32(self + kEngaged, 0);  // li r11,0 ; stw r11,192(r30)
    }
    // loc_82B268C8 -- the cache is published on this path too, so the next call sees the corner
    // that led here.
    ctx.fpscr.disableFlushModeUnconditional();
    const double published = LoadSingle(base, self + kGain);  // lfs f0,60(r30)
    ctx.r3.s64 = 1;                                           // li r3,1
    StoreSingle(base, self + kCachedCorner, corner);          // stfs f31,216(r30)
    StoreSingle(base, self + kCachedGain, published);         // stfs f0,220(r30)
  }

  ctx.r1.u32 = entry_sp;  // addi r1,r1,144
}

// The write set, as the union of both paths -- the path itself is NOT predicted here, deliberately:
// choosing it needs the `fdivs`/`fmuls` at the top of the function, and running that float work
// inside Windows() would execute it under the guest's FPSCR before the original does, for no gain.
// Declaring both paths is safe: a byte a path did not write compares equal anyway.
//
//   * the per-channel histories, [+64, +64 + 16*channels) -- written by this function's own stfs
//     loop on the bypass path and by sub_82B43AF8's 16-byte history block on the filtering path.
//   * [+192, +224) -- the engaged flag, the five coefficients sub_82B43D78 writes, and the two
//     cached singles. One span; the coefficients are exactly [+196, +216).
//   * the two pair words, [+28, +36).
//   * one destination channel per channel index, exactly [out, out+1024): sub_82B43AF8 is a scalar
//     biquad with no dcbzl and no alignment widening, so the span is its count*4 and nothing more.
//     MIRRORS recomp/src/audio_ports/sub_82B43AF8.inc and recomp/src/audio_ports/sub_82B43D78.inc
//     (both aggregator ports_68a, a different translation unit): a change to either port's write
//     set requires a change here.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  if (self == 0 || pair == 0) return false;  // the original would store through null

  // Eight is the object's own limit: the histories run from +64 and the flag sits at +192, so a
  // ninth channel would put a history block on top of the coefficients. It also keeps the channel
  // spans inside the span budget.
  const uint32_t channels = REX_LOAD_U8(self + kChannels);
  if (channels > kMaxChannels) return false;

  const uint32_t in_desc = REX_LOAD_U32(pair + kPairIn);
  const uint32_t out_desc = REX_LOAD_U32(pair + kPairOut);
  // Read unconditionally on the filtering path, which is not predicted here.
  if (in_desc == 0 || out_desc == 0) return false;

  // Everything re-read while the call runs. A declared write covering any of it would stop the
  // addresses and the trip count deriving from entry state -- the gate-2 exit, so it is checked.
  const uint32_t control_addr[3] = {self + kChannels, in_desc + kBufferBase, out_desc + kBufferBase};
  const uint32_t control_len[3] = {1, kDescriptorFields, kDescriptorFields};

  struct Span {
    uint32_t addr;
    uint32_t len;
  };
  Span spans[skate3::audio::kPortMaxSpans];
  uint32_t span_count = 0;
  auto add = [&](uint64_t addr, uint32_t len) {
    if (len == 0) return true;
    if (addr + len > 0x100000000ull) return false;  // would wrap the address space
    for (uint32_t k = 0; k < 3; k++) {
      if (!Disjoint(addr, len, control_addr[k], control_len[k])) return false;
    }
    if (span_count >= skate3::audio::kPortMaxSpans) return false;
    spans[span_count].addr = uint32_t(addr);
    spans[span_count].len = len;
    span_count++;
    return true;
  };

  if (!add(self + kHistory, kHistoryBytes * channels)) return false;
  if (!add(self + kEngaged, kControlSpan)) return false;
  if (!add(pair + kPairIn, 8)) return false;

  const uint64_t out_stride = REX_LOAD_U16(out_desc + kChannelStride);
  const uint64_t out_base = REX_LOAD_U32(out_desc + kBufferBase);
  for (uint32_t index = 0; index < channels && index < kMaxChannels; index++) {
    if (!add(ChannelAddress(out_stride, index, out_base), kBytesPerChannel)) return false;
  }

  // A destination channel must also not land on the object block or the pair words: the histories
  // and the coefficients are read back by sub_82B43AF8 on the NEXT iteration, and the pair words are
  // re-read for the swap.
  for (uint32_t i = 0; i < span_count; i++) {
    for (uint32_t k = 0; k < span_count; k++) {
      if (i == k) continue;
      if (!Disjoint(spans[i].addr, spans[i].len, spans[k].addr, spans[k].len)) return false;
    }
  }

  for (uint32_t i = 0; i < span_count; i++) spec.write(spans[i].addr, spans[i].len);

  // Reads, recorded for replay only, never compared.
  spec.read(self + kChannels, 1);
  spec.read(self + kFrequency, 4);
  spec.read(self + kGain, 4);
  spec.read(self + kEngaged, kControlSpan);  // read before it is written, on both paths
  spec.read(pair + kPairFormat, 4);
  spec.read(REX_LOAD_U32(pair + kPairFormat) + 12, 4);
  spec.read(kRatioScale, 4);
  spec.read(kRatioLimit, 4);
  spec.read(kRatioFloor, 4);
  spec.read(kOne, 4);
  spec.read(kZero, 4);
  spec.read(kDenormBias, 4);   // sub_82B43AF8's per-sample bias
  spec.read(kAlphaScale, 4);   // the three sub_82B43D78 constants beyond kOne
  spec.read(kTwo, 4);
  spec.read(kMinusTwo, 4);
  // The image's sine and cosine, which the coefficient build reaches: their table, 1.0 and the NaN
  // cell. Declared for replay only (reads cost nothing against the budget); without them every
  // vector on the rebuild path is unreplayable. Values from recomp/src/audio_ports/sub_82F4DED0.inc.
  spec.read(0x82FB4E20u, 120);
  spec.read(0x82010108u, 8);
  spec.read(0x82FB58C8u, 8);
  spec.read(in_desc + kBufferBase, kDescriptorFields);
  spec.read(out_desc + kBufferBase, kDescriptorFields);
  // The source channels, summarised by one span over their extremes rather than one each, to leave
  // room in the input slots.
  const uint64_t in_stride = REX_LOAD_U16(in_desc + kChannelStride);
  const uint64_t in_base = REX_LOAD_U32(in_desc + kBufferBase);
  uint32_t lo = uint32_t(in_base);
  uint64_t hi = uint64_t(lo) + kBytesPerChannel;
  for (uint32_t index = 1; index < channels && index < kMaxChannels; index++) {
    const uint32_t begin = uint32_t(ChannelAddress(in_stride, index, in_base));
    if (begin < lo) lo = begin;
    if (uint64_t(begin) + kBytesPerChannel > hi) hi = uint64_t(begin) + kBytesPerChannel;
  }
  if (hi > 0x100000000ull) hi = 0x100000000ull;  // a recorded read never runs off the end
  spec.read(lo, uint32_t(hi - lo));
  return true;
}

}  // namespace port_82B26740
SKATE3_PORT(82B26740, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
