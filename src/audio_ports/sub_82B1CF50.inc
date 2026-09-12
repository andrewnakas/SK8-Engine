// sub_82B1CF50  role: round-half-away-from-zero of scale(+4) * product of `count`(+0) s32 words from +8, as s32   lifted: skate3_recomp.66.cpp:65857 (188 lines)
// STATUS: verified
// calls/session: boot 1264336  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CF50.md
namespace port_82B1CF50 {

// Layout read (nothing in docs/rw_audio_structs.h names this object):
//   +0  u8    count   number of s32 words in the product
//   +4  f32   scale   multiplied in last
//   +8  s32   words[] word k lives at +8 + 4*k; word 0 is always read, even when count is 0
constexpr uint32_t kCount = 0;
constexpr uint32_t kScale = 4;
constexpr uint32_t kWords = 8;

// Two rodata singles: 0.0f at 0x82165A10 (lis r11,-32234 ; lfs f13,23056(r11)) and 0.5f at
// 0x8209975C (lis r11,-32246 ; lfs f13,-26788(r11)). Read from the validated image dump; the
// body loads them through the guest map anyway, so the values need not be trusted here.
constexpr uint32_t kZeroSingle = 0x82165A10;
constexpr uint32_t kHalfSingle = 0x8209975C;

// std rN,-k(r1) ; lfd fM,-k(r1) ; fcfid ; frsp : a sign-extended word to single, held as double.
inline double WordToSingle(uint32_t word) {
  const int64_t wide = static_cast<int32_t>(word);  // extsw
  const double converted = static_cast<double>(wide);  // fcfid
  return static_cast<double>(static_cast<float>(converted));  // frsp
}

// fmuls / fadds / fsubs: the double operation rounded to single, as the lifted line does it.
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double AddSingle(double a, double b) { return static_cast<double>(static_cast<float>(a + b)); }
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// fctiwz f13,f0 ; stfd f13,-8(r1) ; lwz r3,-4(r1): as lifted, NaN -> 0x80000000, above INT_MAX
// -> INT_MAX, else CVTTSD2SI (which yields 0x80000000 below INT_MIN on its own). The lwz takes the
// low word of the big-endian spill, zero-extended into r3.
inline uint64_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX)) ? int64_t(INT_MAX)
                                                        : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(fixed);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t word0 = REX_LOAD_U32(object + kWords);                  // lwz r11,8(r3)
  uint32_t next = 1;                                                     // li r9,1
  const int32_t count = static_cast<int32_t>(REX_LOAD_U8(object + kCount));  // lbz r8,0(r3)
  ctx.fpscr.disableFlushModeUnconditional();                             // lfd f0,-32(r1)
  double acc = WordToSingle(word0);                                      // extsw ; fcfid f13,f0 ; frsp f0,f13

  // cmpwi cr6,r8,1 ; ble cr6,0x82b1d050
  if (count > 1) {
    // addi r11,r8,-1 ; cmpwi cr6,r11,4 ; blt cr6,0x82b1d014
    if (!(count - 1 < 4)) {
      // addi r10,r8,-5 ; rlwinm r10,r10,30,2,31 (32-bit logical >> 2) ; addi r10,r10,1
      const uint32_t quads = ((static_cast<uint32_t>(count) - 5) >> 2) + 1;
      // rlwinm r9,r10,2,0,29 (32-bit << 2) ; addi r9,r9,1
      next = (quads << 2) + 1;
      uint32_t cursor = object - 4;                                      // addi r11,r3,-4
      // mtctr r10 ; loc_82B1CFA0 .. bdnz 0x82b1cfa0
      for (uint32_t i = 0; i < quads; i++) {
        const uint32_t w3 = REX_LOAD_U32(cursor + 28);                   // lwz r10,28(r11)
        const uint32_t w2 = REX_LOAD_U32(cursor + 24);                   // lwz r7,24(r11)
        const uint32_t w1 = REX_LOAD_U32(cursor + 20);                   // lwz r6,20(r11)
        cursor += 16;                                                    // lwzu r10,16(r11)
        const uint32_t w0 = REX_LOAD_U32(cursor);
        ctx.fpscr.disableFlushModeUnconditional();                       // lfd f13,-32(r1)
        const double f2 = WordToSingle(w3);  // f5 = fcfid f13 ; f2 = frsp f5
        const double f3 = WordToSingle(w2);  // f6 = fcfid f12 ; f3 = frsp f6
        const double f7 = WordToSingle(w1);  // f9 = fcfid f10 ; f7 = frsp f9
        const double f4 = WordToSingle(w0);  // f8 = fcfid f11 ; f4 = frsp f8
        acc = MulSingle(f4, acc);   // fmuls f1,f4,f0
        acc = MulSingle(acc, f7);   // fmuls f0,f1,f7
        acc = MulSingle(acc, f3);   // fmuls f13,f0,f3
        acc = MulSingle(acc, f2);   // fmuls f0,f13,f2
      }
    }
    // loc_82B1D014: cmpw cr6,r9,r8 ; bge cr6,0x82b1d050
    if (static_cast<int32_t>(next) < count) {
      // addi r11,r9,1 ; rlwinm r11,r11,2,0,29 ; add r11,r11,r3
      uint32_t cursor = ((next + 1) << 2) + object;
      const uint32_t remaining = static_cast<uint32_t>(count) - next;  // subf r10,r9,r8 ; mtctr r10
      // loc_82B1D030 .. bdnz 0x82b1d030
      for (uint32_t i = 0; i < remaining; i++) {
        cursor += 4;                                                     // lwzu r10,4(r11)
        const uint32_t w = REX_LOAD_U32(cursor);
        ctx.fpscr.disableFlushModeUnconditional();                       // lfd f13,-8(r1)
        acc = MulSingle(WordToSingle(w), acc);                           // fcfid ; frsp ; fmuls f0,f11,f0
      }
    }
  }

  // loc_82B1D050
  ctx.fpscr.disableFlushModeUnconditional();                             // lfs f13,4(r3)
  const double scale = LoadSingle(base, object + kScale);
  acc = MulSingle(scale, acc);                                           // fmuls f0,f13,f0
  const double zero = LoadSingle(base, kZeroSingle);                           // lfs f13,23056(r11)
  const bool below_zero = acc < zero;                                    // fcmpu cr6,f0,f13 (unordered: not lt)
  const double half = LoadSingle(base, kHalfSingle);                           // lfs f13,-26788(r11)
  double rounded;
  // blt cr6,0x82b1d084
  if (below_zero) {
    ctx.fpscr.disableFlushModeUnconditional();
    rounded = SubSingle(acc, half);                                      // fsubs f0,f0,f13
  } else {
    rounded = AddSingle(acc, half);                                      // fadds f0,f0,f13
  }
  ctx.r3.u64 = FctiwzLowWord(rounded);  // fctiwz f13,f0 ; stfd f13,-8(r1) ; lwz r3,-4(r1) ; blr
}

// No stores outside the function's own red zone (-32..-1 of r1, not windowed): the result lives
// in r3 only. Reads: the header and every word the product touches. Word 0 is read even when
// count is 0, and words 1..count-1 otherwise; count is a byte, so the span is at most 1,028 bytes.
// Plus the two rodata singles, so the vector replays without the image.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  const uint32_t count = REX_LOAD_U8(object + kCount);
  const uint32_t words = count > 0 ? count : 1;
  spec.read(object, kWords + 4 * words);
  spec.read(kZeroSingle, 4);
  spec.read(kHalfSingle, 4);
  return true;
}

}  // namespace port_82B1CF50
SKATE3_PORT(82B1CF50, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
