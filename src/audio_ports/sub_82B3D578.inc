// sub_82B3D578  role: store the .rdata zero float into two fields at +16 and +20   lifted: skate3_recomp.68.cpp:9840 (18 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 10 comparable calls in the weaker profile, 11 in the stronger, against 18 lifted lines -- small, but seen ten times. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 10  play 11  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3D578.md
namespace port_82B3D578 {

// lis r11,-32234 ; lfs f0,23056(r11) -- 0x82160000 + 0x5A10. The same cell the queue producer's
// liveness path reads; its value is loaded at run time here rather than assumed to be 0.0f.
constexpr uint32_t kZeroFloatCell = 0x82165A10;

REX_FUNC(Native) {
  ctx.fpscr.disableFlushModeUnconditional();  // emitted before the lfs, kept at the same point
  const uint32_t bits = REX_LOAD_U32(kZeroFloatCell);  // lfs f0,23056(r11)
  // stfs f0,16(r3) then stfs f0,20(r3). The lifted form round-trips the single through a double
  // and back, which is exact for any value a single can hold, so the stored bits are the loaded
  // bits -- except for a signalling NaN, which the round trip would quiet. Reproduce the round
  // trip rather than copying the word, so that case matches too.
  PPCRegister temp{};
  temp.u32 = bits;
  const double widened = double(temp.f32);
  temp.f32 = float(widened);
  REX_STORE_U32(ctx.r3.u32 + 16, temp.u32);
  REX_STORE_U32(ctx.r3.u32 + 20, temp.u32);
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r3.u32 + 16, 8);
  spec.read(kZeroFloatCell, 4);
  return true;
}

}  // namespace port_82B3D578
SKATE3_PORT(82B3D578, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnNone)
