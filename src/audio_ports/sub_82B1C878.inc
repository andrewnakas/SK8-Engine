// sub_82B1C878  role: scan a u32 flag table for the first non-zero entry i and copy value[i] (u32 at +8+4i) into +4; return +4   lifted: skate3_recomp.66.cpp:64848 (50 lines)
// STATUS: verified
// calls/session: boot 195407  play 232114  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C878.md
namespace port_82B1C878 {

// Nothing in docs/rw_audio_structs.h names this object; plain offsets are honest.
constexpr uint32_t kFlagsOffset = 0;  // u16  byte offset, from the object, of the flag table (u32 per entry)
constexpr uint32_t kCount = 2;        // u8   entries in the flag table (also bounds the value table)
constexpr uint32_t kCurrent = 4;      // u32  the selected value; reloaded as the return value
constexpr uint32_t kValues = 8;       // u32[] value table, indexed by the flag entry that was found

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  // lhz r10,0(r3) ; add r10,r10,r3 -- the table address is only ever used as its low 32 bits
  const uint32_t flags = REX_LOAD_U16(object + kFlagsOffset) + object;

  // lbz r9,2(r3) ; cmplwi cr6,r9,0 ; beq cr6,loc_82B1C8C8 -- empty table: no scan, no store
  if (REX_LOAD_U8(object + kCount) != 0) {
    uint32_t index = 0;  // li r11,0
    for (;;) {
      // loc_82B1C890: lwz r9,0(r10) ; cmpwi cr6,r9,0 ; bne cr6,loc_82B1C8B8
      if (REX_LOAD_U32(flags + 4 * index) != 0) {
        // loc_82B1C8B8: addi r11,r11,2 ; rlwinm r10,r11,2,0,29 ; lwzx r9,r10,r3
        // (index + 2) * 4 fits in 32 bits (index <= 254), so the rotate is a plain shift; the
        // lwzx sums the two registers as u32, so the address wraps exactly as `object + ...` does.
        const uint32_t value = REX_LOAD_U32(object + 4 * (index + 2));
        REX_STORE_U32(object + kCurrent, value);  // stw r9,4(r3)
        break;
      }
      // lbz r9,2(r3) ; addi r11,r11,1 ; addi r10,r10,4 ; cmpw cr6,r11,r9 ; blt cr6,loc_82B1C890
      // The count is reloaded every iteration, as the original does (signed 32-bit compare;
      // both sides are small non-negative so the signedness cannot matter).
      const int32_t count = static_cast<int32_t>(REX_LOAD_U8(object + kCount));
      index += 1;
      if (!(static_cast<int32_t>(index) < count)) break;
    }
  }

  // loc_82B1C8C8 (and the loop's fall-through exit): lwz r3,4(r3) ; blr -- zero-extended into
  // all 64 bits of r3, which is what the harness compares.
  ctx.r3.u64 = REX_LOAD_U32(object + kCurrent);
}

// Writes: exactly one, u32 at object+4, on the found path only. Declare it unconditionally: a
// window over a byte the call does not touch is harmless, and it saves predicting the scan.
// Reads: the 8-byte header plus the value table (+8, count entries), and the flag table at
// object + u16[object+0] (count entries). Both spans are bounded by the u8 count (<= 255 * 4).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t object = ctx.r3.u32;
  const uint32_t count = REX_LOAD_U8(object + kCount);
  const uint32_t flags = REX_LOAD_U16(object + kFlagsOffset) + object;
  spec.write(object + kCurrent, 4);
  spec.read(object, kValues + 4 * count);
  spec.read(flags, 4 * count);
  return true;
}

}  // namespace port_82B1C878
SKATE3_PORT(82B1C878, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
