// sub_82B463A8  role: command handler: stamp {0x7FF7FFF1, float} into slot[index] of object+0x10, return 16   lifted: skate3_recomp.68.cpp:31590 (31 lines)
// STATUS: verified
// calls/session: boot 740566  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B463A8.md
namespace port_82B463A8 {

// The record is 16 bytes: {handler +0, object +4, index +8, value(float) +12}. Built by the
// producer at skate3_recomp.11.cpp:59366 (stores this function's address at +0 and advances
// the ring by 16); the drain advances by the value this handler returns in r3.
constexpr uint32_t kRecordObject = 4;
constexpr uint32_t kRecordIndex = 8;
constexpr uint32_t kRecordValue = 12;
constexpr uint32_t kRecordSize = 16;

// object+0x10 holds the base of an array of 8-byte slots {marker u32 +0, value f32 +4}.
// Nothing in docs/rw_audio_structs.h names +0x10 on this object.
constexpr uint32_t kObjectSlots = 16;
constexpr uint32_t kSlotStride = 8;

// lis r10,32759 ; ori r8,r10,65521  ->  0x7FF7FFF1, a NaN payload (docs/command-queue.md).
constexpr uint32_t kSlotMarker = (uint32_t(32759) << 16) | 65521u;
static_assert(kSlotMarker == 0x7FF7FFF1u);

REX_FUNC(Native) {
  const uint32_t record = ctx.r3.u32;
  const uint32_t object = REX_LOAD_U32(record + kRecordObject);  // lwz r11,4(r3)
  const uint32_t index = REX_LOAD_U32(record + kRecordIndex);    // lwz r9,8(r3)
  // lfs f0,12(r3) -- single-precision load widened to double, as the lifted form does
  ctx.fpscr.disableFlushModeUnconditional();
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(record + kRecordValue);
  ctx.f0.f64 = double(temp.f32);
  // rlwinm r10,r9,3,0,28 -- index * 8 in 32 bits (top three bits of index fall off)
  const uint32_t slot_offset = index << 3;
  ctx.r3.s64 = kRecordSize;  // li r3,16
  const uint32_t slots = REX_LOAD_U32(object + kObjectSlots);  // lwz r11,16(r11)
  // add r11,r11,r10 -- 64-bit in the lifted form, but the stores use the low 32 bits
  const uint32_t slot = slots + slot_offset;
  // stfs f0,4(r11) -- narrowed back to single; a signalling NaN comes out quiet, as the original
  temp.f32 = float(ctx.f0.f64);
  REX_STORE_U32(slot + 4, temp.u32);
  REX_STORE_U32(slot + 0, kSlotMarker);  // stw r8,0(r11)
  // blr
}

// The one write is the 8-byte slot at *(object+0x10) + (index << 3), both addends readable
// before the call. Nothing is stored before either load, so no aliasing can move them.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t record = ctx.r3.u32;
  spec.read(record, kRecordSize);
  const uint32_t object = REX_LOAD_U32(record + kRecordObject);
  if (object == 0) return false;  // the original would store through a null-based array; not comparable
  const uint32_t index = REX_LOAD_U32(record + kRecordIndex);
  const uint32_t slots = REX_LOAD_U32(object + kObjectSlots);
  spec.read(object + kObjectSlots, 4);
  if (slots == 0) return false;  // same: a store through null
  spec.write(slots + (index << 3), kSlotStride);
  return true;
}

}  // namespace port_82B463A8
SKATE3_PORT(82B463A8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
