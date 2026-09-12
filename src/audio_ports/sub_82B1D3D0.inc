// sub_82B1D3D0  role: step one oscillator: wrap its phase, sample the waveform (quarter sine table, pulse, ramp, triangle), advance the phase, return the sample as an int   lifted: skate3_recomp.66.cpp:66525 (300 lines)
// STATUS: verified
// calls/session: boot 41789  play 61408  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D3D0.md
namespace port_82B1D3D0 {

// The oscillator object (nothing in docs/rw_audio_structs.h names it):
//   +0   u8   waveform selector: 0 quarter-sine table, 1 pulse, 2 ramp, >= 3 triangle
//   +4   f32  phase -- read and written; the only bytes this call stores
//   +8   s32  period in update ticks; <= 0 returns 0 and touches nothing
//   +12  s32  amplitude, converted to float
constexpr uint32_t kWaveform = 0;
constexpr uint32_t kPhase = 4;
constexpr uint32_t kPeriod = 8;
constexpr uint32_t kAmplitude = 12;

// Pool constants, each computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye:
// one misread digit in sub_82B2FE00 cost this project's first shadow divergence. kUpdateDelta,
// kRoundHalf and kZeroFloat are the same three addresses the verified ports already use.
constexpr uint32_t kUpdateDelta = 0x830775D8;   // lis -31993 ; lfs 30168   (also sub_82B1C910)
constexpr uint32_t kPhaseWrap = 0x8231A844;     // lis -32206 ; lfs -22460
constexpr uint32_t kRoundHalf = 0x8209975C;     // lis -32246 ; lfs -26788
constexpr uint32_t kZeroFloat = 0x82165A10;     // lis -32234 ; lfs 23056
constexpr uint32_t kPhaseToUnits = 0x822F8EA4;  // lis -32208 ; addi -31232 ; lfs 2212
constexpr uint32_t kSineTable = 0x82FD36B8;     // lis -32003 ; addi 14008
constexpr uint32_t kSineNorm = 0x82098D0C;      // lis -32246 ; lfs -29428
constexpr uint32_t kTriangleGain = 0x82060C50;  // lis -32250 ; lfs 3152
// addi r8,r11,512: quadrants 1 and 3 read the same table backwards from this mirror point.
constexpr uint32_t kSineMirror = 512;

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

double DivSingle(double a, double b) { return double(float(a / b)); }
double MulSingle(double a, double b) { return double(float(a * b)); }
double AddSingle(double a, double b) { return double(float(a + b)); }
double SubSingle(double a, double b) { return double(float(a - b)); }

// extsw ; std -16(r1) ; lfd ; fcfid ; frsp -- an int through the red zone and back as a float.
double IntToSingle(int32_t value) { return double(float(double(int64_t(value)))); }

// fctiwz then stfd/lwz of the LOW word: NaN gives 0x80000000, above INT_MAX saturates, and only
// the low 32 bits of the spilled doubleword reach the register.
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(static_cast<uint64_t>(fixed) & 0xFFFFFFFFu);
}

REX_FUNC(Native) {
  const uint32_t osc = ctx.r3.u32;
  const int32_t period = static_cast<int32_t>(REX_LOAD_U32(osc + kPeriod));  // lwz r11,8(r3)
  // cmpwi cr6,r11,0 ; bgt cr6,0x82b1d3e4
  if (!(period > 0)) {
    ctx.r3.s64 = 0;  // li r3,0
    return;          // blr
  }
  const int32_t amplitude_raw = static_cast<int32_t>(REX_LOAD_U32(osc + kAmplitude));  // lwz r10,12(r3)
  ctx.fpscr.disableFlushModeUnconditional();
  double phase = LoadSingle(base, osc + kPhase);              // lfs f11,4(r3)
  const double period_f = IntToSingle(period);                // fcfid f9 ; frsp f7
  const double amplitude = IntToSingle(amplitude_raw);        // fcfid f6 ; frsp f0
  const double wrap = LoadSingle(base, kPhaseWrap);           // lfs f12,-22460(r7)
  const double increment =
      DivSingle(LoadSingle(base, kUpdateDelta), period_f);    // lfs f13,30168(r9) ; fdivs f9,f13,f7

  // fcmpu cr6,f11,f12 ; blt cr6,0x82b1d448 -- an unordered compare falls INTO the loop, and the
  // loop's own bge keeps it there, exactly as on hardware. Reproduced, not guarded.
  if (!(phase < wrap)) {
    do {  // loc_82B1D430
      ctx.fpscr.disableFlushModeUnconditional();
      // lfs f13,4(r3) -- the original re-reads the phase it just stored, so this reloads too.
      phase = SubSingle(LoadSingle(base, osc + kPhase), wrap);  // fsubs f11,f13,f12
      StoreSingle(base, osc + kPhase, phase);                   // stfs f11,4(r3)
    } while (!(phase < wrap));                                  // fcmpu ; bge cr6,0x82b1d430
  }

  // loc_82B1D448
  const uint32_t waveform = REX_LOAD_U8(osc + kWaveform);  // lbz r11,0(r3)
  ctx.fpscr.disableFlushModeUnconditional();
  phase = LoadSingle(base, osc + kPhase);                  // lfs f13,4(r3)
  const double half = LoadSingle(base, kRoundHalf);        // lfs f10,-26788(r10)
  const double zero = LoadSingle(base, kZeroFloat);        // lfs f11,23056(r9)
  double out;
  // cmplwi cr6,r11,0 ; bne cr6,0x82b1d544
  if (waveform == 0) {
    // The phase scales into table units; bits 9:8 of the truncated value pick the quadrant and
    // bits 7:0 the entry, so the constant is a table-units-per-cycle factor.
    double units = MulSingle(phase, LoadSingle(base, kPhaseToUnits));  // lfs f12,2212(r10) ; fmuls
    // fcmpu cr6,f12,f11 ; blt cr6,0x82b1d494 -- round half away from zero, then truncate.
    if (units < zero) {
      ctx.fpscr.disableFlushModeUnconditional();
      units = SubSingle(units, half);  // fsubs f12,f12,f10
    } else {
      units = AddSingle(units, half);  // fadds f12,f12,f10
    }
    const uint32_t fixed = FctiwzLowWord(units);  // fctiwz f8 ; stfd f8,-16(r1) ; lwz r11,-12(r1)
    // loc_82B1D4A4: srawi r10,r11,8 ; clrlwi r10,r10,30 -- arithmetic shift, then two bits.
    const uint32_t quadrant = static_cast<uint32_t>(static_cast<int32_t>(fixed) >> 8) & 3u;
    const uint32_t index = fixed & 0xFFu;   // clrlwi r11,r11,24
    const uint32_t offset = index << 1;     // rlwinm r9,r11,1,0,30 -- a 32-bit shift left by one
    int32_t sample;
    // cmpwi cr6,r10,0 ; bne ; cmpwi cr6,r10,1 ; bne ; cmpwi cr6,r10,2 ; bne
    if (quadrant == 0) {
      sample = static_cast<int32_t>(REX_LOAD_U16(offset + kSineTable));  // lhzx r11,r9,r8
    } else if (quadrant == 1) {
      // addi r8,r11,512 ; subf r7,r9,r8 ; lhz r11,0(r7) -- the 64-bit subtract is truncated by
      // the load's use of r7.u32, so 32-bit address arithmetic is equivalent here.
      sample = static_cast<int32_t>(REX_LOAD_U16(kSineTable + kSineMirror - offset));
    } else if (quadrant == 2) {
      // lhzx r7,r9,r8 ; neg r11,r7 ; extsw r11,r11 -- the negate of a zero-extended u16, low word.
      sample = -static_cast<int32_t>(REX_LOAD_U16(offset + kSineTable));
    } else {
      // subf ; lhz r6,0(r7) ; neg r11,r6 ; extsw r11,r11
      sample = -static_cast<int32_t>(REX_LOAD_U16(kSineTable + kSineMirror - offset));
    }
    ctx.fpscr.disableFlushModeUnconditional();  // loc_82B1D51C, before the lfd of the spill
    const double sample_f = IntToSingle(sample);                     // fcfid f7 ; frsp f6
    out = MulSingle(MulSingle(sample_f, amplitude),                  // fmuls f5,f6,f0
                    LoadSingle(base, kSineNorm));                    // lfs f12 ; fmuls f0,f5,f12
  } else if (waveform == 1) {  // loc_82B1D544
    ctx.fpscr.disableFlushModeUnconditional();
    // fcmpu cr6,f13,f10 ; bge cr6,0x82b1d598 keeps the amplitude ; fmr f0,f11 takes zero.
    out = (phase < half) ? zero : amplitude;
  } else if (waveform == 2) {  // loc_82B1D55C
    ctx.fpscr.disableFlushModeUnconditional();
    out = MulSingle(phase, amplitude);  // fmuls f0,f13,f0
  } else {  // loc_82B1D56C: the triangle, folded about the half point
    ctx.fpscr.disableFlushModeUnconditional();
    // fcmpu cr6,f13,f10 ; bge cr6,0x82b1d588
    if (phase < half) {
      out = MulSingle(MulSingle(phase, amplitude),      // fmuls f12,f13,f0
                      LoadSingle(base, kTriangleGain));  // lfs f0,3152(r11) ; fmuls f0,f12,f0
    } else {
      ctx.fpscr.disableFlushModeUnconditional();  // loc_82B1D588
      // fsubs f8,f12,f13 -- f12 is still the wrap constant loaded at entry, NOT the round-half
      // constant in f10. That is why kPhaseWrap is read again here.
      out = MulSingle(MulSingle(SubSingle(wrap, phase), amplitude),  // fsubs f8 ; fmuls f7,f8,f0
                      LoadSingle(base, kTriangleGain));              // lfs f12 ; fmuls f0,f7,f12
    }
  }

  // loc_82B1D598: the phase advances on every path, including the pulse's flat sections.
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, osc + kPhase, AddSingle(phase, increment));  // fadds f13,f13,f9 ; stfs f13,4(r3)
  // fcmpu cr6,f0,f11 ; blt cr6,0x82b1d5bc -- round half away from zero again.
  if (out < zero) {
    ctx.fpscr.disableFlushModeUnconditional();
    out = SubSingle(out, half);  // fsubs f0,f0,f10
  } else {
    out = AddSingle(out, half);  // fadds f0,f0,f10
  }
  ctx.r3.u64 = FctiwzLowWord(out);  // fctiwz f13 ; stfd f13,-16(r1) ; lwz r3,-12(r1)
}

// The only store is the phase, at a fixed offset from r3, so the write set is known at entry on
// every path. The fctiwz/fcfid spills at r1-16 are the function's own red zone: not windowed.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t osc = ctx.r3.u32;
  // Declared unconditionally: the period <= 0 path writes nothing, and a byte this call leaves
  // alone compares equal anyway.
  spec.write(osc + kPhase, 4);
  spec.read(osc, 16);
  spec.read(kUpdateDelta, 4);
  spec.read(kPhaseWrap, 4);
  spec.read(kRoundHalf, 4);
  spec.read(kZeroFloat, 4);
  const uint32_t waveform = REX_LOAD_U8(osc + kWaveform);
  if (waveform == 0) {
    spec.read(kPhaseToUnits, 4);
    // Which quadrant and entry the phase lands in is not derivable without redoing the float
    // work, so the whole quarter table is recorded. 514 bytes, not 512: quadrants 1 and 3 read
    // backwards from kSineTable+512, so entry 0 there is the halfword AT the mirror point.
    spec.read(kSineTable, kSineMirror + 2);
    spec.read(kSineNorm, 4);
  } else if (waveform >= 3) {
    spec.read(kTriangleGain, 4);
  }
  return true;
}

}  // namespace port_82B1D3D0
SKATE3_PORT(82B1D3D0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
