// sub_82B1BF80  role: take-and-clear accessor: return the u32 at [r3+0] and leave zero in its place   lifted: skate3_recomp.66.cpp:63544 (13 lines)
// STATUS: verified
// calls/session: boot 55578  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1BF80.md
namespace port_82B1BF80 {

REX_FUNC(Native) {
  // The load and the store hit the SAME four bytes, so the order is load-then-store and must
  // stay that way: hoisting the store above the load would return zero. `lwz` zero-extends into
  // the full 64-bit r3, which is what the harness compares, hence the u64 assignment from a
  // 32-bit load. Nothing in docs/rw_audio_structs.h identifies this object, so +0 stays plain.
  // Direct sibling of sub_82B1BF68, which does the same to the field at +16.
  const uint32_t object = ctx.r3.u32;      // mr r11,r3
  ctx.r3.u64 = REX_LOAD_U32(object + 0);   // lwz r3,0(r3)
  REX_STORE_U32(object + 0, 0u);           // li r10,0 ; stw r10,0(r11)
}

// One store, four bytes at the entry pointer. Both the address and the length are known from
// entry state with no loads at all, so the window set is exact and unconditional.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  spec.write(object, 4);  // stw r10,0(r11)
  spec.read(object, 4);   // the same four bytes, read before they are cleared
  return true;
}

}  // namespace port_82B1BF80
SKATE3_PORT(82B1BF80, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
