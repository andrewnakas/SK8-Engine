// sub_82B34268  role: store a 16-bit field at +460 of an object, return 0   lifted: skate3_recomp.67.cpp:54301 (11 lines)
// STATUS: verified
// calls/session: boot 209196  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34268.md
namespace port_82B34268 {

constexpr uint32_t kField460 = 460;  // nothing in docs/rw_audio_structs.h names this offset

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;               // mr r11,r3
  ctx.r3.s64 = 0;                                   // li r3,0
  REX_STORE_U16(object + kField460, ctx.r6.u16);    // sth r6,460(r11)
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r3.u32 + kField460, 2);
  return true;
}

}  // namespace port_82B34268
SKATE3_PORT(82B34268, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
