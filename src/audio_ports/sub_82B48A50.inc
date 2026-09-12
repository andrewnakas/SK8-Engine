// sub_82B48A50  role: tick one scheduler bucket -- walk the node list at scheduler + (bucket<<5) + 16, publish each node's instance at +68, clear the removed flag at +76, call the instance's process fn through its function pointer with delta_time in f1, then record the timebase delta into instance+16 when the descriptor's profiling byte is set (or zero it), and on a self-removal hand scheduler + u32[+72]*32 to sub_82B39690   lifted: skate3_recomp.68.cpp:37355 (128 lines)
// STATUS: gate-1 (the node's own process fn: sub_82B48A50 indirect call, `bctrl` at 0x82B48AC0 -- and gate 3 on top of it, `mftb` through sub_82B1F7E8 twice per node)
// calls/session: boot 37370  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B48A50.md
namespace port_82B48A50 {

// docs/rw_audio_structs.h names most of this: rw_scheduler lives at System + 0x70, its buckets
// are at +0x10 with stride 0x20, and rw_node / rw_instance are its list nodes and payloads.
constexpr uint32_t kBucketHead = 16;   // scheduler + (bucket<<5) + 0x10
constexpr uint32_t kDeltaTime = 64;    // f32 +0x40, passed to every process fn in f1
constexpr uint32_t kRunning = 68;      // u32 +0x44, the INSTANCE (not the node) while it ticks
constexpr uint32_t kRemovedIndex = 72; // u32 +0x48, undocumented: scaled by 32 for sub_82B39690
constexpr uint32_t kNodeRemoved = 76;  // u32 +0x4C, set by a node that removes itself

constexpr uint32_t kNodeNext = 0;      // rw_node +0x00
constexpr uint32_t kNodeInstance = 8;  // rw_node +0x08

constexpr uint32_t kInstanceDescriptor = 0;  // rw_instance +0x00 -> rw_plugin_desc
constexpr uint32_t kInstanceProcess = 4;     // rw_instance +0x04, the indirect target
constexpr uint32_t kInstanceContext = 8;     // rw_instance +0x08, its r3
constexpr uint32_t kInstanceElapsed = 16;    // rw_instance +0x10, the per-tick time
constexpr uint32_t kDescProfiling = 12;      // descriptor +12: zero means do not time this node

constexpr uint32_t kFrameBytes = 128;              // stwu r1,-128(r1)
constexpr uint32_t kIndirectReturn = 0x82B48AC4u;  // the bctrl's return address

// rlwinm rX,rY,5,0,26 -- (value << 5) & 0xFFFFFFE0, i.e. a stride of 32 with the top five bits
// of the index rotated out. Written out because the operand here is a full 32-bit word, not a
// byte load: for an index past 0x07FFFFFF the rotate really does discard bits.
uint32_t Stride32(uint32_t value) { return (value << 5) & 0xFFFFFFE0u; }

REX_FUNC(Native) {
  // The frame is reproduced because all three callees -- two direct, one through a function
  // pointer -- run on it. __savegprlr_27's spills of r27..r31 land inside these 128 bytes, so
  // they are this call's own frame; they are not reproduced because this body keeps those
  // values in locals and never writes the guest registers.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint64_t entry_lr = ctx.lr;               // mflr r12 ; bl 0x82f52114
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t scheduler = ctx.r3.u32;                               // mr r30,r3
  const uint32_t bucket = uint32_t(uint64_t(Stride32(ctx.r4.u32)) + uint64_t(scheduler));
  uint32_t node = REX_LOAD_U32(bucket + kBucketHead);                  // lwz r10,16(r11)

  // cmpwi cr6,r10,0 ; beq cr6,0x82b48b1c -- then `rotlwi r31,r10,0` (a plain move) and
  // cmplwi cr6,r31,0 ; beq cr6,0x82b48b1c: the same word tested twice, signed then unsigned.
  // The second test is dead and is folded into the first.
  if (node != 0) {
    for (;;) {
      // loc_82B48A84 -- bl 0x82b1f7e8: `mftb`. Gate 3 enters here; the value is also the start
      // stamp for the profiling delta below.
      skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
      const uint32_t instance = REX_LOAD_U32(node + kNodeInstance);  // lwz r11,8(r31)
      const uint32_t next = REX_LOAD_U32(node + kNodeNext);          // lwz r27,0(r31)
      const uint32_t started = ctx.r3.u32;                           // mr r28,r3

      // cmplwi cr6,r11,0 ; beq cr6,0x82b48aa4 -- the two arms set r31 to r11 and to r29 (= 0),
      // which is the same value either way, so the branch is folded. A node with no instance
      // therefore dereferences guest address 0 below; reproduced, not guarded.
      const uint32_t current = instance;

      REX_STORE_U32(scheduler + kRunning, current);  // stw r31,68(r30)
      ctx.fpscr.disableFlushModeUnconditional();
      PPCRegister delta{};
      delta.u32 = REX_LOAD_U32(scheduler + kDeltaTime);  // lfs f1,64(r30)
      ctx.f1.f64 = double(delta.f32);                    // the float argument, in f1
      REX_STORE_U32(scheduler + kNodeRemoved, 0);        // stw r29,76(r30), r29 = 0

      // lwz r3,8(r31) ; lwz r11,4(r31) ; mtctr r11 ; bctrl -- process(context) with the frame
      // delta in f1. This is the gate-1 call: the target is a data word and the callee is
      // arbitrary plug-in code.
      ctx.r3.u64 = REX_LOAD_U32(current + kInstanceContext);
      ctx.ctr.u64 = REX_LOAD_U32(current + kInstanceProcess);
      ctx.lr = kIndirectReturn;
      REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

      const uint32_t removed = REX_LOAD_U32(scheduler + kNodeRemoved);  // lwz r4,76(r30)
      REX_STORE_U32(scheduler + kRunning, 0);                           // stw r29,68(r30)
      // cmplwi cr6,r4,0 ; bne cr6,0x82b48afc
      if (removed == 0) {
        const uint32_t descriptor = REX_LOAD_U32(current + kInstanceDescriptor);  // lwz r11,0(r31)
        // lbz r10,12(r11) ; cmplwi cr6,r10,0 ; beq cr6,0x82b48af4
        if (REX_LOAD_U8(descriptor + kDescProfiling) != 0) {
          skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);  // bl 0x82b1f7e8 -- the end stamp
          // subf r11,r28,r3 -- 64-bit on two zero-extended words, truncated at the store.
          const uint64_t elapsed = uint64_t(ctx.r3.u32) - uint64_t(started);
          REX_STORE_U32(current + kInstanceElapsed, uint32_t(elapsed));  // stw r11,16(r31)
        } else {
          // loc_82B48AF4
          REX_STORE_U32(current + kInstanceElapsed, 0);  // stw r29,16(r31)
        }
      } else {
        // loc_82B48AFC -- the node took itself out of the list while it ran.
        const uint32_t removed_index = REX_LOAD_U32(scheduler + kRemovedIndex);  // lwz r11,72(r30)
        // rlwinm r11,r11,5,0,26 ; add r3,r11,r30 ; bl 0x82b39690
        skate3::audio::GuestCall(ctx, base, sub_82B39690,
                                 uint32_t(uint64_t(Stride32(removed_index)) + uint64_t(scheduler)));
        REX_STORE_U32(scheduler + kNodeRemoved, 0);  // stw r29,76(r30)
      }

      // loc_82B48B10 -- the next pointer was read BEFORE the process fn ran, so a node that
      // unlinks itself is still followed to the successor it had on entry.
      node = next;             // mr r31,r27
      if (next == 0) break;    // cmplwi cr6,r27,0 ; bne cr6,0x82b48a84
    }
  }

  // loc_82B48B1C -- no `li r3,...` on any path: r3 is the scheduler when the bucket is empty and
  // the last callee's result otherwise.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,128
  ctx.lr = entry_lr;                      // b 0x82f52164
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B48A50
SKATE3_PORT(82B48A50, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
