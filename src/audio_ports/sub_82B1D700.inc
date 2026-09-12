// sub_82B1D700  role: sum the N u32 words of an operand block, clamped to the cap word (op 30 of the 0x82FD3600 table)   lifted: skate3_recomp.66.cpp:67007 (83 lines)
// STATUS: verified
// calls/session: boot 560549  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D700.md
namespace port_82B1D700 {

// The operand block r3 points at: u8 count at +0, u32 cap at +4, u32 words[count] at +8.
// Nothing in docs/rw_audio_structs.h names this layout, so the offsets stay plain.

REX_FUNC(Native) {
  const uint32_t block = ctx.r3.u32;              // mr r8,r3
  const uint32_t count = REX_LOAD_U8(block + 0);  // lbz r4,0(r3)
  // Every add below is a 64-bit add of zero-extended u32 loads, as in the lifted body; the
  // caller's r3 is compared as all 64 bits, so the sum is kept in a uint64_t throughout.
  uint64_t total = REX_LOAD_U32(block + 8);       // lwz r3,8(r3)
  // cmpwi cr6,r4,1 ; ble cr6,0x82b1d77c -- zero or one word: the first word alone is the sum
  if (static_cast<int32_t>(count) > 1) {
    uint64_t odd = 0;   // li r7,0   words[1], words[3], ...
    uint64_t even = 0;  // li r6,0   words[2], words[4], ...
    uint32_t next = 1;  // li r5,1   index of the first word the pair loop leaves behind
    // addi r11,r4,-1 ; cmpwi cr6,r11,2 ; blt cr6,0x82b1d75c -- exactly two words: no pair loop
    if (static_cast<int32_t>(count - 1) >= 2) {
      // addi r10,r11,-2 ; rlwinm r10,r10,31,1,31 ; addi r10,r10,1
      const uint32_t pairs = ((count - 3) >> 1) + 1;
      // rlwinm r9,r10,1,0,30 ; addi r5,r9,1
      next = ((pairs << 1) & 0xFFFFFFFEu) + 1;
      uint32_t cursor = block + 8;  // addi r11,r8,8
      // mtctr r10 ; ... ; bdnz 0x82b1d748
      for (uint32_t k = pairs; k != 0; --k) {
        const uint64_t a = REX_LOAD_U32(cursor + 4);  // lwz r9,4(r11)
        cursor += 8;                                  // lwzu r10,8(r11)
        const uint64_t b = REX_LOAD_U32(cursor);
        odd += a;   // add r7,r9,r7
        even += b;  // add r6,r10,r6
      }
    }
    // cmpw cr6,r5,r4 ; bge cr6,0x82b1d774 -- an odd word count leaves one trailing word
    if (static_cast<int32_t>(next) < static_cast<int32_t>(count)) {
      // addi r11,r5,2 ; rlwinm r10,r11,2,0,29 ; lwzx r11,r10,r8 ; add r3,r11,r3
      total += REX_LOAD_U32((((next + 2) << 2) & 0xFFFFFFFCu) + block);
    }
    total += even + odd;  // add r11,r6,r7 ; add r3,r11,r3
  }
  const uint64_t cap = REX_LOAD_U32(block + 4);  // lwz r11,4(r8)
  // cmpw cr6,r3,r11 ; blelr cr6 -- signed 32-bit compare of the low words
  if (static_cast<int32_t>(static_cast<uint32_t>(total)) <=
      static_cast<int32_t>(static_cast<uint32_t>(cap))) {
    ctx.r3.u64 = total;  // r3 keeps the full 64-bit sum
    return;
  }
  ctx.r3.u64 = cap;  // mr r3,r11 ; blr
}

// No stores. The read set is the block header plus the words the original touches:
// words[0] is read even when count is 0, otherwise words[0..count-1].
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t block = ctx.r3.u32;
  const uint32_t count = REX_LOAD_U8(block + 0);
  spec.read(block, 8 + 4 * (count < 1 ? 1u : count));
  return true;
}

}  // namespace port_82B1D700
SKATE3_PORT(82B1D700, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
