// sub_82B443F8  role: advance a fill position and clear count frames in every channel of a plan   lifted: skate3_recomp.68.cpp:26626 (122 lines)
// STATUS: verified
// calls/session: boot 500817  play 541975  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B443F8.md
namespace port_82B443F8 {

// Nothing in docs/rw_audio_structs.h names any of these structures; the names describe the
// arithmetic this function performs on them and nothing more.
//
// r4 -- the fill state: three singles and a byte.
constexpr uint32_t kLimitFloor = 40;  // float: the limit at +48 is clamped up to this
constexpr uint32_t kPosition = 44;    // float: frames accounted for so far
constexpr uint32_t kLimit = 48;       // float
constexpr uint32_t kIdleFlag = 69;    // byte: zeroed on the "nothing to do" return
// r5 -- the channel layout.
constexpr uint32_t kChannelCount = 42;  // byte, re-read every pass of the loop
// r3 -- the owner: a pointer to the buffer plan and the count it last cleared.
constexpr uint32_t kPlan = 28;
constexpr uint32_t kFramesCleared = 48;
// The plan: the first channel's buffer and the frame stride between channels.
constexpr uint32_t kPlanBuffer = 4;   // float*
constexpr uint32_t kPlanStride = 14;  // u16, frames

// Four scalar spans plus one per channel must stay inside kPortMaxSpans (32).
constexpr uint32_t kMaxChannels = 24;

inline double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

inline double AddSingle(double a, double b) { return double(float(a + b)); }  // fadds

// extsw r11,r27 ; std r11,80(r1) ; lfd f13,80(r1) ; fcfid f12,f13 ; frsp f11,f12 -- the count as
// a single, held in a double. The spill slot is the function's own frame: not reproduced, not
// windowed.
inline double CountToSingle(uint32_t count) {
  return double(float(double(int64_t(static_cast<int32_t>(count)))));
}

// rlwinm rN,rX,2,0,29 -- the low word scaled by four, the result zero-extended to 64 bits.
inline uint64_t WordsToBytes(uint64_t value) {
  return (uint64_t(uint32_t(value)) << 2) & 0xFFFFFFFCull;
}

// The clear target for channel `index`: rlwinm/mullw/rlwinm then `add r3,r11,r10`, which is a
// 64-bit add of two zero-extended words -- a carry into bit 32 survives into the callee's r3, so
// the 64-bit value is what gets passed and only the store addresses are truncated.
inline uint64_t ChannelTarget(uint32_t stride, uint32_t index, uint32_t buffer) {
  // mullw r9,r11,r31 keeps the full 64-bit product of the sign-extended halves; the rlwinm that
  // follows then takes its LOW word and scales that by four.
  const uint64_t frames =
      uint64_t(int64_t(static_cast<int32_t>(stride)) * int64_t(static_cast<int32_t>(index)));
  return WordsToBytes(frames) + uint64_t(buffer);
}

REX_FUNC(Native) {
  const uint32_t owner = ctx.r3.u32;   // mr r26,r3
  const uint32_t state = ctx.r4.u32;
  const uint32_t layout = ctx.r5.u32;  // mr r29,r5
  const uint32_t count = ctx.r6.u32;   // mr r27,r6

  ctx.fpscr.disableFlushModeUnconditional();
  const double floor_limit = LoadSingle(base, state + kLimitFloor);  // lfs f0,40(r4)
  const double limit = LoadSingle(base, state + kLimit);             // lfs f13,48(r4)
  // fcmpu cr6,f13,f0 ; bge cr6 -- an unordered compare takes the branch, so a NaN on either side
  // skips the store, exactly as `limit < floor_limit` does here.
  if (limit < floor_limit) {
    StoreSingle(base, state + kLimit, floor_limit);  // stfs f0,48(r4)
  }

  ctx.fpscr.disableFlushModeUnconditional();
  const double limit_now = LoadSingle(base, state + kLimit);    // lfs f13,48(r4) -- reloaded
  const double position = LoadSingle(base, state + kPosition);  // lfs f0,44(r4)
  // fcmpu cr6,f0,f13 ; blt cr6 -- not-less, NaN included, falls out with zero.
  if (!(position < limit_now)) {
    REX_STORE_U8(state + kIdleFlag, 0);  // li r11,0 ; stb r11,69(r4)
    ctx.r3.s64 = 0;                      // li r3,0
    return;
  }

  ctx.fpscr.disableFlushModeUnconditional();
  // fadds f10,f11,f0 ; stfs f10,44(r4) -- the position moves before anything is cleared.
  StoreSingle(base, state + kPosition, AddSingle(CountToSingle(count), position));

  const uint32_t channels = REX_LOAD_U8(layout + kChannelCount);  // lbz r10,42(r29)
  const uint32_t plan = REX_LOAD_U32(owner + kPlan);              // lwz r30,28(r26)
  // cmplwi cr6,r10,0 ; beq cr6 -- zero channels clears nothing but still publishes the count.
  if (channels != 0) {
    const uint64_t bytes = WordsToBytes(count);  // rlwinm r28,r27,2,0,29
    uint32_t index = 0;                          // li r31,0
    do {
      // Both reloaded every pass, as the original does: a clear that lands on the plan changes
      // the next pass's target.
      const uint32_t stride = REX_LOAD_U16(plan + kPlanStride);  // lhz r11,14(r30)
      const uint32_t buffer = REX_LOAD_U32(plan + kPlanBuffer);  // lwz r10,4(r30)
      // bl 0x82ee5e80 -- memset(target, 0, bytes); li r4,0 supplies the fill byte.
      skate3::audio::GuestCall(ctx, base, sub_82EE5E80, ChannelTarget(stride, index, buffer),
                               uint64_t(0), bytes);
      index += 1;  // addi r31,r31,1
      // lbz r8,42(r29) ; cmplw cr6,r31,r8 ; blt cr6 -- the count is re-read, unsigned compare.
    } while (index < REX_LOAD_U8(layout + kChannelCount));
  }
  REX_STORE_U32(owner + kFramesCleared, count);  // stw r27,48(r26)
  ctx.r3.s64 = 1;                                // li r3,1
}

// Writes, the union of both paths (a byte a given call leaves alone compares equal anyway): the
// limit at r4+48, the position at r4+44, the idle byte at r4+69, the published count at r3+48,
// and per channel `bytes` at plan_buffer + 4*stride*channel, which is what sub_82EE5E80 (memset)
// writes -- it writes exactly [r3, r3+r5) and nothing else, on every one of its paths.
//
// The loop's control values (the channel count at r5+42, the plan's buffer and stride) are
// re-read inside the loop, so a clear that overlaps them would move the remaining targets:
// those cases are not enumerable from entry state and return false.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  const uint32_t layout = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;

  spec.write(state + kPosition, 8);  // +44 and +48, adjacent
  spec.write(state + kIdleFlag, 1);
  spec.write(owner + kFramesCleared, 4);
  spec.read(state + kLimitFloor, 4);  // +44/+48 are in the write windows already
  spec.read(layout + kChannelCount, 1);
  spec.read(owner + kPlan, 4);

  const uint32_t channels = REX_LOAD_U8(layout + kChannelCount);
  if (channels == 0) return true;  // the loop, and every memset with it, is skipped

  const uint32_t plan = REX_LOAD_U32(owner + kPlan);
  if (plan == 0) return false;  // the original would read the plan through null
  spec.read(plan + kPlanBuffer, 12);  // +4 the buffer, +14 the stride

  const uint64_t bytes = WordsToBytes(count);
  if (bytes == 0) return true;  // every memset is a no-op
  if (channels > kMaxChannels) return false;                               // over the span budget
  if (bytes * channels > skate3::audio::kPortWatchCap) return false;       // over the byte budget

  const uint32_t stride = REX_LOAD_U16(plan + kPlanStride);
  const uint32_t buffer = REX_LOAD_U32(plan + kPlanBuffer);
  if (buffer == 0) return false;  // the clear would run from guest address 0; not comparable

  auto overlaps = [](uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
    return alen != 0 && blen != 0 && a < b + blen && b < a + alen;
  };
  for (uint32_t index = 0; index < channels && index < kMaxChannels; index++) {
    const uint64_t target = ChannelTarget(stride, index, buffer);
    const uint32_t addr = uint32_t(target);
    if (uint64_t(addr) + bytes > 0x100000000ull) return false;  // the span would wrap
    if (overlaps(addr, bytes, uint64_t(plan) + kPlanBuffer, 12) ||
        overlaps(addr, bytes, uint64_t(layout) + kChannelCount, 1)) {
      return false;  // a clear over the loop's own control words
    }
    spec.write(addr, uint32_t(bytes));
  }
  return true;
}

}  // namespace port_82B443F8
SKATE3_PORT(82B443F8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
