// sub_82B1F5F8  role: flush the object's dirty 12-byte state records (the array at +28, count at +14) through sub_82B1BE30 -- one call per record whose desired word at +8 differs from the applied word at +4, then copy +8 over +4 -- and then query the device at +8 through its vtable slot 20 into an eleven-word frame buffer, re-initialising the object via sub_82B1F440 when the first word comes back zero and otherwise absorbing the reported words into the record just past the array   lifted: skate3_recomp.67.cpp:3760 (154 lines)
// STATUS: gate-1 (its own `bctrl` at 0x82B1F66C, the device vtable's slot-20 entry, and sub_82B1BE30 reaches six more: census depth 0)
// calls/session: boot 3097  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1F5F8.md
namespace port_82B1F5F8 {

// The object (r3/r30). Nothing in docs/rw_audio_structs.h names this layout.
constexpr uint32_t kDevice = 8;        // lwz r3,8(r30) -- the object with the vtable
constexpr uint32_t kRecordCount = 14;  // u8, RE-READ at the bottom of every iteration
constexpr uint32_t kPairFlag = 15;     // u8 -- gates the two-word copy and shifts the eight-word one
constexpr uint32_t kBlockFlag = 17;    // u8 -- gates the eight-word copy
constexpr uint32_t kRecords = 28;      // addi r31,r3,28 -- the first 12-byte record

// A 12-byte state record.
constexpr uint32_t kRecordId = 0;       // u8 passed to sub_82B1BE30 as r4
constexpr uint32_t kRecordApplied = 4;  // s32 -- what was last pushed to the device
constexpr uint32_t kRecordWanted = 8;   // s32 -- what should be pushed; compared against +4
constexpr uint32_t kRecordStride = 12;  // addi r31,r31,12

// The device's vtable.
constexpr uint32_t kVptr = 0;        // lwz r11,0(r3)
constexpr uint32_t kQuerySlot = 20;  // lwz r10,20(r11) ; mtctr ; bctrl

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)
// addi r5,r1,80 -- the out buffer, passed BY ADDRESS to the indirect callee. r4 is 11 and the
// function reads exactly eleven words from it (80, 84, 88, then 92..120), so the literal is a
// word count for this same buffer.
constexpr uint32_t kOutBuffer = 80;
constexpr uint32_t kOutWords = 11;  // li r4,11
constexpr uint32_t kBlockWords = 8;

REX_FUNC(Native) {
  // __savegprlr_29 spills r29-r31 and the link register into this call's own frame and
  // __restgprlr_29 reloads them; the harness compares r13-r31, so the entry values have to
  // survive here. The spill slots are inside the 160 bytes the stwu claims, so they are not
  // windowed and are not reproduced.
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  // The frame IS reproduced: kOutBuffer is passed by address to the vtable call, so it has to
  // land inside the 160 bytes this call allocates rather than in the caller's frame.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-160(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t object_reg = ctx.r3.u64;  // mr r30,r3
  const uint32_t object = uint32_t(object_reg);
  int64_t cursor = ctx.r3.s64 + kRecords;  // addi r31,r3,28 -- s64, and it survives the loop

  // lbz r11,14(r3) ; cmplwi cr6,r11,0 ; beq cr6,0x82b1f654
  if (REX_LOAD_U8(object + kRecordCount) != 0) {
    int64_t index = 0;  // li r29,0
    int32_t count;
    do {
      // loc_82B1F61C
      const uint32_t record = uint32_t(cursor);
      const uint32_t wanted = REX_LOAD_U32(record + kRecordWanted);   // lwz r5,8(r31)
      const uint32_t applied = REX_LOAD_U32(record + kRecordApplied);  // lwz r11,4(r31)
      // cmpw cr6,r5,r11 ; beq cr6,0x82b1f640 -- SIGNED, though only equality is tested.
      if (int32_t(wanted) != int32_t(applied)) {
        const uint32_t id = REX_LOAD_U8(record + kRecordId);      // lbz r4,0(r31)
        const uint32_t device = REX_LOAD_U32(object + kDevice);   // lwz r3,8(r30)
        // bl 0x82b1be30 -- (device, id, wanted). Six indirect calls live under it.
        skate3::audio::GuestCall(ctx, base, sub_82B1BE30, uint64_t(device), uint64_t(id),
                                 uint64_t(wanted));
        // The wanted word is RELOADED after the call, not carried from above: the callee could
        // have changed it.
        REX_STORE_U32(record + kRecordApplied,
                      REX_LOAD_U32(record + kRecordWanted));  // lwz r11,8(r31) ; stw r11,4(r31)
      }
      // loc_82B1F640 -- the bound is re-read every pass, and the cursor advances on both arms.
      count = int32_t(REX_LOAD_U8(object + kRecordCount));  // lbz r11,14(r30)
      index += 1;                                           // addi r29,r29,1
      cursor += kRecordStride;                              // addi r31,r31,12
      // cmpw cr6,r29,r11 ; blt cr6,0x82b1f61c -- SIGNED, against a zero-extended byte.
    } while (int32_t(uint32_t(index)) < count);
  }

  // loc_82B1F654 -- the device query. r4 is the word count, r5 the buffer.
  const uint32_t device = REX_LOAD_U32(object + kDevice);   // lwz r3,8(r30)
  const uint32_t vtable = REX_LOAD_U32(device + kVptr);     // lwz r11,0(r3)
  ctx.r3.u64 = device;
  ctx.r4.s64 = kOutWords;                    // li r4,11
  ctx.r5.s64 = ctx.r1.s64 + kOutBuffer;      // addi r5,r1,80 -- on the full 64-bit r1
  ctx.ctr.u64 = REX_LOAD_U32(vtable + kQuerySlot);  // lwz r10,20(r11) ; mtctr r10
  ctx.lr = 0x82B1F670;                              // bctrl
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

  const uint32_t out = frame + kOutBuffer;
  // lwz r9,80(r1) ; cmpwi cr6,r9,0 ; bne cr6,0x82b1f68c -- SIGNED against zero.
  if (int32_t(REX_LOAD_U32(out + 0)) == 0) {
    // bl 0x82b1f440 -- (object). Its result is this function's result: there is no `li r3,...`
    // on this path.
    skate3::audio::GuestCall(ctx, base, sub_82B1F440, object_reg);
  } else {
    // loc_82B1F68C
    uint32_t block = uint32_t(cursor);  // mr r11,r31 -- the record just past the array
    // lbz r10,15(r30) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1f6b0
    if (REX_LOAD_U8(object + kPairFlag) != 0) {
      const uint32_t second = REX_LOAD_U32(out + 8);  // lwz r10,88(r1)
      block = uint32_t(cursor + 8);                   // addi r11,r31,8
      const uint32_t first = REX_LOAD_U32(out + 4);   // lwz r9,84(r1)
      // Stored HIGH word first: +4 then +0, as the original has it.
      REX_STORE_U32(uint32_t(cursor) + 4, second);  // stw r10,4(r31)
      REX_STORE_U32(uint32_t(cursor) + 0, first);   // stw r9,0(r31)
    }
    // loc_82B1F6B0 -- the eight-word block lands at r31+8 when the pair above was written and at
    // r31+0 otherwise, so a clear +15 lets this overwrite the same two words.
    // lbz r10,17(r30) ; cmplwi cr6,r10,0 ; beq cr6,0x82b1f6fc
    if (REX_LOAD_U8(object + kBlockFlag) != 0) {
      // All eight loads precede all eight stores in the original (r10,r9,r8,r7,r6,r5,r4,r3 then
      // stw in the same order), so the two passes are kept separate rather than interleaved.
      uint32_t words[kBlockWords];
      for (uint32_t i = 0; i < kBlockWords; i++) {
        words[i] = REX_LOAD_U32(out + 12 + 4 * i);  // lwz from 92,96,100,104,108,112,116,120(r1)
      }
      for (uint32_t i = 0; i < kBlockWords; i++) {
        REX_STORE_U32(block + 4 * i, words[i]);  // stw to 0,4,8,12,16,20,24,28(r11)
      }
    }
    // loc_82B1F6FC
    ctx.r3.s64 = 1;  // li r3,1
  }

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
  ctx.lr = entry_lr;                      // __restgprlr_29 restores lr and r29-r31
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

}  // namespace port_82B1F5F8
SKATE3_PORT(82B1F5F8, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
