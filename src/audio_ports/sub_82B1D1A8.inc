// sub_82B1D1A8  role: return the difference of two u32 fields, [+0] - [+4] (register-only)   lifted: skate3_recomp.66.cpp:66206 (11 lines)
// STATUS: verified
// calls/session: boot 68584  play 110741  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D1A8.md
namespace port_82B1D1A8 {

REX_FUNC(Native) {
  // Both loads are zero-extended into 64-bit registers and `subf` is a 64-bit subtract, so when
  // the second field exceeds the first the borrow runs into the upper 32 bits (0xFFFFFFFF_xxxxxxxx)
  // instead of wrapping at bit 32; the caller's r3 is compared as all 64 bits. Sibling of the
  // verified sub_82B1D198 (sum of the same two fields) and sub_82B1D1B8 (their product); nothing
  // in docs/rw_audio_structs.h names this 8-byte object, so the offsets stay plain.
  const uint64_t first = REX_LOAD_U32(ctx.r3.u32 + 0);   // lwz r11,0(r3)
  const uint64_t second = REX_LOAD_U32(ctx.r3.u32 + 4);  // lwz r10,4(r3)
  ctx.r3.u64 = first - second;                           // subf r3,r10,r11
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 8);  // no stores: the result lives in r3 only
  return true;
}

}  // namespace port_82B1D1A8
SKATE3_PORT(82B1D1A8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
