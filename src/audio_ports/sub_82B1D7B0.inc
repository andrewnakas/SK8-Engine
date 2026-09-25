// sub_82B1D7B0  role: product of two int32 fields, capped at a third   lifted: skate3_recomp.66.cpp:67111 (19 lines)
// STATUS: verified
// calls/session: boot 2527  play 5739  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D7B0.md
namespace port_82B1D7B0 {

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const int32_t third = static_cast<int32_t>(REX_LOAD_U32(object + 8));   // lwz r10,8(r3)
  const int32_t second = static_cast<int32_t>(REX_LOAD_U32(object + 4));  // lwz r9,4(r3)
  const uint64_t cap = REX_LOAD_U32(object + 0);                          // lwz r11,0(r3)
  // mullw r3,r10,r9 -- RexGlue keeps the FULL 64-bit product here, so a product that overflows
  // 32 bits is returned whole on the untaken path. Only its low word is compared.
  const int64_t product = static_cast<int64_t>(third) * static_cast<int64_t>(second);
  ctx.r3.s64 = product;
  // cmpw cr6,r3,r11 ; blelr cr6 -- cap only when the product's low word exceeds it
  if (static_cast<int32_t>(product) > static_cast<int32_t>(cap)) {
    ctx.r3.u64 = cap;  // mr r3,r11 (zero-extended, as loaded)
  }
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 12);  // no stores: the result is r3 alone
  return true;
}

}  // namespace port_82B1D7B0
SKATE3_PORT(82B1D7B0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
