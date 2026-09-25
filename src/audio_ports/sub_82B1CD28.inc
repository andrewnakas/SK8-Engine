// sub_82B1CD28  role: u32 delay ring: re-derive the write offset when the source's delay changes, push the source word, return the word at the read slot, advance both   lifted: skate3_recomp.66.cpp:65542 (132 lines)
// STATUS: verified
// calls/session: boot 108149  play 122771  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CD28.md
namespace port_82B1CD28 {

// Layout read (nothing in docs/rw_audio_structs.h names this object; these are readings):
//   +0  u16  offset of a source record from the object itself (record = r3 + this)
//   +2  u16  capacity (ring length in words)
//   +4  u16  write index         +6  u16  read index
//   +8  u32  last delay seen (compared with record +4)
//   +12 u32  ring[]: slot k lives at +4*(k+3)
// Source record: +0 u32 the word pushed, +4 s32 delay (clamped to 0 in place when negative).
constexpr uint32_t kRingBase = 12;

// Two singles loaded by absolute address. 0x8209975C is 0.5f in the image dump. 0x830775D8 read
// 0.0f in a dump taken at the second guest function of boot, so it is presumably a global set at
// run time (the divisor). Both are loaded live, exactly as the original does.
constexpr uint32_t kDivisorCell = 0x83070000u + 30168u;  // lis r6,-31993 ; lfs f0,30168(r6)
constexpr uint32_t kHalfCell = 0x820A0000u - 26788u;     // lis r5,-32246 ; lfs f13,-26788(r5)

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadF32(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// fctiwz f9,f10 ; stfd f9,-16(r1) ; lwz r10,-12(r1): as lifted, NaN -> 0x80000000, above INT_MAX
// -> INT_MAX, else CVTTSD2SI (0x80000000 below INT_MIN on its own). lwz takes the low word of the
// big-endian spill, zero-extended.
inline uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX)) ? int64_t(INT_MAX)
                                                        : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(fixed);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                            // mr r11,r3
  const uint32_t record_offset = REX_LOAD_U16(object + 0);       // lhz r10,0(r3)
  const uint32_t last_delay = REX_LOAD_U32(object + 8);          // lwz r9,8(r3)
  const uint32_t record = record_offset + object;                // add r8,r10,r3
  const uint32_t delay_seen = REX_LOAD_U32(record + 4);          // lwz r10,4(r8)

  // cmpw cr6,r10,r9 ; beq cr6,0x82b1cdb4
  if (static_cast<int32_t>(delay_seen) != static_cast<int32_t>(last_delay)) {
    REX_STORE_U32(object + 8, delay_seen);                       // stw r10,8(r3)
    // lwz r10,4(r8) ; cmpwi cr6,r10,0 ; bge cr6,0x82b1cd5c
    if (static_cast<int32_t>(REX_LOAD_U32(record + 4)) < 0) {
      REX_STORE_U32(record + 4, 0);                              // stw r7,4(r8)  (r7 = 0)
    }
    // loc_82B1CD5C
    const uint32_t delay = REX_LOAD_U32(record + 4);             // lwz r10,4(r8)
    const uint64_t capacity = REX_LOAD_U16(object + 2);          // lhz r9,2(r11)
    ctx.fpscr.disableFlushModeUnconditional();                   // lfd f0,-16(r1)
    // extsw r4,r10 ; std r4,-16(r1) ; lfd f0,-16(r1) ; fcfid f13,f0
    const double wide = double(int64_t(static_cast<int32_t>(delay)));
    const double divisor = LoadF32(base, kDivisorCell);          // lfs f0,30168(r6)
    const double narrow = double(float(wide));                   // frsp f12,f13
    const double half = LoadF32(base, kHalfCell);                // lfs f13,-26788(r5)
    const double quotient = double(float(narrow / divisor));     // fdivs f11,f12,f0
    const double rounded = double(float(quotient + half));       // fadds f10,f11,f13
    uint64_t offset = FctiwzLowWord(rounded);                    // fctiwz ; stfd ; lwz r10,-12(r1)
    // cmpw cr6,r10,r9 ; blt cr6,0x82b1cda8
    if (!(static_cast<int32_t>(static_cast<uint32_t>(offset)) <
          static_cast<int32_t>(static_cast<uint32_t>(capacity)))) {
      offset = capacity - 1;                                     // addi r10,r9,-1 (64-bit)
    }
    // loc_82B1CDA8
    const uint64_t read_index = REX_LOAD_U16(object + 6);        // lhz r9,6(r11)
    const uint64_t write_index = read_index + offset;            // add r10,r9,r10
    REX_STORE_U16(object + 4, static_cast<uint16_t>(write_index));  // sth r10,4(r11)
  }

  // loc_82B1CDB4
  const uint32_t capacity = REX_LOAD_U16(object + 2);            // lhz r10,2(r11)
  const uint32_t write_index = REX_LOAD_U16(object + 4);         // lhz r9,4(r11)
  // cmplw cr6,r9,r10 ; blt cr6,0x82b1cdcc
  if (!(write_index < capacity)) {
    REX_STORE_U16(object + 4, static_cast<uint16_t>(write_index - capacity));  // subf r9,r10,r9 ; sth r9,4(r11)
  }
  // loc_82B1CDCC: lhz r9,6(r11) ; cmplw cr6,r9,r10 ; blt cr6,0x82b1cddc
  if (!(REX_LOAD_U16(object + 6) < capacity)) {
    REX_STORE_U16(object + 6, 0);                                // sth r7,6(r11)  (r7 = 0)
  }

  // loc_82B1CDDC
  const uint32_t write_slot = REX_LOAD_U16(object + 4);          // lhz r10,4(r11)
  const uint32_t word = REX_LOAD_U32(record + 0);                // lwz r9,0(r8)
  // addi r8,r10,3 ; rlwinm r7,r8,2,0,29 ; stwx r9,r7,r11
  REX_STORE_U32(((write_slot + 3) << 2) + object, word);
  const uint32_t write_after = REX_LOAD_U16(object + 4);         // lhz r9,4(r11)
  const uint32_t read_slot = REX_LOAD_U16(object + 6);           // lhz r10,6(r11)
  // addi r6,r10,3 ; rlwinm r5,r6,2,0,29 ; addi r4,r10,1 ; addi r9,r9,1 ; lwzx r3,r5,r11
  ctx.r3.u64 = REX_LOAD_U32(((read_slot + 3) << 2) + object);
  REX_STORE_U16(object + 6, static_cast<uint16_t>(read_slot + 1));    // sth r4,6(r11)
  REX_STORE_U16(object + 4, static_cast<uint16_t>(write_after + 1));  // sth r9,4(r11) ; blr
}

// The path is chosen by `record+4 != +8`, both read before any store, so it is known at entry.
// Unchanged delay: no float, no store before loc_82B1CDB4, so the stwx slot is exact:
//   W = +4 >= cap ? +4 - cap : +4.
// Changed delay: the slot needs the float, so cover the ring instead. With divisor and half not
// negative (NaN allowed), fctiwz yields n in [0, INT_MAX] or 0x80000000, so d = min(n, cap-1)
// has low 16 bits in [0, cap-1] and the new write index is R + d with R = +6. The only store that
// can touch +6 first is the zeroing `stw r7,4(r8)`, which can only lower R. So with 1 <= cap and
// R <= cap, W lies in [R, R+cap-1] within [0, 2cap-1], and after the single wrap W is in
// [0, cap-1]. Anything else returns false.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t record = REX_LOAD_U16(object + 0) + object;
  const uint32_t capacity = REX_LOAD_U16(object + 2);
  const uint32_t write0 = REX_LOAD_U16(object + 4);
  const uint32_t read0 = REX_LOAD_U16(object + 6);
  const bool changed =
      static_cast<int32_t>(REX_LOAD_U32(record + 4)) != static_cast<int32_t>(REX_LOAD_U32(object + 8));

  spec.write(object + 4, 8);  // sth +4, sth +6 (every path); stw +8 (changed path)
  spec.read(object, 4);
  spec.read(record, 8);

  if (!changed) {
    const uint32_t write_slot = write0 >= capacity ? write0 - capacity : write0;
    const uint32_t read_slot = read0 >= capacity ? 0 : read0;
    spec.write(((write_slot + 3) << 2) + object, 4);  // stwx r9,r7,r11
    spec.read(((read_slot + 3) << 2) + object, 4);    // lwzx r3,r5,r11
    return true;
  }

  const double divisor = LoadF32(base, kDivisorCell);
  const double half = LoadF32(base, kHalfCell);
  if (divisor < 0.0 || half < 0.0) return false;  // a negative quotient puts W anywhere
  if (capacity == 0 || read0 > capacity) return false;
  if (12 + 4 * capacity > skate3::audio::kPortWatchCap) return false;
  spec.write(record + 4, 4);                   // stw r7,4(r8) when the delay was negative
  spec.write(object + kRingBase, 4 * capacity);  // stwx somewhere in ring[0 .. cap-1]
  spec.read(kDivisorCell, 4);
  spec.read(kHalfCell, 4);
  return true;
}

}  // namespace port_82B1CD28
SKATE3_PORT(82B1CD28, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
