// sub_82B1D200  role: signed remainder of two int32 fields, 0 when the divisor is 0   lifted: skate3_recomp.66.cpp:66261 (34 lines)
// STATUS: verified
// calls/session: boot 11845  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D200.md
namespace port_82B1D200 {

REX_FUNC(Native) {
  const uint64_t divisor_bits = REX_LOAD_U32(ctx.r3.u32 + 4);  // lwz r11,4(r3)
  const int32_t divisor = static_cast<int32_t>(divisor_bits);
  // cmpwi cr6,r11,0 ; beq -> li r3,0
  if (divisor == 0) {
    ctx.r3.s64 = 0;
    return;
  }
  const uint64_t dividend_bits = REX_LOAD_U32(ctx.r3.u32 + 0);  // lwz r9,0(r3)
  const int32_t dividend = static_cast<int32_t>(dividend_bits);
  // twllei r11,0 -- both halves are dead here: the divisor cannot be 0 past the branch above,
  // and an unsigned value is never below 0. Omitted rather than transcribed, because clang
  // rejects the tautological unsigned compare outside generated/.
  // divw r8,r9,r11 -- RexGlue yields 0 for the INT32_MIN / -1 overflow case rather than the
  // hardware's 0x80000000, and this reproduces RexGlue, which is what the harness compares.
  const int32_t quotient =
      (dividend == INT32_MIN && divisor == -1) ? 0 : static_cast<int32_t>(dividend / divisor);
  const uint64_t quotient_bits = static_cast<uint32_t>(quotient);
  // mullw r6,r8,r11 keeps the full 64-bit product of the sign-extended operands, and
  // subf r3,r6,r9 subtracts it from the ZERO-extended dividend in 64 bits. So a negative
  // remainder leaves bit 32 set in r3 (dividend 0xFFFFFFFA, divisor 3 gives 0x100000000).
  // A 32-bit remainder would diverge on every negative dividend.
  const uint64_t product =
      static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(quotient_bits)) *
                            static_cast<int64_t>(divisor));
  ctx.r3.u64 = dividend_bits - product;
  // twlgei r5,-1 -- the divw overflow trap, reachable only for INT32_MIN / -1. andc of the
  // divisor with (rotl32(dividend,1) - 1) leaves -1 exactly in that case.
  const uint64_t rotated = static_cast<uint64_t>(__builtin_rotateleft32(
      static_cast<uint32_t>(dividend_bits), 1));
  const uint64_t mask = static_cast<uint64_t>(static_cast<int64_t>(rotated) - 1);
  if (static_cast<int32_t>(divisor_bits & ~mask) == -1) {
    ppc_trap(ctx, base, 0);
  }
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 8);  // no stores: the result is r3 alone
  return true;
}

}  // namespace port_82B1D200
SKATE3_PORT(82B1D200, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
