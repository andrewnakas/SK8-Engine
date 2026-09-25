// sub_82B47658  role: decode four variable-length fields into a 4-word result   lifted: skate3_recomp.68.cpp:34405 (60 lines)
// STATUS: verified
// calls/session: boot 2508  play 2060  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B47658.md
namespace port_82B47658 {

// The four sub-objects, 16 bytes apart, each {cursor* +0, accumulated +4, remaining +8, flag +12}.
constexpr uint32_t kFieldOffsets[4] = {4, 20, 36, 52};
constexpr uint32_t kSubObjectCursor = 0;

REX_FUNC(Native) {
  const uint32_t out = ctx.r3.u32;  // mr r30,r3
  const uint32_t in = ctx.r4.u32;   // mr r31,r4
  for (int i = 0; i < 4; i++) {
    // addi r3,r31,k ; bl 0x82b47010 -- the callee stays lifted (or its own port) via GuestCall
    skate3::audio::GuestCall(ctx, base, sub_82B47010, in + kFieldOffsets[i]);
    REX_STORE_U32(out + 4 * i, ctx.r3.u32);  // stw r3,4i(r30)
  }
}

// Writes: the 16-byte result; per sub-object, sub_82B47010 writes +8 (remaining) and +12 (flag),
// and through sub_82B46FB0 the cursor's byte pointer (*cursor +0) and the accumulator (+4).
// All four sub-objects may share a cursor; overlapping windows are fine (rewound last-first).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)base;
  const uint32_t out = ctx.r3.u32;
  const uint32_t in = ctx.r4.u32;
  spec.write(out, 16);
  for (uint32_t k : kFieldOffsets) {
    const uint32_t obj = in + k;
    const uint32_t cursor = REX_LOAD_U32(obj + kSubObjectCursor);
    if (cursor == 0) return false;  // the original would store through null; not comparable
    spec.write(obj + 4, 9);
    spec.write(cursor, 4);
    spec.read(obj, 4);
    // The byte stream. Each sub-object can consume two codes of up to five bytes, and all four may
    // share one cursor, so the stream can advance 40 bytes before the last read. Recorded for
    // replay only; 8 bytes left most shared-cursor vectors unreplayable.
    spec.read(REX_LOAD_U32(cursor), 40);
  }
  return true;
}

}  // namespace port_82B47658
SKATE3_PORT(82B47658, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
