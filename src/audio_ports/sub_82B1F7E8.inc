// sub_82B1F7E8  role: read the guest timebase into r3   lifted: skate3_recomp.67.cpp:4044 (7 lines)
// STATUS: gate-3 (mftb: two calls return two values, so a comparison can never pass)
// calls/session: boot 11147383  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1F7E8.md
namespace port_82B1F7E8 {

REX_FUNC(Native) {
  (void)base;
  ctx.r3.u64 = REX_QUERY_TIMEBASE();  // mftb r3
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port and the generator needs no special case.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1F7E8
SKATE3_PORT(82B1F7E8, skate3::audio::kPortGate3, skate3::audio::kReturnR3)
