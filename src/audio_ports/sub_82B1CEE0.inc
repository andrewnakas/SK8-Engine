// sub_82B1CEE0  role: return the smaller of two int32 fields, by signed compare   lifted: skate3_recomp.66.cpp:65788 (15 lines)
// STATUS: verified
// calls/session: boot 14552  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CEE0.md
namespace port_82B1CEE0 {

REX_FUNC(Native) {
  // Both loads zero-extend into 64-bit registers; the comparison is signed on the low words,
  // and whichever field wins is returned still zero-extended, not sign-extended.
  const uint64_t first = REX_LOAD_U32(ctx.r3.u32 + 0);   // lwz r11,0(r3)
  const uint64_t second = REX_LOAD_U32(ctx.r3.u32 + 4);  // lwz r3,4(r3)
  ctx.r3.u64 = second;                                   // r3 already holds the second field
  // cmpw cr6,r11,r3 ; bgelr cr6 -- return the second unless the first is strictly smaller
  if (static_cast<int32_t>(first) < static_cast<int32_t>(second)) {
    ctx.r3.u64 = first;                                  // mr r3,r11
  }
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 8);  // no stores: the result is r3 alone
  return true;
}

}  // namespace port_82B1CEE0
SKATE3_PORT(82B1CEE0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
