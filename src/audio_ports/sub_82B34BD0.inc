// sub_82B34BD0  role: unlink a voice, fold its gains into the owner, and clear its fields   lifted: skate3_recomp.67.cpp:55669 (109 lines)
// STATUS: verified
// calls/session: boot 9408  play 10860  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34BD0.md
namespace port_82B34BD0 {

constexpr uint32_t kPrev = 0;
constexpr uint32_t kNext = 4;
constexpr uint32_t kLiveWord = 8;
constexpr uint32_t kListToken = 12;   // zero means already detached: the function returns at once
constexpr uint32_t kOwner = 16;
constexpr uint32_t kShortField = 20;
constexpr uint32_t kGainCount = 22;   // u8, reloaded every iteration
constexpr uint32_t kOwnerHead = 4;
constexpr uint32_t kOwnerDirty = 46;
constexpr uint32_t kOwnerGains = 12;  // the accumulation target, four bytes per gain

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;
  if (REX_LOAD_U32(self + kListToken) == 0) {
    return;  // beqlr -- writes nothing at all
  }
  const uint32_t owner = REX_LOAD_U32(self + kOwner);
  const uint32_t head = REX_LOAD_U32(owner + kOwnerHead);
  if (self == head) {
    REX_STORE_U32(owner + kOwnerHead, REX_LOAD_U32(head + kPrev));  // stw r11,4(r10)
  }
  const uint32_t next = REX_LOAD_U32(self + kNext);
  if (static_cast<int32_t>(next) != 0) {
    REX_STORE_U32(next + kPrev, REX_LOAD_U32(self + kPrev));  // stw r10,0(r11)
  }
  const uint32_t prev = REX_LOAD_U32(self + kPrev);
  if (static_cast<int32_t>(prev) != 0) {
    REX_STORE_U32(prev + kNext, REX_LOAD_U32(self + kNext));  // stw r10,4(r11)
  }
  const uint32_t gains = ctx.r4.u32;
  if (gains != 0) {
    REX_STORE_U8(REX_LOAD_U32(self + kOwner) + kOwnerDirty, 1);  // stb r9,46(r11)
    if (REX_LOAD_U8(self + kGainCount) != 0) {
      uint32_t offset = kOwnerGains;
      for (int32_t i = 0;;) {
        // The owner pointer is RELOADED each iteration, and so is the count. Reproduced.
        const uint32_t target = REX_LOAD_U32(self + kOwner) + offset;
        ctx.fpscr.disableFlushModeUnconditional();
        PPCRegister added{};
        added.u32 = REX_LOAD_U32(gains + (offset - kOwnerGains));  // lfsx f0,r8,r11
        const double increment = double(added.f32);
        i++;
        offset += 4;
        PPCRegister current{};
        current.u32 = REX_LOAD_U32(target);                        // lfs f13,0(r9)
        current.f32 = float(increment + double(current.f32));      // fadds f12,f0,f13
        REX_STORE_U32(target, current.u32);                        // stfs f12,0(r9)
        if (i >= static_cast<int32_t>(REX_LOAD_U8(self + kGainCount))) break;
      }
    }
  }
  REX_STORE_U32(self + kListToken, 0);  // stw r7,12(r3)
  REX_STORE_U32(self + kOwner, 0);      // stw r7,16(r3)
  REX_STORE_U32(self + kLiveWord, 0);   // stw r7,8(r3)
  REX_STORE_U8(self + kGainCount, 0);   // stb r7,22(r3)
  REX_STORE_U16(self + kShortField, 0); // sth r7,20(r3)
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  if (REX_LOAD_U32(self + kListToken) == 0) {
    spec.read(self + kListToken, 4);
    return true;  // no writes on this path
  }
  const uint32_t owner = REX_LOAD_U32(self + kOwner);
  const uint32_t count = REX_LOAD_U8(self + kGainCount);
  const uint32_t gains = ctx.r4.u32;
  if (gains != 0 && count != 0) {
    // The owner pointer and the count are both re-read inside the loop. If either field lives
    // inside the accumulated range, its value changes mid-call and neither the addresses nor
    // the trip count derive from entry state -- gate 2.
    const uint32_t lo = owner + kOwnerGains;
    const uint32_t hi = lo + 4 * count;
    if (self + kOwner + 3 >= lo && self + kOwner < hi) return false;
    if (self + kGainCount >= lo && self + kGainCount < hi) return false;
    spec.write(lo, 4 * count);
    spec.write(owner + kOwnerDirty, 1);
    spec.read(gains, 4 * count);
  } else if (gains != 0) {
    spec.write(owner + kOwnerDirty, 1);
  }
  const uint32_t head = REX_LOAD_U32(owner + kOwnerHead);
  const uint32_t next = REX_LOAD_U32(self + kNext);
  const uint32_t prev = REX_LOAD_U32(self + kPrev);
  if (self == head) {
    spec.write(owner + kOwnerHead, 4);
    if (head != 0) spec.read(head + kPrev, 4);
  }
  if (static_cast<int32_t>(next) != 0) spec.write(next + kPrev, 4);
  if (static_cast<int32_t>(prev) != 0) spec.write(prev + kNext, 4);
  spec.write(self + kLiveWord, 12);   // 8, 12, 16
  spec.write(self + kShortField, 3);  // 20, 21, 22
  spec.read(self, 24);
  spec.read(owner + kOwnerHead, 4);
  return true;
}

}  // namespace port_82B34BD0
SKATE3_PORT(82B34BD0, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
