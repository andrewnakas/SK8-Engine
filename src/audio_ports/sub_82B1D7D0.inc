// sub_82B1D7D0  role: take-and-clear a byte flag at +25 of an object, return the old value   lifted: skate3_recomp.66.cpp:67131 (13 lines)
// STATUS: verified
// calls/session: boot 268706  play 311108  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D7D0.md
namespace port_82B1D7D0 {

constexpr uint32_t kFlag25 = 25;  // nothing in docs/rw_audio_structs.h ties this object to a named layout

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;                     // mr r11,r3
  ctx.r3.u64 = REX_LOAD_U8(object + kFlag25);             // lbz r3,25(r3)  (zero-extended to 64 bits)
  REX_STORE_U8(object + kFlag25, static_cast<uint8_t>(0)); // li r10,0 ; stb r10,25(r11)
}                                                         // blr

// The single store: one byte at r3+25, known from entry state. Leaf, no callees.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r3.u32 + kFlag25, 1);
  return true;
}

}  // namespace port_82B1D7D0
SKATE3_PORT(82B1D7D0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
