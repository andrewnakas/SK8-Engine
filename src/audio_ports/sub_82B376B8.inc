// sub_82B376B8  role: the metering tick -- count off the interval at +380, and when it expires re-derive the ring length from the current rate, clear both history rings, then run the meters and re-expand the parameter blocks   lifted: skate3_recomp.67.cpp:62133 (171 lines)
// STATUS: verified
// calls/session: boot 23  play 10147  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B376B8.md
namespace port_82B376B8 {

// The meter object (r3). Every name below is sub_82B373C8.inc's or sub_82B370E8.inc's for the
// same cell -- this is the same object all three take, which is what makes the mirror below
// possible. docs/rw_audio_structs.h names none of it.
constexpr uint32_t kChannels = 42;       // u8   sub_82B373C8's kChannelCount, sub_82B370E8's kLayout
constexpr uint32_t kMeterArrays = 240;   // sub_82B373C8's kRmsSum; kLevel/kPeak/kPeakHold follow
constexpr uint32_t kRateCache = 368;     // f32  the rate this object was last configured for
constexpr uint32_t kHistorySeconds = 372;  // f32 the history length the two counts below come from
constexpr uint32_t kFrames = 376;        // u32  sub_82B373C8's kFrames: ring length in blocks
constexpr uint32_t kInterval = 380;      // u32  how many ticks to skip between meter runs
constexpr uint32_t kLevelHistory = 384;  // u16  byte offset, in this object, of the level ring
constexpr uint32_t kPeakHistory = 386;   // u16  same, for the peak ring
constexpr uint32_t kCursor = 388;        // u16  sub_82B373C8's kCursor
constexpr uint32_t kTicks = 390;         // u16  the interval counter

// The descriptor (r4).
constexpr uint32_t kDescFormat = 40;    // rw_ptr, RE-READ mid-call; its +12 is the rate
constexpr uint32_t kDescChannels = 28;  // rw_ptr, sub_82B373C8's r4 (its kChanBase/kChanStride)
constexpr uint32_t kFormatRate = 12;    // f32 on the format object

// The constant pool at 0x822F8600 -- sub_82B2FE00.inc's kPool. Computed, not eyeballed:
// lis r11,-32208 -> ((-32208 & 0xFFFF) << 16) = 0x82300000 ; addi r11,r11,-31232 -> 0x822F8600.
constexpr uint32_t kPool = 0x822F8600;
constexpr uint32_t kBlocksPerHz = kPool + 2156;   // lfs f12,2156(r11) -> 0x822F8E6C
constexpr uint32_t kMaxBlockRate = kPool + 444;   // lfs f12,444(r11)  -> 0x822F87BC
// lis r9,-32246 -> 0x820A0000 ; lfs f13,-26788(r9) -> 0x820A0000 - 0x68A4 = 0x8209975C, which
// several ports in this tree read out of the validated image dump as 0.5f.
constexpr uint32_t kHalfSingle = 0x8209975C;

// sub_82B373C8 overruns its own red zone past eight channels, so its port refuses there; this
// function calls it unconditionally, so the same bound applies here.
constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kMeterArrayBytes = 4 * kMaxChannels * 4;  // 240..368, all four arrays
constexpr uint32_t kParamBlocks = 48;       // sub_82B370E8's GroupDest(0)
constexpr uint32_t kParamBlockBytes = 192;  // its three groups (48..192) plus the tails (..240)

constexpr uint32_t kFrameSize = 128;  // stwu r1,-128(r1)

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

double MulSingle(double a, double b) { return double(float(a * b)); }
double FmaSingle(double a, double b, double c) { return double(float(std::fma(a, b, c))); }

// fctiwz then stfd/lwz of the low word: NaN gives 0x80000000, above INT_MAX saturates, and only
// the low 32 bits of the spilled doubleword reach the register.
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return uint32_t(uint64_t(fixed) & 0xFFFFFFFFu);
}

// fctidz then the same spill: NaN gives the 64-bit sign bit, above LLONG_MAX saturates, and the
// reload takes the low word of the 64-bit result.
uint32_t FctidzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x8000000000000000ULL)
                            : (value > double(LLONG_MAX))
                                  ? int64_t(LLONG_MAX)
                                  : int64_t(simde_mm_cvttsd_si64(simde_mm_load_sd(&value)));
  return uint32_t(uint64_t(fixed) & 0xFFFFFFFFu);
}

// The ring byte count: round(seconds * maxBlockRate) channels wide, four bytes each, with a
// one-entry-per-channel floor when the product comes out zero. Broken out because Windows() has
// to derive exactly the same number to size the two memsets, and one divergent bit there is a
// write outside a window.
uint32_t RingBytes(uint8_t* base, uint32_t object, double seconds, double half) {
  const double max_rate = LoadSingle(base, kMaxBlockRate);              // lfs f12,444(r11)
  const uint32_t channels = REX_LOAD_U8(object + kChannels);            // lbz r11,42(r31)
  const double capacity = FmaSingle(seconds, max_rate, half);           // fmadds f0,f0,f12,f13
  const uint32_t blocks = FctidzLowWord(capacity);  // fctidz f13,f0 ; stfd f13,80(r1) ; lwz r10,84(r1)
  // mullw r9,r10,r11 -- the 64-bit product of the sign-extended low words
  const int64_t product = int64_t(int32_t(blocks)) * int64_t(int32_t(channels));
  // rlwinm r30,r9,2,0,29 -- the LOW word shifted left two with the bottom two bits cleared
  const uint32_t bytes = (uint32_t(uint64_t(product)) << 2) & 0xFFFFFFFCu;
  // cmplwi cr6,r30,0 ; bne cr6,0x82b37784 ; rlwinm r30,r11,2,0,29
  return bytes != 0 ? bytes : ((channels << 2) & 0xFFFFFFFCu);
}

REX_FUNC(Native) {
  // stwu r1,-128(r1). Reproduced: sub_82B373C8 works entirely out of its own red zone below r1
  // (r1-320 .. r1), so its scratch has to land where the original put it. The fctiwz/fctidz
  // spills at r1+80 are inside this frame and are done in registers below instead -- the round
  // trip through guest memory is bit-preserving, the same shortcut port_82B373C8 takes for its
  // own std/lfd bridge.
  const uint32_t frame = ctx.r1.u32 - kFrameSize;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;

  const uint32_t interval = REX_LOAD_U32(ctx.r3.u32 + kInterval);  // lwz r10,380(r3)
  const uint64_t object_wide = ctx.r3.u64;                         // mr r31,r3
  const uint32_t object = ctx.r3.u32;
  const uint32_t ticks = REX_LOAD_U16(object + kTicks);            // lhz r11,390(r3)
  const uint32_t desc = ctx.r4.u32;                                // mr r29,r4

  // cmpw cr6,r11,r10 ; bge cr6,0x82b376f0 -- a SIGNED compare of the zero-extended u16 against
  // the word, so a negative interval always takes the work path.
  if (int32_t(ticks) < int32_t(interval)) {
    REX_STORE_U16(object + kTicks, uint16_t(ticks + 1));  // addi r11,r11,1 ; sth r11,390(r3)
    ctx.r3.s64 = 1;                                       // li r3,1
    ctx.r1.u32 = frame + kFrameSize;                      // addi r1,r1,128
    return;
  }

  // loc_82B376F0
  const uint32_t format = REX_LOAD_U32(desc + kDescFormat);  // lwz r11,40(r29)
  ctx.fpscr.disableFlushModeUnconditional();
  const double cached = LoadSingle(base, object + kRateCache);      // lfs f0,368(r31)
  const double rate = LoadSingle(base, format + kFormatRate);       // lfs f13,12(r11)
  // fcmpu cr6,f0,f13 ; beq cr6,0x82b377b4 -- an unordered compare (either operand NaN) is not
  // equal, so a NaN rate rebuilds the rings every tick.
  if (!(cached == rate)) {
    // rotlwi r10,r11,0 is `mr r10,r11`: the same format pointer.
    const double seconds = LoadSingle(base, object + kHistorySeconds);  // lfs f0,372(r31)
    const double rate_again = LoadSingle(base, format + kFormatRate);   // lfs f13,12(r10)
    StoreSingle(base, object + kRateCache, rate_again);                 // stfs f13,368(r31)
    // lwz r8,40(r29) -- the descriptor's format pointer is re-read, after the store above
    const uint32_t format_again = REX_LOAD_U32(desc + kDescFormat);
    const double half = LoadSingle(base, kHalfSingle);                  // lfs f13,-26788(r9)
    const double blocks_per_hz = LoadSingle(base, kBlocksPerHz);        // lfs f12,2156(r11)
    const double rate_now = LoadSingle(base, format_again + kFormatRate);  // lfs f11,12(r8)
    const double blocks_per_second = MulSingle(rate_now, blocks_per_hz);  // fmuls f10,f11,f12
    // fmadds f9,f10,f0,f13 -- one rounding, then fctiwz and the low word of the spill
    const double frames_f = FmaSingle(blocks_per_second, seconds, half);
    const uint32_t frames = FctiwzLowWord(frames_f);  // fctiwz f8 ; stfd f8,80(r1) ; lwz r7,84(r1)
    REX_STORE_U32(object + kFrames, frames);          // stw r7,376(r31)
    // cmpwi cr6,r7,0 ; bgt cr6,0x82b37758 -- the store above stands only when it is positive
    if (!(int32_t(frames) > 0)) {
      REX_STORE_U32(object + kFrames, 1);  // stw r28,376(r31)
    }

    // loc_82B37758
    ctx.fpscr.disableFlushModeUnconditional();
    const uint32_t ring_bytes = RingBytes(base, object, seconds, half);

    // loc_82B37784. add r3,r11,r31 is a 64-bit add of the zero-extended u16 and the object; the
    // memset addresses with the low word. bl 0x82ee5e80 twice -- the XDK memset, [dst, dst+n).
    const uint32_t level_off = REX_LOAD_U16(object + kLevelHistory);  // lhz r11,384(r31)
    skate3::audio::GuestCall(ctx, base, sub_82EE5E80, uint64_t(level_off) + object_wide,
                             uint64_t(0), uint64_t(ring_bytes));
    // lhz r11,386(r31) -- read AFTER the first memset, so a ring that covered this field would
    // change the second destination mid-call. Windows() refuses that case.
    const uint32_t peak_off = REX_LOAD_U16(object + kPeakHistory);
    skate3::audio::GuestCall(ctx, base, sub_82EE5E80, uint64_t(peak_off) + object_wide,
                             uint64_t(0), uint64_t(ring_bytes));
    REX_STORE_U16(object + kCursor, 0);  // li r11,0 ; sth r11,388(r31)
  }

  // loc_82B377B4
  const uint32_t channel_desc = REX_LOAD_U32(desc + kDescChannels);  // lwz r4,28(r29)
  REX_STORE_U16(object + kTicks, 1);                                 // sth r28,390(r31)
  // bl 0x82b373c8 -- (object, channel descriptor); a verified port, so through GuestCall on the
  // live context, where its own hook decides how it runs.
  skate3::audio::GuestCall(ctx, base, sub_82B373C8, object_wide, uint64_t(channel_desc));
  // bl 0x82b370e8 with NO register setup: the original relies on sub_82B373C8 leaving the object
  // in r3, which is a whole-program register-allocation fact and not the ABI (that callee carries
  // the object in r3 throughout and never writes it, and neither does its port). GuestCall with no
  // arguments writes no argument register, so this is exactly the original's machine state.
  skate3::audio::GuestCall(ctx, base, sub_82B370E8);
  ctx.r3.s64 = 1;                   // li r3,1
  ctx.r1.u32 = frame + kFrameSize;  // addi r1,r1,128
}

// True when [a, a+alen) and [b, b+blen) share a byte.
bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

// The header fields that are re-read after one of this call's stores -- +386 between the two
// memsets, and all five of them by sub_82B373C8 and sub_82B370E8 afterwards. Only the two ring
// clears can reach them, and if one does, neither the second memset's address nor the callees'
// window addresses derive from entry state any more: that is the gate-2 exit, checked rather
// than assumed (the same guard port_82B34E08 applies to its reloaded fields).
bool ClearsControlFields(uint32_t object, uint32_t addr, uint32_t len) {
  return !Overlaps(addr, len, object + kChannels, 1) &&
         !Overlaps(addr, len, object + kFrames, 4) &&
         !Overlaps(addr, len, object + kLevelHistory, 2) &&
         !Overlaps(addr, len, object + kPeakHistory, 2) &&
         !Overlaps(addr, len, object + kCursor, 2);
}

// Every byte the call writes, from entry state. This function's own stores are six fixed cells
// plus the two ring clears; the rest is folded in from the two callees' verified ports, whose
// window builders are mirrored here (recomp/src/audio_ports/sub_82B373C8.inc and
// sub_82B370E8.inc) because both run on the state this call has just published.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t desc = ctx.r4.u32;
  if (object == 0 || desc == 0) return false;  // the original would load through null
  const uint32_t interval = REX_LOAD_U32(object + kInterval);
  const uint32_t ticks = REX_LOAD_U16(object + kTicks);
  spec.read(object + kRateCache, 28);  // 368..395: the cache, seconds, frames, interval, the u16s
  spec.read(object + kChannels, 1);

  // The counting path is decided entirely on entry state and writes one halfword.
  if (int32_t(ticks) < int32_t(interval)) {
    spec.write(object + kTicks, 2);
    return true;
  }

  // Past here sub_82B373C8 always runs, and past eight channels it walks off its own red zone --
  // its port refuses there, so this one has to as well.
  const uint32_t channels = REX_LOAD_U8(object + kChannels);
  if (channels > kMaxChannels) return false;

  spec.write(object + kTicks, 2);  // sth r28,390(r31)

  // Predict the resize, because sub_82B373C8's ring addresses are computed from the two fields
  // this call publishes: kFrames and kCursor.
  uint32_t frames = REX_LOAD_U32(object + kFrames);
  uint32_t cursor = REX_LOAD_U16(object + kCursor);
  const uint32_t format = REX_LOAD_U32(desc + kDescFormat);
  if (format == 0) return false;  // lfs f13,12(r11) through null; not comparable
  spec.read(desc + kDescChannels, 4);
  spec.read(desc + kDescFormat, 4);
  spec.read(format + kFormatRate, 4);
  spec.read(kHalfSingle, 4);
  spec.read(kBlocksPerHz, 4);
  spec.read(kMaxBlockRate, 4);

  const double cached = LoadSingle(base, object + kRateCache);
  const double rate = LoadSingle(base, format + kFormatRate);
  if (!(cached == rate)) {
    const double seconds = LoadSingle(base, object + kHistorySeconds);
    const double half = LoadSingle(base, kHalfSingle);
    const double blocks_per_second = MulSingle(rate, LoadSingle(base, kBlocksPerHz));
    frames = FctiwzLowWord(FmaSingle(blocks_per_second, seconds, half));
    if (!(int32_t(frames) > 0)) frames = 1;
    cursor = 0;
    const uint32_t ring_bytes = RingBytes(base, object, seconds, half);
    const uint32_t level_ring = object + REX_LOAD_U16(object + kLevelHistory);
    const uint32_t peak_ring = object + REX_LOAD_U16(object + kPeakHistory);
    // Both clears are inside the object by construction (a u16 offset from it), so neither can
    // wrap; the length is the one thing worth guarding.
    if (uint64_t(level_ring) + ring_bytes > 0x100000000ull) return false;
    if (uint64_t(peak_ring) + ring_bytes > 0x100000000ull) return false;
    if (!ClearsControlFields(object, level_ring, ring_bytes)) return false;
    if (!ClearsControlFields(object, peak_ring, ring_bytes)) return false;
    spec.write(object + kRateCache, 4);  // stfs f13,368(r31)
    spec.write(object + kFrames, 4);     // stw r7,376(r31), possibly twice
    spec.write(level_ring, ring_bytes);  // bl 0x82ee5e80
    spec.write(peak_ring, ring_bytes);   // bl 0x82ee5e80
    spec.write(object + kCursor, 2);     // sth r11,388(r31)
  }

  // sub_82B373C8's write set, restated for the state above. Its four per-channel arrays are
  // declared at their full eight-channel extent as one span, 240..368: the bytes past `channels`
  // are written by neither run and compare equal, and this keeps the span count down.
  spec.write(object + kMeterArrays, kMeterArrayBytes);
  spec.write(object + kCursor, 2);  // it advances the cursor itself, on every path
  const uint32_t level_row = object + REX_LOAD_U16(object + kLevelHistory);
  const uint32_t peak_row = object + REX_LOAD_U16(object + kPeakHistory);
  spec.read(object + kMeterArrays, kMeterArrayBytes);
  for (uint32_t channel = 0; channel < channels && channel < kMaxChannels; channel++) {
    // mullw r8,r8,r31 ; add r7,r8,r9 ; rlwinm r7,r7,2,0,29 -- its own index arithmetic, on the
    // frames and cursor this call published.
    const uint32_t slot = ((frames * channel + cursor) << 2) & 0xFFFFFFFCu;
    if (!ClearsControlFields(object, slot + level_row, 4)) return false;
    if (!ClearsControlFields(object, slot + peak_row, 4)) return false;
    spec.write(slot + level_row, 4);
    spec.write(slot + peak_row, 4);
  }

  // sub_82B370E8's write set. Layouts 1, 2 and 4 fill a subset of 48..192 and 7-and-above fills
  // 48..240; the whole 192-byte region is declared for any layout that writes at all, again to
  // spend bytes rather than spans. Layouts 0, 3 and 5 write nothing.
  const uint32_t layout = channels;  // its kLayout is this object's +42, the same byte
  if (layout == 1 || layout == 2 || layout == 4 || layout >= 6) {
    spec.write(object + kParamBlocks, kParamBlockBytes);
    spec.read(object + 272, 96);  // its three eight-float source blocks, 272..367
  }
  return true;
}

}  // namespace port_82B376B8
SKATE3_PORT(82B376B8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
