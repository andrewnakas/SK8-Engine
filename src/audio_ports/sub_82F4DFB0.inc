// sub_82F4DFB0  role: cos(double) -- f1 in, cos(f1) out in f1: |x| shifted by pi/2 and reduced as the sine is, with the quotient offset by one half so the same odd polynomial serves; |x| == 0 returns exactly 1.0 and (|x| + pi/2) >= 2.2e8 gives NaN   lifted: skate3_recomp.106.cpp:53967 (124 lines)
// STATUS: verified -- zero register and memory divergence over 268332 comparable calls under scripted play and 228467 at boot; the weaker profile is 228467 calls against 124 lifted lines, far past the promotion criterion
// calls/session: boot 228467  play 268332  map ?          thread: first called on Main XThread; also RwAudioCore Dac, through the spatial and filter callers
// note: probe/ports/notes/sub_82F4DFB0.md
namespace port_82F4DFB0 {

// The same table sub_82F4DED0 reads, and the same two cells. Copied rather than referenced, which
// the lint forbids across ports: if that port's constants move, move these too.
//   +0   pi/2   +8  1/pi   +16  2.2e8   +24  0.0f   +28  1.0f   +36  0.5f
//   +40  pi, high part     +48  pi, low part         +56..+112  the odd Taylor coefficients
constexpr uint32_t kTable = 0x82FB0000u + 20000;  // lis r11,-32005 ; addi r11,r11,20000
constexpr uint32_t kTableBytes = 120;              // lfd f8,112(r11) is the last read
constexpr uint32_t kOne = 0x82010000u + 264;       // lis r10,-32255 ; lfd f31,264(r10) = 1.0
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
  // stfd f31,-8(r1): where the flush mode is set. f31 is saved and restored around the body and
  // this port never touches it.
  ctx.fpscr.disableFlushModeUnconditional();
  const uint64_t abs_bits = ctx.f1.u64 & ~0x8000000000000000ULL;
  double ax;
  std::memcpy(&ax, &abs_bits, sizeof ax);                  // fabs f0,f1

  const double one = LoadDouble(base, kOne);               // lfd f31,264(r10)
  const double half_pi = LoadDouble(base, kTable + 0);     // lfd f13,0(r11)
  const double half = LoadSingle(base, kTable + 36);       // lfs f11,36(r11)
  const double pi_hi = LoadDouble(base, kTable + 40);      // lfd f10,40(r11)
  const double shifted = half_pi + ax;                     // fadd f12,f13,f0
  const double inv_pi = LoadDouble(base, kTable + 8);      // lfd f13,8(r11)
  const double pi_lo = LoadDouble(base, kTable + 48);      // lfd f9,48(r11)
  const double c9 = LoadDouble(base, kTable + 112);        // lfd f8,112(r11)
  const double c8 = LoadDouble(base, kTable + 104);        // lfd f7,104(r11)
  const double c7 = LoadDouble(base, kTable + 96);         // lfd f6,96(r11)
  const double c6 = LoadDouble(base, kTable + 88);         // lfd f5,88(r11)
  const double c5 = LoadDouble(base, kTable + 80);         // lfd f4,80(r11)
  const double c4 = LoadDouble(base, kTable + 72);         // lfd f3,72(r11)
  const double c3 = LoadDouble(base, kTable + 64);         // lfd f2,64(r11)
  const double c2 = LoadDouble(base, kTable + 56);         // lfd f1,56(r11) -- f1 reused as a coefficient
  const double q = inv_pi * shifted;                       // fmul f13,f13,f12
  const double n = double(Fctid(q));                       // fctid f13,f13 ; fcfid f13,f13
  const double m = n - half;                               // fsub f11,f13,f11
  const int64_t n_int = Fctidz(n);                         // fctidz f13,f13
  const bool odd = (uint64_t(n_int) & 1) != 0;             // stfd ; ld r9 ; clrldi r8,r9,63
  const double r_hi = -std::fma(pi_hi, m, -ax);            // fnmsub f10,f10,f11,f0
  const double r = -std::fma(pi_lo, m, -r_hi);             // fnmsub f9,f9,f11,f10
  const double r2 = r * r;                                 // fmul f13,f9,f9
  double p = std::fma(c9, r2, c8);                         // fmadd f11,f8,f13,f7
  p = std::fma(p, r2, c7);                                 // fmadd f10,f11,f13,f6
  p = std::fma(p, r2, c6);                                 // fmadd f8,f10,f13,f5
  p = std::fma(p, r2, c5);                                 // fmadd f7,f8,f13,f4
  p = std::fma(p, r2, c4);                                 // fmadd f6,f7,f13,f3
  p = std::fma(p, r2, c3);                                 // fmadd f5,f6,f13,f2
  p = std::fma(p, r2, c2);                                 // fmadd f4,f5,f13,f1
  p = std::fma(p, r2, one);                                // fmadd f3,f4,f13,f31
  double c = p * r;                                        // fmul f13,f3,f9
  if (odd) c = Negate(c);                                  // fneg f13,f13

  // loc_82F4E05C
  ctx.fpscr.disableFlushModeUnconditional();
  const double zero = LoadSingle(base, kTable + 24);       // lfs f11,24(r11)
  if (ax == zero) {                                        // fcmpu cr6,f0,f11 ; bne -- NaN is unordered
    ctx.f1.f64 = LoadSingle(base, kTable + 28);            // lfs f1,28(r11) = 1.0
    return;                                                // lfd f31,-8(r1) ; blr
  }

  // loc_82F4E074
  ctx.fpscr.disableFlushModeUnconditional();
  const double limit = LoadDouble(base, kTable + 16);      // lfd f0,16(r11)
  const double over = shifted - limit;                     // fsub f12,f12,f0
  const double nan = LoadDouble(base, kNaN);               // lfd f0,22728(r11)
  ctx.f1.f64 = over >= 0.0 ? nan : c;                      // fsel f1,f12,f0,f13
}

// No stores outside the red zone below r1. The result is f1 only. Reads declared for replay.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  spec.read(kTable, kTableBytes);
  spec.read(kOne, 8);
  spec.read(kNaN, 8);
  return true;
}

}  // namespace port_82F4DFB0
SKATE3_PORT(82F4DFB0, skate3::audio::kPortVerified, skate3::audio::kReturnF1)
