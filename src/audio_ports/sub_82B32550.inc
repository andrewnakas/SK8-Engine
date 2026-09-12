// sub_82B32550  role: claim the current slot of a 20-entry ring if free, and advance the index   lifted: skate3_recomp.67.cpp:50075 (50 lines)
// STATUS: verified
// calls/session: boot 2935  play 5735  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B32550.md
namespace port_82B32550 {

constexpr uint32_t kIndexByte = 473;    // u8 cursor into the ring
constexpr uint32_t kSlotStride = 16;    // rotlwi r8,r11,4 -- 16 bytes per slot
constexpr uint32_t kSlotBusy = 113;     // the busy byte inside a slot
constexpr uint32_t kRingEntries = 20;   // the index wraps here

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t index = REX_LOAD_U8(object + kIndexByte);   // lbz r11,473(r3)
  // rotlwi r8,r11,4 ; add r8,r8,r3 -- a byte rotated left 4 is index*16 for any index < 2^28.
  const uint32_t slot = object + __builtin_rotateleft32(index, 4);
  // lbz r7,113(r8) ; bne -> li r3,0: an occupied slot is a refusal, and writes nothing.
  if (REX_LOAD_U8(slot + kSlotBusy) != 0) {
    ctx.r3.s64 = 0;
    return;
  }
  REX_STORE_U32(ctx.r4.u32 + 0, index);  // stw r9,0(r4) -- the claimed index, before advancing
  ctx.r3.s64 = 1;                        // li r3,1
  // addi/clrlwi then a subfic/subfe pair whose only effect is to select 0 when the incremented
  // index equals 20, and the index otherwise. Written as the wrap it implements.
  const uint32_t next = (index + 1) & 0xFF;
  REX_STORE_U8(object + kIndexByte, static_cast<uint8_t>(next == kRingEntries ? 0u : next));
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  spec.write(ctx.r4.u32 + 0, 4);
  spec.write(ctx.r3.u32 + kIndexByte, 1);
  const uint32_t index = REX_LOAD_U8(ctx.r3.u32 + kIndexByte);
  spec.read(ctx.r3.u32 + kIndexByte, 1);
  spec.read(ctx.r3.u32 + __builtin_rotateleft32(index, 4) + kSlotBusy, 1);
  return true;
}

}  // namespace port_82B32550
SKATE3_PORT(82B32550, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
