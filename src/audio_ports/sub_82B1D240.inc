// sub_82B1D240  role: drive an audio object's three-state transition -- clamp the request at +24 to 0..2, and if it differs from the latched state byte at +12 either release the instance at +8 (state 0: the instance vtable's slot 0, then sub_82B1F440), resume it (state 1: slot 8, or build a fresh instance with sub_82B1F4C8 from the stride-12 table at +4 indexed by +20, or sub_82B1F440 when that record's halfword tag is 0xFFFF) or suspend it (state 2: slot 4); then latch the new state at +12 with the old one at +13, run sub_82B1F5F8 when state 1 has a live instance, and finish by poking the instance's vtable slot 24   lifted: skate3_recomp.66.cpp:66296 (228 lines)
// STATUS: gate-1 (three of its own bctrl sites: the instance vtable's slot 0 at `bctrl` 0x82B1D2A8, the shared slot-8/slot-4 site at 0x82B1D368 and the slot-24 poke at 0x82B1D3BC -- census depth 0, indirect 3; slot 0 is a release, so a replay would double-release)
// calls/session: boot 957760  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1D240.md
namespace port_82B1D240 {

// docs/rw_audio_structs.h names none of this object. The offsets below are the ones this
// function touches; sub_82B1F440 and sub_82B1F5F8 describe +14 and +28 on the same object.
constexpr uint32_t kTable = 4;       // lwz r9,4(r31) -- the stride-12 record table
constexpr uint32_t kInstance = 8;    // lwz r3,8(r31) -- the owned instance, vtable at +0
constexpr uint32_t kState = 12;      // lbz/stb 12(r31) -- SIGNED byte, extsb before the compare
constexpr uint32_t kPrevState = 13;  // stb 13(r31) -- the state this call displaced
constexpr uint32_t kIndex = 20;      // lwz r11,20(r31) -- the record index, clamped below
constexpr uint32_t kRequest = 24;    // lwz r11,24(r3) -- the requested state, clamped to 0..2

constexpr uint32_t kTableCount = 0;    // lwz r10,0(r9) -- the clamp limit
constexpr uint32_t kRecordStride = 12; // r11*3 then <<2
constexpr uint32_t kRecordOffset = 4;  // addi r4,r11,4 -- what sub_82B1F4C8 receives in r4
constexpr uint32_t kRecordTag = 4;     // lhz r10,4(r11) -- 0xFFFF means "no record"
constexpr uint32_t kNoRecord = 65535;  // cmplwi cr6,r10,65535

constexpr uint32_t kVptr = 0;            // lwz r11,0(r3)
constexpr uint32_t kSlotRelease = 0;     // lwz r10,0(r11)  -- state 0
constexpr uint32_t kSlotSuspend = 4;     // lwz r10,4(r11)  -- state 2
constexpr uint32_t kSlotResume = 8;      // lwz r10,8(r11)  -- state 1 with a live instance
constexpr uint32_t kSlotPoke = 24;       // lwz r10,24(r11) -- the unconditional tail call

constexpr uint32_t kFrameBytes = 112;  // stwu r1,-112(r1)
// __savegprlr_29 / __restgprlr_29 slots, negative offsets from the ENTRY sp, all inside the
// 112 bytes this call allocates, so none of them is a window.
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3

  // The frame is reproduced because all three named callees run on it (sub_82B1F4C8 opens with
  // stwu r1,-208(r1)), as do the three indirect targets.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                              // mflr r12
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);   // bl 0x82f5211c -> std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);   //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);   //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32);  //                  stw r12,-8(r1)
  const uint32_t frame = entry_sp - kFrameBytes;     // stwu r1,-112(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  // lwz r11,24(r3) -- zero-extended, then compared SIGNED against 0 and 2.
  const uint32_t request = REX_LOAD_U32(self + kRequest);
  // li r29,0 -- the zero that `stw r29,8(r31)` and the `mr r3,r29` exit both use.
  uint64_t state;  // r30, 64-bit: the clamped state, later overwritten by sub_82B1F5F8's r3
  if (static_cast<int32_t>(request) < 0) {
    state = 0;  // mr r30,r29
  } else if (static_cast<int32_t>(request) > 2) {
    state = 2;  // li r30,2 kept
  } else {
    state = request;  // mr r30,r11 -- the zero-extended word, not a narrowed copy
  }

  // lbz r11,12(r31) ; extsb r11,r11 -- the latched state is SIGNED.
  const int32_t latched = static_cast<int32_t>(static_cast<int8_t>(REX_LOAD_U8(self + kState)));
  // cmpw cr6,r30,r11 ; beq cr6,0x82b1d378
  if (static_cast<int32_t>(static_cast<uint32_t>(state)) != latched) {
    const uint32_t instance = REX_LOAD_U32(self + kInstance);  // lwz r3,8(r31)
    ctx.r3.u64 = instance;  // the `this` every bctrl below receives

    if (state == 0) {
      // cmplwi cr6,r3,0 ; beq cr6,0x82b1d36c -- no instance, nothing to release.
      if (instance != 0) {
        // lwz r11,0(r3) ; lwz r10,0(r11) ; mtctr r10 ; bctrl -- THE release entry.
        const uint32_t vtable = REX_LOAD_U32(instance + kVptr);
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotRelease);
        ctx.lr = 0x82B1D2AC;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        REX_STORE_U32(self + kInstance, 0);  // stw r29,8(r31)
        // bl 0x82b1f440 -- sees the null it just wrote, so it only does the bookkeeping.
        skate3::audio::GuestCall(ctx, base, sub_82B1F440, static_cast<uint64_t>(self));
      }
    } else if (state == 1) {
      // loc_82B1D2BC: cmpwi cr6,r30,1 ; bne cr6,0x82b1d354
      if (instance != 0) {
        // lwz r11,0(r3) ; lwz r10,8(r11) ; b 0x82b1d364
        const uint32_t vtable = REX_LOAD_U32(instance + kVptr);
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotResume);  // loc_82B1D364: mtctr r10 ; bctrl
        ctx.lr = 0x82B1D36C;
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      } else {
        // loc_82B1D2D8: r11 is still the extsb'd latched state here, not the request.
        // cmpwi cr6,r11,2 ; bne cr6,0x82b1d2ec ; lbz r11,13(r31) ; cmplwi cr6,r11,1
        const bool resume_from_suspend =
            latched == 2 && REX_LOAD_U8(self + kPrevState) == 1;
        if (!resume_from_suspend) {
          // loc_82B1D2EC: pick the record, clamping the index into 0 .. count-1.
          const uint32_t table = REX_LOAD_U32(self + kTable);            // lwz r9,4(r31)
          const uint32_t index = REX_LOAD_U32(self + kIndex);            // lwz r11,20(r31)
          const uint32_t count = REX_LOAD_U32(table + kTableCount);      // lwz r10,0(r9)
          uint64_t chosen;
          // cmpw cr6,r11,r10 ; blt cr6,0x82b1d308 -- SIGNED.
          if (static_cast<int32_t>(index) < static_cast<int32_t>(count)) {
            // loc_82B1D308: rlwinm r10,r11,1,31,31 picks bit 31 of the low word, addi r10,r10,-1
            // turns it into 0 or all-ones, and the AND clamps a negative index to zero.
            const uint64_t sign = (static_cast<uint64_t>(index) >> 31) & 1ull;
            const uint64_t mask = sign - 1ull;
            chosen = mask & static_cast<uint64_t>(index);
          } else {
            // addi r11,r10,-1 -- 64-bit, so a count of 0 leaves 0xFFFFFFFFFFFFFFFF here and the
            // masked multiplies below carry that through. Reproduced, not guarded.
            chosen = static_cast<uint64_t>(static_cast<int64_t>(count) - 1);
          }
          // loc_82B1D314: rlwinm r10,r11,1,0,30 ; add r11,r11,r10 ; rlwinm r11,r11,2,0,29 --
          // index*3 then *4, each rotate masked to 32 bits but the `add` a full 64-bit add.
          const uint64_t twice =
              (static_cast<uint64_t>(static_cast<uint32_t>(chosen)) << 1) & 0xFFFFFFFEull;
          const uint64_t thrice = chosen + twice;
          const uint64_t scaled =
              (static_cast<uint64_t>(static_cast<uint32_t>(thrice)) << 2) & 0xFFFFFFFCull;
          // add r11,r11,r9
          const uint32_t entry = static_cast<uint32_t>(scaled + static_cast<uint64_t>(table));
          // addi r4,r11,4 -- formed before the tag test and only read by sub_82B1F4C8.
          const uint64_t record = static_cast<uint64_t>(entry) + kRecordOffset;
          ctx.r4.u64 = record;
          const uint32_t tag = REX_LOAD_U16(entry + kRecordTag);  // lhz r10,4(r11)
          // cmplwi cr6,r10,65535 ; bne cr6,0x82b1d344
          if (tag == kNoRecord) {
            // The instance is already null on this path; the store is redundant in the original
            // and is kept.
            REX_STORE_U32(self + kInstance, 0);  // stw r29,8(r31)
            skate3::audio::GuestCall(ctx, base, sub_82B1F440, static_cast<uint64_t>(self));
          } else {
            // loc_82B1D344: bl 0x82b1f4c8 ; stw r3,8(r31)
            skate3::audio::GuestCall(ctx, base, sub_82B1F4C8, static_cast<uint64_t>(self), record);
            REX_STORE_U32(self + kInstance, ctx.r3.u32);
          }
        }
      }
    } else {
      // loc_82B1D354: state 2, the only remaining value after the clamp.
      // cmplwi cr6,r3,0 ; beq cr6,0x82b1d36c
      if (instance != 0) {
        const uint32_t vtable = REX_LOAD_U32(instance + kVptr);        // lwz r11,0(r3)
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotSuspend);             // lwz r10,4(r11)
        ctx.lr = 0x82B1D36C;                                           // loc_82B1D364: bctrl
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
      }
    }

    // loc_82B1D36C: every path above converges here. The byte at +12 is RELOADED, because the
    // callees and the indirect targets may have written it.
    const uint32_t displaced = REX_LOAD_U8(self + kState);             // lbz r11,12(r31)
    REX_STORE_U8(self + kState, static_cast<uint8_t>(state));          // stb r30,12(r31)
    REX_STORE_U8(self + kPrevState, static_cast<uint8_t>(displaced));  // stb r11,13(r31)
  }

  // loc_82B1D378: cmpwi cr6,r30,1 -- only a resume flushes the pending state records, and only
  // when an instance exists after everything above.
  if (state == 1 && REX_LOAD_U32(self + kInstance) != 0) {
    // bl 0x82b1f5f8 ; mr r30,r3 -- its result BECOMES the returned value.
    skate3::audio::GuestCall(ctx, base, sub_82B1F5F8, static_cast<uint64_t>(self));
    state = ctx.r3.u64;
  }

  // loc_82B1D398: the instance is loaded a third time; a null one returns 0 with no poke.
  const uint32_t live = REX_LOAD_U32(self + kInstance);  // lwz r3,8(r31)
  if (live == 0) {
    ctx.r3.u64 = 0;  // mr r3,r29
  } else {
    // loc_82B1D3B0: lwz r11,0(r3) ; lwz r10,24(r11) ; mtctr r10 ; bctrl
    ctx.r3.u64 = live;
    const uint32_t vtable = REX_LOAD_U32(live + kVptr);
    ctx.ctr.u64 = REX_LOAD_U32(vtable + kSlotPoke);
    ctx.lr = 0x82B1D3C0;
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    ctx.r3.u64 = state;  // mr r3,r30
  }

  // Both exits share this epilogue: addi r1,r1,112 ; b __restgprlr_29.
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
  ctx.r29.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR29);   // ld r29,-32(r1)
  ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR30);   // ld r30,-24(r1)
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR31);   // ld r31,-16(r1)
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot);  // lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                                // mtlr r12
}

// Never called: the SKATE3_PORT macro skips the shadow branch for a gate-labelled port. Kept so
// the file has the same shape as every other port; the write set it would declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B1D240
SKATE3_PORT(82B1D240, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
