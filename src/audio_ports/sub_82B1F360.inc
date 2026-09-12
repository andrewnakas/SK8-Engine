// sub_82B1F360  role: advance a six-word cascading counter at a fixed global   lifted: skate3_recomp.67.cpp:3375 (137 lines)
// STATUS: verified
// calls/session: boot 2841  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1F360.md
namespace port_82B1F360 {

// lis r11,-31993 ; addi r11,r11,30192 -- 0x83070000 + 0x75F0. Six u32 words, +0 through +20.
constexpr uint32_t kCounter = 0x830775F0;

REX_FUNC(Native) {
  (void)base;
  const uint32_t w20 = REX_LOAD_U32(kCounter + 20);
  const uint32_t w16 = REX_LOAD_U32(kCounter + 16);
  const uint32_t w12 = REX_LOAD_U32(kCounter + 12);
  const uint32_t w8 = REX_LOAD_U32(kCounter + 8);
  const uint32_t w4 = REX_LOAD_U32(kCounter + 4);
  const uint32_t w0 = REX_LOAD_U32(kCounter + 0);

  // Each word accumulates the one below it, carrying upward. Every `add` here is 64-bit on
  // zero-extended words, so a sum can carry into bit 32; the stores keep only the low word, but
  // r3 keeps the full 64-bit value. Truncating the chain to 32 bits made the STORES match and
  // r3 diverge on the 120th call -- registers 94BF290A01000000 against 00000000.
  const uint64_t s16 = uint64_t(w16) + w20;
  const uint32_t carry16 = (uint32_t(s16) < w20 || uint32_t(s16) < w16) ? 1u : 0u;
  const uint64_t s12 = uint64_t(w12) + s16 + carry16;
  const uint32_t carry12 = (uint32_t(s12) < w12) ? 1u : 0u;  // subfc/subfe on the low words
  const uint64_t s8 = uint64_t(w8) + s12 + carry12;
  const uint32_t carry8 = (uint32_t(s8) < w8) ? 1u : 0u;
  const uint64_t s4 = uint64_t(w4) + s8 + carry8;
  const uint32_t carry4 = (uint32_t(s4) < w4) ? 1u : 0u;
  const uint64_t s0 = uint64_t(w0) + s4 + carry4;
  const uint32_t next20 = w20 + 1;

  // Store order as lifted: 16, 12, 8, 4, 20, then 0. Each store keeps the low word only.
  REX_STORE_U32(kCounter + 16, uint32_t(s16));
  REX_STORE_U32(kCounter + 12, uint32_t(s12));
  REX_STORE_U32(kCounter + 8, uint32_t(s8));
  REX_STORE_U32(kCounter + 4, uint32_t(s4));
  REX_STORE_U32(kCounter + 20, next20);
  REX_STORE_U32(kCounter + 0, uint32_t(s0));
  ctx.r3.u64 = s0;  // add r3,r5,r4 -- the FULL 64-bit sum, before the first bnelr

  // The low word wrapped: ripple a single increment upward, stopping at the first word that
  // does not wrap. Each step re-stores the word it just incremented.
  // Each addic. tests only the LOW word of the incremented 64-bit value.
  if (next20 != 0) return;
  const uint64_t r16 = s16 + 1;
  REX_STORE_U32(kCounter + 16, uint32_t(r16));
  if (uint32_t(r16) != 0) return;
  const uint64_t r12 = s12 + 1;
  REX_STORE_U32(kCounter + 12, uint32_t(r12));
  if (uint32_t(r12) != 0) return;
  const uint64_t r8 = s8 + 1;
  REX_STORE_U32(kCounter + 8, uint32_t(r8));
  if (uint32_t(r8) != 0) return;
  const uint64_t r4 = s4 + 1;
  REX_STORE_U32(kCounter + 4, uint32_t(r4));
  if (uint32_t(r4) != 0) return;
  ctx.r3.u64 = s0 + 1;  // addi r3,r3,1 -- 64-bit, like the sum it increments
  REX_STORE_U32(kCounter + 0, uint32_t(s0 + 1));
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  spec.write(kCounter, 24);  // a fixed global: the write set does not depend on any argument
  spec.read(kCounter, 24);
  return true;
}

}  // namespace port_82B1F360
SKATE3_PORT(82B1F360, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
