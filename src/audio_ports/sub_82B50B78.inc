// sub_82B50B78  role: alias entry point for the XMA input-buffer feeder -- a bare tail branch to sub_82B4FE40   lifted: skate3_recomp.68.cpp:57213 (6 lines)
// STATUS: gate-1 (XMA import: sub_82B4FE40 calls XMASetInputBuffer0Valid/XMASetInputBuffer1Valid, XMAEnableContext/XMADisableContext, XMASetInputBufferReadOffset)
// calls/session: boot 2993  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B50B78.md
namespace port_82B50B78 {

REX_FUNC(Native) {
  // b 0x82b4fe40 -- a tail BRANCH, not a call: every argument register passes through
  // untouched, lr is left as the caller set it, and whatever sub_82B4FE40 leaves in r3 is
  // this function's result. GuestCall with no arguments is the faithful form: it writes no
  // argument register, so r3..r10 arrive exactly as they did here.
  skate3::audio::GuestCall(ctx, base, sub_82B4FE40);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file
// has the same shape as every other port.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B50B78
SKATE3_PORT(82B50B78, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
