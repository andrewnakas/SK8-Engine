// sub_82B49268  role: publish a float from a command record onto the object at its +4   lifted: skate3_recomp.68.cpp:38538 (17 lines)
// STATUS: verified
// calls/session: boot 745  play 981  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B49268.md
namespace port_82B49268 {

constexpr uint32_t kRecordTarget = 4;   // the object the value lands on
constexpr uint32_t kRecordValue = 8;    // the float to publish
constexpr uint32_t kTargetField = 56;   // where it lands on that object
constexpr uint32_t kRecordSize = 12;    // returned in r3, the record's size

REX_FUNC(Native) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t target = REX_LOAD_U32(record + kRecordTarget);  // lwz r11,4(r3)
  ctx.fpscr.disableFlushModeUnconditional();                     // emitted before the lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(record + kRecordValue);                // lfs f0,8(r3)
  const double widened = double(temp.f32);
  ctx.r3.s64 = kRecordSize;                                      // li r3,12
  temp.f32 = float(widened);                                     // stfs f0,56(r11)
  REX_STORE_U32(target + kTargetField, temp.u32);
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t target = REX_LOAD_U32(record + kRecordTarget);
  if (target == 0) return false;  // the original would store through null; not comparable
  spec.write(target + kTargetField, 4);
  spec.read(record, kRecordSize);
  return true;
}

}  // namespace port_82B49268
SKATE3_PORT(82B49268, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
