// sub_82B1C7E8  role: single-float timer: optionally zero the accumulator (+8 != 0), then fire when it reaches float(s32 +12) -- +4 = 1, park it at -1.0, return 1 -- else add a live step and return 0   lifted: skate3_recomp.66.cpp:64757 (90 lines)
// STATUS: verified
// calls/session: boot 77185  play 95865  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C7E8.md
namespace port_82B1C7E8 {

// Layout read (nothing in docs/rw_audio_structs.h names this 16-byte object):
//   +0   f32  accumulator
//   +4   u8   fired     1 for the call that reaches the threshold, else 0 (also returned in r3)
//   +8   s32  reset     non-zero: zero the accumulator before the test
//   +12  s32  threshold converted to single (extsw ; fcfid ; frsp) for the compare
constexpr uint32_t kAccumulator = 0;
constexpr uint32_t kFired = 4;
constexpr uint32_t kReset = 8;
constexpr uint32_t kThreshold = 12;
constexpr uint32_t kObjectBytes = 16;

// Singles loaded through the guest map; values from the validated image dump (probe/harness/out/image):
//   0x82165A10  0.0f   (lis r10,-32234 ; lfs f0,23056(r10)) -- the same zero sub_82B1CF50 loads
//   0x8216DEE0 -1.0f   (lis r10,-32233 ; lfs f0,-8480(r10))  -- the parked value after firing
//   0x830775D8  0.0f in the dump, but it lies outside the rodata the other two share, so it may be
//               written at runtime (a per-tick step?). Treat it as a variable: the body loads it live.
constexpr uint32_t kZeroSingle = 0x82165A10;
constexpr uint32_t kParkedSingle = 0x8216DEE0;
constexpr uint32_t kStepSingle = 0x830775D8;

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// extsw r9,r10 ; std r9,-16(r1) ; lfd f0,-16(r1) ; fcfid f12,f0 ; frsp f11,f12
// The red-zone spill is the function's own frame and is not reproduced (nor windowed).
inline double WordToSingle(uint32_t word) {
  const int64_t wide = static_cast<int32_t>(word);             // extsw
  const double converted = static_cast<double>(wide);          // fcfid
  return static_cast<double>(static_cast<float>(converted));   // frsp
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                                         // mr r11,r3
  const int32_t reset = static_cast<int32_t>(REX_LOAD_U32(object + kReset));  // lwz r10,8(r3)
  ctx.fpscr.disableFlushModeUnconditional();                                  // lfs f0,23056(r10)
  const double zero = LoadSingle(base, kZeroSingle);

  // cmpwi cr6,r10,0 ; beq cr6,loc_82B1C808
  if (reset != 0) {
    StoreSingle(base, object + kAccumulator, zero);                           // stfs f0,0(r3)
    // b loc_82B1C814
  } else {
    // loc_82B1C808
    ctx.fpscr.disableFlushModeUnconditional();                                // lfs f13,0(r11)
    const double accumulator = LoadSingle(base, object + kAccumulator);
    // fcmpu cr6,f13,f0 ; blt cr6,loc_82B1C864 -- a NaN does not branch
    if (accumulator < zero) {
      // loc_82B1C864
      ctx.r3.s64 = 0;                                                         // li r3,0
      REX_STORE_U8(object + kFired, 0);                                       // stb r10,4(r11)  (r10 = 0)
      return;                                                                 // blr
    }
  }

  // loc_82B1C814
  const uint32_t threshold_word = REX_LOAD_U32(object + kThreshold);          // lwz r10,12(r11)
  ctx.fpscr.disableFlushModeUnconditional();                                  // lfs f13,0(r11)
  const double accumulator = LoadSingle(base, object + kAccumulator);         // reloaded after the store
  const double threshold = WordToSingle(threshold_word);                      // extsw ; fcfid ; frsp
  // fcmpu cr6,f13,f11 ; blt cr6,loc_82B1C854 -- a NaN does not branch, so it fires
  if (!(accumulator < threshold)) {
    ctx.r3.s64 = 1;                                                           // li r3,1
    REX_STORE_U8(object + kFired, 1);                                         // stb r9,4(r11)   (r9 = 1)
    const double parked = LoadSingle(base, kParkedSingle);                    // lfs f0,-8480(r10)
    StoreSingle(base, object + kAccumulator, parked);                         // stfs f0,0(r11)
    return;                                                                   // blr
  }

  // loc_82B1C854
  ctx.fpscr.disableFlushModeUnconditional();                                  // lfs f0,30168(r10)
  const double step = LoadSingle(base, kStepSingle);
  // fadds f0,f13,f0 -- the double sum rounded to single
  const double advanced = static_cast<double>(static_cast<float>(accumulator + step));
  StoreSingle(base, object + kAccumulator, advanced);                         // stfs f0,0(r11)

  // loc_82B1C864
  ctx.r3.s64 = 0;                                                             // li r3,0
  REX_STORE_U8(object + kFired, 0);                                           // stb r10,4(r11)  (r10 = 0)
  // blr
}

// Writes: the accumulator (+0, f32) on three of the four paths and the fired byte (+4) on every
// path -- one 5-byte span off the entry object, declared unconditionally. Reads: the 16-byte object
// and the three singles, so the vector replays without the image.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.write(object + kAccumulator, 5);  // +0..+3 accumulator, +4 fired
  spec.read(object, kObjectBytes);
  spec.read(kZeroSingle, 4);
  spec.read(kParkedSingle, 4);
  spec.read(kStepSingle, 4);
  return true;
}

}  // namespace port_82B1C7E8
SKATE3_PORT(82B1C7E8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
