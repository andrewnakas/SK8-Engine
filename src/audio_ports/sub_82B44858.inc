// sub_82B44858  role: run one profiled pass over a node graph -- first publish three 276-byte-strided descriptor records at the global 0x83077650, each taking the object at self+0x18 and its byte at +0xFC plus one of self+0/+4/+8, and hand their addresses back at self+0x1C/+0x20/+0x24; then for each of the u16[+0x14] entries of the array at params+8, take the node, copy the params' leading doubleword to self+0x10, let sub_82B444C0 decide where to resume, and walk the node's u8[+68] children (pointers at node+80+4*i) calling each one's own process function through its 8-byte descriptor in the table at node+0x18, falling back to sub_82B443F8 when that returns zero; every child, every node and the whole call are timed with sub_82B1F7E8 and the elapsed tick counts land in child+36, node+60, node+0 (a three-tap average over the two slots at node+4/+8) and self+0x2C   lifted: skate3_recomp.68.cpp:27274 (401 lines)
// STATUS: gate-1 (one indirect call, depth 0: each child's process function is called through `lwz r11,8(r7)` / `mtctr` / `bctrl` at 0x82B44A20, where r7 comes from a table in guest memory -- an arbitrary callee that allocates, signals or submits. It also fails gate 3 -- sub_82B1F7E8 is `mftb`, called up to 4 + 2N times here, and six stores are differences of its readings)
// calls/session: boot 18684  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B44858.md
namespace port_82B44858 {

// The object in r3. Nothing in docs/rw_audio_structs.h covers it; every name below is read off
// this one function and says what the field is used for, not what it means.
constexpr uint32_t kSource0 = 0;        // +0x00 -> goes into descriptor record 0's +4
constexpr uint32_t kSource1 = 4;        // +0x04 -> record 1's +4
constexpr uint32_t kSource2 = 8;        // +0x08 -> record 2's +4
constexpr uint32_t kParamsCopy = 16;    // +0x10  8 bytes copied verbatim from params+0
constexpr uint32_t kDevice = 24;        // +0x18  the object each record's +0 and +16 come from
constexpr uint32_t kRecordPtr0 = 28;    // +0x1C  where record 0's address is published
constexpr uint32_t kRecordPtr1 = 32;    // +0x20
constexpr uint32_t kRecordPtr2 = 36;    // +0x24
constexpr uint32_t kParams = 40;        // +0x28  the r4 argument, parked and then RE-READ often
constexpr uint32_t kTotalTime = 44;     // +0x2C  the whole call's elapsed ticks
constexpr uint32_t kScratchFloat = 56;  // +0x38  reset from a pool constant every outer pass

// The device object at self+0x18: only its byte at +0xFC is read, into each record's +16.
constexpr uint32_t kDeviceByte = 252;

// The params block (r4). +0 is a doubleword copied as bits, +8 an array of 8-byte entries whose
// first word is a node pointer, +20 a u16 count.
constexpr uint32_t kParamsDouble = 0;
constexpr uint32_t kParamsArray = 8;
constexpr uint32_t kParamsCount = 20;
constexpr uint32_t kParamsStride = 8;  // addi r23,r23,8

// The three descriptor records, ((lis_imm & 0xFFFF) << 16) + offset, computed not read by eye:
// lis r4,-31993 -> (-31993 & 0xFFFF) = 0x8307 -> 0x83070000 ; addi r11,r4,30288 (0x7650).
constexpr uint32_t kRecordBase = 0x83077650;
constexpr uint32_t kRecordStride = 276;  // addi r7,r11,276 ; addi r6,r11,552
constexpr uint32_t kRecDevice = 0;       // stw of self+0x18
constexpr uint32_t kRecSource = 4;       // stw of self+0/+4/+8
constexpr uint32_t kRecZero = 12;        // sth 0
constexpr uint32_t kRecSize = 14;        // sth 256
constexpr uint32_t kRecDeviceByte = 16;  // stb of device+0xFC
constexpr uint32_t kRecSizeValue = 256;  // li r7,256 / li r4,256 / li r29,256

// A node (r30), reached through the params array.
constexpr uint32_t kNodeAverage = 0;    // float: (slot_a + slot_b + this pass) * kTimeScale
constexpr uint32_t kNodeSlotA = 4;      // float, one of the two history slots
constexpr uint32_t kNodeSlotB = 8;      // float, the other
constexpr uint32_t kNodeSlotFlag = 12;  // u32 0/1: which slot this pass writes, then inverted
constexpr uint32_t kNodeTable = 24;     // -> the 8-byte-per-child descriptor table
constexpr uint32_t kNodeFloat44 = 44;   // float, zeroed when child 0 reports non-zero
constexpr uint32_t kNodeElapsed = 60;   // u32 ticks this node took
constexpr uint32_t kNodeChildCount = 68;  // u8, RE-READ at the bottom of every child pass
constexpr uint32_t kNodeProgress = 69;    // u8, how far a previous pass got; set to the count
constexpr uint32_t kNodeState = 70;       // u8, 255 selects the restart-from-zero path
constexpr uint32_t kNodeChildIndexBias = 20;  // addi r11,r29,20 then *4: the child pointer array
                                              // therefore starts at node+80
constexpr uint32_t kTableStride = 8;      // rlwinm r11,r29,3,0,28
constexpr uint32_t kDescriptorProcess = 8;  // lwz r11,8(r7) -- the function pointer

// A child (r28).
constexpr uint32_t kChildElapsed = 36;  // u32 ticks this child took
constexpr uint32_t kChildSkip = 40;     // u8 non-zero: skip it entirely, but still time it

// Float pool constants, each ((lis_imm & 0xFFFF) << 16) + the displacements, computed:
//   lis r10,-32234 -> 0x82160000 ; lfs f31,23056(r10)          -> 0x82165A10
//   lis r11,-32208 -> 0x82300000 ; addi r9,r11,-31232 ; lfs 440 -> 0x822F87B8
//   lis r8,-32206  -> 0x82320000 ; lfs f30,-22460(r8)          -> 0x8231A844
// 0x82165A10 is the same constant sub_82B2DAC8.inc already calls kZeroFloat, which is what makes
// "zero" here a cross-check rather than a guess. The other two values were not read.
constexpr uint32_t kZeroFloat = 0x82165A10;
constexpr uint32_t kTimeScale = 0x822F87B8;   // the fadds chain's multiplier
constexpr uint32_t kScratchInit = 0x8231A844;  // what self+0x38 is reset to

constexpr uint32_t kFrameBytes = 208;  // stwu r1,-208(r1)
constexpr uint32_t kSpillSlot = 80;    // std r9,80(r1) ; lfd f11,80(r1) -- the fcfid round trip

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

double AddSingle(double a, double b) { return double(float(a + b)); }
double MulSingle(double a, double b) { return double(float(a * b)); }

REX_FUNC(Native) {
  // __savegprlr_21 spills r21-r31 and the link register, and three stfd spill f29-f31; all the
  // slots are inside the 208 bytes the stwu below claims (the stfd addresses are entry_sp-120 and
  // up, the frame starts at entry_sp-208), so no callee can reach them and none is reproduced.
  // The harness compares r13-r31 and f14-f31, so the entry values have to survive.
  const PPCRegister saved_r21 = ctx.r21;
  const PPCRegister saved_r22 = ctx.r22;
  const PPCRegister saved_r23 = ctx.r23;
  const PPCRegister saved_r24 = ctx.r24;
  const PPCRegister saved_r25 = ctx.r25;
  const PPCRegister saved_r26 = ctx.r26;
  const PPCRegister saved_r27 = ctx.r27;
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister saved_f29 = ctx.f29;
  const PPCRegister saved_f30 = ctx.f30;
  const PPCRegister saved_f31 = ctx.f31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;
  ctx.fpscr.disableFlushModeUnconditional();  // ahead of the three stfd, as lifted

  // The frame IS reproduced: the fcfid round trip at the bottom goes through r1+80, and all three
  // guest callees build their own frames below r1.
  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-208(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t self = ctx.r3.u32;    // mr r31,r3
  const uint32_t params = ctx.r4.u32;  // mr r30,r4

  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);  // bl 0x82b1f7e8 -- mftb, nothing else
  REX_STORE_U32(self + kParams, params);             // stw r30,40(r31)
  const uint64_t call_start = ctx.r3.u64;            // mr r21,r3 -- captured mid-block, as lifted

  // ---- the three descriptor records --------------------------------------------------------
  // Same five fields each, but the compiler scheduled the three groups in three different orders,
  // so they are written out rather than looped. Each group RELOADS self+0x18 and reads the device
  // byte again; those reloads are the original's.
  {
    const uint32_t device = REX_LOAD_U32(self + kDevice);           // lwz r10,24(r31)
    const uint32_t device_byte = REX_LOAD_U32(device + kDeviceByte); // lwz r6,252(r10)
    const uint32_t source = REX_LOAD_U32(self + kSource0);           // lwz r9,0(r31)
    const uint32_t record = kRecordBase;                            // addi r11,r4,30288
    REX_STORE_U32(record + kRecDevice, device);                     // stw r10,30288(r4)
    REX_STORE_U16(record + kRecSize, uint16_t(kRecSizeValue));       // sth r7,14(r11)
    REX_STORE_U8(record + kRecDeviceByte, uint8_t(device_byte));      // stb r6,16(r11)
    REX_STORE_U16(record + kRecZero, 0);                             // sth r8,12(r11)
    REX_STORE_U32(record + kRecSource, source);                      // stw r9,4(r11)
    REX_STORE_U32(self + kRecordPtr0, record);                       // stw r11,28(r31)
  }
  {
    const uint32_t device = REX_LOAD_U32(self + kDevice);            // lwz r10,24(r31)
    const uint32_t device_byte = REX_LOAD_U32(device + kDeviceByte);  // lwz r3,252(r10)
    const uint32_t source = REX_LOAD_U32(self + kSource1);            // lwz r9,4(r31)
    const uint32_t record = kRecordBase + kRecordStride;              // addi r7,r11,276
    REX_STORE_U8(record + kRecDeviceByte, uint8_t(device_byte));       // stb r3,292(r11)
    REX_STORE_U32(record + kRecDevice, device);                       // stw r10,276(r11)
    REX_STORE_U16(record + kRecZero, 0);                              // sth r5,288(r11)
    REX_STORE_U16(record + kRecSize, uint16_t(kRecSizeValue));         // sth r4,290(r11)
    REX_STORE_U32(record + kRecSource, source);                        // stw r9,280(r11)
    REX_STORE_U32(self + kRecordPtr1, record);                         // stw r7,32(r31)
  }
  {
    const uint32_t device = REX_LOAD_U32(self + kDevice);            // lwz r10,24(r31)
    const uint32_t device_byte = REX_LOAD_U32(device + kDeviceByte);  // lwz r8,252(r10)
    const uint32_t source = REX_LOAD_U32(self + kSource2);            // lwz r9,8(r31)
    const uint32_t record = kRecordBase + 2 * kRecordStride;          // addi r6,r11,552
    REX_STORE_U32(record + kRecDevice, device);                       // stw r10,552(r11)
    REX_STORE_U16(record + kRecZero, 0);                              // sth r30,564(r11)
    REX_STORE_U16(record + kRecSize, uint16_t(kRecSizeValue));         // sth r29,566(r11)
    REX_STORE_U8(record + kRecDeviceByte, uint8_t(device_byte));       // stb r8,568(r11)
    REX_STORE_U32(record + kRecSource, source);                        // stw r9,556(r11)
    REX_STORE_U32(self + kRecordPtr2, record);                         // stw r6,36(r31)
  }

  uint64_t status = 0;  // li r27,0 -- the value returned in r3, so a zero count returns zero

  // lwz r7,40(r31) ; lhz r6,20(r7) ; cmplwi cr6,r6,0 ; beq cr6,0x82b44af8
  if (REX_LOAD_U16(REX_LOAD_U32(self + kParams) + kParamsCount) != 0) {
    // The three pool constants are loaded ONCE, outside the loop, into callee-saved f29-f31.
    ctx.fpscr.disableFlushModeUnconditional();
    const double zero = LoadSingle(base, kZeroFloat);          // lfs f31,23056(r10)
    const double scale = LoadSingle(base, kTimeScale);         // lfs f29,440(r9)
    const double scratch_init = LoadSingle(base, kScratchInit);  // lfs f30,-22460(r8)

    uint64_t array_offset = 0;  // li r23,0 -- the byte offset into the params array
    uint64_t index = 0;         // li r24,0 -- the entry counter
    uint32_t count;
    do {
      // loc_82B44950
      const uint32_t array = REX_LOAD_U32(REX_LOAD_U32(self + kParams) + kParamsArray);
      skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
      // lwzx r30,r30,r23 -- the first word of the 8-byte entry; the other four bytes are unread.
      const uint32_t node = REX_LOAD_U32(uint32_t(array + uint32_t(array_offset)));
      const uint64_t node_start = ctx.r3.u64;                     // mr r22,r3
      const uint32_t table = REX_LOAD_U32(node + kNodeTable);      // lwz r25,24(r30)
      ctx.fpscr.disableFlushModeUnconditional();
      StoreSingle(base, self + kScratchFloat, scratch_init);       // stfs f30,56(r31)

      uint64_t child_index;  // r29
      // lbz r10,70(r30) ; cmplwi cr6,r10,255 ; bge cr6,0x82b449a4 -- UNSIGNED, and the byte
      // cannot exceed 255, so `bge` here means exactly 255.
      if (REX_LOAD_U8(node + kNodeState) < 255) {
        // lfd f0,0(r11) ; stfd f0,16(r31) -- eight bytes of the params moved as bits, with no
        // conversion and no flush-mode change of its own.
        REX_STORE_U64(self + kParamsCopy,
                      REX_LOAD_U64(REX_LOAD_U32(self + kParams) + kParamsDouble));
        // bl 0x82b444c0 (self, table, node) -- decides where the child walk resumes.
        skate3::audio::GuestCall(ctx, base, sub_82B444C0, self, table, node);
        status = ctx.r3.u64;  // mr r27,r3
        // lbz r11,70(r30) ; addi r29,r11,1 -- RELOADED: the callee may have moved the state on.
        child_index = uint64_t(REX_LOAD_U8(node + kNodeState)) + 1;
      } else {
        // loc_82B449A4
        child_index = 0;  // li r29,0
        status = 1;       // li r27,1
      }

      // loc_82B449AC -- the same eight bytes again, on both paths, from a re-read params pointer.
      ctx.fpscr.disableFlushModeUnconditional();
      REX_STORE_U64(self + kParamsCopy,
                    REX_LOAD_U64(REX_LOAD_U32(self + kParams) + kParamsDouble));

      // cmpwi cr6,r27,1 ; bne cr6,0x82b44a88 -- SIGNED against 1, not "non-zero".
      if (int32_t(uint32_t(status)) == 1) {
        bool set_progress = true;
        // lbz r11,68(r30) ; cmpw cr6,r29,r11 ; bge cr6,0x82b44a80 -- SIGNED. Nothing to walk
        // sends it straight to the progress store with status still 1.
        if (int32_t(uint32_t(child_index)) < int32_t(REX_LOAD_U8(node + kNodeChildCount))) {
          for (;;) {
            // loc_82B449CC
            skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
            const uint64_t child_start = ctx.r3.u64;  // mr r26,r3
            // addi r11,r29,20 ; rlwinm r10,r11,2,0,29 -- 4*(i+20), i.e. the pointer array at
            // node+80. The index is under 256 so the rotate cannot wrap and the 0xFFFFFFFC mask
            // drops nothing; the stride is exactly 4.
            const uint32_t slot = 4 * uint32_t(child_index + kNodeChildIndexBias);  // = 80 + 4*i
            const uint32_t child = REX_LOAD_U32(node + slot);      // lwzx r28,r10,r30
            // lbz r9,40(r28) ; cmplwi cr6,r9,0 ; bne cr6,0x82b44a5c -- a skipped child is still
            // timed and still advances the index, and `status` keeps whatever it held.
            if (REX_LOAD_U8(child + kChildSkip) == 0) {
              // rlwinm r11,r29,3,0,28 ; lwzx r7,r11,r25 -- the first word of the 8-byte table
              // entry for this child.
              const uint32_t descriptor =
                  REX_LOAD_U32(table + kTableStride * uint32_t(child_index));
              // subfc r9,r29,r10 ; eqv r8,r29,r10 ; rlwinm r6,r8,1,31,31 ; addze r5,r6 ;
              // clrlwi r5,r5,31 -- the carry-plus-sign-equality idiom that reduces a SIGNED
              // `progress < i` to 0 or 1 without a branch. Both operands come from an lbz or a
              // counter bounded by one, so signed and unsigned agree; the third argument is that
              // boolean: "this child is past where the last pass got to".
              const uint32_t progress = REX_LOAD_U8(node + kNodeProgress);  // lbz r10,69(r30)
              const uint64_t past_progress =
                  uint64_t(int32_t(progress) < int32_t(uint32_t(child_index)));
              ctx.ctr.u64 = REX_LOAD_U32(descriptor + kDescriptorProcess);  // lwz r11,8(r7) ; mtctr
              ctx.r3.u64 = child;          // mr r3,r28
              ctx.r4.u64 = self;           // mr r4,r31
              ctx.r5.u64 = past_progress;  // the addze/clrlwi result
              ctx.lr = 0x82B44A20;
              REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);  // bctrl
              status = ctx.r3.u64;                  // mr r27,r3
              // cmpwi cr6,r3,0 ; bne cr6,0x82b44a50
              if (ctx.r3.s32 == 0) {
                // bl 0x82b443f8 (self, node, child, 256) -- the fallback when the child's own
                // process function declined.
                skate3::audio::GuestCall(ctx, base, sub_82B443F8, self, node, child,
                                         kRecSizeValue);
                status = ctx.r3.u64;  // mr r27,r3
                // cmpwi cr6,r3,0 ; beq cr6,0x82b44a78 -- leaves the walk WITHOUT timing this
                // child and WITHOUT advancing the index.
                if (ctx.r3.s32 == 0) break;
              } else if (int32_t(uint32_t(child_index)) == 0) {
                // loc_82B44A50 -- cmpwi cr6,r29,0 ; bne cr6,0x82b44a5c
                ctx.fpscr.disableFlushModeUnconditional();
                StoreSingle(base, node + kNodeFloat44, zero);  // stfs f31,44(r30)
              }
            }

            // loc_82B44A5C
            skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
            const uint64_t child_elapsed = ctx.r3.u64 - child_start;  // subf r11,r26,r3
            child_index += 1;                                         // addi r29,r29,1
            REX_STORE_U32(child + kChildElapsed, uint32_t(child_elapsed));  // stw r11,36(r28)
            // lbz r11,68(r30) ; cmpw cr6,r29,r11 ; blt cr6,0x82b449cc -- the bound is RE-READ
            // every pass and compared SIGNED.
            if (!(int32_t(uint32_t(child_index)) < int32_t(REX_LOAD_U8(node + kNodeChildCount))))
              break;
          }
          // loc_82B44A78 -- cmpwi cr6,r27,1 ; bne cr6,0x82b44a88. Reached both by finishing the
          // walk and by the early exit above, so the status is re-tested here.
          set_progress = int32_t(uint32_t(status)) == 1;
        }
        if (set_progress) {
          // loc_82B44A80 -- lbz r11,68(r30) ; stb r11,69(r30): the count is RELOADED for this.
          REX_STORE_U8(node + kNodeProgress, uint8_t(REX_LOAD_U8(node + kNodeChildCount)));
        }
      }

      // loc_82B44A88 -- the node's timing tail, run on every path.
      skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
      const uint64_t node_elapsed = ctx.r3.u64 - node_start;  // subf r10,r22,r3
      ctx.fpscr.disableFlushModeUnconditional();
      const double slot_b = LoadSingle(base, node + kNodeSlotB);     // lfs f0,8(r30)
      const uint32_t slot_flag = REX_LOAD_U32(node + kNodeSlotFlag);  // lwz r11,12(r30)
      const uint64_t low = node_elapsed & 0xFFFFFFFFull;              // clrldi r9,r10,32
      const double slot_a = LoadSingle(base, node + kNodeSlotA);      // lfs f13,4(r30)
      const double history = AddSingle(slot_b, slot_a);               // fadds f12,f0,f13
      const uint64_t next_flag = uint64_t(slot_flag) + 1;             // addi r8,r11,1
      // std r9,80(r1) ; lfd f11,80(r1) ; fcfid f10,f11 -- the elapsed count is spilled and read
      // back as a doubleword so fcfid can convert it. The round trip is inside this call's own
      // frame; it is reproduced because the store is real, and the value is non-negative and
      // under 2^32 so the conversion is exact before the frsp.
      REX_STORE_U64(frame + kSpillSlot, low);
      const double as_double = double(int64_t(REX_LOAD_U64(frame + kSpillSlot)));
      // rlwinm r7,r8,2,0,29 -- 4*(flag+1), which is node+4 or node+8: the slot NOT read as
      // `history`'s first term this pass.
      const uint32_t slot_store = 4 * uint32_t(next_flag);
      const double as_single = double(float(as_double));  // frsp f9,f10
      REX_STORE_U32(node + kNodeElapsed, uint32_t(node_elapsed));  // stw r10,60(r30)
      index += 1;                        // addi r24,r24,1
      array_offset += kParamsStride;     // addi r23,r23,8
      const double total = AddSingle(history, as_single);  // fadds f8,f12,f9
      StoreSingle(base, node + kNodeAverage, MulSingle(total, scale));  // fmuls f7 ; stfs f7,0(r30)
      StoreSingle(base, node + slot_store, as_single);                  // stfsx f9,r7,r30
      // lwz r6,12(r30) ; cntlzw r5,r6 ; rlwinm r4,r5,27,31,31 ; stw r4,12(r30) -- bit 5 of a
      // count-leading-zeros result is set only for the value 32, so this stores 1 when the flag
      // was zero and 0 otherwise: the two slots alternate.
      const uint32_t reloaded_flag = REX_LOAD_U32(node + kNodeSlotFlag);
      REX_STORE_U32(node + kNodeSlotFlag, reloaded_flag == 0 ? 1u : 0u);
      // lwz r3,40(r31) ; lhz r11,20(r3) ; cmpw cr6,r24,r11 ; blt cr6,0x82b44950 -- SIGNED, and
      // the count is RE-READ through a re-read params pointer every pass.
      count = REX_LOAD_U16(REX_LOAD_U32(self + kParams) + kParamsCount);
    } while (int32_t(uint32_t(index)) < int32_t(count));
  }

  // loc_82B44AF8
  skate3::audio::GuestCall(ctx, base, sub_82B1F7E8);
  const uint64_t total_elapsed = ctx.r3.u64 - call_start;  // subf r11,r21,r3
  ctx.r3.u64 = status;                                     // mr r3,r27
  REX_STORE_U32(self + kTotalTime, uint32_t(total_elapsed));  // stw r11,44(r31)

  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;      // addi r1,r1,208
  ctx.fpscr.disableFlushModeUnconditional();  // ahead of the three lfd, as lifted
  ctx.f29 = saved_f29;                        // lfd f29,-120(r1)
  ctx.f30 = saved_f30;                        // lfd f30,-112(r1)
  ctx.f31 = saved_f31;                        // lfd f31,-104(r1)
  ctx.lr = entry_lr;                          // __restgprlr_21 restores lr and r21-r31
  ctx.r12.u64 = entry_lr;
  ctx.r21 = saved_r21;
  ctx.r22 = saved_r22;
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

}  // namespace port_82B44858
SKATE3_PORT(82B44858, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
