// sub_82B1C910  role: step a multi-segment envelope: start it, jump it to a named segment, or count the current segment down and re-arm the next one; returns the value as an int   lifted: skate3_recomp.66.cpp:64938 (274 lines)
// STATUS: verified
// calls/session: boot 32520  play 32076  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C910.md
namespace port_82B1C910 {

// The envelope object (nothing in docs/rw_audio_structs.h names it):
//   +0   u16  byte offset, within this same object, of the mode word (lhz then lwzx r9,r8,r3)
//   +2   u8   the mode word's low byte as of the previous call; the tail rewrites it
//   +3   u8   current segment index
//   +4   f32  time left in the current segment
//   +8   f32  per-tick increment added to the value
//   +12  f32  current value -- the number this call returns, rounded
//   +16  u8   segment count
//   +18  u16  low byte is a segment index to jump to; the SIGNED halfword gates the jump
//   +20  f32  initial value, used when the envelope starts
//   +24  segments, stride 8: {f32 duration, f32 target}
// Mode values that matter: 1 start/run, 2 hold (the value is left alone), 3 jump/run.
constexpr uint32_t kModeOffset = 0;
constexpr uint32_t kModePrev = 2;
constexpr uint32_t kIndex = 3;
constexpr uint32_t kTimer = 4;
constexpr uint32_t kStep = 8;
constexpr uint32_t kValue = 12;
constexpr uint32_t kCount = 16;
constexpr uint32_t kJump = 18;
constexpr uint32_t kInitial = 20;
constexpr uint32_t kSegments = 24;
constexpr uint32_t kSegmentStride = 8;
constexpr uint32_t kSegmentTarget = 4;  // lfs 28(r9) with r9 = object + index*8

// Pool constants, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read off by eye.
// All three are addresses the already-verified ports use.
constexpr uint32_t kZeroFloat = 0x82165A10;    // lis -32234 ; lfs 23056
constexpr uint32_t kUpdateDelta = 0x830775D8;  // lis -31993 ; lfs 30168   (also sub_82B1D3D0)
constexpr uint32_t kRoundHalf = 0x8209975C;    // lis -32246 ; lfs -26788

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
  const uint32_t env = ctx.r3.u32;
  const uint32_t mode_offset = REX_LOAD_U16(env + kModeOffset);                  // lhz r8,0(r3)
  const int32_t mode = static_cast<int32_t>(REX_LOAD_U32(mode_offset + env));    // lwzx r9,r8,r3
  ctx.fpscr.disableFlushModeUnconditional();
  const double zero = LoadSingle(base, kZeroFloat);  // lfs f13,23056(r11) -- held in f13 to the tail
  bool handled = false;      // a branch that jumps straight to loc_82B1CA90
  bool clear_value = false;  // loc_82B1CA8C: the value is forced to zero

  // cmpwi cr6,r9,1 ; bne ; lbz r11,2(r3) ; cmplwi cr6,r11,0 ; bne -- a start, seen once
  if (mode == 1 && REX_LOAD_U8(env + kModePrev) == 0) {
    const double initial = LoadSingle(base, env + kInitial);            // lfs f0,20(r3)
    REX_STORE_U8(env + kIndex, 0);                                      // stb r11,3(r3): r11 is 0
    StoreSingle(base, env + kValue, initial);                           // stfs f0,12(r3)
    const double duration = LoadSingle(base, env + kSegments);          // lfs f12,24(r3)
    StoreSingle(base, env + kTimer, duration);                          // stfs f12,4(r3)
    const double target = LoadSingle(base, env + kSegments + kSegmentTarget);  // lfs f11,28(r3)
    const double slope = DivSingle(SubSingle(target, initial), duration);      // fsubs f10 ; fdivs f9
    StoreSingle(base, env + kStep,
                MulSingle(slope, LoadSingle(base, kUpdateDelta)));      // fmuls f8 ; stfs f8,8(r3)
    handled = true;                                                     // b 0x82b1ca90
  }

  // loc_82B1C968: cmpwi cr6,r9,3 ; bne ; lbz r11,2(r3) ; cmplwi cr6,r11,3 ; beq
  if (!handled && mode == 3 && REX_LOAD_U8(env + kModePrev) != 3) {
    const uint32_t jump = REX_LOAD_U16(env + kJump);              // lhz r11,18(r3)
    const int32_t current = static_cast<int32_t>(REX_LOAD_U8(env + kIndex));  // lbz r10,3(r3)
    // extsw/extsh r7,r11 ; cmpw cr6,r10,r7 ; bge -- a SIGNED halfword compare against the index
    if (current < static_cast<int32_t>(static_cast<int16_t>(jump))) {
      const uint32_t index = jump & 0xFFu;  // clrlwi r10,r11,24 -- only the low byte indexes
      ctx.fpscr.disableFlushModeUnconditional();
      const double value = LoadSingle(base, env + kValue);        // lfs f12,12(r3)
      REX_STORE_U8(env + kIndex, static_cast<uint8_t>(index));    // stb r10,3(r3)
      // rlwinm r11,r11,3,21,28 gives index*8; rlwinm r7,r10+3,3,0,28 gives index*8 + 24.
      const uint32_t segment = env + kSegments + index * kSegmentStride;
      const double duration = LoadSingle(base, segment);          // lfsx f11,r7,r3
      StoreSingle(base, env + kTimer, duration);                  // stfs f11,4(r3)
      const double target = LoadSingle(base, segment + kSegmentTarget);  // lfs f10,28(r9)
      const double slope = DivSingle(SubSingle(target, value), duration);  // fsubs f9 ; fdivs f8
      StoreSingle(base, env + kStep,
                  MulSingle(slope, LoadSingle(base, kUpdateDelta)));  // fmuls f7 ; stfs f7,8(r3)
      handled = true;                                             // b 0x82b1ca90
    }
  }

  // loc_82B1C9D4: cmpwi cr6,r9,1 ; beq ; cmpwi cr6,r9,3 ; bne cr6,0x82b1ca84
  if (!handled) {
    if (mode == 1 || mode == 3) {
      const uint32_t count = REX_LOAD_U8(env + kCount);  // lbz r10,16(r3)
      const uint32_t index = REX_LOAD_U8(env + kIndex);  // lbz r11,3(r3)
      // cmplw cr6,r11,r10 ; bge cr6,0x82b1ca84 -- an unsigned compare of the two bytes
      if (index < count) {
        ctx.fpscr.disableFlushModeUnconditional();
        const double timer = LoadSingle(base, env + kTimer);      // lfs f12,4(r3)
        const double delta = LoadSingle(base, kUpdateDelta);      // lfs f0,30168(r9)
        const double left = SubSingle(timer, delta);              // fsubs f11,f12,f0
        StoreSingle(base, env + kTimer, left);                    // stfs f11,4(r3)
        // fcmpu cr6,f11,f13 ; bgt cr6,0x82b1ca70 -- unordered takes the segment-done side
        if (left > zero) {  // loc_82B1CA70
          ctx.fpscr.disableFlushModeUnconditional();
          const double value = LoadSingle(base, env + kValue);    // lfs f0,12(r3)
          const double step = LoadSingle(base, env + kStep);      // lfs f12,8(r3)
          StoreSingle(base, env + kValue, AddSingle(step, value));  // fadds f11,f12,f0 ; stfs 12(r3)
        } else {
          // The segment is done: the value snaps to its target and the index advances.
          const uint32_t next = (index + 1u) & 0xFFu;  // addi r11,r11,1 ; clrlwi r7,r11,24
          const double reached =
              LoadSingle(base, env + kSegments + index * kSegmentStride + kSegmentTarget);  // lfs 28(r9)
          REX_STORE_U8(env + kIndex, static_cast<uint8_t>(next));  // stb r7,3(r3)
          StoreSingle(base, env + kValue, reached);                // stfs f12,12(r3)
          // cmplw cr6,r7,r10 ; bge cr6,0x82b1ca8c
          if (next < count) {
            const uint32_t segment = env + kSegments + next * kSegmentStride;
            const double duration = LoadSingle(base, segment);    // lfsx f11,r7,r3
            StoreSingle(base, env + kTimer, duration);            // stfs f11,4(r3)
            const double target = LoadSingle(base, segment + kSegmentTarget);  // lfs f9,28(r6)
            // The slope starts from the target just reached, not from the stored value.
            const double slope = DivSingle(SubSingle(target, reached), duration);  // fsubs ; fdivs
            StoreSingle(base, env + kStep, MulSingle(slope, delta));  // fmuls f6,f7,f0 ; stfs 8(r3)
          } else {
            clear_value = true;  // bge cr6,0x82b1ca8c
          }
        }
      } else {
        clear_value = true;  // loc_82B1CA84 with mode 1 or 3: cmpwi r9,2 cannot match
      }
    } else if (mode != 2) {
      clear_value = true;  // loc_82B1CA84: cmpwi cr6,r9,2 ; beq cr6,0x82b1ca90
    }
  }
  if (clear_value) {  // loc_82B1CA8C
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, env + kValue, zero);  // stfs f13,12(r3)
  }

  // loc_82B1CA90. The mode word is re-read AFTER the stores above, which is observable when the
  // offset at +0 points inside +2..+15, so it is reloaded rather than reused.
  const uint32_t mode_again = REX_LOAD_U32(mode_offset + env);  // lwzx r11,r8,r3
  ctx.fpscr.disableFlushModeUnconditional();
  double value = LoadSingle(base, env + kValue);                // lfs f0,12(r3)
  // fcmpu cr6,f0,f13 happens BEFORE the byte store and before f13 is reloaded with the constant.
  const bool negative = value < zero;
  REX_STORE_U8(env + kModePrev, static_cast<uint8_t>(mode_again & 0xFFu));  // stb r11,2(r3)
  const double half = LoadSingle(base, kRoundHalf);              // lfs f13,-26788(r11)
  // blt cr6,0x82b1cac0 -- round half away from zero, then truncate.
  if (negative) {
    ctx.fpscr.disableFlushModeUnconditional();
    value = SubSingle(value, half);  // fsubs f0,f0,f13
  } else {
    value = AddSingle(value, half);  // fadds f0,f0,f13
  }
  ctx.r3.u64 = FctiwzLowWord(value);  // fctiwz f13 ; stfd f13,-16(r1) ; lwz r3,-12(r1)
}

// Every store is at a fixed offset from r3 -- +2, +3, +4, +8, +12 -- so the write set is known at
// entry on every path. Declared as the union of all paths, two contiguous spans. The fctiwz spill
// at r1-16 is the function's own red zone: not windowed.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t env = ctx.r3.u32;
  spec.write(env + kModePrev, 2);  // +2 always, +3 on the start, jump and segment-done paths
  spec.write(env + kTimer, 12);    // the timer at +4, the step at +8 and the value at +12
  const uint32_t mode_offset = REX_LOAD_U16(env + kModeOffset);
  spec.read(env, kSegments);
  spec.read(mode_offset + env, 4);
  // The segment array is indexed by the count at +16 or the low byte of the jump at +18, both
  // single bytes, so 256 entries plus one look-ahead is a hard ceiling. One span, replay only.
  const uint32_t count = REX_LOAD_U8(env + kCount);
  const uint32_t jump = REX_LOAD_U16(env + kJump) & 0xFFu;
  const uint32_t entries = (count > jump ? count : jump) + 2u;
  spec.read(env + kSegments, entries * kSegmentStride);
  spec.read(kZeroFloat, 4);
  spec.read(kUpdateDelta, 4);
  spec.read(kRoundHalf, 4);
  return true;
}

}  // namespace port_82B1C910
SKATE3_PORT(82B1C910, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
