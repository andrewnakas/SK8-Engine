// sub_82B2F038  role: per-frame reset pass for one mixer object -- always rewrite the float at +1100 from the pool constant, then either (gate at +52 above zero) advance the phase word at +1092 from 2 to 3 through sub_82B2F150, or (gate at +352 above zero) rewind six 60-byte records from +696 and zero six floats from +516, and then the +1089 records from +140, each record taking its new word from the parallel u32 array   lifted: skate3_recomp.67.cpp:42122 (169 lines)
// STATUS: gate-1 (sub_82B2F150 reaches an indirect call one level down: sub_82B3D668 `bctrl`, census depth 2)
// calls/session: boot 37370  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2F038.md
namespace port_82B2F038 {

// Constant-pool addresses, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye.
constexpr uint32_t kZeroFloat = 0x82165A10;      // lis -32234 ; lfs 23056 -- the same zero the
                                                 // rest of the audio code compares against
constexpr uint32_t kResetConstant = 0x822F87CC;  // lis -32208 ; addi -31232 ; lfs 460

// The mixer object (r3/r31). Nothing in docs/rw_audio_structs.h names this layout.
constexpr uint32_t kPhaseGate = 52;     // f32 -- strictly above zero selects the phase path
constexpr uint32_t kResetGate = 352;    // f32 -- strictly above zero selects the rewind path
constexpr uint32_t kSourceA = 384;      // u32 array, lwzu stride 4: first read at +388
constexpr uint32_t kFloatsA = 480;      // f32 array, stfsu stride 36: first write at +516
constexpr uint32_t kRecordsA = 696;     // record cursor, stride 60
constexpr uint32_t kRecordsB = 140;     // record cursor for the second pass, stride 60
constexpr uint32_t kTailCount = 1089;   // u8 -- how many records the second pass rewinds
constexpr uint32_t kPhase = 1092;       // s32 -- exactly 2 advances to 3
constexpr uint32_t kSourceB = 1068;     // u32 array, lwzu stride 4: first read at +1072
constexpr uint32_t kResetSlot = 1100;   // f32 written unconditionally, before either branch

constexpr uint32_t kFirstCount = 6;     // li r10,6 ; mtctr r10
constexpr uint32_t kRecordStride = 60;  // stbu r10,60(r11)
constexpr uint32_t kFloatStride = 36;   // stfsu f0,36(r8)
constexpr uint32_t kSourceStride = 4;   // lwzu r7,4(r9)

constexpr uint32_t kFrameBytes = 96;  // stwu r1,-96(r1)

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

// The five stores both passes share, in the original's order: the two words at +24 and +28 are
// read first and written back to +36 and +56, so the record is partly rotated rather than simply
// cleared. Returns the advanced cursor; `stbu` advances by 60 in 32 bits.
uint32_t ResetRecord(uint8_t* base, uint32_t cursor, uint32_t word) {
  const uint32_t keep_a = REX_LOAD_U32(cursor + 24);  // lwz r6,24(r11)
  const uint32_t keep_b = REX_LOAD_U32(cursor + 28);  // lwz r5,28(r11)
  REX_STORE_U32(cursor + 40, 0);       // stw r10,40(r11)
  REX_STORE_U32(cursor + 44, word);    // stw r7,44(r11)
  REX_STORE_U32(cursor + 36, keep_a);  // stw r6,36(r11)
  REX_STORE_U32(cursor + 56, keep_b);  // stw r5,56(r11)
  REX_STORE_U8(cursor + kRecordStride, 0);  // stbu r10,60(r11) -- the byte lands at the next
  return cursor + kRecordStride;            //                     cursor, which is where r11 goes
}

REX_FUNC(Native) {
  // The prologue spills r12 (the link register) and r31 into this call's own frame and the
  // epilogue reloads both. The harness compares r13-r31, so the entry value has to survive here;
  // the spill slots are inside the 96 bytes the stwu claims, so they are not windowed.
  const PPCRegister saved_r31 = ctx.r31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-96(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint64_t object_reg = ctx.r3.u64;  // mr r31,r3
  const uint32_t object = uint32_t(object_reg);

  ctx.fpscr.disableFlushModeUnconditional();
  const double phase_gate = LoadSingle(base, object + kPhaseGate);  // lfs f12,52(r3)
  const double zero = LoadSingle(base, kZeroFloat);                 // lfs f0,23056(r10)
  const double reset_value = LoadSingle(base, kResetConstant);      // lfs f13,460(r9)
  // The store is BEFORE the branch in the original and runs on every path, including the one
  // that does nothing else.
  StoreSingle(base, object + kResetSlot, reset_value);  // stfs f13,1100(r3)

  // fcmpu cr6,f12,f0 ; bgt cr6,0x82b2f11c
  if (phase_gate > zero) {
    // loc_82B2F11C -- cmpwi cr6,r11,2 ; bne cr6,0x82b2f138. Exactly 2, signed.
    if (int32_t(REX_LOAD_U32(object + kPhase)) == 2) {
      // bl 0x82b2f150 -- (object). This is the callee whose closure reaches sub_82B3D668's
      // bctrl, which is what fails gate 1.
      skate3::audio::GuestCall(ctx, base, sub_82B2F150, object_reg);
      REX_STORE_U32(object + kPhase, 3);  // li r11,3 ; stw r11,1092(r31)
    }
  } else {
    const double reset_gate = LoadSingle(base, object + kResetGate);  // lfs f13,352(r3)
    // fcmpu cr6,f13,f0 ; ble cr6,0x82b2f138 -- `ble` is false only for greater, so an unordered
    // compare (a NaN gate) also skips, exactly as here.
    if (reset_gate > zero) {
      uint32_t source = object + kSourceA;    // addi r9,r3,384
      uint32_t floats = object + kFloatsA;    // addi r8,r3,480
      uint32_t cursor = object + kRecordsA;   // addi r11,r3,696
      // mtctr 6 ; bdnz -- a fixed six iterations, not a memory-bounded loop.
      for (uint32_t i = 0; i < kFirstCount; i++) {
        // loc_82B2F094
        source += kSourceStride;                      // lwzu r7,4(r9): pre-increment, then load
        // addi r7,r7,1 on a zero-extended load, kept in 64 bits and narrowed at the store. The
        // SECOND pass omits this increment; it is the one difference between the two bodies.
        const uint64_t word = uint64_t(REX_LOAD_U32(source)) + 1;
        cursor = ResetRecord(base, cursor, uint32_t(word));
        ctx.fpscr.disableFlushModeUnconditional();
        floats += kFloatStride;             // stfsu f0,36(r8): pre-increment, then store
        StoreSingle(base, floats, zero);
      }

      // lbz r11,1089(r31) ; cmplwi cr6,r11,0 ; beq cr6,0x82b2f138
      if (REX_LOAD_U8(object + kTailCount) != 0) {
        uint32_t tail_source = object + kSourceB;  // addi r8,r31,1068
        uint32_t tail_cursor = object + kRecordsB;  // addi r11,r31,140
        int64_t seen = 0;                           // mr r9,r10 -- r10 is still zero
        int32_t count;
        do {
          // loc_82B2F0D8
          tail_source += kSourceStride;  // lwzu r7,4(r8)
          const uint32_t word = REX_LOAD_U32(tail_source);  // no +1 here
          seen += 1;                                        // addi r9,r9,1
          tail_cursor = ResetRecord(base, tail_cursor, word);
          count = int32_t(REX_LOAD_U8(object + kTailCount));  // lbz r4,1089(r31) -- re-read
          // cmpw cr6,r9,r4 ; blt cr6,0x82b2f0d8 -- SIGNED, against a zero-extended byte.
        } while (int32_t(uint32_t(seen)) < count);
      }
    }
  }

  // loc_82B2F138 -- no `li r3,...` on any path: r3 is the entry object on every path that does
  // not call, and whatever sub_82B2F150 left on the one that does.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,96
  ctx.r12.u64 = entry_lr;                 // lwz r12,-8(r1)
  ctx.lr = entry_lr;                      // mtlr r12
  ctx.r31 = saved_r31;                    // ld r31,-16(r1)
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B2F038
SKATE3_PORT(82B2F038, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
