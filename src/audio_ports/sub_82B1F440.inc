// sub_82B1F440  role: deactivate an audio object -- release the instance it owns at +8 through that instance's vtable slot 0, null the pointer, then zero the two words of the stride-12 slot the byte index at +14 selects; returns 0   lifted: skate3_recomp.67.cpp:3513 (75 lines)
// STATUS: gate-1 (its own bctrl: the release entry of whatever vtable sits at u32[object+8], `bctrl` at 0x82B1F474)
// calls/session: boot 451  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1F440.md
namespace port_82B1F440 {

constexpr uint32_t kOwned = 8;         // lwz r3,8(r3) / stw r30,8(r31) -- the owned instance
constexpr uint32_t kReleaseSlot = 0;   // lwz r10,0(r11): slot 0 of that instance's vtable
constexpr uint32_t kSlotArmed = 15;    // lbz r11,15(r31) -- gate on the tail
constexpr uint32_t kSlotIndex = 14;    // lbz r11,14(r31) -- which slot to clear
constexpr uint32_t kSlotStride = 12;   // rotlwi r10,r11,1 ; add ; rlwinm r11,r11,2,0,29
constexpr uint32_t kSlotWordA = 32;    // stw r30,32(r11) -- stored FIRST
constexpr uint32_t kSlotWordB = 28;    // stw r30,28(r11) -- stored second

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;

  // lwz r3,8(r3) -- also the argument of the release call below; zero means already released.
  const uint32_t owned = REX_LOAD_U32(object + kOwned);
  ctx.r3.u64 = owned;  // left in r3 on both paths, as the load does
  // cmplwi cr6,r3,0 ; beq cr6,0x82b1f47c
  if (owned != 0) {
    const uint32_t vtable = REX_LOAD_U32(owned + 0);    // lwz r11,0(r3)
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kReleaseSlot);  // lwz r10,0(r11) ; mtctr r10
    ctx.lr = 0x82B1F478;                                // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    REX_STORE_U32(object + kOwned, 0);                  // stw r30,8(r31)
  }

  // loc_82B1F47C
  const uint32_t armed = REX_LOAD_U8(object + kSlotArmed);  // lbz r11,15(r31)
  ctx.r3.s64 = 0;                                           // li r3,0 -- the only result
  // cmplwi cr6,r11,0 ; beq cr6,0x82b1f4ac
  if (armed != 0) {
    // lbz r11,14(r31) ; rotlwi r10,r11,1 ; add r11,r11,r10 ; rlwinm r11,r11,2,0,29 is
    // index*2 + index = index*3, then <<2: a 12-byte stride. The rlwinm mask 0xFFFFFFFC and
    // the 64-bit add cannot bite -- a byte index caps the product at 12*255 = 3060.
    const uint64_t index = REX_LOAD_U8(object + kSlotIndex);
    // add r11,r11,r31 -- both operands zero-extended, so the sum is 64-bit; the stores truncate.
    const uint32_t slot =
        static_cast<uint32_t>(index * kSlotStride + static_cast<uint64_t>(object));
    // addi r10,r11,28 computes the slot base into r10 and nothing reads it: dead, not reproduced.
    REX_STORE_U32(slot + kSlotWordA, 0);  // stw r30,32(r11) -- +32 before +28, as lifted
    REX_STORE_U32(slot + kSlotWordB, 0);  // stw r30,28(r11)
  }
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1F440
SKATE3_PORT(82B1F440, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
