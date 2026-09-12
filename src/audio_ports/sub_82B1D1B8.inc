// sub_82B1D1B8  role: return the signed product of two 32-bit fields at +4 and +0 (register-only)   lifted: skate3_recomp.66.cpp:66218 (11 lines)
// STATUS: verified
// calls/session: boot 170206  play 212282  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D1B8.md
namespace port_82B1D1B8 {

REX_FUNC(Native) {
  // Both loads are zero-extended u32; mullw multiplies the LOW 32 bits of each as signed and the
  // lifted form keeps the full 64-bit product in r3 (not truncated to 32 bits), so this is a
  // 64-bit multiply of two sign-extended s32 values. The caller's r3 is compared as all 64 bits.
  // Sibling of sub_82B1D198, which sums the same two fields; nothing in docs/rw_audio_structs.h
  // names this 8-byte object.
  const int32_t second = static_cast<int32_t>(REX_LOAD_U32(ctx.r3.u32 + 4));  // lwz r11,4(r3)
  const int32_t first = static_cast<int32_t>(REX_LOAD_U32(ctx.r3.u32 + 0));   // lwz r10,0(r3)
  ctx.r3.s64 = int64_t(second) * int64_t(first);                               // mullw r3,r11,r10
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 8);  // no stores: the result lives in r3 only
  return true;
}

}  // namespace port_82B1D1B8
SKATE3_PORT(82B1D1B8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
