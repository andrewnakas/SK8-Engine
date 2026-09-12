// sub_82F52318  role: atan2(y=f1, x=f2) in double -- octant-reduced rational atan, result in f1 (leaf)   lifted: skate3_recomp.106.cpp:66150 (176 lines)
// STATUS: verified
// calls/session: boot 73848  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82F52318.md
namespace port_82F52318 {

// The constant pool, reached by `lis r11,-32005 ; addi r11,r11,20584`. Nothing in
// docs/rw_audio_structs.h names it. Values as read from the dumped image
// (probe/harness/out/image/g_82E0.bin) are given for the reader; the body loads every one.
constexpr uint32_t kPool = 0x82FB5068u;
constexpr uint32_t kHalfPi = 8;           // f64 1.5707963267948966
constexpr uint32_t kPi = 16;              // f64 3.141592653589793
constexpr uint32_t kTanPiOver12 = 24;     // f64 0.2679491924311227
constexpr uint32_t kSqrt3 = 40;           // f64 1.7320508075688772
constexpr uint32_t kOffsetTable = 128;    // f64[4] {0, pi/6, pi/2, pi/3}, indexed by the octant code
constexpr uint32_t kZeroSingle = 168;     // f32 0.0
constexpr uint32_t kOneSingle = 176;      // f32 1.0
constexpr uint32_t kPoolReadStart = 8;    // lowest pool byte read
constexpr uint32_t kPoolReadEnd = 180;    // one past the highest (the f32 at +176)

// stfd f1,16(r1) ; stfd f2,24(r1): scratch in the CALLER's frame (above the entry r1; this leaf
// never allocates one). The high words are reloaded below for the sign bits.
constexpr uint32_t kSpillY = 16;
constexpr uint32_t kSpillX = 24;
constexpr uint32_t kSignBit = 0x80000000u;          // rlwinm. rN,rN,0,0,0
constexpr uint64_t kSignBit64 = 0x8000000000000000ull;

inline double FromBits(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof d);
  return d;
}
inline uint64_t ToBits(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof bits);
  return bits;
}
// lfd: the 8 bytes verbatim.
inline double LoadF64(uint8_t* base, uint32_t addr) { return FromBits(REX_LOAD_U64(addr)); }
// lfs: a single widened to double, as the lifted `double(temp.f32)`.
inline double LoadF32(uint8_t* base, uint32_t addr) {
  const uint32_t bits = REX_LOAD_U32(addr);
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return double(f);
}
// fabs / fneg are sign-bit operations on the register, not arithmetic.
inline double Abs(double d) { return FromBits(ToBits(d) & ~kSignBit64); }
inline double Neg(double d) { return FromBits(ToBits(d) ^ kSignBit64); }

REX_FUNC(Native) {
  const uint32_t sp = ctx.r1.u32;
  const double y = ctx.f1.f64;
  const double x = ctx.f2.f64;

  ctx.fpscr.disableFlushModeUnconditional();
  REX_STORE_U64(sp + kSpillY, ctx.f1.u64);  // stfd f1,16(r1)
  REX_STORE_U64(sp + kSpillX, ctx.f2.u64);  // stfd f2,24(r1)
  const double zero = LoadF32(base, kPool + kZeroSingle);  // lfs f0,168(r11)

  double angle;  // f0 on arrival at loc_82F52428
  // fcmpu cr6,f2,f0 ; bne cr6,0x82f52370   (NaN is unordered: not eq, takes the branch)
  if (x == zero) {
    // fcmpu cr6,f1,f0 ; bne cr6,0x82f52368
    if (y == zero) {
      // lwz r10,24(r1) ; rlwinm. r10,r10,0,0,0 ; beqlr -- x is +0: f1 (y, +/-0) returned as is
      if ((REX_LOAD_U32(sp + kSpillX) & kSignBit) == 0) return;
      // lwz r10,16(r1) ; rlwinm. r10,r10,0,0,0 ; beq 0x82f52360
      if ((REX_LOAD_U32(sp + kSpillY) & kSignBit) == 0) {
        ctx.fpscr.disableFlushModeUnconditional();
        ctx.f1.u64 = REX_LOAD_U64(kPool + kPi);  // lfd f1,16(r11) ; blr
        return;
      }
      ctx.f1.u64 = REX_LOAD_U64(kPool + kPi) ^ kSignBit64;  // lfd f0,16(r11) ; fneg f1,f0 ; blr
      return;
    }
    ctx.fpscr.disableFlushModeUnconditional();
    angle = LoadF64(base, kPool + kHalfPi);  // lfd f0,8(r11) ; b 0x82f52428
  } else {
    // loc_82F52370
    ctx.fpscr.disableFlushModeUnconditional();
    const double abs_x = Abs(x);  // fabs f13,f2
    uint32_t octant = 0;          // li r10,0
    double num = Abs(y);          // fabs f0,f1
    double den = abs_x;           // fmr f12,f13
    // fcmpu cr6,f0,f13 ; ble cr6,0x82f52394
    if (num > abs_x) {
      den = num;     // fmr f12,f0
      octant = 2;    // li r10,2
      num = abs_x;   // fmr f0,f13
    }
    // loc_82F52394
    ctx.fpscr.disableFlushModeUnconditional();
    double t = num / den;                                            // fdiv f0,f0,f12
    const double tan_pi_12 = LoadF64(base, kPool + kTanPiOver12);    // lfd f13,24(r11)
    // fcmpu cr6,f0,f13 ; ble cr6,0x82f523bc
    if (t > tan_pi_12) {
      const double sqrt3 = LoadF64(base, kPool + kSqrt3);   // lfd f13,40(r11)
      octant = octant + 1;                                  // addi r10,r10,1
      const double one = LoadF32(base, kPool + kOneSingle); // lfs f12,176(r11)
      const double denom = sqrt3 + t;                       // fadd f11,f13,f0
      t = std::fma(sqrt3, t, -one);                         // fmsub f0,f13,f0,f12
      t = t / denom;                                        // fdiv f0,f0,f11
    }
    // loc_82F523BC
    ctx.fpscr.disableFlushModeUnconditional();
    const double t2 = t * t;                         // fmul f5,f0,f0
    const double c72 = LoadF64(base, kPool + 72);    // lfd f12,72(r11)
    const double c80 = LoadF64(base, kPool + 80);    // lfd f13,80(r11)
    const double c112 = LoadF64(base, kPool + 112);  // lfd f11,112(r11)
    const double c64 = LoadF64(base, kPool + 64);    // lfd f10,64(r11)
    const double c104 = LoadF64(base, kPool + 104);  // lfd f9,104(r11)
    const double c56 = LoadF64(base, kPool + 56);    // lfd f8,56(r11)
    const double c96 = LoadF64(base, kPool + 96);    // lfd f7,96(r11)
    const double c88 = LoadF64(base, kPool + 88);    // lfd f6,88(r11)
    double p = std::fma(c80, t2, c72);  // fmadd f13,f13,f5,f12
    double q = c112 + t2;               // fadd f12,f11,f5
    p = std::fma(p, t2, c64);           // fmadd f13,f13,f5,f10
    q = std::fma(q, t2, c104);          // fmadd f12,f12,f5,f9
    p = std::fma(p, t2, c56);           // fmadd f13,f13,f5,f8
    q = std::fma(q, t2, c96);           // fmadd f12,f12,f5,f7
    p = p * t2;                         // fmul f13,f13,f5
    q = std::fma(q, t2, c88);           // fmadd f12,f12,f5,f6
    p = p * t;                          // fmul f13,f13,f0
    p = p / q;                          // fdiv f13,f13,f12
    t = p + t;                          // fadd f0,f13,f0
    // cmpwi cr6,r10,1 ; ble cr6,0x82f52418
    if (static_cast<int32_t>(octant) > 1) t = Neg(t);  // fneg f0,f0
    // loc_82F52418: rlwinm r10,r10,3,0,28 ; addi r9,r11,128 ; lfdx f13,r10,r9
    ctx.fpscr.disableFlushModeUnconditional();
    const double offset = LoadF64(base, kPool + kOffsetTable + (octant << 3));
    angle = offset + t;  // fadd f0,f13,f0
  }

  // loc_82F52428
  ctx.fpscr.disableFlushModeUnconditional();
  const double pi = LoadF64(base, kPool + kPi);  // lfd f13,16(r11)
  const double reflected = pi - angle;           // fsub f13,f13,f0
  // lwz r11,16(r1) ; rlwinm. r11,r11,0,0,0
  const bool y_negative = (REX_LOAD_U32(sp + kSpillY) & kSignBit) != 0;
  ctx.f1.f64 = x >= 0.0 ? angle : reflected;  // fsel f1,f2,f0,f13
  if (!y_negative) return;                    // beqlr
  ctx.f1.u64 = ctx.f1.u64 ^ kSignBit64;       // fneg f1,f1 ; blr
}

// Writes: the 16 bytes at entry r1+16 (stfd f1 / stfd f2). They are in the caller's frame, not
// this leaf's, and the body reloads them for the sign tests -- unwindowed, the native run would
// read the lifted run's spill back and agree for the wrong reason. Result: f1 only (every one of
// the 22 call sites reads it through `frsp`); f0, f5-f13, r9-r11, cr0/cr6 are volatile scratch.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r1.u32 + kSpillY, 16);
  spec.read(kPool + kPoolReadStart, kPoolReadEnd - kPoolReadStart);
  return true;
}

}  // namespace port_82F52318
SKATE3_PORT(82F52318, skate3::audio::kPortVerified, skate3::audio::kReturnF1)
