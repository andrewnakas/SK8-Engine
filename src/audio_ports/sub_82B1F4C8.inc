// sub_82B1F4C8  role: open a device for this object -- when the global singleton at 0x82FD35F8 exists, build two argument blocks in the frame (the record count and the array pointer at +80, and six descriptor bytes each shifted up a byte at +96) and call the singleton's vtable slot 0 with a table entry selected by the descriptor's signed halfword; on a nonzero handle push every one of the object's 12-byte state records to it through sub_82B1BE30, copying +8 over +4, and return the handle; otherwise re-initialise the object via sub_82B1F440 and return 0   lifted: skate3_recomp.67.cpp:3589 (170 lines)
// STATUS: gate-1 (its own `bctrl` at 0x82B1F58C, the singleton vtable's slot-0 entry -- a device open -- and sub_82B1BE30 reaches six more: census depth 0)
// calls/session: boot 514  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1F4C8.md
namespace port_82B1F4C8 {

// lis r10,-32003 -> 0x82FD0000 ; lwz r3,13816(r10). Computed as ((imm & 0xFFFF) << 16) + offset.
constexpr uint32_t kSingletonSlot = 0x82FD35F8;

// The object (r3/r30) -- the same layout sub_82B1F5F8 walks.
constexpr uint32_t kConfig = 0;         // lwz r5,0(r30) -- holds the two pointers and the table
constexpr uint32_t kRecordCount = 14;   // u8, RE-READ at the bottom of every iteration
constexpr uint32_t kRecords = 28;       // addi r31,r3,28 -- the first 12-byte record
constexpr uint32_t kRecordStride = 12;  // stwu r10,12(r31)

// A 12-byte state record, as addressed through the -8-biased cursor this function uses.
constexpr uint32_t kRecordId = 8;       // lbz r4,8(r31)   -- record +0
constexpr uint32_t kRecordApplied = 12; // stwu r10,12(r31) -- record +4
constexpr uint32_t kRecordWanted = 16;  // lwz r5,16(r31)  -- record +8
constexpr uint32_t kCursorBias = 8;     // addi r31,r31,-8

// The config object at object+0.
constexpr uint32_t kTableBase = 64;  // lwz r10,64(r5) -- base of a u32 table AND the value the
                                     //                   selected entry is added to
constexpr uint32_t kConfigArg7 = 72;  // lwz r7,72(r5)
constexpr uint32_t kConfigArg8 = 76;  // lwz r8,76(r5)

// The descriptor (r4/r11).
constexpr uint32_t kDescIndex = 0;   // lhz then extsh: a SIGNED halfword
constexpr uint32_t kDescByte2 = 2;   // lbz r5,2(r11) -- passed raw, not shifted
constexpr uint32_t kDescByte3 = 3;
constexpr uint32_t kDescByte4 = 4;
constexpr uint32_t kDescByte5 = 5;
constexpr uint32_t kDescByte6 = 6;
constexpr uint32_t kDescByte7 = 7;
constexpr uint32_t kDescByte8 = 8;   // read BOTH as a byte (the shifted block) and as a word
constexpr int64_t kIndexBias = 3;    // addi r7,r10,3

// The singleton's vtable.
constexpr uint32_t kVptr = 0;      // lwz r24,0(r3)
constexpr uint32_t kOpenSlot = 0;  // lwz r29,0(r24) ; mtctr r29 ; bctrl

constexpr uint32_t kFrameBytes = 208;  // stwu r1,-208(r1)
// The two blocks the callee receives by address. r1+88 and r1+92 sit between them and are never
// written, so whatever the frame held is what the callee would see there.
constexpr uint32_t kCountSlot = 80;  // r9 = r1+80 -> {record count, record array pointer}
constexpr uint32_t kArraySlot = 84;
constexpr uint32_t kByteBlock = 96;  // r6 = r1+96 -> six words, descriptor bytes 3..8

// rotlwi rX,rX,8 on a zero-extended byte: the source is at most 0xFF, so nothing rotates out of
// bit 31 and the result is exactly byte << 8.
uint32_t ByteShifted(uint32_t byte) { return byte << 8; }

REX_FUNC(Native) {
  // __savegprlr_23 spills r23-r31 and the link register into this call's own frame and
  // __restgprlr_23 reloads them; the harness compares r13-r31, so all nine entry values have to
  // survive here. The spill slots are inside the 208 bytes the stwu claims, so they are not
  // windowed and are not reproduced.
  const PPCRegister saved_r23 = ctx.r23;
  const PPCRegister saved_r24 = ctx.r24;
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  // The frame IS reproduced: both argument blocks are passed by address to the vtable call, so
  // they have to land inside the 208 bytes this call allocates rather than in the caller's frame.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-208(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t object_reg = ctx.r3.u64;  // mr r30,r3
  const uint32_t object = uint32_t(object_reg);
  const uint32_t desc = ctx.r4.u32;             // mr r11,r4
  int64_t cursor = ctx.r3.s64 + kRecords;       // addi r31,r3,28
  const uint32_t singleton = REX_LOAD_U32(kSingletonSlot);  // lwz r3,13816(r10)

  uint64_t handle = 0;
  // cmplwi cr6,r3,0 ; beq cr6,0x82b1f59c -- no singleton means no device.
  if (singleton != 0) {
    // The loads below are kept in the original's order; the stores that follow start only after
    // the table entry has been fetched.
    const uint32_t index_half = REX_LOAD_U16(desc + kDescIndex);  // lhz r10,0(r4)
    const uint32_t config = REX_LOAD_U32(object + kConfig);       // lwz r5,0(r30)
    // extsh r10,r10 ; addi r7,r10,3 ; rlwinm r7,r7,2,0,29 -- a SIGNED halfword, so a negative
    // descriptor index reaches below the table. Scaled in the low 32 bits.
    const int64_t index = int64_t(int16_t(uint16_t(index_half))) + kIndexBias;
    const uint64_t scaled = uint64_t((uint32_t(index) << 2) & 0xFFFFFFFCu);
    const uint32_t byte4 = REX_LOAD_U8(desc + kDescByte4);          // lbz r8,4(r11)
    const uint32_t byte3 = REX_LOAD_U8(desc + kDescByte3);          // lbz r4,3(r4)
    const uint32_t byte5 = REX_LOAD_U8(desc + kDescByte5);          // lbz r29,5(r11)
    const uint32_t byte8 = REX_LOAD_U8(desc + kDescByte8);          // lbz r8,8(r11)
    const uint32_t table = REX_LOAD_U32(config + kTableBase);       // lwz r10,64(r5)
    const uint32_t byte6 = REX_LOAD_U8(desc + kDescByte6);          // lbz r28,6(r11)
    const uint32_t byte7 = REX_LOAD_U8(desc + kDescByte7);          // lbz r27,7(r11)
    const uint32_t count = REX_LOAD_U8(object + kRecordCount);      // lbz r25,14(r30)
    const uint32_t vtable = REX_LOAD_U32(singleton + kVptr);        // lwz r24,0(r3)
    // lwzx r8,r7,r10 -- a 32-bit wrapping sum of the scaled index and the table base.
    const uint32_t entry = REX_LOAD_U32(uint32_t(uint32_t(scaled) + table));

    REX_STORE_U32(frame + kByteBlock + 0, ByteShifted(byte3));   // stw r4,96(r1)
    REX_STORE_U32(frame + kByteBlock + 8, ByteShifted(byte5));   // stw r29,104(r1)
    // add r4,r8,r10 -- 64 bit on two zero-extended loads, and the sum is an ARGUMENT, so a carry
    // out of bit 31 reaches the callee. Kept in 64 bits.
    const uint64_t selected = uint64_t(entry) + uint64_t(table);
    REX_STORE_U32(frame + kByteBlock + 4, ByteShifted(byte4));   // stw r26,100(r1)
    REX_STORE_U32(frame + kCountSlot, count);                    // stw r25,80(r1)
    REX_STORE_U32(frame + kByteBlock + 12, ByteShifted(byte6));  // stw r28,108(r1)
    REX_STORE_U32(frame + kByteBlock + 16, ByteShifted(byte7));  // stw r27,112(r1)
    REX_STORE_U32(frame + kByteBlock + 20, ByteShifted(byte8));  // stw r23,116(r1)
    REX_STORE_U32(frame + kArraySlot, uint32_t(cursor));         // stw r31,84(r1)

    const uint32_t arg7 = REX_LOAD_U32(config + kConfigArg7);  // lwz r7,72(r5)
    const uint32_t target = REX_LOAD_U32(vtable + kOpenSlot);  // lwz r29,0(r24)
    // The same offset +8 is read a second time, now as a WORD -- big-endian, so byte8 above is
    // this word's high byte. Both reads are reproduced.
    const uint32_t word8 = REX_LOAD_U32(desc + kDescByte8);        // lwz r10,8(r11)
    const uint32_t arg8_base = REX_LOAD_U32(config + kConfigArg8); // lwz r8,76(r5)
    const uint64_t arg8 = uint64_t(arg8_base) + uint64_t(word8);   // add r8,r8,r10 -- 64 bit
    const uint32_t byte2 = REX_LOAD_U8(desc + kDescByte2);         // lbz r5,2(r11)

    ctx.r3.u64 = singleton;
    ctx.r4.u64 = selected;
    ctx.r5.u64 = byte2;
    ctx.r6.s64 = ctx.r1.s64 + kByteBlock;   // addi r6,r1,96 -- on the full 64-bit r1
    ctx.r7.u64 = arg7;
    ctx.r8.u64 = arg8;
    ctx.r9.s64 = ctx.r1.s64 + kCountSlot;   // addi r9,r1,80
    ctx.r10.u64 = word8;                    // left in r10 by the reload; set for fidelity
    ctx.ctr.u64 = target;                   // mtctr r29
    ctx.lr = 0x82B1F590;                    // bctrl
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    handle = ctx.r3.u64;  // mr r29,r3
  }

  // cmplwi cr6,r3,0 ; bne cr6,0x82b1f5b0. The no-singleton path branched straight here, and
  // `handle` is still its initial zero on that path, so the one test covers both.
  if (uint32_t(handle) == 0) {
    // loc_82B1F59C -- the test is on the LOW word (cmplwi), so a handle whose low word is zero
    // takes the failure path even with high bits set.
    // bl 0x82b1f440 -- (object), then a constant zero result.
    skate3::audio::GuestCall(ctx, base, sub_82B1F440, object_reg);
    ctx.r3.s64 = 0;  // li r3,0
  } else {
    // loc_82B1F5B0
    // lbz r11,14(r30) ; cmplwi cr6,r11,0 ; beq cr6,0x82b1f5ec -- re-read, not the copy stored at
    // r1+80 above.
    if (REX_LOAD_U8(object + kRecordCount) != 0) {
      cursor -= kCursorBias;  // addi r31,r31,-8 -- which is why the offsets below read 8/12/16
      int64_t seen = 0;       // li r28,0
      int32_t count;
      do {
        // loc_82B1F5C4 -- unconditional, unlike sub_82B1F5F8's loop: every record is pushed.
        const uint32_t slot = uint32_t(cursor);
        const uint32_t id = REX_LOAD_U8(slot + kRecordId);         // lbz r4,8(r31)
        const uint32_t wanted = REX_LOAD_U32(slot + kRecordWanted);  // lwz r5,16(r31)
        // bl 0x82b1be30 -- (handle, id, wanted). Six indirect calls live under it.
        skate3::audio::GuestCall(ctx, base, sub_82B1BE30, handle, uint64_t(id), uint64_t(wanted));
        // The wanted word is RELOADED after the call, not carried from above.
        const uint32_t applied = REX_LOAD_U32(slot + kRecordWanted);  // lwz r10,16(r31)
        seen += 1;                                                    // addi r28,r28,1
        // stwu r10,12(r31): store at r31+12, then r31 = that address -- a 32-bit cursor.
        REX_STORE_U32(slot + kRecordApplied, applied);
        cursor = int64_t(uint32_t(slot + kRecordStride));
        count = int32_t(REX_LOAD_U8(object + kRecordCount));  // lbz r9,14(r30)
        // cmpw cr6,r28,r9 ; blt cr6,0x82b1f5c4 -- SIGNED, against a zero-extended byte.
      } while (int32_t(uint32_t(seen)) < count);
    }
    // loc_82B1F5EC
    ctx.r3.u64 = handle;  // mr r3,r29
  }

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,208
  ctx.lr = entry_lr;                      // __restgprlr_23 restores lr and r23-r31
  ctx.r23 = saved_r23;
  ctx.r24 = saved_r24;
  ctx.r25 = saved_r25;
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

}  // namespace port_82B1F4C8
SKATE3_PORT(82B1F4C8, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
