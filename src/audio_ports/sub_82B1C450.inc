// sub_82B1C450  role: apply a pending setpoint -- when the applied value at +16 differs from the requested one at +20, latch +20 into +16 and notify sub_828E2F38 with the requested value clamped into [+8, +12] in a stack slot; always returns 0   lifted: skate3_recomp.66.cpp:64248 (60 lines)
// STATUS: gate-1 (the notify callee broadcasts to a linked list of handlers through ctr: sub_828E2F38 indirect call, `bctrl` at 0x828E2FCC)
// calls/session: boot 316972  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C450.md
namespace port_82B1C450 {

// Nothing in docs/rw_audio_structs.h names this object; plain offsets are honest. The bounds are
// the same {low, high} pair shape sub_82B1C4B8 and sub_82B1C778 carry, at different offsets.
constexpr uint32_t kLow = 8;         // s32  lower clamp
constexpr uint32_t kHigh = 12;       // s32  upper clamp
constexpr uint32_t kApplied = 16;    // s32  the applied value -- the only object word written
constexpr uint32_t kRequested = 20;  // s32  the pending value

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)
constexpr uint32_t kLinkSlot = 8;     // stw r12,-8(r1), i.e. frame + 88
constexpr uint32_t kArgSlot = 80;     // stw ...,80(r1) ; addi r4,r1,80 -- outgoing by-ref arg

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;

  // The frame is reproduced because it carries an outgoing argument by address: sub_828E2F38
  // reads the clamped value through r4 = r1+80. The slot and the link slot are both inside the
  // 96 bytes this call allocates, so neither is a window.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  // stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const int32_t requested = static_cast<int32_t>(REX_LOAD_U32(object + kRequested));  // lwz r11,20(r3)
  const int32_t applied = static_cast<int32_t>(REX_LOAD_U32(object + kApplied));      // lwz r10,16(r3)

  // cmpw cr6,r10,r11 ; beq cr6,0x82b1c4a0 -- already applied: nothing is written and nobody is
  // notified. Signed compare, as lifted.
  if (applied != requested) {
    const int32_t low = static_cast<int32_t>(REX_LOAD_U32(object + kLow));    // lwz r10,8(r3)
    const int32_t high = static_cast<int32_t>(REX_LOAD_U32(object + kHigh));  // lwz r9,12(r3)
    // stw r11,16(r3) -- the RAW requested value is latched, not the clamped one. Deliberate in
    // the original and reproduced: only the notification sees the clamp.
    REX_STORE_U32(object + kApplied, static_cast<uint32_t>(requested));
    // cmpw cr6,r11,r10 ; bge cr6,0x82b1c488
    if (requested < low) {
      REX_STORE_U32(frame + kArgSlot, static_cast<uint32_t>(low));  // stw r10,80(r1)
    } else {
      // loc_82B1C488: the high bound is stored FIRST and then overwritten when the value is in
      // range. Two stores to the same slot, in this order, as lifted.
      REX_STORE_U32(frame + kArgSlot, static_cast<uint32_t>(high));  // stw r9,80(r1)
      // cmpw cr6,r11,r9 ; bgt cr6,0x82b1c498
      if (!(requested > high)) {
        REX_STORE_U32(frame + kArgSlot, static_cast<uint32_t>(requested));  // stw r11,80(r1)
      }
    }
    // loc_82B1C498: addi r4,r1,80 ; bl 0x828e2f38 -- r3 still holds the object from entry.
    // Through GuestCall on the live context so the callee's own hook decides how it runs.
    skate3::audio::GuestCall(ctx, base, sub_828E2F38, object, frame + kArgSlot);
  }

  // loc_82B1C4A0
  ctx.r3.s64 = 0;                                      // li r3,0
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;               // addi r1,r1,96
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1C450
SKATE3_PORT(82B1C450, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
