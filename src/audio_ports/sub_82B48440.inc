// sub_82B48440  role: drain the system's list at +0x20 -- for each object on it, replay every id of every record in its u8[+68]-entry table at +80 through sub_82B48BB8(system+112, id), then unlink the object's node at +28 from the list headed at u32[u32[+16]+32], clear u8[+71] and hand the object to sub_82B49280   lifted: skate3_recomp.68.cpp:36425 (142 lines)
// STATUS: gate-1 (sub_82B49280 reaches the core allocator through two bctrls one level down -- the vtable's +4 allocate at 0x82B492EC and its +12 free at 0x82B49328; the call here is `bl 0x82b49280` at 0x82B4851C)
// calls/session: boot 18685  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B48440.md
namespace port_82B48440 {

// rw_system, from docs/rw_audio_structs.h: +0x20 is `list_20`, documented as "walked by
// sub_82B48440" -- this is that walk. +112 (0x70) is the scheduler, and the object handed to
// sub_82B48BB8 as its r3 is `system + 112`, i.e. the scheduler itself.
constexpr uint32_t kListHead = 32;  // system +0x20, and the same +32 on the object's own owner
constexpr uint32_t kScheduler = 112;  // system +0x70 -> rw_scheduler, sub_82B48BB8's r3

// The object on the list. Nothing names it; plain offsets are honest. Its list node is at +28,
// so the list threads through the middle of the object and the walk does container_of by hand.
constexpr uint32_t kLinkOffset = 28;  // `addi r29,r28,-28`: link -> object
constexpr uint32_t kOwner = 16;       // u32 -> the object whose +32 heads this list
constexpr uint32_t kSlotCount = 68;   // u8, how many 4-byte slots at +80 are live
constexpr uint32_t kSlots = 80;       // u32[count], each a record pointer
constexpr uint32_t kFlag71 = 71;      // u8, cleared just before the release

constexpr uint32_t kLinkNext = 0;  // link +0
constexpr uint32_t kLinkPrev = 4;  // link +4

// The record each slot points at.
constexpr uint32_t kRecordCount = 43;  // u8, how many ids follow; reloaded every iteration
constexpr uint32_t kRecordIds = 20;    // the `lwzu r4,4(r6)` base: the first id is at +24

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)

REX_FUNC(Native) {
  // The frame is reproduced because both callees run on it. __savegprlr_26's spills of r26..r31
  // land inside these 144 bytes, so they are this call's own frame; they are not reproduced
  // because the nonvolatile values live in locals here and the guest registers are untouched.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f52110
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-144(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t system = ctx.r3.u32;                  // mr r26,r3
  uint32_t link = REX_LOAD_U32(system + kListHead);    // lwz r28,32(r3)

  // cmplwi cr6,r28,0 ; beq cr6,0x82b48528
  while (link != 0) {
    // loc_82B48460 -- cmplwi cr6,r28,0 ; addi r29,r28,-28 ; bne ; mr r29,r27 (r27 = 0). The
    // null guard is dead given the loop condition, but it is a real branch and is kept.
    const uint32_t object = (link != 0) ? uint32_t(link - kLinkOffset) : 0u;

    const uint32_t slots = REX_LOAD_U8(object + kSlotCount);  // lbz r11,68(r29)
    link = REX_LOAD_U32(link + kLinkNext);  // lwz r28,0(r28) -- the successor is read BEFORE the
                                            // body unlinks this object

    // cmplwi cr6,r11,0 ; beq cr6,0x82b484d0
    if (slots != 0) {
      uint32_t slot = object + kSlots;  // addi r31,r29,80
      int32_t index = 0;               // mr r30,r27
      for (;;) {
        // loc_82B48488
        ctx.r5.u64 = REX_LOAD_U32(slot + 0);                     // lwz r5,0(r31)
        ctx.r7.s64 = 0;                                          // mr r7,r27
        const uint32_t ids = REX_LOAD_U8(ctx.r5.u32 + kRecordCount);  // lbz r11,43(r5)
        // cmplwi cr6,r11,0 ; beq cr6,0x82b484bc
        if (ids != 0) {
          ctx.r3.u64 = system + kScheduler;            // addi r3,r26,112
          ctx.r6.s64 = ctx.r5.s64 + kRecordIds;        // addi r6,r5,20
          for (;;) {
            // loc_82B484A4 -- lwzu r4,4(r6): pre-increment, so the first id read is +24.
            const uint32_t ea = 4u + ctx.r6.u32;
            ctx.r4.u64 = REX_LOAD_U32(ea);
            ctx.r6.u32 = ea;
            // bl 0x82b48bb8 -- r3, r5, r6 and r7 are all VOLATILE and all four are live across
            // this call: the original leaves them in registers and reads them again below. So
            // they are kept in the guest registers here, not in locals -- whatever the callee
            // leaves in them is what the original's loop sees. (That the compiler emitted this
            // at all implies sub_82B48BB8 preserves them, but the port does not rely on it.)
            skate3::audio::GuestCall(ctx, base, sub_82B48BB8);
            const uint32_t count = REX_LOAD_U8(ctx.r5.u32 + kRecordCount);  // lbz r11,43(r5)
            ctx.r7.s64 = ctx.r7.s64 + 1;                                    // addi r7,r7,1
            // cmpw cr6,r7,r11 ; blt cr6,0x82b484a4 -- SIGNED, and the count is reloaded every
            // iteration, so the callee can shorten the walk.
            if (!(ctx.r7.s32 < static_cast<int32_t>(count))) break;
          }
        }
        // loc_82B484BC -- the slot count is reloaded here too.
        const uint32_t live = REX_LOAD_U8(object + kSlotCount);  // lbz r11,68(r29)
        index += 1;                                              // addi r30,r30,1
        slot += 4;                                               // addi r31,r31,4
        // cmpw cr6,r30,r11 ; blt cr6,0x82b48488 -- SIGNED
        if (!(index < static_cast<int32_t>(live))) break;
      }
    }

    // loc_82B484D0 -- unlink the node at object+28 from the list headed at owner+32.
    const uint32_t owner = REX_LOAD_U32(object + kOwner);  // lwz r9,16(r29)
    const uint32_t node = object + kLinkOffset;            // addi r11,r29,28
    const uint32_t head = REX_LOAD_U32(owner + kListHead); // lwz r10,32(r9)
    // cmplw cr6,r11,r10 ; bne cr6,0x82b484ec -- this object is the head: pop it.
    if (node == head) {
      REX_STORE_U32(owner + kListHead, REX_LOAD_U32(head + kLinkNext));  // stw r10,32(r9)
    }
    // loc_82B484EC -- lwz r10,4(r11) ; cmpwi cr6,r10,0 ; beq: SIGNED test against zero.
    const uint32_t prev = REX_LOAD_U32(node + kLinkPrev);
    if (static_cast<int32_t>(prev) != 0) {
      REX_STORE_U32(prev + kLinkNext, REX_LOAD_U32(node + kLinkNext));  // stw r9,0(r10)
    }
    // loc_82B48500 -- the next pointer is RELOADED, after the store above that could alias it.
    const uint32_t next = REX_LOAD_U32(node + kLinkNext);
    if (static_cast<int32_t>(next) != 0) {
      REX_STORE_U32(next + kLinkPrev, REX_LOAD_U32(node + kLinkPrev));  // stw r11,4(r10)
    }
    // loc_82B48514
    REX_STORE_U8(object + kFlag71, 0);  // stb r27,71(r29)
    // bl 0x82b49280 -- the gate-1 call: it allocates and frees through the core allocator.
    skate3::audio::GuestCall(ctx, base, sub_82B49280, object);
    // cmplwi cr6,r28,0 ; bne cr6,0x82b48460 -- the successor captured before the body ran.
  }

  // loc_82B48528 -- no `li r3,...` on any path: r3 is the system when the list is empty and
  // sub_82B49280's result otherwise.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
  ctx.lr = entry_lr;                      // b 0x82f52160
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B48440
SKATE3_PORT(82B48440, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
