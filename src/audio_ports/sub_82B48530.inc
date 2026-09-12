// sub_82B48530  role: the System drain, once per audio frame -- four lock/unlock brackets around four phases: tick scheduler bucket 0; walk the +0x14 list clamping each record's fade value at +0x30 up to its floor at +0x28 and firing sub_82B49100(record, 3) once the trigger at +0x2C has been reached; walk the two other lists (sub_82B482F8, sub_82B48440); drain the command ring at +0x30 by calling each record's own handler through its first word and advancing by the size it returns, then update the high-water mark at +0xD0, reset the write offset at +0xCC and bump the drain counter at +0x100; finally tick bucket 1 and run sub_82B39820 twice. Every phase is timed with sub_82B1F7E8 and the elapsed values land in +0xE8, +0xEC and +0xF4   lifted: skate3_recomp.68.cpp:36568 (394 lines)
// STATUS: gate-1 (nine indirect calls, depth 0: the lock and unlock hooks at +0x54/+0x58 are called through mtctr/bctrl at all eight bracket sites, and each command record's handler is called through its own first word at 0x82B486C0; the fallback arms reach RtlEnterCriticalSection / RtlLeaveCriticalSection instead. It also fails gate 3 -- sub_82B1F7E8 is `mftb`)
// calls/session: boot 18685  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B48530.md
namespace port_82B48530 {

// rw_system (docs/rw_audio_structs.h). Every offset this function touches is documented there
// except +0xEC; the name for that one is this port's reading, not the doc's.
constexpr uint32_t kList14 = 20;           // +0x14  head of the list the fade pass walks
constexpr uint32_t kCmdBuffer = 48;        // +0x30  command ring base
constexpr uint32_t kLockFn = 84;           // +0x54  optional lock hook, else RtlEnter
constexpr uint32_t kUnlockFn = 88;         // +0x58  optional unlock hook, else RtlLeave
constexpr uint32_t kCriticalSection = 96;  // +0x60  RTL_CRITICAL_SECTION*
constexpr uint32_t kScheduler = 112;       // +0x70  rw_scheduler, passed by address
constexpr uint32_t kCmdWriteOff = 204;     // +0xCC  bytes used; reset by this drain
constexpr uint32_t kCmdHighWater = 208;    // +0xD0  peak ring usage
constexpr uint32_t kTimeCommands = 232;    // +0xE8  command-phase elapsed
constexpr uint32_t kTimeScheduler = 236;   // +0xEC  the two scheduler-tick phases, SUMMED.
                                           //        docs/rw_audio_structs.h has this inside
                                           //        _padec; the name is read off this one site.
constexpr uint32_t kTimePhases = 244;      // +0xF4  accumulated earlier phases
constexpr uint32_t kDrainCount = 256;      // +0x100 incremented once per drain

// The scheduler's bucket stride, applied from the scheduler base rather than from buckets[0] at
// +0x10 -- sub_82B39820 evidently takes the enclosing 0x20-byte block, not the head pointer.
constexpr uint32_t kBucketStride = 32;

// A node in the +0x14 list, and the record it is embedded in. The node is 28 bytes into the
// record, which is how the fade fields below are addressed.
constexpr uint32_t kNodeNext = 0;     // lwz r30,0(r30)
constexpr uint32_t kRecordBack = 28;  // addi r9,r30,-28

// The record's three floats. Named for the comparison each takes part in; nothing documents them.
constexpr uint32_t kFadeFloor = 40;    // +0x28  the value at +0x30 is clamped UP to this
constexpr uint32_t kFadeTrigger = 44;  // +0x2C  once it is >= the value, sub_82B49100 fires
constexpr uint32_t kFadeValue = 48;    // +0x30  the only float written

// rw_command: the handler is the record's first word and returns the record's own size.
constexpr uint32_t kCommandHandler = 0;

// The two kernel imports the fallback arms reach, by the guest address
// generated/skate3_init.cpp registers for each thunk -- probe/ports/lint.py check 6 forbids
// naming an import thunk, and a kernel import has no guest port to route a GuestCall through.
// Unlike sub_82B4FAF8.inc these do not write ctr first: a `bl` does not write ctr, so the bare
// form is the more faithful of the two spellings. The bctrl arms DO write ctr, as the original.
constexpr uint32_t kRtlEnterCriticalSection = 0x82F9CB44;
constexpr uint32_t kRtlLeaveCriticalSection = 0x82F9CB54;

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)

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

// The eight bracket sites are the same five instructions each, differing only in the return
// address the two arms leave in lr -- the `bl` lands on the instruction after it and then
// branches to the join, while the `bctrl` returns straight to the join, so the two are different
// addresses and both are passed in. On the hook arm r3 is NOT set: the hook is entered with
// whatever the previous call left there (the System at the first site, a timebase reading at
// every later one), so it takes no argument.
void Lock(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t after_import,
          uint32_t after_hook) {
  const uint32_t hook = REX_LOAD_U32(system + kLockFn);  // lwz r11,84(r31)
  // cmplwi cr6,r11,0 ; bne cr6,<the bctrl>
  if (hook != 0) {
    ctx.ctr.u64 = hook;  // mtctr r11
    ctx.lr = after_hook;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bctrl
    return;
  }
  ctx.r3.u64 = REX_LOAD_U32(system + kCriticalSection);  // lwz r3,96(r31)
  ctx.lr = after_import;
  REX_CALL_INDIRECT_FUNC(kRtlEnterCriticalSection);  // bl 0x82f9cb44
}

void Unlock(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t after_import,
            uint32_t after_hook) {
  const uint32_t hook = REX_LOAD_U32(system + kUnlockFn);  // lwz r11,88(r31)
  if (hook != 0) {
    ctx.ctr.u64 = hook;  // mtctr r11
    ctx.lr = after_hook;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bctrl
    return;
  }
  ctx.r3.u64 = REX_LOAD_U32(system + kCriticalSection);  // lwz r3,96(r31)
  ctx.lr = after_import;
  REX_CALL_INDIRECT_FUNC(kRtlLeaveCriticalSection);  // bl 0x82f9cb54
}

REX_FUNC(Native) {
  // __savegprlr_26 spills r26-r31 and the link register into slots inside the 144 bytes the stwu
  // claims; the harness compares r13-r31, so the entry values have to survive, but the slots are
  // this call's own frame and are neither windowed nor reproduced.
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  // The frame is reproduced because every callee builds its own below r1.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-144(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t system = ctx.r3.u32;  // mr r31,r3

  // ---- phase 1: scheduler bucket 0 ----------------------------------------------------------
  Lock(ctx, base, system, 0x82B48554, 0x82B48560);
  // loc_82B48560
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);  // bl 0x82b1f7e8 -- mftb, nothing else
  const uint64_t phase1_start = ctx.r3.u64;           // mr r30,r3
  // addi r29,r31,112 -- 64-bit, and the scheduler is only ever passed on, never addressed here.
  const uint64_t scheduler = uint64_t(system) + kScheduler;
  skate3::audio::GuestCall(ctx, base, sub_82B48A50, scheduler, 0);  // li r4,0 ; bl 0x82b48a50
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  // lwz r11,88(r31) is issued here, before the subf; Unlock() loads it again. Nothing stores
  // between the two, so the order is not observable.
  const uint64_t phase1_elapsed = ctx.r3.u64 - phase1_start;  // subf r26,r30,r3 -- held to the end
  Unlock(ctx, base, system, 0x82B48594, 0x82B485A0);

  // ---- phase 2: the fade pass over the +0x14 list, OUTSIDE the lock -------------------------
  // loc_82B485A0
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t phase2_start = ctx.r3.u64;              // mr r28,r3
  uint32_t node = REX_LOAD_U32(system + kList14);        // lwz r30,20(r31)
  // cmplwi cr6,r30,0 ; beq cr6,0x82b485fc, and the bottom test at loc_82B485F4 is the same
  // compare against the already-advanced pointer, so this is a plain while loop.
  while (node != 0) {
    // loc_82B485B4 -- subfic r11,r30,0 ; subfe r8,r10,r10 ; and r3,r8,r9 is the null-safe
    // downcast idiom: `0 - node` carries out only when node is zero, and subfe turns that carry
    // into a mask (0 when node was null, all ones otherwise) which is ANDed with node-28. We are
    // only ever here with a non-zero node, so the mask is always all ones -- but it is kept
    // because the `and` is 64-bit and `addi r9,r30,-28` can go negative for a node under 28.
    const uint64_t mask = ~uint64_t(0) + uint64_t(node == 0);
    const uint64_t back = uint64_t(int64_t(node) - kRecordBack);
    // lwz r30,0(r30) -- the NEXT pointer is loaded here, before the float work and before
    // sub_82B49100 can run, so a record that unlinks itself does not break the walk.
    const uint32_t next = REX_LOAD_U32(node + kNodeNext);
    const uint64_t record64 = mask & back;
    const uint32_t record = uint32_t(record64);

    ctx.fpscr.disableFlushModeUnconditional();
    const double floor_value = LoadSingle(base, record + kFadeFloor);  // lfs f0,40(r3)
    const double current = LoadSingle(base, record + kFadeValue);      // lfs f13,48(r3)
    // fcmpu cr6,f13,f0 ; bge cr6,0x82b485dc -- a NaN on either side leaves cr6.lt clear, so no
    // store happens; `current < floor_value` is false for a NaN too.
    if (current < floor_value) {
      StoreSingle(base, record + kFadeValue, floor_value);  // stfs f0,48(r3)
    }

    // loc_82B485DC
    ctx.fpscr.disableFlushModeUnconditional();
    const double trigger = LoadSingle(base, record + kFadeTrigger);  // lfs f0,44(r3)
    // lfs f13,48(r3) -- a genuine RELOAD: the store above may have just changed it.
    const double reloaded = LoadSingle(base, record + kFadeValue);
    // fcmpu cr6,f0,f13 ; blt cr6,0x82b485f4 -- only a strictly-less trigger skips the call, so a
    // NaN on either side (lt clear) DOES call. `!(trigger < reloaded)` reproduces both.
    if (!(trigger < reloaded)) {
      skate3::audio::GuestCall(ctx, base, sub_82B49100, record64, 3);  // li r4,3 ; bl 0x82b49100
    }
    // loc_82B485F4
    node = next;
  }

  // loc_82B485FC -- subf r11,r28,r3 ; stw r11,244(r31): the phase time is STORED, not accumulated.
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  REX_STORE_U32(system + kTimePhases, uint32_t(ctx.r3.u64 - phase2_start));

  // ---- phase 3: the two other list walks, under the lock ------------------------------------
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t phase3_start = ctx.r3.u64;  // mr r30,r3
  Lock(ctx, base, system, 0x82B48624, 0x82B48630);
  // loc_82B48630
  skate3::audio::GuestCall(ctx, base, sub_82B482F8, system);  // mr r3,r31 ; bl 0x82b482f8
  skate3::audio::GuestCall(ctx, base, sub_82B48440, system);  // mr r3,r31 ; bl 0x82b48440
  Unlock(ctx, base, system, 0x82B48654, 0x82B48660);

  // loc_82B48660 -- lwz r10,244(r31) ; subf r9,r30,r10 ; add r8,r3,r9 ; stw r8,244(r31): this
  // one ACCUMULATES onto what phase 2 stored. All three sums stay 64-bit and truncate at the
  // store. lwz r11,84(r31) is issued before the store; Lock() reloads it, and +0x54 cannot
  // alias +0xF4.
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t phases = REX_LOAD_U32(system + kTimePhases);
  REX_STORE_U32(system + kTimePhases, uint32_t(ctx.r3.u64 + (phases - phase3_start)));
  Lock(ctx, base, system, 0x82B48688, 0x82B48694);

  // ---- phase 4: drain the command ring, under the lock --------------------------------------
  // loc_82B48694
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t phase4_start = ctx.r3.u64;                     // mr r27,r3
  uint64_t cursor = REX_LOAD_U32(system + kCmdBuffer);          // lwz r30,48(r31)
  const uint64_t used = REX_LOAD_U32(system + kCmdWriteOff);    // lwz r11,204(r31)
  const uint64_t end = used + cursor;                           // add r28,r11,r30
  // cmplw cr6,r30,r28 ; bge cr6,0x82b486cc -- UNSIGNED on the low words, and the bottom test at
  // 0x82B486C8 is the same compare, so a plain while loop. The bound is NOT re-read: a handler
  // that appends to the ring is not seen by this drain.
  while (uint32_t(cursor) < uint32_t(end)) {
    // loc_82B486B0 -- the record's own first word is its handler, and it returns the record's
    // size, which is how a variable-length ring is walked.
    ctx.ctr.u64 = REX_LOAD_U32(uint32_t(cursor) + kCommandHandler);  // lwz r11,0(r30) ; mtctr r11
    ctx.r3.u64 = cursor;                                             // mr r3,r30
    ctx.lr = 0x82B486C0;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bctrl
    cursor += ctx.r3.u64;                 // add r30,r3,r30
  }

  // loc_82B486CC -- the high-water mark, UNSIGNED, using the write offset RE-READ after the
  // handlers ran rather than the `used` above.
  const uint32_t write_off = REX_LOAD_U32(system + kCmdWriteOff);  // lwz r11,204(r31)
  // lwz r10,208(r31) ; cmplw cr6,r11,r10 ; ble cr6,0x82b486e0
  if (write_off > REX_LOAD_U32(system + kCmdHighWater)) {
    REX_STORE_U32(system + kCmdHighWater, write_off);  // stw r11,208(r31)
  }

  // loc_82B486E0 -- the ring is frame-scoped: the write offset is reset to zero outright.
  const uint64_t drains = REX_LOAD_U32(system + kDrainCount);  // lwz r11,256(r31)
  REX_STORE_U32(system + kCmdWriteOff, 0);                    // stw r10,204(r31)
  REX_STORE_U32(system + kDrainCount, uint32_t(drains + 1));  // addi r9,r11,1 ; stw r9,256(r31)
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  // subf r8,r27,r3 ; stw r8,232(r31) -- issued before the unlock's lwz r11,88(r31) in the
  // original, which Unlock() reloads; +0x58 cannot alias +0xE8.
  REX_STORE_U32(system + kTimeCommands, uint32_t(ctx.r3.u64 - phase4_start));
  Unlock(ctx, base, system, 0x82B48714, 0x82B48720);

  // ---- phase 5: scheduler bucket 1 and the two sub_82B39820 passes --------------------------
  // loc_82B48720 -- a bare unlock/lock pair with nothing between them.
  Lock(ctx, base, system, 0x82B48734, 0x82B48740);
  // loc_82B48740
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t phase5_start = ctx.r3.u64;                        // mr r28,r3
  skate3::audio::GuestCall(ctx, base, sub_82B48A50, scheduler, 1);  // li r4,1 ; bl 0x82b48a50
  // li r30,2 ; loc_82B48758 ... addic. r30,r30,-1 ; bne -- exactly two passes, the argument
  // advancing by one bucket stride between them.
  uint64_t bucket = scheduler;  // mr r3,r29
  for (uint32_t left = 2; left != 0; left--) {
    skate3::audio::GuestCall(ctx, base, sub_82B39820, bucket);  // bl 0x82b39820
    bucket += kBucketStride;                                    // addi r29,r29,32
  }
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  // subf r10,r28,r26 ; add r9,r3,r10 ; stw r9,236(r31) -- this phase's elapsed PLUS phase 1's,
  // which is what r26 has been holding since the top. Both scheduler ticks, summed.
  REX_STORE_U32(system + kTimeScheduler, uint32_t(ctx.r3.u64 + (phase1_elapsed - phase5_start)));
  Unlock(ctx, base, system, 0x82B48790, 0x82B487A0);

  // Both exits share this epilogue; no `li r3,...` on either path, so r3 is whatever the unlock
  // left behind.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
  ctx.lr = entry_lr;                      // __restgprlr_26 restores lr and r26-r31
  ctx.r12.u64 = entry_lr;
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B48530
SKATE3_PORT(82B48530, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
