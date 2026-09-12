// sub_82B4F8A8  role: size of a 28-byte-per-pair record plus an 88-byte header; writes 16 to *r4   lifted: skate3_recomp.68.cpp:54330 (17 lines)
// STATUS: verified
// calls/session: boot 1930  play 2166  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B4F8A8.md
namespace port_82B4F8A8 {

constexpr uint32_t kAlignment = 16;  // li r9,16 -- stored through r4
constexpr uint32_t kHeaderBytes = 88;
constexpr uint32_t kBytesPerPair = 28;

REX_FUNC(Native) {
  // rlwinm r10,r11,31,1,31 is a 32-bit rotate-left-31 masked to bits 1..31, which is exactly
  // an unsigned shift right by one: pairs = (count + 1) / 2, on the low 32 bits only.
  const uint32_t pairs = static_cast<uint32_t>(ctx.r3.u32 + 1) >> 1;  // addi r11,r3,1 ; rlwinm
  REX_STORE_U32(ctx.r4.u32 + 0, kAlignment);                          // stw r9,0(r4)
  // mulli r11,r10,28 ; addi r3,r11,88 -- the multiply is on the zero-extended pair count, so
  // the result cannot wrap into the upper half.
  ctx.r3.s64 = static_cast<int64_t>(static_cast<uint64_t>(pairs) * kBytesPerPair) + kHeaderBytes;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.write(ctx.r4.u32 + 0, 4);
  return true;
}

}  // namespace port_82B4F8A8
SKATE3_PORT(82B4F8A8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
