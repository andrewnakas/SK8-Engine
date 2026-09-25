// sub_82B1D198  role: return the sum of two u32 fields at +0 and +4 (register-only)   lifted: skate3_recomp.66.cpp:66194 (11 lines)
// STATUS: verified
// calls/session: boot 698485  play 856261  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D198.md
namespace port_82B1D198 {

REX_FUNC(Native) {
  // Both loads are zero-extended into 64-bit registers and the add is 64-bit, so the result
  // can carry into bit 32; the caller's r3 is compared as all 64 bits.
  const uint64_t second = REX_LOAD_U32(ctx.r3.u32 + 4);  // lwz r11,4(r3)
  const uint64_t first = REX_LOAD_U32(ctx.r3.u32 + 0);   // lwz r10,0(r3)
  ctx.r3.u64 = second + first;                            // add r3,r11,r10
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 8);  // no stores: the result lives in r3 only
  return true;
}

}  // namespace port_82B1D198
SKATE3_PORT(82B1D198, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
