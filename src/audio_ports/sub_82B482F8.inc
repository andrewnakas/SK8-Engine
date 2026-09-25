// sub_82B482F8  role: drain the system list at +28 -- push each element's child sends into the pool, unlink its node, then reset and recycle the element   lifted: skate3_recomp.68.cpp:36228 (196 lines)
// STATUS: gate-2 (list walk of unknown length; three objects patched per node -- measured, see docs/shadow-harness.md)
// calls/session: boot 18685  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B482F8.md
namespace port_82B482F8 {

// The System object (r3). docs/rw_audio_structs.h names none of these; docs/decompilation-status.md
// identifies +28 (System+0x1C) as one of the two further lists the queue drain walks.
constexpr uint32_t kSysList = 28;   // the intrusive list head this call drains
constexpr uint32_t kSysPool = 112;  // sub_82B48B28's first argument -- the sink for each send

// The element. Its list node sits at +28, so a node pointer less 28 is the element.
constexpr uint32_t kElemNodeOffset = 28;
constexpr uint32_t kElemFloat0 = 0;
constexpr uint32_t kElemFloat1 = 4;
constexpr uint32_t kElemFloat2 = 8;
constexpr uint32_t kElemOwner = 16;       // rw_ptr, RE-READ after sub_82B49438
constexpr uint32_t kElemWord60 = 60;
constexpr uint32_t kElemChildCount = 68;  // u8, RELOADED every iteration of the child loop
constexpr uint32_t kElemState = 71;       // u8: zeroed while retiring, 4 once recycled
constexpr uint32_t kElemChildren = 80;    // kElemChildCount pointers, one per child

// The child (a voice-side object). Its send words start at +24: the original biases the pointer to
// +20 and uses `lwzu r4,4(r5)`, so the first load is from +24.
constexpr uint32_t kChildSendCount = 43;  // u8, RELOADED every iteration of the send loop
constexpr uint32_t kChildSendBase = 20;   // addi r5,r31,20 ; lwzu r4,4(r5)

// The owner, element[+16]: two intrusive list heads.
constexpr uint32_t kOwnerFree = 24;  // the list the node is pushed onto when sub_82B49438 agrees
constexpr uint32_t kOwnerLive = 28;  // the list the node is unlinked from

// The node itself. The head fix-up publishes node[+0], so +0 is the forward link.
constexpr uint32_t kNodeNext = 0;
constexpr uint32_t kNodePrev = 4;

// lis r11,-32234 ; lfs f31,23056(r11): ((-32234 & 0xFFFF) << 16) + 23056 = 0x82160000 + 0x5A10.
// The same constant port_82B269C0 names kZeroFloat.
constexpr uint32_t kZeroFloat = 0x82165A10;

constexpr uint32_t kFrameSize = 160;  // stwu r1,-160(r1)

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

REX_FUNC(Native) {
  const uint64_t self_wide = ctx.r3.u64;         // mr r25,r3 -- all 64 bits, for addi r3,r25,112
  const uint32_t self = ctx.r3.u32;
  uint32_t node = REX_LOAD_U32(self + kSysList);  // lwz r26,28(r3)

  // stwu r1,-160(r1). Reproduced only so the two callees' frames land where the original put
  // them; the frame's own contents are this function's business and are not windowed. The
  // __savegprlr_24 spills and the f31 spill below it are not reproduced -- r24-r31 and f31 are
  // never written here, so there is nothing to restore.
  const uint32_t frame = ctx.r1.u32 - kFrameSize;
  REX_STORE_U32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;

  // cmplwi cr6,r26,0 ; beq cr6,0x82b48434 -- an empty list writes nothing at all.
  if (node != 0) {
    // The prologue's stfd left flush mode disabled before any float work; the lfs follows it.
    ctx.fpscr.disableFlushModeUnconditional();
    const double zero = LoadSingle(base, kZeroFloat);  // lfs f31,23056(r11)
    // li r27,0 ; li r24,4 -- the two constants every store below uses.

    do {  // loc_82B48328
      // cmplwi cr6,r26,0 ; addi r30,r26,-28 ; bne cr6 ; mr r30,r27. Neither entry to this block
      // can reach it with a null node, but the original still computes the fallback, so it is
      // reproduced rather than assumed away.
      const uint64_t element_wide =
          node != 0 ? uint64_t(int64_t(uint64_t(node)) - int64_t(kElemNodeOffset)) : 0;
      const uint32_t element = uint32_t(element_wide);

      const uint32_t children = REX_LOAD_U8(element + kElemChildCount);  // lbz r11,68(r30)
      // lwz r26,0(r26) -- the walk's next pointer is read BEFORE the unlink below removes this
      // node from the very list being walked. That ordering is what makes the walk survive.
      node = REX_LOAD_U32(node + kNodeNext);

      // cmplwi cr6,r11,0 ; beq cr6,0x82b48398
      if (children != 0) {
        uint32_t slot = uint32_t(element_wide + kElemChildren);  // addi r29,r30,80
        int32_t index = 0;                                      // mr r28,r27
        for (;;) {                                              // loc_82B48350
          const uint32_t child = REX_LOAD_U32(slot);            // lwz r31,0(r29)
          ctx.r6.u64 = 0;                                       // mr r6,r27
          // lbz r11,43(r31) ; cmplwi cr6,r11,0 ; beq cr6,0x82b48384
          if (REX_LOAD_U8(child + kChildSendCount) != 0) {
            // r3 and r5 are established ONCE, outside the send loop, and are live across every
            // call in it: the original relies on sub_82B48B28 leaving both alone, which is a
            // whole-program register-allocation fact, not the ABI. GuestCall clobbers exactly
            // what the real call clobbers, so r3, r5 and the r6 counter are read back out of ctx
            // each iteration instead of being cached in locals.
            ctx.r3.s64 = int64_t(self_wide) + int64_t(kSysPool);              // addi r3,r25,112
            ctx.r5.s64 = int64_t(uint64_t(child)) + int64_t(kChildSendBase);  // addi r5,r31,20
            for (;;) {                                                       // loc_82B4836C
              const uint32_t ea = 4 + ctx.r5.u32;      // lwzu r4,4(r5)
              const uint32_t send = REX_LOAD_U32(ea);
              ctx.r5.u32 = ea;
              // bl 0x82b48b28 -- (pool, send)
              skate3::audio::GuestCall(ctx, base, sub_82B48B28, ctx.r3.u64, uint64_t(send));
              // lbz r11,43(r31) -- the send count is reloaded every iteration
              const int32_t sends = int32_t(REX_LOAD_U8(child + kChildSendCount));
              ctx.r6.s64 = ctx.r6.s64 + 1;                        // addi r6,r6,1
              if (!(ctx.r6.s32 < sends)) break;  // cmpw cr6,r6,r11 ; blt cr6,0x82b4836c
            }
          }
          // loc_82B48384: lbz r11,68(r30) -- the child count is reloaded every iteration too
          const int32_t count_now = int32_t(REX_LOAD_U8(element + kElemChildCount));
          index++;    // addi r28,r28,1
          slot += 4;  // addi r29,r29,4
          if (!(index < count_now)) break;  // cmpw cr6,r28,r11 ; blt cr6,0x82b48350
        }
      }

      // loc_82B48398 -- unlink the node from the owner's live list. Three objects are patched:
      // the owner's head, the previous node and the next node.
      const uint32_t owner_before = REX_LOAD_U32(element + kElemOwner);  // lwz r10,16(r30)
      const uint32_t self_node = uint32_t(element_wide + kElemNodeOffset);  // addi r31,r30,28
      const uint32_t head = REX_LOAD_U32(owner_before + kOwnerLive);        // lwz r11,28(r10)
      // cmplw cr6,r31,r11 ; bne cr6,0x82b483b4
      if (self_node == head) {
        // lwz r11,0(r11) ; stw r11,28(r10)
        REX_STORE_U32(owner_before + kOwnerLive, REX_LOAD_U32(head + kNodeNext));
      }
      // loc_82B483B4: lwz r11,4(r31) ; cmpwi cr6,r11,0 ; beq cr6,0x82b483c8
      const uint32_t prev = REX_LOAD_U32(self_node + kNodePrev);
      if (static_cast<int32_t>(prev) != 0) {
        // lwz r10,0(r31) ; stw r10,0(r11) -- prev->next = node->next
        REX_STORE_U32(prev + kNodeNext, REX_LOAD_U32(self_node + kNodeNext));
      }
      // loc_82B483C8: lwz r11,0(r31) -- re-read, after the store above could have aliased it
      const uint32_t next = REX_LOAD_U32(self_node + kNodeNext);
      if (static_cast<int32_t>(next) != 0) {  // cmpwi cr6,r11,0 ; beq cr6,0x82b483dc
        // lwz r10,4(r31) ; stw r10,4(r11) -- next->prev = node->prev
        REX_STORE_U32(next + kNodePrev, REX_LOAD_U32(self_node + kNodePrev));
      }
      // loc_82B483DC
      REX_STORE_U8(element + kElemState, 0);  // stb r27,71(r30)

      // mr r3,r30 ; bl 0x82b49438 -- (element). A non-zero low byte means it can be recycled.
      skate3::audio::GuestCall(ctx, base, sub_82B49438, element_wide);
      // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; beq cr6,0x82b4842c
      if ((ctx.r3.u32 & 0xFF) != 0) {
        const uint32_t owner = REX_LOAD_U32(element + kElemOwner);     // lwz r11,16(r30) -- re-read
        const uint32_t free_head = REX_LOAD_U32(owner + kOwnerFree);   // lwz r10,24(r11)
        REX_STORE_U32(self_node + kNodePrev, 0);                       // stw r27,4(r31)
        REX_STORE_U32(self_node + kNodeNext, free_head);               // stw r10,0(r31)
        // lwz r10,24(r11) -- reloaded, after the two stores above could have aliased it
        const uint32_t free_head_again = REX_LOAD_U32(owner + kOwnerFree);
        if (free_head_again != 0) {  // cmplwi cr6,r10,0 ; beq cr6,0x82b48414
          REX_STORE_U32(free_head_again + kNodePrev, self_node);  // stw r31,4(r10)
        }
        // loc_82B48414
        REX_STORE_U32(owner + kOwnerFree, self_node);  // stw r31,24(r11)
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, element + kElemFloat0, zero);  // stfs f31,0(r30)
        StoreSingle(base, element + kElemFloat1, zero);  // stfs f31,4(r30)
        REX_STORE_U8(element + kElemState, 4);           // stb r24,71(r30)
        StoreSingle(base, element + kElemFloat2, zero);  // stfs f31,8(r30)
        REX_STORE_U32(element + kElemWord60, 0);         // stw r27,60(r30)
      }
      // loc_82B4842C: cmplwi cr6,r26,0 ; bne cr6,0x82b48328
    } while (node != 0);
  }

  // loc_82B48434: addi r1,r1,160. The lfd f31,-80(r1) that follows restores a register this body
  // never wrote.
  ctx.r1.u32 = frame + kFrameSize;
}

// Not enumerable from entry state -- gate 2, and the canonical one: docs/shadow-harness.md records
// this function as the measured "address set data-dependent" failure, and docs/PLAN.md phase 2
// gate 2 names it too.
//
// The walk is over the list at self+28 and its length is whatever the drain left there -- nothing
// readable before the call bounds it. Worse, per node the call patches THREE separate objects
// whose addresses are only reachable through that node: the owner at element[+16] (its head at
// +28, then its free head at +24), the node at element[+28]+4 (prev) and the node at
// element[+28]+0 (next). Both of the latter are themselves nodes of the same list, so the second
// iteration's write set depends on the surgery the first iteration performed. sub_82B48B28's
// subtree adds a further unbounded set keyed on each child's +43 count, which is reloaded inside
// its own loop. Enumerating any of this would mean replaying the whole walk inside the window
// builder, which is exactly the mistake the gate exists to prevent.
//
// Not rescuable by mirroring a callee's Windows(): neither sub_82B48B28 nor sub_82B49438 has a
// port in recomp/src/audio_ports/, and the unenumerable part is this function's own list walk,
// not a callee's.
//
// The body above is still exact and still the reference for the Rust port. Coverage for it has to
// come piecewise, from ports of sub_82B48B28 and sub_82B49438 plus the unlink pattern already
// verified in port_82B34BD0.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B482F8
SKATE3_PORT(82B482F8, skate3::audio::kPortGate2, skate3::audio::kReturnR3)
