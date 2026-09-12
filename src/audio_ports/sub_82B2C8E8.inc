// sub_82B2C8E8  role: return the fourth argument unchanged -- r3 = r6, 64-bit (register-only leaf)   lifted: skate3_recomp.67.cpp:36218 (7 lines)
// STATUS: verified
// calls/session: boot 131457  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2C8E8.md
namespace port_82B2C8E8 {

REX_FUNC(Native) {
  (void)base;  // no memory access at all
  // A 64-bit register move, not a 32-bit one: whatever the caller left in r6's upper half is
  // carried into r3, and r3 is compared as all 64 bits. No mask, no zero-extension.
  ctx.r3.u64 = ctx.r6.u64;  // mr r3,r6
  // blr
}

// No stores: nothing to window. No loads either, so there is no read set to record; the whole
// input is r3..r10 in the entry registers the vector already carries, and the whole result is r3.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return true;
}

}  // namespace port_82B2C8E8
SKATE3_PORT(82B2C8E8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
