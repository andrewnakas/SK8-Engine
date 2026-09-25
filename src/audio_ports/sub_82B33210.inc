// sub_82B33210  role: table entry -- run sub_82B32328 over the object held at +4 of its argument, return the constant 8   lifted: skate3_recomp.67.cpp:51942 (27 lines)
// STATUS: gate-1 (indirect call: sub_82B33F98, reached through sub_82B32328, ends in a bctrl)
// calls/session: boot 587  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B33210.md
namespace port_82B33210 {

REX_FUNC(Native) {
  // lwz r3,4(r3) -- the argument is a small record; the object to act on is its second word.
  const uint32_t object = REX_LOAD_U32(ctx.r3.u32 + 4);
  // bl 0x82b32328
  skate3::audio::GuestCall(ctx, base, sub_82B32328, object);
  // li r3,8 -- a constant, overriding whatever the callee returned.
  ctx.r3.s64 = 8;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B33210
SKATE3_PORT(82B33210, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
