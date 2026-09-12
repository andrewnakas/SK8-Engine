// sub_82B28970  role: shut a player object down -- tear the decoder at +336 down through sub_82B3C930 and null it, retire the record at +56 through sub_82B489D0 when the byte at +373 is set (clearing +24 and +43 first), then free the block at +80 through the owner's allocator vtable slot +12   lifted: skate3_recomp.67.cpp:26470 (84 lines)
// STATUS: gate-1 (two reasons: its own bctrl, the free entry of u32[u32[object+8]+36]'s vtable at 0x82B289E4, and sub_82B3C930, whose four indirect calls release and unregister the voice)
// calls/session: boot 1  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B28970.md
namespace port_82B28970 {

constexpr uint32_t kOwner = 8;      // lwz r3,8(r31) -- the System* in the Player layout
constexpr uint32_t kPending = 24;   // stw r30,24(r31)
constexpr uint32_t kRecord = 56;    // addi r4,r31,56 -- sub_82B489D0's second argument
constexpr uint32_t kRecordLive = 43;  // stb r30,43(r31)
constexpr uint32_t kClientBlock = 80; // lwz r4,80(r31) -- the pointer that gets freed
constexpr uint32_t kDecoder = 336;    // lwz r3,336(r3) / stw r30,336(r31)
constexpr uint32_t kRecordArmed = 373; // lbz r11,373(r31)
constexpr uint32_t kAllocator = 36;    // lwz r3,36(r11) -- the owner's allocator
constexpr uint32_t kFreeSlot = 12;     // lwz r9,12(r10) -- the allocator vtable's free entry

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;

  // lwz r3,336(r3) -- the live decoder, and sub_82B3C930's only argument.
  const uint32_t decoder = REX_LOAD_U32(object + kDecoder);
  ctx.r3.u64 = decoder;  // left in r3 on both paths, as the load does
  // cmplwi cr6,r3,0 ; beq cr6,0x82b289a0
  if (decoder != 0) {
    skate3::audio::GuestCall(ctx, base, sub_82B3C930, decoder);  // bl 0x82b3c930
    REX_STORE_U32(object + kDecoder, 0);                         // stw r30,336(r31)
  }

  // loc_82B289A0
  // lbz r11,373(r31) ; cmplwi ; beq cr6,0x82b289c0
  if (REX_LOAD_U8(object + kRecordArmed) != 0) {
    REX_STORE_U32(object + kPending, 0);                  // stw r30,24(r31)
    const uint32_t record = object + kRecord;             // addi r4,r31,56
    REX_STORE_U8(object + kRecordLive, 0);                // stb r30,43(r31) -- after +24
    const uint32_t owner = REX_LOAD_U32(object + kOwner); // lwz r3,8(r31) -- loaded after both
                                                          // stores, as lifted
    // bl 0x82b489d0 -- retires the record from the owner's lists (it reads +180 and +112 off
    // the owner and the discriminator at record+20).
    skate3::audio::GuestCall(ctx, base, sub_82B489D0, owner, record);
  }

  // loc_82B289C0
  ctx.r4.u64 = REX_LOAD_U32(object + kClientBlock);  // lwz r4,80(r31) -- also the free argument
  const uint32_t block = ctx.r4.u32;
  // cmplwi cr6,r4,0 ; beq cr6,0x82b289e8
  if (block != 0) {
    const uint32_t owner = REX_LOAD_U32(object + kOwner);      // lwz r11,8(r31)
    ctx.r5.s64 = 0;                                            // li r5,0
    const uint32_t allocator = REX_LOAD_U32(owner + kAllocator);  // lwz r3,36(r11)
    ctx.r3.u64 = allocator;
    const uint32_t vtable = REX_LOAD_U32(allocator + 0);       // lwz r10,0(r3)
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kFreeSlot);            // lwz r9,12(r10) ; mtctr r9
    ctx.lr = 0x82B289E8;                                       // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  }
  // The pointer at +80 is NOT nulled after the free. Reproduced, not fixed.
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B28970
SKATE3_PORT(82B28970, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
