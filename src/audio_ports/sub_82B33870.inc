// sub_82B33870  role: advance one slot of the 80-byte array at +96 -- when the slot's state byte at +73 is 0, or the counter at +24 of the 48-byte parallel record is still below the slot's limit at +16, copy the live handle from +48 to +52, fetch the next source through sub_82B32550, record its low byte at +74 and install whatever sub_82B33418 returns at +48; otherwise defer to sub_82B332D0 and return its boolean   lifted: skate3_recomp.67.cpp:52848 (147 lines)
// STATUS: gate-1 (sub_82B33418 reaches an indirect call two levels down: sub_82B3C870 `bctrl`, census depth 3 -- the boot session also never called it, but the gate is the label that matters: kPortUncalled is shadowable and this must not be shadowed)
// calls/session: boot 0  play ?  map ?          thread: RwAudioCore Dac (static attribution only)
// note: probe/ports/notes/sub_82B33870.md
namespace port_82B33870 {

// Nothing in docs/rw_audio_structs.h names either record, so the offsets stay plain constants
// named for what this function does with them.
//
// The owner object (r3).
constexpr uint32_t kSlotArray = 96;      // lwz r10,96(r3) -- base of the 80-byte slot array
constexpr uint32_t kParallelBias = 464;  // lhz r11,464(r3) -- byte bias of the 48-byte array,
                                         //                    measured from the object itself

// A slot (r31 = *(owner+96) + index*80).
constexpr uint32_t kLimit = 16;        // s32 compared against the parallel counter
constexpr uint32_t kHandle = 48;       // u32 handed to sub_82B33418 and overwritten with its result
constexpr uint32_t kPrevHandle = 52;   // u32 -- the handle at +48 before this call
constexpr uint32_t kState = 73;        // u8: 0 = advance, 1 = defer, anything else = test the counter
constexpr uint32_t kSourceByte = 74;   // u8 low byte of what sub_82B32550 leaves in the local

// A parallel record (r7 = owner + *(owner+464) + index*48).
constexpr uint32_t kCounter = 24;      // s32 compared against the slot's limit

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
constexpr uint32_t kLocalSlot = 80;    // the zeroed word at r1+80, passed BY ADDRESS to sub_82B32550

// rlwinm r,N,0,31-N then add: both halves are zero-extended 32-bit quantities, but the add is
// 64 bit, so a carry out of bit 31 survives into the register the loads address through. Kept in
// 64 bits and narrowed only where the lifted form uses `.u32`.
uint64_t ScaleByFive(uint64_t index) {
  const uint64_t times_four = uint64_t((uint32_t(index) << 2) & 0xFFFFFFFCu);  // rlwinm r9,r4,2,0,29
  return index + times_four;                                                   // add r8,r4,r9
}
uint64_t ScaleByThree(uint64_t index) {
  const uint64_t times_two = uint64_t((uint32_t(index) << 1) & 0xFFFFFFFEu);  // rlwinm r11,r29,1,0,30
  return index + times_two;                                                   // add r8,r29,r11
}
uint64_t TimesSixteen(uint64_t value) {
  return uint64_t((uint32_t(value) << 4) & 0xFFFFFFF0u);  // rlwinm r11,r8,4,0,27
}

REX_FUNC(Native) {
  // __savegprlr_29 spills r29-r31 and the link register into this call's own frame; the harness
  // compares r13-r31, so the entry values have to survive here too. The spill slots are inside
  // the 128 bytes the stwu claims, so they are not windowed and are not reproduced; r29-r31 are
  // modelled as C++ locals and put back where __restgprlr_29 puts them back.
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  ctx.r12.u64 = ctx.lr;  // mflr r12 -- the value __restgprlr_29 returns to lr on the way out

  // The frame IS reproduced: the word at r1+80 is passed by address to sub_82B32550, so it has to
  // land inside the 128 bytes this call allocates rather than in the caller's frame.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t owner_reg = ctx.r3.u64;  // mr r30,r3
  const uint64_t index_reg = ctx.r4.u64;  // mr r29,r4
  const uint64_t extra_reg = ctx.r5.u64;  // r5 is never written before the deferral calls
  const uint32_t owner = uint32_t(owner_reg);

  const uint32_t bias = REX_LOAD_U16(owner + kParallelBias);   // lhz r11,464(r3)
  const uint32_t array = REX_LOAD_U32(owner + kSlotArray);     // lwz r10,96(r3)
  const uint64_t parallel_base = uint64_t(bias) + owner_reg;   // add r9,r11,r3
  // add r31,r11,r10
  const uint32_t slot = uint32_t(TimesSixteen(ScaleByFive(index_reg)) + uint64_t(array));

  const uint32_t state = REX_LOAD_U8(slot + kState);  // lbz r11,73(r31)
  bool advance;
  if (state == 0) {
    advance = true;  // cmplwi cr6,r11,0 ; beq cr6,0x82b338f8
  } else if (state == 1) {
    advance = false;  // cmplwi cr6,r11,1 ; bne cr6,0x82b338d8 -- fall through to the deferral
  } else {
    // loc_82B338D8
    const uint32_t limit = REX_LOAD_U32(slot + kLimit);  // lwz r10,16(r31)
    const uint32_t record =
        uint32_t(TimesSixteen(ScaleByThree(index_reg)) + parallel_base);  // add r7,r11,r9
    const uint32_t counter = REX_LOAD_U32(record + kCounter);             // lwz r6,24(r7)
    // cmpw cr6,r6,r10 ; bge cr6,0x82b33944 -- SIGNED, and only strictly-below advances.
    advance = (int32_t(counter) < int32_t(limit));
  }

  if (!advance) {
    // The state==1 fallthrough and loc_82B33944 set up the identical call: r3 and r5 untouched,
    // r4 the index (loc_82B33944's `mr r4,r29` is a no-op, nothing has clobbered r4), r6=1, r7=0.
    // bl 0x82b332d0
    skate3::audio::GuestCall(ctx, base, sub_82B332D0, owner_reg, index_reg, extra_reg,
                             uint64_t(1), uint64_t(0));
    const uint32_t ok = ctx.r3.u32 & 0xFF;  // clrlwi r11,r3,24 ; cmplwi cr6,r11,0
    ctx.r3.s64 = (ok != 0) ? 1 : 0;         // li r3,1 at loc_82B33964, li r3,0 otherwise
  } else {
    // loc_82B338F8
    const uint32_t handle = REX_LOAD_U32(slot + kHandle);  // lwz r11,48(r31)
    REX_STORE_U32(frame + kLocalSlot, 0);                  // stw r10,80(r1)
    REX_STORE_U32(slot + kPrevHandle, handle);             // stw r11,52(r31)
    // bl 0x82b32550 -- (owner, &local). r3 is still the entry value; only r4 is set. The callee
    // fills the word at r1+80 and ALSO leaves a value in r5, which the caller reads below.
    skate3::audio::GuestCall(ctx, base, sub_82B32550, owner_reg, uint64_t(frame + kLocalSlot));
    const uint32_t produced = REX_LOAD_U32(frame + kLocalSlot);  // lwz r9,80(r1)
    const uint64_t extra = ctx.r5.u64;                           // mr r6,r5 -- an out-register
    REX_STORE_U8(slot + kSourceByte, uint8_t(produced));         // stb r9,74(r31)
    // The handle is RELOADED after the stb rather than carried from the load above.
    const uint32_t current = REX_LOAD_U32(slot + kHandle);  // lwz r4,48(r31)
    // bl 0x82b33418 -- (owner, current handle, index, extra, 1, 0). This is the callee whose
    // closure reaches sub_82B3C870's bctrl, which is what fails gate 1.
    skate3::audio::GuestCall(ctx, base, sub_82B33418, owner_reg, uint64_t(current), index_reg,
                             extra, uint64_t(1), uint64_t(0));
    REX_STORE_U32(slot + kHandle, ctx.r3.u32);  // stw r3,48(r31)
    ctx.r3.s64 = 1;                             // li r3,1
  }

  // loc_82B33968
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,128
  ctx.lr = ctx.r12.u64;                   // __restgprlr_29 restores lr and r29-r31
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

}  // namespace port_82B33870
SKATE3_PORT(82B33870, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
