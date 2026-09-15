// sub_82B225A0  role: republish one contribution into the owner's running total -- in mode 1 the length at +140 as a float, corrected through sub_82F55068 when the coefficient at +92 is non-zero, otherwise zero; store it at +28, add the change since +32 into u32[+12]+40, and record it at +32   lifted: skate3_recomp.67.cpp:10994 (135 lines)
// STATUS: verified
// calls/session: boot 88821  play 116945  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B225A0.md
namespace port_82B225A0 {

// r3 = the object. Nothing in docs/rw_audio_structs.h names this layout, so the offsets stay
// numbers with descriptive names.
constexpr uint32_t kOwner = 12;        // rw_ptr  the accumulator this contribution belongs to
constexpr uint32_t kValue = 28;        // float   the uncorrected value, published on both arms
constexpr uint32_t kPublished = 32;    // float   what was last added into the owner's total
constexpr uint32_t kMode = 64;         // int32   1 selects the computed arm, anything else zero
constexpr uint32_t kCoefficient = 92;  // float   zero means "no correction"
constexpr uint32_t kLength = 140;      // int32   converted to float as the raw value
constexpr uint32_t kOwnerTotal = 40;   // float   the running total on the owner

// lis -32234 ; lfs 23056 -- ((-32234 & 0xFFFF) << 16) + 23056, computed, not read by eye. The same
// word port_82B269C0 names kZeroFloat.
constexpr uint32_t kZeroFloat = 0x82165A10;
// lis -32225 ; lfs 6032
constexpr uint32_t kCorrectionScale = 0x821F1790;

constexpr uint32_t kFrameBytes = 112;
constexpr uint64_t kSignBit = 0x8000000000000000ull;

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

double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }
double MulSingle(double a, double b) { return double(float(a * b)); }
double DivSingle(double a, double b) { return double(float(a / b)); }

// fneg / fabs are bit operations on the double, so -0 and NaN payloads survive unchanged.
double BitNeg(double v) {
  PPCRegister temp{};
  temp.f64 = v;
  temp.u64 ^= kSignBit;
  return temp.f64;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3

  // stfd f31,-24(r1) -- the first float touch, and where the lifted form settles the rounding
  // mode. The r12/r31/f31 spills themselves are this function's own frame and are not reproduced:
  // nothing here writes r31 or f31, and sub_82F55068's subtree touches only f0-f13 and r6-r11, so
  // their exit values are their entry values either way.
  ctx.fpscr.disableFlushModeUnconditional();

  // stwu r1,-112(r1) -- reproduced: sub_82F55068 allocates 96 bytes of its own and its callee
  // sub_82F54ED8 scratches at r1+16 *inside that frame*, so the frame must sit below this one.
  const uint32_t sp = ctx.r1.u32 - kFrameBytes;
  REX_STORE_U32(sp, ctx.r1.u32);  // the back chain; inside this function's own frame
  ctx.r1.u32 = sp;

  double published{};  // f0 at loc_82B22658 -- what both arms leave at +32

  // lwz r11,64(r3) ; cmpwi cr6,r11,1 ; bne -- SIGNED, against 1 exactly
  if (int32_t(REX_LOAD_U32(self + kMode)) == 1) {
    const uint32_t length = REX_LOAD_U32(self + kLength);  // lwz r11,140(r3)
    PPCRegister coef{};
    coef.u32 = REX_LOAD_U32(self + kCoefficient);  // lfs f13,92(r3)
    const double coefficient = double(coef.f32);
    // extsw r9,r11 ; std r9,80(r1) ; lfd f0,80(r1) ; fcfid f12,f0 -- the GPR-to-FPR round trip
    // through the frame, then the signed 64-bit convert. Done natively; that frame word is scratch
    // nothing else reads.
    const double value = double(float(double(int64_t(int32_t(length)))));  // frsp f31,f12
    published = value;                                                     // fmr f0,f31

    // fcmpu cr6,f13,f0 ; beq -- an unordered compare: a NaN coefficient is NOT equal, so it takes
    // the correction path.
    if (!(coefficient == LoadSingle(base, kZeroFloat))) {
      PPCRegister magnitude{};
      magnitude.f64 = coefficient;
      magnitude.u64 &= ~kSignBit;  // fabs f1,f13
      ctx.f1.u64 = magnitude.u64;
      // bl 0x82f55068 -- f1 = transform(|coefficient|). It and sub_82F54ED8 write only their own
      // frames (and 82F54ED8's scratch inside 82F55068's), so they need no windows.
      skate3::audio::GuestCall(ctx, base, sub_82F55068);
      ctx.fpscr.disableFlushModeUnconditional();
      const double transformed = double(float(ctx.f1.f64));  // frsp f13,f1
      const double scale = LoadSingle(base, kCorrectionScale);
      // fmuls f12,f31,f0 ; fdivs f11,f12,f13 -- two single-rounded operations, not one
      const double correction = DivSingle(MulSingle(value, scale), transformed);
      published = SubSingle(value, correction);  // fsubs f0,f31,f11
    }

    // loc_82B22614
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, self + kValue, value);  // stfs f31,28(r31)
    // lwz r11,12(r31) -- loaded AFTER the store above, which cannot alias +12
    const uint32_t owner = REX_LOAD_U32(self + kOwner);
    const double previous = LoadSingle(base, self + kPublished);  // lfs f13,32(r31)
    const double delta = SubSingle(published, previous);          // fsubs f12,f0,f13
    const double total = LoadSingle(base, owner + kOwnerTotal);   // lfs f11,40(r11)
    // fadds f10,f12,f11 ; stfs f10,40(r11)
    StoreSingle(base, owner + kOwnerTotal, AddSingle(delta, total));
  } else {
    // loc_82B22634 -- withdraw whatever was last published and settle at zero.
    ctx.fpscr.disableFlushModeUnconditional();
    const double previous = LoadSingle(base, self + kPublished);  // lfs f0,32(r31)
    const double negated = BitNeg(previous);                      // fneg f13,f0
    const uint32_t owner = REX_LOAD_U32(self + kOwner);           // lwz r10,12(r31)
    published = LoadSingle(base, kZeroFloat);                     // lfs f0,23056(r11)
    StoreSingle(base, self + kValue, published);                  // stfs f0,28(r31)
    const double total = LoadSingle(base, owner + kOwnerTotal);   // lfs f12,40(r10)
    // fadds f11,f13,f12 ; stfs f11,40(r10)
    StoreSingle(base, owner + kOwnerTotal, AddSingle(negated, total));
  }

  // loc_82B22658
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, self + kPublished, published);  // stfs f0,32(r31)
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;            // addi r1,r1,112 -- a 64-bit add, as lifted
}

// Three words on both arms: +28, +32 and the owner's total. Every address is entry state -- the
// owner pointer is loaded from +12, which neither store can alias, and the two callees write only
// their own frames.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  if (self == 0) return false;  // the original would load through null; not comparable
  const uint32_t owner = REX_LOAD_U32(self + kOwner);

  spec.read(self + kOwner, 4);
  spec.read(self + kValue, 8);  // +28 and +32
  spec.read(self + kMode, 4);
  spec.read(self + kCoefficient, 4);
  spec.read(self + kLength, 4);
  spec.read(owner + kOwnerTotal, 4);
  spec.read(kZeroFloat, 4);
  spec.read(kCorrectionScale, 4);
  // The correction path reaches sub_82F55068 and its natural log, whose rodata no window of theirs
  // is recorded under this function's name. Declared here for replay only -- reads cost nothing
  // against the budget -- so a correction-path vector carries every byte the callees read. Values
  // and offsets are the ones recomp/src/audio_ports/sub_82F55068.inc declares for itself.
  spec.read(0x82050000u + 8344, 144);  // the log pool, log10(e) inside it at +8368
  spec.read(0x82010000u + 248, 24);    // 0.0, -1.0, 1.0
  spec.read(0x82010000u + 648, 8);     // 0.5
  spec.read(0x82FB0000u + 22720, 16);  // +inf and the NaN, both negated on the way out

  spec.write(self + kValue, 8);           // stfs +28 ; stfs +32
  spec.write(owner + kOwnerTotal, 4);     // stfs f10,40(r11) / stfs f11,40(r10)
  return true;
}

}  // namespace port_82B225A0
SKATE3_PORT(82B225A0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
