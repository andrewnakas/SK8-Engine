// sub_82B1F440  role: deactivate an audio object -- release the instance it owns at +8 through that instance's vtable slot 0, null the pointer, then zero the two words of the stride-12 slot the byte index at +14 selects; returns 0   lifted: skate3_recomp.67.cpp:3513 (75 lines)
// STATUS: partial -- compared 367, skipped 544, zero divergence (session g1_splits, 150 s scripted); substantive -- the whole tail compares. NEVER promoted: the skipped path has a native body but no comparison. (comparable only when u32[object+8] == 0 -- the release `bctrl` at 0x82B1F474 is the only unreplayable call and it sits on the other side of that test, so the whole tail still compares)
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

// Split by path. The only thing here the harness cannot replay is the release entry called
// through u32[u32[object+8]+0], and it is reached only when that pointer is non-null -- which is
// readable before the call, because nothing stores anything before the test. So: false when the
// instance is still live (the macro runs the original and counts a PortSkip), true when it is
// already null, where the comparable path is the whole tail -- the armed test at +15, the byte
// index at +14, the stride-12 arithmetic and both stores.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  spec.read(object + kOwned, 4);
  // lwz r3,8(r3) ; cmplwi cr6,r3,0 ; beq -- non-zero reaches the `bctrl`. Replaying that would
  // run an unknown release entry a second time; no window rewinds a release.
  if (REX_LOAD_U32(object + kOwned) != 0) return false;

  // On this path the first store in the function is the one below, so the flag and the index read
  // here are exactly the bytes the body reads.
  spec.read(object + kSlotIndex, 2);  // +14 the index, +15 the armed flag
  // lbz r11,15(r31) ; cmplwi ; beq -- unarmed writes nothing at all and returns the same 0.
  if (REX_LOAD_U8(object + kSlotArmed) != 0) {
    const uint64_t index = REX_LOAD_U8(object + kSlotIndex);
    const uint32_t slot =
        static_cast<uint32_t>(index * kSlotStride + static_cast<uint64_t>(object));
    // stw +32 then stw +28: adjacent words, one span from the lower of the two.
    spec.write(slot + kSlotWordB, 8);
  }
  return true;
}

}  // namespace port_82B1F440
SKATE3_PORT(82B1F440, skate3::audio::kPortPartial, skate3::audio::kReturnR3)
