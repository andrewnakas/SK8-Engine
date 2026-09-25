// sub_82B49238  role: table entry -- run sub_82B48F28 (unlink and release the object's children) over the object at +4 with the flag clear, return the constant 8   lifted: skate3_recomp.68.cpp:38508 (29 lines)
// STATUS: gate-1 (indirect call: sub_82B48F28 has three bctrl calls through each child's vtable)
// calls/session: boot 2152  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B49238.md
namespace port_82B49238 {

REX_FUNC(Native) {
  // lwz r3,4(r3) -- the argument is a small record; the object to act on is its second word.
  // Read before the argument registers are set up, since r3 is both the record and the first
  // argument. (`li r4,0` precedes the load in the lifted order; neither touches memory.)
  const uint32_t object = REX_LOAD_U32(ctx.r3.u32 + 4);
  // li r4,0 ; bl 0x82b48f28 -- the second argument is the literal zero.
  skate3::audio::GuestCall(ctx, base, sub_82B48F28, object, uint64_t(0));
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

}  // namespace port_82B49238
SKATE3_PORT(82B49238, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
