// sub_82B3CD38  role: construct one request-ring object -- ask the descriptor's sizing entry (+0) how big the derived part is, add the 480-byte 20-slot ring (8-aligned) and, when the descriptor's u16 at +24 is non-zero, a 16-aligned 20-byte trailer, allocate that through the System allocator, fill the fixed fields, run the descriptor's init entry (+4), allocate u16[desc+24]*channels floats out of the OWNER's allocator into +16, fill the trailer, and zero every slot's +0 and +12 -- returns the object, or 0 after handing a half-built one to sub_82B3C930   lifted: skate3_recomp.68.cpp:8582 (298 lines)
// STATUS: gate-1 (four of its own bctrl: the descriptor's sizing entry at 0x82B3CD60 and init entry at 0x82B3CE28, both function pointers held as data, and two allocations, at 0x82B3CDEC through the System allocator's vtable slot +4 and at 0x82B3CED8 through the owner's -- sub_82B3CD38 indirect call)
// calls/session: boot 1933  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B3CD38.md
namespace port_82B3CD38 {

// The object this builds is port_82B3C870's: the names below that it already fixed are kept
// verbatim, and this constructor is what confirms them -- the ring is 20 slots of 24 bytes
// (the 480 in the size), +36 is the slot array's offset RELATIVE to the object, and the two words
// zeroed per slot are kSlotWord0 and kSlotBusy.
constexpr uint32_t kVptr = 0;           // object +0 -> its vtable, written with a constant
constexpr uint32_t kObjOwner = 4;       // r5 on entry
constexpr uint32_t kObjSelf = 8;        // the object's own address, stored into itself
constexpr uint32_t kObjWord12 = 12;     // from u32[desc+8]
constexpr uint32_t kObjFloats = 16;     // the second allocation; 0 until it succeeds
constexpr uint32_t kObjWord20 = 20;     // from u32[desc+12]
constexpr uint32_t kObjWord24 = 24;     // from u32[desc+20]
constexpr uint32_t kLatch = 28;         // port_82B3C870's kLatch
constexpr uint32_t kObjSize = 32;       // the byte size handed to the allocator
constexpr uint32_t kSlotArrayOff = 36;  // port_82B3C870's kSlotArrayOff
constexpr uint32_t kObjTrailerOff = 40; // u32, the trailer's offset from the object, 16 bits wide
constexpr uint32_t kObjHalf44 = 44;     // u16, zeroed
constexpr uint32_t kObjChannels = 46;   // u8, the low byte of r4 on entry
constexpr uint32_t kWriteIndex = 47;    // port_82B3C870's kWriteIndex
constexpr uint32_t kObjByte48 = 48;     // u8, zeroed; port_82B3C870 does not read it
constexpr uint32_t kLatchIndex = 49;    // port_82B3C870's kLatchIndex
constexpr uint32_t kSlotCount = 50;     // port_82B3C870's kSlotCount -- set to 20 here
constexpr uint32_t kObjHasTrailer = 51; // u8, (u16[desc+24] != 0)

constexpr uint32_t kSlotWord0 = 0;    // port_82B3C870's kSlotWord0
constexpr uint32_t kSlotBusy = 12;    // port_82B3C870's kSlotBusy
constexpr uint32_t kSlotStride = 24;  // port_82B3C870's kSlotStride
constexpr uint32_t kSlots = 20;       // li r9,20 -- 20 * 24 == the 480 below
constexpr uint32_t kRingBytes = 480;  // addi r29,r11,480
static_assert(kSlots * kSlotStride == kRingBytes, "20 slots of 24 bytes");

// The descriptor (r3 on entry). Two of its words are called, not read as data.
constexpr uint32_t kDescSizeFn = 0;   // (*)(channels, uint32* align_out) -> derived size in bytes
constexpr uint32_t kDescInitFn = 4;   // (*)(object) -> low byte zero means failure
constexpr uint32_t kDescWord8 = 8;    // -> object +12
constexpr uint32_t kDescWord12 = 12;  // -> object +20
constexpr uint32_t kDescWord20 = 20;  // -> object +24
constexpr uint32_t kDescCount = 24;   // u16: per-channel float count, and whether a trailer exists

// The 20-byte trailer, laid down inside the same block after the ring.
constexpr uint32_t kTrailerOwner = 0;     // r5 on entry, again
constexpr uint32_t kTrailerFloats = 4;    // the second allocation
constexpr uint32_t kTrailerHalf12 = 12;   // u16, zeroed
constexpr uint32_t kTrailerCount = 14;    // u16, u16[desc+24]
constexpr uint32_t kTrailerChannels = 16; // u8, the same low byte as object +46

// lis r11,-32206 ; addi r10,r11,-17760 -- ((-32206 & 0xFFFF) << 16) - 17760.
constexpr uint32_t kObjectVtable = 0x82320000u - 17760u;  // 0x8231BAA0
// lis r9,-32241 ; addi r5,r9,-10800 -- the second allocation's tag; sub_82B3D668's sits 68 bytes
// further on in the same rodata, which is the only reason it is read as a tag at all.
constexpr uint32_t kFloatsTag = 0x820F0000u - 10800u;  // 0x820ED5D0

// lis r11,-31993 ; lwz r11,30252(r11) -> the global System pointer (port_82B1E458's kSystemPtr).
constexpr uint32_t kSystemPtr = 0x83070000u + 30252u;  // 0x8307762C
constexpr uint32_t kAllocator = 36;  // System+36, and owner+36: both allocator objects
constexpr uint32_t kAllocSlot = 4;   // allocator vtable +4: allocate

constexpr uint32_t kFrameBytes = 192;  // stwu r1,-192(r1)
constexpr uint32_t kAlignOut = 80;     // addi r4,r1,80 -- the sizing entry's out-parameter

// bctrl return addresses, reproduced so a callee that reads lr sees what it would see.
constexpr uint32_t kSizeReturn = 0x82B3CD64u;
constexpr uint32_t kAllocReturn = 0x82B3CDF0u;
constexpr uint32_t kInitReturn = 0x82B3CE2Cu;
constexpr uint32_t kFloatsReturn = 0x82B3CEDCu;

REX_FUNC(Native) {
  // The frame is reproduced because all five callees run on it AND because the sizing entry
  // writes its alignment out-parameter into it at +80, which is read back below. __savegprlr_21's
  // spills of r21..r31 and lr land inside these 192 bytes, so they are this call's own frame and
  // are not windowed; they are not reproduced either, because this body keeps those values in
  // locals and never writes the guest registers.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f520fc
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-192(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t desc = ctx.r3.u32;       // mr r30,r3
  const uint64_t channels = ctx.r4.u64;   // mr r27,r4
  const uint64_t owner = ctx.r5.u64;      // mr r23,r5

  // lwz r11,0(r3) ; mtctr r11 ; bctrl -- the descriptor's +0 is the TARGET, not a vtable: called
  // with the channel count and a stack slot to leave the required alignment in.
  ctx.ctr.u64 = REX_LOAD_U32(desc + kDescSizeFn);
  ctx.r3.u64 = channels;              // mr r3,r27
  ctx.r4.u64 = frame + kAlignOut;     // addi r4,r1,80
  ctx.lr = kSizeReturn;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  const uint64_t body_bytes = ctx.r3.u64;                   // mr r26,r3
  const uint64_t count = REX_LOAD_U16(desc + kDescCount);   // lhz r9,24(r30)
  uint64_t align = REX_LOAD_U32(frame + kAlignOut);         // lwz r10,80(r1)
  // addic r8,r9,-1 ; subfe r25,r8,r9 -- the carry-chain idiom for `count != 0`, leaving 0 or 1.
  const uint64_t has_trailer = (uint32_t(count) != 0) ? 1u : 0u;
  // addi r7,r3,7 ; rlwinm r11,r7,0,0,28 ; addi r29,r11,480 -- the derived part rounded up to 8,
  // then the ring.
  uint64_t size = (uint64_t(uint32_t(body_bytes + 7)) & 0xFFFFFFF8u) + kRingBytes;
  uint64_t trailer_align = 0;  // mr r22,r28 -- zero unless there is a trailer
  // clrlwi r24,r25,24 ; cmplwi cr6,r24,0 ; beq cr6,0x82b3cdb4
  if ((has_trailer & 0xFFu) != 0) {
    trailer_align = 16;  // li r22,16
    // addi r11,r29,15 ; rlwinm r11,r11,0,0,27 ; addi r29,r11,20
    size = (uint64_t(uint32_t(size + 15)) & 0xFFFFFFF0u) + 20;
    // cmplwi cr6,r10,16 ; bgt cr6,0x82b3cdb4 ; li r10,16 -- the alignment is raised to 16, never
    // lowered. The size update above happens on both sides of this branch.
    if (!(uint32_t(align) > 16)) align = 16;
  }

  // loc_82B3CDB4 -- mr r4,r29 ; cmplwi cr6,r29,0 ; bne cr6,0x82b3cdc4 ; li r4,52. A zero size can
  // only come of a wrap, and the original substitutes 52; reproduced.
  const uint64_t request = (uint32_t(size) != 0) ? size : 52u;

  // loc_82B3CDC4 -- allocate(request, 0, 0, align, 0) through the System allocator's slot +4.
  // The first gate-1 allocation.
  const uint32_t system = REX_LOAD_U32(kSystemPtr);
  const uint32_t allocator = REX_LOAD_U32(system + kAllocator);
  const uint32_t alloc_vtable = REX_LOAD_U32(allocator + kVptr);
  ctx.ctr.u64 = REX_LOAD_U32(alloc_vtable + kAllocSlot);
  ctx.r3.u64 = allocator;
  ctx.r4.u64 = request;
  ctx.r5.s64 = 0;       // li r5,0
  ctx.r6.s64 = 0;       // li r6,0
  ctx.r7.u64 = align;   // mr r7,r10
  ctx.r8.s64 = 0;       // li r8,0
  ctx.lr = kAllocReturn;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  const uint64_t object = ctx.r3.u64;  // mr r31,r3
  // cmplwi cr6,r3,0 ; beq cr6,0x82b3cef0 -- a failed allocation returns 0 and writes nothing.
  if (uint32_t(object) == 0) {
    ctx.r3.s64 = 0;                         // li r3,0
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,192
    ctx.lr = entry_lr;                      // b 0x82f5214c (__restgprlr_21)
    return;
  }

  // The fixed fields, in the lifted store order. clrlwi r21,r27,24 is the channel count's low
  // byte, stored twice (here and in the trailer).
  const uint8_t channels_byte = uint8_t(channels);
  REX_STORE_U32(object + kVptr, kObjectVtable);             // stw r10,0(r3)
  const uint64_t desc_word8 = REX_LOAD_U32(desc + kDescWord8);  // lwz r11,8(r30)
  REX_STORE_U32(object + kObjOwner, uint32_t(owner));       // stw r23,4(r3)
  REX_STORE_U8(object + kObjChannels, channels_byte);       // stb r21,46(r3)
  REX_STORE_U32(object + kObjFloats, 0);                    // stw r28,16(r3)
  REX_STORE_U32(object + kObjWord12, uint32_t(desc_word8)); // stw r11,12(r3)

  // lwz r10,4(r30) ; mtctr r10 ; bctrl -- the descriptor's init entry, again a raw function
  // pointer. r3 still holds the object from the allocation; the other argument registers hold
  // whatever the allocator left, so only r3 is set here.
  ctx.ctr.u64 = REX_LOAD_U32(desc + kDescInitFn);
  ctx.r3.u64 = object;
  ctx.lr = kInitReturn;
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  // clrlwi r9,r3,24 ; cmplwi cr6,r9,0 ; beq cr6,0x82b3cee8 -- only the LOW BYTE is tested.
  bool torn_down = (ctx.r3.u32 & 0xFFu) == 0;
  uint64_t ring_base = 0;

  if (!torn_down) {
    REX_STORE_U32(object + kObjSelf, uint32_t(object));  // stw r31,8(r31)
    // add r11,r31,r26 ; addi r8,r11,7 ; rlwinm r10,r8,0,0,28 -- the ring starts at the first
    // 8-aligned address past the derived part.
    const uint64_t after_body = object + body_bytes;
    const uint64_t desc_word12 = REX_LOAD_U32(desc + kDescWord12);  // lwz r10,12(r30)
    REX_STORE_U32(object + kObjWord20, uint32_t(desc_word12));      // stw r10,20(r31)
    ring_base = uint64_t(uint32_t(after_body + 7)) & 0xFFFFFFF8u;
    const uint64_t desc_word20 = REX_LOAD_U32(desc + kDescWord20);  // lwz r7,20(r30)
    REX_STORE_U32(object + kObjWord24, uint32_t(desc_word20));      // stw r7,24(r31)
    const uint64_t ring_offset = ring_base - object;                // subf r6,r31,r10
    REX_STORE_U32(object + kObjSize, uint32_t(size));               // stw r29,32(r31)
    REX_STORE_U8(object + kObjHasTrailer, uint8_t(has_trailer));    // stb r25,51(r31)
    REX_STORE_U16(object + kObjHalf44, 0);                          // sth r28,44(r31)
    REX_STORE_U32(object + kLatch, 0);                              // stw r28,28(r31)
    REX_STORE_U8(object + kWriteIndex, 0);                          // stb r28,47(r31)
    REX_STORE_U8(object + kObjByte48, 0);                           // stb r28,48(r31)
    REX_STORE_U8(object + kLatchIndex, 0);                          // stb r28,49(r31)
    REX_STORE_U8(object + kSlotCount, uint8_t(kSlots));             // stb r9,50(r31)
    REX_STORE_U32(object + kSlotArrayOff, uint32_t(ring_offset));   // stw r6,36(r31)

    // beq cr6,0x82b3cf14 -- the cr6 still holding the `count != 0` test from before the
    // allocation. No trailer means no second allocation either.
    if ((has_trailer & 0xFFu) != 0) {
      // addi r11,r22,-1 ; add r10,r11,r10 ; addi r7,r10,480 ; andc r29,r7,r11 -- the first
      // 16-aligned address past the ring.
      const uint64_t mask = trailer_align - 1;  // 15
      const uint64_t trailer = (ring_base + mask + kRingBytes) & ~mask;
      // subf r4,r31,r29 ; clrlwi r3,r4,16 ; stw r3,40(r31) -- the offset, truncated to 16 bits
      // and stored in a WORD.
      REX_STORE_U32(object + kObjTrailerOff, uint32_t(trailer - object) & 0xFFFFu);

      // lhz r11,24(r30) ; mullw r4,r11,r27 ; rlwinm r4,r4,2,0,29 -- the count is RELOADED; the
      // init entry above could have changed the descriptor. allocate(count*channels*4, tag, 0,
      // 128, 0) out of the OWNER's allocator, not the System's. The second gate-1 allocation.
      const uint64_t count_now = REX_LOAD_U16(desc + kDescCount);
      const uint64_t floats =
          uint64_t(int64_t(int32_t(uint32_t(count_now))) * int64_t(int32_t(uint32_t(channels))));
      const uint32_t owner_allocator = REX_LOAD_U32(uint32_t(owner) + kAllocator);  // lwz r3,36
      const uint32_t owner_vtable = REX_LOAD_U32(owner_allocator + kVptr);
      ctx.ctr.u64 = REX_LOAD_U32(owner_vtable + kAllocSlot);
      ctx.r3.u64 = owner_allocator;
      ctx.r4.u64 = (uint64_t(uint32_t(floats)) << 2) & 0xFFFFFFFCull;
      ctx.r5.s64 = int32_t(kFloatsTag);  // lis ; addi -- sign-extended, as lifted
      ctx.r6.s64 = 0;                    // li r6,0
      ctx.r7.s64 = 128;                  // li r7,128
      ctx.r8.s64 = 0;                    // li r8,0
      ctx.lr = kFloatsReturn;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

      const uint64_t block = ctx.r3.u64;
      REX_STORE_U32(object + kObjFloats, uint32_t(block));  // stw r3,16(r31)
      // cmplwi cr6,r3,0 ; bne cr6,0x82b3cefc
      if (uint32_t(block) == 0) {
        torn_down = true;  // falls into loc_82B3CEE8
      } else {
        // loc_82B3CEFC -- the trailer, in the lifted store order. The count is reloaded a third
        // time for its own field.
        const uint64_t count_again = REX_LOAD_U16(desc + kDescCount);   // lhz r11,24(r30)
        REX_STORE_U32(trailer + kTrailerOwner, uint32_t(owner));        // stw r23,0(r29)
        REX_STORE_U16(trailer + kTrailerHalf12, 0);                     // sth r28,12(r29)
        REX_STORE_U8(trailer + kTrailerChannels, channels_byte);        // stb r21,16(r29)
        REX_STORE_U32(trailer + kTrailerFloats, uint32_t(block));       // stw r3,4(r29)
        REX_STORE_U16(trailer + kTrailerCount, uint16_t(count_again));  // sth r11,14(r29)
      }
    }
  }

  if (torn_down) {
    // loc_82B3CEE8 -- hand the half-built object to sub_82B3C930 and return 0.
    skate3::audio::GuestCall(ctx, base, sub_82B3C930, object);
    ctx.r3.s64 = 0;                         // li r3,0
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,192
    ctx.lr = entry_lr;                      // b 0x82f5214c
    return;
  }

  // loc_82B3CF14 -- zero every slot's +0 and +12. Both the slot count and the array offset are
  // RELOADED from the object: the init entry may have changed either.
  const uint64_t slots = REX_LOAD_U8(object + kSlotCount);          // lbz r10,50(r31)
  const uint64_t array_off = REX_LOAD_U32(object + kSlotArrayOff);  // lwz r11,36(r31)
  // rotlwi r9,r10,1 ; add r10,r10,r9 ; rlwinm r10,r10,3,0,28 -- count*3 then *8, i.e. *24.
  const uint64_t triple = slots + uint64_t(__builtin_rotateleft32(uint32_t(slots), 1));
  const uint64_t span = (uint64_t(uint32_t(triple) << 3)) & 0xFFFFFFF8u;
  static_assert(kSlotStride == 24, "count*3*8");
  uint64_t slot = object + array_off;  // add r11,r31,r11
  const uint64_t end = span + slot;    // add r10,r10,r11
  // cmplw cr6,r11,r10 ; beq cr6,0x82b3cf4c -- a do-while on UNSIGNED inequality of the low words.
  while (uint32_t(slot) != uint32_t(end)) {
    // loc_82B3CF38
    REX_STORE_U32(slot + kSlotWord0, 0);  // stw r28,0(r11)
    REX_STORE_U32(slot + kSlotBusy, 0);   // stw r28,12(r11)
    slot += kSlotStride;                  // addi r11,r11,24
  }

  // loc_82B3CF4C
  ctx.r3.u64 = object;                    // mr r3,r31
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,192
  ctx.lr = entry_lr;                      // b 0x82f5214c (__restgprlr_21)
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B3CD38
SKATE3_PORT(82B3CD38, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
