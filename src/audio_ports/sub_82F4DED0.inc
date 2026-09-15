// sub_82F4DED0  role: sin(double) -- f1 in, sin(f1) out in f1: |x| reduced by pi in two parts (Cody-Waite), an odd Taylor polynomial through x^19 in Horner form, the sign restored from the input and the quotient's parity; |x| >= 2.2e8 gives NaN and +/-0 passes through unchanged   lifted: skate3_recomp.106.cpp:53843 (123 lines)
// STATUS: verified -- zero register and memory divergence over 436266 comparable calls under scripted play and 414229 at boot; the weaker profile is 414229 calls against 123 lifted lines, far past the promotion criterion
// calls/session: boot 414229  play 436266  map ?          thread: first called on load_thread; also RwAudioCore Dac, through the spatial and filter callers
// note: probe/ports/notes/sub_82F4DED0.md
namespace port_82F4DED0 {

// One constant table, reached as lis r11,-32005 ; addi r11,r11,20000. Values measured from the
// image dump, listed so a reader can see what the polynomial is; every one is read live below,
// as the original reads it, and nothing is folded in:
//   +8   1/pi                +16  2.2e8, the range limit   +28  1.0f   +32  -1.0f
//   +40  pi, high part       +48  pi, low part (1.2154e-10)
//   +56  -1/6   +64  1/120   +72  -1/5040   +80  1/362880   +88  -1/39916800
//   +96  1/6227020800        +104 -1/1307674368000         +112 1/355687428096000
constexpr uint32_t kTable = 0x82FB0000u + 20000;  // lis r11,-32005 ; addi r11,r11,20000
constexpr uint32_t kTableBytes = 120;              // lfd f8,112(r11) is the last read
constexpr uint32_t kOne = 0x82010000u + 264;       // lis r10,-32255 ; lfd f30,264(r10) = 1.0
constexpr uint32_t kNaN = 0x82FB0000u + 22728;     // lis r11,-32005 ; lfd f0,22728(r11) = NaN
static_assert(kTable == 0x82FB4E20u && kOne == 0x82010108u && kNaN == 0x82FB58C8u,
              "((imm & 0xFFFF) << 16) + offset, computed rather than read by eye");

inline double LoadDouble(uint8_t* base, uint32_t address) {
  const uint64_t bits = REX_LOAD_U64(address);
  double value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return double(temp.f32);
}

inline double Negate(double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  bits ^= 0x8000000000000000ULL;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

// fctid and fctidz, the lifted lines verbatim, NaN and overflow edges included.
inline int64_t Fctid(double value) {
  return std::isnan(value)             ? int64_t(0x8000000000000000ULL)
         : (value > double(LLONG_MAX)) ? LLONG_MAX
                                       : simde_mm_cvtsd_si64(simde_mm_load_sd(&value));
}

inline int64_t Fctidz(double value) {
  return std::isnan(value)             ? int64_t(0x8000000000000000ULL)
         : (value > double(LLONG_MAX)) ? LLONG_MAX
                                       : simde_mm_cvttsd_si64(simde_mm_load_sd(&value));
}

REX_FUNC(Native) {
  // stfd f30,-16(r1): where the flush mode is set. f30 and f31 are saved to the red zone and
  // restored at the end; this body never touches them, so the saves have nothing to protect.
  ctx.fpscr.disableFlushModeUnconditional();
  const double x = ctx.f1.f64;
  // fabs f0,f1 -- also stored to -24(r1) and read back as an integer for the +/-0 test below.
  const uint64_t abs_bits = ctx.f1.u64 & ~0x8000000000000000ULL;
  double ax;
  std::memcpy(&ax, &abs_bits, sizeof ax);

  const double one = LoadDouble(base, kOne);               // lfd f30,264(r10)
  const double inv_pi = LoadDouble(base, kTable + 8);      // lfd f11,8(r11)
  const double minus_one = LoadSingle(base, kTable + 32);  // lfs f12,32(r11)
  const double pi_hi = LoadDouble(base, kTable + 40);      // lfd f10,40(r11)
  const double q = inv_pi * ax;                            // fmul f13,f11,f0
  const double pi_lo = LoadDouble(base, kTable + 48);      // lfd f9,48(r11)
  const double c9 = LoadDouble(base, kTable + 112);        // lfd f8,112(r11)
  const double c8 = LoadDouble(base, kTable + 104);        // lfd f7,104(r11)
  const double c7 = LoadDouble(base, kTable + 96);         // lfd f6,96(r11)
  const double c6 = LoadDouble(base, kTable + 88);         // lfd f5,88(r11)
  const double c5 = LoadDouble(base, kTable + 80);         // lfd f4,80(r11)
  const double c4 = LoadDouble(base, kTable + 72);         // lfd f3,72(r11)
  const double c3 = LoadDouble(base, kTable + 64);         // lfd f2,64(r11)
  const double c2 = LoadDouble(base, kTable + 56);         // lfd f31,56(r11)
  const int64_t n_rounded = Fctid(q);                      // fctid f11,f13
  const double plus_one = LoadSingle(base, kTable + 28);   // lfs f13,28(r11)
  const double sign = x >= 0.0 ? plus_one : minus_one;     // fsel f12,f1,f13,f12
  const double n = double(n_rounded);                      // fcfid f13,f11
  const double r_hi = -std::fma(pi_hi, n, -ax);            // fnmsub f11,f10,f13,f0
  const int64_t n_int = Fctidz(n);                         // fctidz f10,f13
  const bool odd = (uint64_t(n_int) & 1) != 0;             // stfd ; ld r9 ; clrldi r8,r9,63
  const double r = -std::fma(pi_lo, n, -r_hi);             // fnmsub f9,f9,f13,f11
  const double r2 = r * r;                                 // fmul f13,f9,f9
  double p = std::fma(c9, r2, c8);                         // fmadd f11,f8,f13,f7
  p = std::fma(p, r2, c7);                                 // fmadd f10,f11,f13,f6
  p = std::fma(p, r2, c6);                                 // fmadd f8,f10,f13,f5
  p = std::fma(p, r2, c5);                                 // fmadd f7,f8,f13,f4
  p = std::fma(p, r2, c4);                                 // fmadd f6,f7,f13,f3
  p = std::fma(p, r2, c3);                                 // fmadd f5,f6,f13,f2
  p = std::fma(p, r2, c2);                                 // fmadd f4,f5,f13,f31
  p = std::fma(p, r2, one);                                // fmadd f3,f4,f13,f30
  double s = p * r;                                        // fmul f13,f3,f9
  if (odd) s = Negate(s);                                  // fneg f13,f13

  // loc_82F4DF80
  ctx.fpscr.disableFlushModeUnconditional();
  const double signed_s = s * sign;                        // fmul f12,f13,f12
  if (abs_bits != 0) {                                     // ld r10,-24(r1) ; cmpdi ; beq
    const double limit = LoadDouble(base, kTable + 16);    // lfd f13,16(r11)
    const double over = ax - limit;                        // fsub f13,f0,f13
    const double nan = LoadDouble(base, kNaN);             // lfd f0,22728(r11)
    ctx.f1.f64 = over >= 0.0 ? nan : signed_s;             // fsel f1,f13,f0,f12
  }
  // +/-0 leaves f1 exactly as it arrived, sign included: the original never writes f1 on that path.

  // loc_82F4DFA4
  ctx.fpscr.disableFlushModeUnconditional();               // at lfd f30,-16(r1)
}

// No stores outside the red zone below r1, which is this call's own scratch and is never a
// window. The result is f1 only. The table and both cells are declared as reads so a recorded
// vector replays; a read costs nothing against the window budget.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  spec.read(kTable, kTableBytes);
  spec.read(kOne, 8);
  spec.read(kNaN, 8);
  return true;
}

}  // namespace port_82F4DED0
SKATE3_PORT(82F4DED0, skate3::audio::kPortVerified, skate3::audio::kReturnF1)
