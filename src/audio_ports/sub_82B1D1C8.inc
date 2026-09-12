// sub_82B1D1C8  role: signed quotient of the two words at +0 and +4, 0 on a zero divisor (op 25 of the 0x82FD3600 table)   lifted: skate3_recomp.66.cpp:66230 (30 lines)
// STATUS: verified
// calls/session: boot 118225  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D1C8.md
namespace port_82B1D1C8 {

// Argument: r3 = an 8-byte operand block {+0, +4}. Nothing in docs/rw_audio_structs.h names it,
// so the offsets stay literal. Siblings in the same opcode table: slot 23 sub_82B1D1A8
// (+0 minus +4), slot 24 sub_82B1D1B8 (product), slot 36 sub_82B1D198 (sum).

REX_FUNC(Native) {
  const uint32_t block = ctx.r3.u32;

  // lwz r11,4(r3) -- zero-extended into a 64-bit register
  const uint32_t divisor = REX_LOAD_U32(block + 4);
  // cmpwi cr6,r11,0 ; beq cr6,loc_82B1D1F4 -- signed 32-bit compare of the low word
  if (static_cast<int32_t>(divisor) == 0) {
    ctx.r3.s64 = 0;  // loc_82B1D1F4: li r3,0 ; blr
    return;
  }

  // lwz r9,0(r3)
  const uint32_t dividend = REX_LOAD_U32(block + 0);

  // twllei r11,0 -- divide-by-zero trap. Unreachable: the beq above already took that case.
  // Kept because the original keeps it; ppc_trap(type 0) only warns and returns. The lifted
  // line's second half, `r11.u32 < 0u`, is dead (an unsigned word is never negative) and is
  // dropped rather than written out, which clang would reject as a tautological compare.
  if (static_cast<int32_t>(divisor) == 0) ppc_trap(ctx, base, 0);

  // rotlwi r10,r9,1 -- computed BEFORE the divide, as lifted; feeds the overflow trap below
  const uint64_t rotated = __builtin_rotateleft32(dividend, 1);

  // divw r3,r9,r11 -- the quotient is a u32 zero-extended into r3, so r3's high word is 0.
  // RexGlue defines the overflow case (INT32_MIN / -1) as 0, which is what the harness
  // compares against; real divw leaves 0x80000000 there. Reproduce RexGlue's definition, and
  // keep the overflow guard: the C++ division itself would be undefined behaviour. The lifted
  // guard's other half, `r11.s32 &&`, is already decided by the branch above.
  const int32_t dividend_s = static_cast<int32_t>(dividend);
  const int32_t divisor_s = static_cast<int32_t>(divisor);
  const bool overflows = dividend_s == INT32_MIN && divisor_s == -1;
  ctx.r3.u64 = overflows ? 0u : static_cast<uint32_t>(dividend_s / divisor_s);

  // addi r8,r10,-1 -- 64-bit add on the zero-extended rotate, so dividend == 0 gives -1 here
  const int64_t rotated_minus_one = static_cast<int64_t>(rotated) - 1;
  // andc r7,r11,r8
  const uint64_t overflow_guard =
      static_cast<uint64_t>(divisor) & ~static_cast<uint64_t>(rotated_minus_one);
  // twlgei r7,-1 -- r7 never exceeds 32 bits (r11 is a zero-extended load), so the lifted
  // line's `r7.u32 > UINT32_MAX` half is dead and this fires only when the low word is
  // 0xFFFFFFFF: divisor == -1 and rotl(dividend,1) == 1, i.e. dividend == INT32_MIN. It is
  // the divw overflow trap, checked after the divide exactly as the original does.
  if (static_cast<int32_t>(static_cast<uint32_t>(overflow_guard)) == -1) ppc_trap(ctx, base, 0);

  // blr -- r7/r8/r9/r10/r11 and cr6 are volatile scratch, not compared, not reproduced
}

// No stores: the result lives in r3 only. Reads the divisor word always and the dividend word
// only on the nonzero-divisor path; the whole 8-byte block is recorded so the vector replays
// either way.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 8);
  return true;
}

}  // namespace port_82B1D1C8
SKATE3_PORT(82B1D1C8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
