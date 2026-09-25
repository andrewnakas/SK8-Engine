// sub_82B2FEA8  role: map a six-point ramp through a 1652-entry ascending table into six integers   lifted: skate3_recomp.67.cpp:44318 (138 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 4 comparable calls in the weaker profile, 4 in the stronger, against 138 lifted lines -- four calls. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 8  play 8  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2FEA8.md
namespace port_82B2FEA8 {

// Addresses computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye: one misread
// digit in sub_82B2FE00 cost this project's first shadow divergence. The pool base is the same
// one sub_82B2FE00 uses (it loads 468/472/476 from it); this function loads 456/460/464.
constexpr uint32_t kPool = 0x822F8600;         // lis -32208 ; addi r6,r10,-31232
constexpr uint32_t kInputScale = kPool + 456;  // lfs f8,456(r6)
constexpr uint32_t kThreshold = kPool + 460;   // lfs f12,460(r6)
constexpr uint32_t kHighScale = kPool + 464;   // lfs f9,464(r6)
constexpr uint32_t kBaseline = 0x8231A844;     // lis -32206 ; lfs f10,-22460(r9)

constexpr uint32_t kTableOffset = 1096;  // lwz r9,1096(r3)
constexpr int32_t kTableEntries = 1652;  // cmpwi cr6,r11,1652
constexpr uint32_t kPoints = 6;          // li r10,6 ; mtctr r10

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

double MulSingle(double a, double b) { return double(float(a * b)); }

// fctiwz then stfiwx of the low word: NaN gives 0x80000000, above INT_MAX saturates, and only
// the low 32 bits of the doubleword reach memory.
uint32_t FctiwzLowWord(double value) {
  const int64_t fixed = std::isnan(value)
                            ? int64_t(0x80000000U)
                            : (value > double(INT_MAX))
                                  ? int64_t(INT_MAX)
                                  : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
  return static_cast<uint32_t>(static_cast<uint64_t>(fixed) & 0xFFFFFFFFu);
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t in = ctx.r4.u32;   // six floats; the block sub_82B2FE00 fills
  const uint32_t out = ctx.r5.u32;  // six words
  const double input = ctx.f1.f64;  // f1, read on every point and never modified
  REX_STORE_U32(out + 20, 0);       // stw r11,20(r5) -- only the LAST word is cleared
  ctx.fpscr.disableFlushModeUnconditional();
  const double baseline = LoadSingle(base, kBaseline);       // lfs f10,-22460(r9)
  const double high_scale = LoadSingle(base, kHighScale);    // lfs f9,464(r6)
  const double threshold = LoadSingle(base, kThreshold);     // lfs f12,460(r6)
  const double input_scale = LoadSingle(base, kInputScale);  // lfs f8,456(r6)
  // r11: the table cursor index. It persists ACROSS the six points, so the search over the
  // whole call is a single forward sweep and a point can find nothing at all.
  int32_t index = 0;  // li r11,0
  for (uint32_t point = 0; point < kPoints; point++) {  // mtctr 6 ; bdnz 0x82b2fedc
    const uint32_t slot = out + 4 * point;              // r8, addi r8,r8,4 each pass
    ctx.fpscr.disableFlushModeUnconditional();
    // lfsx f0,r7,r8 with r7 = r4 - r5 (subf): the low word of the sum is in + 4*point.
    const double ramp = LoadSingle(base, in + 4 * point);
    const double scaled_ramp = MulSingle(ramp, input_scale);  // fmuls f11,f0,f8 (unconditional)
    double selected;  // f0
    double gain;      // f13
    // fcmpu cr6,f1,f12 ; ble -- an unordered compare (NaN input) takes the else.
    if (input > threshold) {
      selected = threshold;                  // fmr f0,f12
      gain = MulSingle(input, high_scale);   // fmuls f13,f1,f9
    } else {
      ctx.fpscr.disableFlushModeUnconditional();
      selected = input;   // fmr f0,f1
      gain = baseline;    // fmr f13,f10
    }
    // Reloaded on every point, and after this call's own stores, because out could alias it.
    const uint32_t table = REX_LOAD_U32(object + kTableOffset);  // lwz r9,1096(r3)
    ctx.fpscr.disableFlushModeUnconditional();
    const double target = MulSingle(selected, scaled_ramp);  // fmuls f0,f0,f11
    // cmpwi cr6,r11,1652 ; bge -- a sweep that already ran off the end skips the search.
    if (index < kTableEntries) {
      uint32_t cursor = table + (static_cast<uint32_t>(index) << 2);  // rlwinm r10,r11,2,0,29 ; add
      for (;;) {                                                      // loc_82B2FF18
        ctx.fpscr.disableFlushModeUnconditional();
        const double entry = LoadSingle(base, cursor);  // lfs f11,0(r10)
        if (entry > target) {                           // fcmpu cr6,f11,f0 ; bgt
          // The found element is RELOADED through the index rather than reused from f11.
          const uint32_t found = table + (static_cast<uint32_t>(index) << 2);  // rlwinm r10,r11,2,0,29
          index += 1;                                                          // addi r11,r11,1
          ctx.fpscr.disableFlushModeUnconditional();
          const double found_value = LoadSingle(base, found);  // lfsx f0,r10,r9
          REX_STORE_U32(slot, FctiwzLowWord(found_value));     // fctiwz f11,f0 ; stfiwx f11,0,r8
          break;
        }
        index += 1;                            // addi r11,r11,1
        cursor += 4;                           // addi r10,r10,4
        if (!(index < kTableEntries)) break;   // cmpwi ; blt 0x82b2ff18 -- else fall to F4C
      }
    }
    // loc_82B2FF4C: on both paths, including the one that stored nothing this pass.
    ctx.fpscr.disableFlushModeUnconditional();
    if (gain > baseline) {  // fcmpu cr6,f13,f10 ; ble
      // The word is read BACK out of memory rather than kept in a register, so a point that
      // stored nothing above rescales whatever was already in the slot.
      const int64_t stored = static_cast<int32_t>(REX_LOAD_U32(slot));  // lwz r10,0(r8) ; extsw
      const double widened = static_cast<double>(stored);  // std -16(r1) ; lfd ; fcfid f11,f0
      const double as_float = double(float(widened));      // frsp f7,f11
      const double rescaled = MulSingle(as_float, gain);   // fmuls f6,f7,f13
      REX_STORE_U32(slot, FctiwzLowWord(rescaled));        // fctiwz f5,f6 ; stfiwx f5,0,r8
    }
  }
  ctx.r3.s64 = 1;  // li r3,1
}

// Every store lands in the six words at r5: the entry stw at +20 and one or two per point, with
// r8 running r5, r5+4 .. r5+20. The fctiwz spill at r1-16 is the function's own red zone.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t table = REX_LOAD_U32(ctx.r3.u32 + kTableOffset);
  if (table == 0) return false;  // the original would sweep from guest address 0
  spec.write(ctx.r5.u32, 4 * kPoints);
  spec.read(ctx.r4.u32, 4 * kPoints);
  spec.read(ctx.r3.u32 + kTableOffset, 4);
  // The index sweeps forward across all six points, so any entry can be read; declared whole
  // rather than guessed, which is what makes a recorded vector replayable.
  spec.read(table, 4 * static_cast<uint32_t>(kTableEntries));
  spec.read(kBaseline, 4);
  spec.read(kInputScale, 12);  // 456, 460 and 464 are contiguous
  return true;
}

}  // namespace port_82B2FEA8
SKATE3_PORT(82B2FEA8, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnR3)
