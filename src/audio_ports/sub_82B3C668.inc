// sub_82B3C668  role: add each pending delta, times a 16-entry pool curve, into the head of its row, then clear the delta   lifted: skate3_recomp.68.cpp:7552 (292 lines)
// STATUS: verified
// calls/session: boot 4266  play 4560  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3C668.md
namespace port_82B3C668 {

// Arguments: r3 = the row-owner object, r4 = an array of `count` singles, r5 = count.
// Nothing in docs/rw_audio_structs.h names this object. Two fields are read, both every
// iteration:
//   +4   rw_ptr  row base
//   +14  u16     row pitch in SINGLES; the byte pitch is 4x this
constexpr uint32_t kRowBase = 4;
constexpr uint32_t kRowStride = 14;

// Sixteen consecutive singles in the audio pool, loaded once before the loop:
// lis r11,-32208 ; addi r7,r11,-31232 -> 0x822F8600 (the same kPool cell sub_82B2FE00 names),
// then lfs at 1028, 1032, ... 1088. f30 is +1028 and f0 is +1088, so tap k is +1028 + 4k and the
// body applies them in ascending order. Addresses computed as ((imm & 0xFFFF) << 16) + offset,
// never read off by eye. No value is assumed here: the body loads them through the guest map.
constexpr uint32_t kPool = 0x822F8600;
constexpr uint32_t kTapTable = kPool + 1028;  // 0x822F8A04 .. 0x822F8A40 inclusive
constexpr uint32_t kTapCount = 16;
constexpr uint32_t kRowBytes = 4 * kTapCount;  // 64: every row gets exactly its first 16 singles

// lis r8,-32234 ; lfs f29,23056(r8) -- the 0.0f cell three verified ports already read. Stored
// over each delta once it has been consumed.
constexpr uint32_t kZeroSingle = 0x82165A10;

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// stfs / stfsu: the double rounded back to a single.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

// fmadds f28,f28,fC,f27 -- one fused product, then a single rounding. Every one of the sixteen
// lifted lines has the operands in this order (delta, tap, accumulator); kept, because a fused
// multiply-add is only commutative in its product for non-NaN operands.
inline double Fmadds(double delta, double tap, double acc) {
  return double(float(std::fma(delta, tap, acc)));
}

// mullw r7,r8,r9 ; rlwinm r8,r7,2,0,29 -- the 64-bit product of the sign-extended words, then the
// LOW word shifted left two with the bottom two bits cleared. The product is kept 64-bit and
// truncated at the mask, as the lifted pair does.
inline uint32_t RowOffset(uint32_t stride, uint32_t index) {
  const int64_t product = int64_t(int32_t(stride)) * int64_t(int32_t(index));
  return (uint32_t(uint64_t(product)) << 2) & 0xFFFFFFFCu;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t count = ctx.r5.u32;  // cmplwi cr6,r5,0 tests the low word; mtctr r5 counts it
  if (count == 0) return;             // beq cr6,0x82b3c7fc -- not one store

  // The taps and the fill are hoisted out of the loop by the original, so a store that aliased
  // the pool would not be seen by later iterations. Hoisted here too.
  ctx.fpscr.disableFlushModeUnconditional();       // lfs f29,23056(r8)
  const double fill = LoadSingle(base, kZeroSingle);
  double tap[kTapCount];
  for (uint32_t k = 0; k < kTapCount; k++) {       // lfs f0,1088(r7) down to lfs f30,1028(r7)
    tap[k] = LoadSingle(base, kTapTable + 4 * k);
  }

  uint32_t cursor = ctx.r4.u32 - 4;                // addi r10,r4,-4: the stfsu pre-increments
  for (uint32_t i = 0; i < count; i++) {           // loc_82B3C6DC .. bdnz 0x82b3c6dc
    const uint32_t stride = REX_LOAD_U16(object + kRowStride);  // lhz r8,14(r3) -- every iteration
    ctx.fpscr.disableFlushModeUnconditional();                  // lfs f28,4(r10)
    const uint32_t row_base = REX_LOAD_U32(object + kRowBase);  // lwz r11,4(r3) -- every iteration
    // add r11,r8,r11 on two zero-extended words: the sum is 64-bit and the stores keep the low
    // word, so the truncation is explicit here.
    const uint32_t row = uint32_t(uint64_t(RowOffset(stride, i)) + uint64_t(row_base));
    for (uint32_t k = 0; k < kTapCount; k++) {
      // The delta is RELOADED from 4(r10) before every one of the sixteen fmadds, so a row that
      // overlapped it would feed the changed value forward. Reproduced.
      const double delta = LoadSingle(base, cursor + 4);   // lfs f28/f27,4(r10)
      const double acc = LoadSingle(base, row + 4 * k);    // lfs f28/f27,4k(r11)
      StoreSingle(base, row + 4 * k, Fmadds(delta, tap[k], acc));  // fmadds ; stfs f28,4k(r11)
    }
    const uint32_t ea = cursor + 4;  // stfsu f29,4(r10): store at r10+4, then r10 = r10+4
    StoreSingle(base, ea, fill);
    cursor = ea;
  }
}

// Every byte written, from entry state. Both address fields are re-read inside the loop, so a
// write span covering either one makes the addresses unknowable before the call: that is the
// gate-2 exit, checked below rather than assumed away.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t deltas = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;

  spec.read(object + kRowBase, 4);
  spec.read(object + kRowStride, 2);
  spec.read(kTapTable, kRowBytes);
  spec.read(kZeroSingle, 4);
  if (count == 0) return true;  // the beq path writes nothing

  const uint32_t stride = REX_LOAD_U16(object + kRowStride);
  const uint32_t row_base = REX_LOAD_U32(object + kRowBase);
  const uint32_t pitch = stride * 4u;  // <= 0x3FFFC; the u16 cannot overflow this

  // The delta array: one single per iteration, ascending from r4, so one span.
  const uint64_t delta_len = 4ull * count;
  if (uint64_t(deltas) + delta_len > 0x100000000ull) return false;  // the cursor would wrap

  // The rows. pitch == 0 puts every iteration on the same 64 bytes; pitch <= 64 makes the rows
  // abut or overlap, so their union is one span; otherwise they are disjoint and cost one span
  // each, which only fits the 32-span budget for small counts.
  uint32_t row_lo = row_base;
  uint64_t row_len = kRowBytes;
  bool merged = true;
  if (pitch > kRowBytes) {
    merged = false;
    // 64-bit, so that count == 0xFFFFFFFF cannot wrap its way past the cap.
    if (uint64_t(count) + 1 > skate3::audio::kPortMaxSpans) return false;  // a span per row, plus the deltas
  } else if (pitch != 0) {
    const uint64_t last = uint64_t(pitch) * (count - 1);
    // Guard the monotone assumption: if the index math wrapped, the union is not this interval.
    if (last > 0xFFFFFFFFull || RowOffset(stride, count - 1) != uint32_t(last)) return false;
    row_len = last + kRowBytes;
  }
  if (merged && uint64_t(row_lo) + row_len > 0x100000000ull) return false;

  // 32 KB of windows, and the merged case can ask for far more than that.
  const uint64_t total = delta_len + (merged ? row_len : uint64_t(kRowBytes) * count);
  if (total > skate3::audio::kPortWatchCap) return false;

  // Both re-read fields must stay outside every write span, or neither the row addresses nor the
  // pitch derive from entry state.
  auto hits = [](uint32_t addr, uint32_t len, uint32_t lo, uint64_t span) {
    return uint64_t(addr) < uint64_t(lo) + span && uint64_t(lo) < uint64_t(addr) + len;
  };
  if (hits(object + kRowBase, 4, deltas, delta_len)) return false;
  if (hits(object + kRowStride, 2, deltas, delta_len)) return false;
  if (merged) {
    if (hits(object + kRowBase, 4, row_lo, row_len)) return false;
    if (hits(object + kRowStride, 2, row_lo, row_len)) return false;
    spec.write(row_lo, uint32_t(row_len));
  } else {
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t row = uint32_t(uint64_t(RowOffset(stride, i)) + uint64_t(row_base));
      if (uint64_t(row) + kRowBytes > 0x100000000ull) return false;  // this row would wrap
      if (hits(object + kRowBase, 4, row, kRowBytes)) return false;
      if (hits(object + kRowStride, 2, row, kRowBytes)) return false;
      spec.write(row, kRowBytes);
    }
  }
  spec.write(deltas, uint32_t(delta_len));
  return true;
}

}  // namespace port_82B3C668
SKATE3_PORT(82B3C668, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
