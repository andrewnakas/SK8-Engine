// sub_82B1CF38  role: signed max of the two s32 words at +0 and +4, returned zero-extended in r3 (ties return +4)   lifted: skate3_recomp.66.cpp:65841 (15 lines)
// STATUS: verified
// calls/session: boot 33196  play 62633  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1CF38.md
namespace port_82B1CF38 {

// Nothing in docs/rw_audio_structs.h names this object; plain offsets are honest.
constexpr uint32_t kFirst = 0;   // s32
constexpr uint32_t kSecond = 4;  // s32

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  // Both loads are `lwz`, so each value is zero-extended into 64 bits; the compare is signed on
  // the low words (cmpw), and whichever word wins is returned in its zero-extended form.
  const uint64_t first = REX_LOAD_U32(object + kFirst);    // lwz r11,0(r3)
  const uint64_t second = REX_LOAD_U32(object + kSecond);  // lwz r3,4(r3)
  // cmpw cr6,r11,r3 ; blelr cr6 -- first <= second (signed): return second as loaded
  if (static_cast<int32_t>(first) <= static_cast<int32_t>(second)) {
    ctx.r3.u64 = second;
    return;  // blelr cr6
  }
  ctx.r3.u64 = first;  // mr r3,r11
}                      // blr

// No stores on any path: leaf, result in r3 only. The read set is the two words.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32 + kFirst, 8);  // +0 and +4
  return true;
}

}  // namespace port_82B1CF38
SKATE3_PORT(82B1CF38, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
