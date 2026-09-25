// sub_82B1C8D0  role: any-nonzero reducer: return 1 if any of the `count`(+0) u32 words at +4 is nonzero, else 0   lifted: skate3_recomp.66.cpp:64899 (38 lines)
// STATUS: verified
// calls/session: boot 108418  play 123521  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C8D0.md
namespace port_82B1C8D0 {

// Layout read (nothing in docs/rw_audio_structs.h names this object; the sibling nodes
// sub_82B1CF50 and sub_82B1C878 carry the same "u8 count then an inline word array" shape):
//   +0  u8    count      number of words in the array
//   +4  u32[] entries    scanned in order; the scan stops at the first nonzero word
constexpr uint32_t kCount = 0;
constexpr uint32_t kEntries = 4;

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  // lbz r9,0(r3) -- zero-extended, so the count is 0..255 and the signed compare below can
  // only be false for exactly zero.
  const uint32_t count = REX_LOAD_U8(object + kCount);

  // cmpwi cr6,r9,0 ; ble cr6,loc_82B1C900 -- empty array: no scan, return 0
  if (count != 0) {
    uint32_t entry = object + kEntries;  // addi r10,r3,4 (used only as its low 32 bits)
    int32_t index = 0;                   // li r11,0
    for (;;) {
      // loc_82B1C8E4: lwz r8,0(r10) ; cmpwi cr6,r8,0 ; bne cr6,loc_82B1C908
      if (REX_LOAD_U32(entry) != 0) {
        ctx.r3.s64 = 1;  // loc_82B1C908: li r3,1
        return;          // blr
      }
      // addi r11,r11,1 ; addi r10,r10,4 ; cmpw cr6,r11,r9 ; blt cr6,loc_82B1C8E4
      // The count is NOT reloaded: the original keeps it in r9 across the whole loop.
      index += 1;
      entry += 4;
      if (!(index < static_cast<int32_t>(count))) break;
    }
  }

  // loc_82B1C900 (and the loop's fall-through exit): li r3,0 ; blr
  ctx.r3.s64 = 0;
}

// No stores at all: the function is a read-only predicate and its only result is r3, so the
// write set is empty. Reads are the header byte plus the `count` words at +4, bounded by the
// u8 count (at most 4 + 255 * 4 bytes). The original stops at the first nonzero word; the
// whole array is declared instead of replaying the scan here -- reads are recorded, not
// compared, so a span wider than the call actually touched is harmless.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  const uint32_t count = REX_LOAD_U8(object + kCount);
  spec.read(object, kEntries + 4 * count);
  return true;
}

}  // namespace port_82B1C8D0
SKATE3_PORT(82B1C8D0, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
