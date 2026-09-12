// sub_82B1D790  role: return max(u32[+4] - u32[+8], u32[+0]) of an operand block (op 31 of the 0x82FD3600 table)   lifted: skate3_recomp.66.cpp:67091 (19 lines)
// STATUS: verified
// calls/session: boot 76831  play 114948  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D790.md
namespace port_82B1D790 {

// r3 points at the interpreter's operand block; the three words this op reads live at +0, +4
// and +8. Nothing in docs/rw_audio_structs.h names this layout, so the offsets stay plain.

REX_FUNC(Native) {
  const uint32_t block = ctx.r3.u32;
  // All three loads are zero-extended into 64-bit registers and the subtract is a 64-bit
  // subtract, so when minuend < subtrahend the high word becomes 0xFFFFFFFF; the caller's r3 is
  // compared as all 64 bits, so the difference is kept in a uint64_t.
  const uint64_t minuend = REX_LOAD_U32(block + 4);     // lwz r10,4(r3)
  const uint64_t subtrahend = REX_LOAD_U32(block + 8);  // lwz r9,8(r3)
  const uint64_t floor_word = REX_LOAD_U32(block + 0);  // lwz r11,0(r3)
  const uint64_t difference = minuend - subtrahend;     // subf r3,r9,r10
  // cmpw cr6,r3,r11 ; bgelr cr6 -- signed compare of the LOW words only; on the taken branch
  // r3 keeps the full 64-bit subf result.
  if (static_cast<int32_t>(static_cast<uint32_t>(difference)) >=
      static_cast<int32_t>(static_cast<uint32_t>(floor_word))) {
    ctx.r3.u64 = difference;
    return;                                             // bgelr cr6
  }
  ctx.r3.u64 = floor_word;                              // mr r3,r11
}                                                       // blr

// No stores: the result lives in r3 only. The read set is the three-word head of the block.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  spec.read(ctx.r3.u32, 12);
  return true;
}

}  // namespace port_82B1D790
SKATE3_PORT(82B1D790, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
