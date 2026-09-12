// sub_82B332D0  role: refill one slot of the 80-byte array at +96 -- first drop out when the 48-byte parallel record's state byte at +46 reads 1 and the slot's handle at +44 has gone stale in the 328-byte handle table (clearing the record's +20 and returning 0), otherwise pull the next chunk from sub_82B33C08, add its size to the slot's outstanding-byte count at +24, claim a 16-byte ring slot through sub_82B32550, record the claimed index at the slot's +74 and the chunk's third out-word at ring_slot+100, and hand the chunk to sub_82B33418   lifted: skate3_recomp.67.cpp:52060 (179 lines)
// STATUS: gate-1 (two independent failures on the refill path: sub_82B33C08 takes the critical section -- RtlEnterCriticalSection/RtlLeaveCriticalSection through sub_82EC1290 -- and sub_82B33418 reaches sub_82B3C870's indirect call; census depth 2 either way)
// calls/session: boot 1071  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B332D0.md
namespace port_82B332D0 {

// The same two parallel arrays sub_82B33870 walks, with the same constants; nothing in
// docs/rw_audio_structs.h names either, so the offsets stay plain and are named for use.
//
// The owner object (r3).
constexpr uint32_t kSlotArray = 96;       // lwz r10,96(r3) -- base of the 80-byte slot array
constexpr uint32_t kParallelBias = 464;   // lhz r9,464(r3) -- byte bias of the 48-byte array,
                                          //                   measured from the object itself
constexpr uint32_t kRingStride = 16;      // sub_82B32550's ring: 16 bytes per entry, base = object
constexpr uint32_t kRingPayload = 100;    // u32 written into the claimed ring entry

// A slot (r29), stride 80.
constexpr uint32_t kSlotOutstanding = 24; // u32 bytes owed: the chunk size is ADDED here, and
                                          //     sub_82B32480 SUBTRACTS the buffer's byte count
                                          //     from the same field when the entry retires
constexpr uint32_t kSlotContext = 40;   // u32 -> the object that owns the handle table
constexpr uint32_t kSlotHandle = 44;    // s32 handle; its low byte indexes the table
constexpr uint32_t kSlotRingIndex = 74; // u8 the claimed ring index (kSourceByte in sub_82B33870)

// A parallel record (r9), stride 48.
constexpr uint32_t kRecordCleared = 20;  // u32 zeroed when the handle is found stale
constexpr uint32_t kRecordState = 46;    // u8; exactly 1 arms the staleness check

// The handle table, reached as u32[u32[slot+40] + 8].
constexpr uint32_t kTableOwner = 8;     // context+8 -> the table object
constexpr uint32_t kTableEntries = 40;  // table+40 -> the entry array
constexpr uint32_t kTableCount = 44;    // table+44 -> s32 entry count
constexpr uint32_t kEntryStride = 328;  // mulli r11,r11,328
constexpr uint32_t kEntryHandle = 0;    // entry+0 must equal the handle to be live
constexpr uint32_t kEntryLive = 4;      // entry+4 nonzero means the resource is still there

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)
constexpr uint32_t kClaimed = 80;      // the zeroed word at r1+80, passed BY ADDRESS to sub_82B32550
constexpr uint32_t kOutChunk = 84;     // r1+84, +88, +92: sub_82B33C08's three out-params
constexpr uint32_t kOutSize = 88;
constexpr uint32_t kOutPayload = 92;

// rlwinm r,N,0,31-N then add: both halves are zero-extended 32-bit quantities but the add is
// 64 bit, so a carry out of bit 31 survives into the register the loads address through. Kept in
// 64 bits and narrowed only where the lifted form uses `.u32`.
inline uint64_t ScaleByThree(uint64_t index) {
  const uint64_t times_two = uint64_t((uint32_t(index) << 1) & 0xFFFFFFFEu);  // rlwinm r8,r4,1,0,30
  return index + times_two;                                                   // add r8,r4,r8
}
inline uint64_t ScaleByFive(uint64_t index) {
  const uint64_t times_four = uint64_t((uint32_t(index) << 2) & 0xFFFFFFFCu);  // rlwinm r11,r4,2,0,29
  return index + times_four;                                                   // add r6,r4,r11
}
inline uint64_t TimesSixteen(uint64_t value) {
  return uint64_t((uint32_t(value) << 4) & 0xFFFFFFF0u);  // rlwinm r8,r8,4,0,27
}

REX_FUNC(Native) {
  // __savegprlr_25 spills r25-r31 and the link register; the harness compares r13-r31, so the
  // entry values have to survive here too. The spill slots are this call's own and not windowed.
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  ctx.r12.u64 = ctx.lr;  // mflr r12 -- the value __restgprlr_25 puts back in lr on the way out

  // The frame IS reproduced: four words in it (+80, +84, +88, +92) are passed by address to the
  // callees, so they have to land inside the 160 bytes this call allocates.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-160(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t owner_reg = ctx.r3.u64;  // mr r31,r3
  const uint64_t index_reg = ctx.r4.u64;  // mr r30,r4
  const uint64_t arg_r5 = ctx.r5.u64;     // mr r27,r5  -- passed straight through to sub_82B33418
  const uint64_t arg_r6 = ctx.r6.u64;     // mr r26,r6
  const uint64_t arg_r7 = ctx.r7.u64;     // mr r25,r7
  const uint32_t owner = uint32_t(owner_reg);

  const uint32_t bias = REX_LOAD_U16(owner + kParallelBias);  // lhz r9,464(r3)
  const uint32_t array = REX_LOAD_U32(owner + kSlotArray);    // lwz r10,96(r3)
  // add r9,r9,r8 ; add r9,r9,r3 -- the 48-byte record, biased from the object itself.
  const uint64_t record_reg = uint64_t(bias) + TimesSixteen(ScaleByThree(index_reg)) + owner_reg;
  const uint32_t record = uint32_t(record_reg);
  // add r29,r11,r10 -- the 80-byte slot, off the pointer at +96.
  const uint64_t slot_reg = TimesSixteen(ScaleByFive(index_reg)) + uint64_t(array);
  const uint32_t slot = uint32_t(slot_reg);

  REX_STORE_U32(frame + kClaimed, 0);  // stw r7,80(r1) -- li r7,0 first; r7 stays 0 for the +20 store

  // lbz r5,46(r9) ; cmplwi cr6,r5,1 ; bne cr6,0x82b33388 -- exactly 1, not nonzero.
  bool refill = REX_LOAD_U8(record + kRecordState) != 1;
  if (!refill) {
    // The staleness check. `handle` is compared SIGNED against 0, and its low byte is the index.
    const uint32_t handle = REX_LOAD_U32(slot + kSlotHandle);  // lwz r8,44(r29)
    if (static_cast<int32_t>(handle) == 0) {
      refill = true;  // beq cr6,0x82b33388 -- no handle at all: nothing to invalidate
    } else {
      const uint32_t entry_index = handle & 0xFF;                   // clrlwi r11,r8,24
      const uint32_t context = REX_LOAD_U32(slot + kSlotContext);   // lwz r10,40(r29)
      const uint32_t table = REX_LOAD_U32(context + kTableOwner);   // lwz r10,8(r10)
      const uint32_t count = REX_LOAD_U32(table + kTableCount);     // lwz r6,44(r10)
      // cmpw cr6,r11,r6 ; bge cr6,0x82b33378 -- SIGNED; out of range means stale.
      bool stale = static_cast<int32_t>(entry_index) >= static_cast<int32_t>(count);
      if (!stale) {
        const uint32_t entries = REX_LOAD_U32(table + kTableEntries);  // lwz r10,40(r10)
        // mulli r11,r11,328 ; add r11,r11,r10 -- 64-bit add of two zero-extended halves.
        const uint32_t entry =
            uint32_t(uint64_t(entry_index) * uint64_t(kEntryStride) + uint64_t(entries));
        // cmpw cr6,r8,r10 ; bne cr6,0x82b33378 -- a reused table slot carries a different handle.
        if (static_cast<int32_t>(handle) !=
            static_cast<int32_t>(REX_LOAD_U32(entry + kEntryHandle))) {
          stale = true;
        } else {
          // lwz r11,4(r11) ; cmpwi cr6,r11,0 ; bne cr6,0x82b33388 -- still live, so refill.
          refill = static_cast<int32_t>(REX_LOAD_U32(entry + kEntryLive)) != 0;
          stale = !refill;
        }
      }
      if (stale) {
        // loc_82B33378: stw r7,20(r9) -- r7 is still the 0 stored at +80 above.
        REX_STORE_U32(record + kRecordCleared, 0);
      }
    }
  }

  if (!refill) {
    // loc_82B3337C
    ctx.r3.s64 = 0;  // li r3,0
    ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
    ctx.lr = ctx.r12.u64;                   // __restgprlr_25 restores lr and r25-r31
    ctx.r25 = saved_r25;
    ctx.r26 = saved_r26;
    ctx.r27 = saved_r27;
    ctx.r28 = saved_r28;
    ctx.r29 = saved_r29;
    ctx.r30 = saved_r30;
    ctx.r31 = saved_r31;
    return;
  }

  // loc_82B33388 -- bl 0x82b33c08(owner, index, &out84, &out88, &out92). This is the callee that
  // takes the critical section, which is one of the two gate-1 failures.
  skate3::audio::GuestCall(ctx, base, sub_82B33C08, owner_reg, index_reg,
                           uint64_t(frame + kOutChunk), uint64_t(frame + kOutSize),
                           uint64_t(frame + kOutPayload));
  const uint32_t chunk = REX_LOAD_U32(frame + kOutChunk);  // lwz r28,84(r1)
  // cmplwi cr6,r28,0 ; beq cr6,0x82b3337c -- UNSIGNED; nothing produced means return 0.
  bool produced = chunk != 0;
  if (produced) {
    // bl 0x82b32550(owner, &claimed) -- claims a 16-byte ring entry, returns 1 or 0.
    skate3::audio::GuestCall(ctx, base, sub_82B32550, owner_reg, uint64_t(frame + kClaimed));
    const uint32_t claim_ok = ctx.r3.u32 & 0xFF;  // clrlwi r9,r3,24

    // The running total is updated BEFORE the claim is tested, so a refused claim still advances
    // it. Reproduced, not fixed: `add r8,r11,r10` is a 64-bit add of two zero-extended words and
    // the store narrows it, so a wrap past 2^32 is lost exactly as on the machine.
    const uint64_t total = REX_LOAD_U32(slot + kSlotOutstanding);          // lwz r10,24(r29)
    const uint64_t added = REX_LOAD_U32(frame + kOutSize);           // lwz r11,88(r1)
    REX_STORE_U32(slot + kSlotOutstanding, uint32_t(added + total));       // stw r8,24(r29)

    if (claim_ok == 0) {
      produced = false;  // beq cr6,0x82b3337c
    } else {
      const uint32_t claimed = REX_LOAD_U32(frame + kClaimed);   // lwz r11,80(r1)
      const uint32_t payload = REX_LOAD_U32(frame + kOutPayload);  // lwz r9,92(r1)
      // rlwinm r10,r11,4,0,27 ; add r11,r10,r31 -- the ring entry inside the object itself.
      const uint32_t ring = uint32_t(TimesSixteen(claimed) + owner_reg);
      REX_STORE_U8(slot + kSlotRingIndex, uint8_t(claimed));  // stb r6,74(r29)
      REX_STORE_U32(ring + kRingPayload, payload);            // stw r9,100(r11)
      // bl 0x82b33418(owner, chunk, index, arg_r5, arg_r6, arg_r7) -- the second gate-1 failure:
      // its closure reaches sub_82B3C870's bctrl.
      skate3::audio::GuestCall(ctx, base, sub_82B33418, owner_reg, uint64_t(chunk), index_reg,
                               arg_r5, arg_r6, arg_r7);
      ctx.r3.s64 = 1;  // li r3,1
    }
  }
  if (!produced) {
    ctx.r3.s64 = 0;  // loc_82B3337C: li r3,0
  }

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
  ctx.lr = ctx.r12.u64;                   // __restgprlr_25 restores lr and r25-r31
  ctx.r25 = saved_r25;
  ctx.r26 = saved_r26;
  ctx.r27 = saved_r27;
  ctx.r28 = saved_r28;
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare, including the one path
// that is genuinely replayable, is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B332D0
SKATE3_PORT(82B332D0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
