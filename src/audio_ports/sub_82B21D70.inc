// sub_82B21D70  role: one-shot wrapper -- forward the object to sub_82B21AC0's locked pass, then report success   lifted: skate3_recomp.67.cpp:9744 (25 lines)
// STATUS: gate-1 (RtlEnterCriticalSection plus two indirect calls: sub_82B21AC0)
// calls/session: boot 1  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B21D70.md
namespace port_82B21D70 {

REX_FUNC(Native) {
  // bl 0x82b21ac0 -- r3 is passed through exactly as it arrived; nothing else is set up.
  skate3::audio::GuestCall(ctx, base, sub_82B21AC0, ctx.r3.u64);
  // li r3,0 -- the callee's result is discarded and replaced by a constant zero.
  ctx.r3.s64 = 0;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B21D70
SKATE3_PORT(82B21D70, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
