// sub_82B2F150  role: (re)acquire this object's mix slots -- walk the six primary slots at +720 (stride 60), write each one's four gains at +504 (stride 36) from the negated pair at +436/+460, and when the slot's generation word no longer matches its index+1 ask sub_82B3D668 for a fresh request block at slot-20 and republish the slot (cursor and flag cleared, generation set, the two words at +0/+4 copied to +12/+32); then, once per object (latched by the byte at +1088), do the same for the secondary set at +164 with the count at +1089; any failed request aborts with r3 = 0   lifted: skate3_recomp.67.cpp:42292 (222 lines)
// STATUS: gate-1 (sub_82B3D668 allocates: it loads a global at r20+30252, takes that object's +36 allocator, and calls its vtable slot +4 with a 128-byte size through `bctrl` at 0x82B3D6F8 -- one level down, and an allocation can never be replayed on rewound memory)
// calls/session: boot 4  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2F150.md
namespace port_82B2F150 {

// Nothing in docs/rw_audio_structs.h names this ~1090-byte object, so the offsets stay plain
// numbers. Only the fields whose use is unambiguous get a name.
//
// The primary set: three parallel tables indexed by the same 0..5 counter, with three different
// strides. The lifted form walks them with r30/r29/r31, and this port keeps that view because
// the request block handed to sub_82B3D668 is r31-20, i.e. 20 bytes BEFORE the slot pointer.
constexpr uint32_t kPrimaryIndices = 388;  // addi r30,r3,388 -- u32 per entry, stride 4
constexpr uint32_t kPrimaryGains = 504;    // addi r29,r3,504 -- four singles per entry, stride 36
constexpr uint32_t kPrimarySlots = 720;    // addi r31,r3,720 -- stride 60
constexpr uint32_t kPrimaryCount = 6;      // cmpwi cr6,r28,6
constexpr uint32_t kPrimaryIndexStride = 4;
constexpr uint32_t kPrimaryGainStride = 36;
constexpr uint32_t kSlotStride = 60;

// Offsets from the slot pointer (r31/r30 base + stride*i), shared by both sets.
constexpr int32_t kSlotCursor = 16;      // stw r26,16(r31) -- cleared on republish
constexpr int32_t kSlotGeneration = 20;  // the word the primary skip test compares
constexpr int32_t kSlotFlag = 36;        // stb r26,36(r31) -- cleared on republish
constexpr int32_t kSlotRequest = -20;    // addi r3,r31,-20 -- sub_82B3D668's first argument

// The two singles the primary gains are built from, read off the index pointer.
constexpr int32_t kPrimaryGainSourceA = 48;  // lfs f0,48(r30)
constexpr int32_t kPrimaryGainSourceB = 72;  // lfs f13,72(r30)

// The secondary set. r30 starts at +1068, and the iteration reads a single at r30-8 and an index
// at r30+4, so the two tables it walks are at +1060 and +1072, both stride 4.
constexpr uint32_t kSecondaryGains = 92;    // addi r29,r27,92 -- two singles per entry, stride 24
constexpr uint32_t kSecondarySlots = 164;   // addi r31,r27,164 -- stride 60
constexpr uint32_t kSecondaryWalk = 1068;   // addi r30,r27,1068 -- see kSecondary*Offset below
constexpr int32_t kSecondaryGainOffset = -8;   // lfs f13,-8(r30)  -> object + 1060 + 4*i
constexpr int32_t kSecondaryIndexOffset = 4;   // lwz r11,4(r30)   -> object + 1072 + 4*i
constexpr uint32_t kSecondaryGainStride = 24;
constexpr uint32_t kSecondaryWalkStride = 4;
constexpr uint32_t kSecondaryShared = 1084;  // lfs f0,1084(r27) -- reloaded every iteration
constexpr uint32_t kSecondaryLatch = 1088;   // lbz/stb 1088(r27) -- nonzero skips the whole set
constexpr uint32_t kSecondaryCount = 1089;   // lbz 1089(r27) -- reloaded at the foot of the loop

// The kind selector sub_82B3D668 takes in r4: the table index plus a per-set bias.
constexpr uint64_t kPrimaryKindBias = 3;    // addi r4,r11,3
constexpr uint64_t kSecondaryKindBias = 2;  // addi r4,r11,2

// lis r11,-32208 ; addi r10,r11,-31232 ; lfs f31,876(r10)
// ((-32208 & 0xFFFF) << 16) + (-31232 + 876) = 0x82300000 - 30356. Computed, not read by eye.
constexpr uint32_t kGainConstant = 0x822F896C;

constexpr uint32_t kFrameBytes = 144;  // stwu r1,-144(r1)
// __savegprlr_26 / __restgprlr_26 slots, plus the f31 spill just below them. All negative
// offsets from the ENTRY sp, all inside the 144 bytes this call allocates.
constexpr uint32_t kSaveR26 = 56;
constexpr uint32_t kSaveR27 = 48;
constexpr uint32_t kSaveR28 = 40;
constexpr uint32_t kSaveR29 = 32;
constexpr uint32_t kSaveR30 = 24;
constexpr uint32_t kSaveR31 = 16;
constexpr uint32_t kLinkSlot = 8;
constexpr uint32_t kSaveF31 = 64;

// lfs: the word reinterpreted as a single, widened to double, exactly as lifted.
inline double LoadSingle(uint8_t* base, uint32_t address) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(address);
  return static_cast<double>(temp.f32);
}

// stfs: the double rounded back to a single, its bits stored.
inline void StoreSingle(uint8_t* base, uint32_t address, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(address, temp.u32);
}

// fneg is a sign-bit flip on the doubleword, not `-value`: it must negate a NaN's sign too.
inline double Fneg(double value) {
  PPCRegister temp{};
  temp.f64 = value;
  temp.u64 ^= 0x8000000000000000ull;
  return temp.f64;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r27,r3

  // The frame is reproduced because sub_82B3D668 runs on it. Every slot written below is inside
  // the 144 bytes this call allocates, so none of them is a window; the spills are written and
  // read back exactly as __savegprlr_26/__restgprlr_26 do, even though this body keeps r26-r31
  // and f31 in C++ locals and never clobbers the registers themselves.
  const uint32_t entry_sp = ctx.r1.u32;
  ctx.r12.u64 = ctx.lr;                             // mflr r12
  REX_STORE_U64(entry_sp - kSaveR26, ctx.r26.u64);  // bl 0x82f52110 -> std r26,-56(r1)
  REX_STORE_U64(entry_sp - kSaveR27, ctx.r27.u64);  //                  std r27,-48(r1)
  REX_STORE_U64(entry_sp - kSaveR28, ctx.r28.u64);  //                  std r28,-40(r1)
  REX_STORE_U64(entry_sp - kSaveR29, ctx.r29.u64);  //                  std r29,-32(r1)
  REX_STORE_U64(entry_sp - kSaveR30, ctx.r30.u64);  //                  std r30,-24(r1)
  REX_STORE_U64(entry_sp - kSaveR31, ctx.r31.u64);  //                  std r31,-16(r1)
  REX_STORE_U32(entry_sp - kLinkSlot, ctx.r12.u32); //                  stw r12,-8(r1)
  ctx.fpscr.disableFlushModeUnconditional();
  REX_STORE_U64(entry_sp - kSaveF31, ctx.f31.u64);  // stfd f31,-64(r1)
  const uint32_t frame = entry_sp - kFrameBytes;    // stwu r1,-144(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  // lfs f31,876(r10) -- one pool constant, held in a callee-saved register across both calls, so
  // a single load before the loops is what the original does too.
  const double gain_constant = LoadSingle(base, kGainConstant);

  // li r26,0 -- the zero every `stw r26`/`stb r26` below stores.
  bool ok = true;

  // loc_82B2F184: six primary slots, counter r28, pointers r30/r29/r31 advancing by 4/36/60.
  for (uint32_t i = 0; i < kPrimaryCount && ok; i++) {
    const uint32_t index_at = self + kPrimaryIndices + kPrimaryIndexStride * i;  // r30
    const uint32_t gains = self + kPrimaryGains + kPrimaryGainStride * i;        // r29
    const uint32_t slot = self + kPrimarySlots + kSlotStride * i;                // r31

    ctx.fpscr.disableFlushModeUnconditional();
    const double source_a = LoadSingle(base, index_at + kPrimaryGainSourceA);  // lfs f0,48(r30)
    const uint32_t index = REX_LOAD_U32(index_at);                             // lwz r11,0(r30)
    const double source_b = LoadSingle(base, index_at + kPrimaryGainSourceB);  // lfs f13,72(r30)
    const double negated_a = Fneg(source_a);  // fneg f12,f0
    const double negated_b = Fneg(source_b);  // fneg f11,f13
    StoreSingle(base, gains - 4, negated_a);  // stfs f12,-4(r29)
    StoreSingle(base, gains + 0, negated_b);  // stfs f11,0(r29)
    // addi r4,r11,3 -- formed here in the original, before the skip test, and only ever read by
    // the call below. 64-bit: the index is a zero-extended u32 load.
    const uint64_t kind = static_cast<uint64_t>(index) + kPrimaryKindBias;
    ctx.r4.u64 = kind;
    StoreSingle(base, gains + 4, negated_a);       // stfs f12,4(r29)
    StoreSingle(base, gains + 8, gain_constant);   // stfs f31,8(r29)

    // lwz r10,20(r31) ; lwz r11,0(r30) -- the index is read a SECOND time here; keep the reload.
    const uint32_t generation = REX_LOAD_U32(slot + kSlotGeneration);
    const uint64_t wanted = static_cast<uint64_t>(REX_LOAD_U32(index_at)) + 1;  // addi r9,r11,1
    // cmpw cr6,r10,r9 ; beq cr6,0x82b2f1f8 -- SIGNED 32-bit compare of the low words.
    if (static_cast<int32_t>(generation) == static_cast<int32_t>(static_cast<uint32_t>(wanted))) {
      continue;
    }

    // addi r3,r31,-20 ; bl 0x82b3d668 -- this is the gate-1 call: sub_82B3D668 allocates.
    skate3::audio::GuestCall(ctx, base, sub_82B3D668,
                             static_cast<uint64_t>(slot + kSlotRequest), kind);
    // clrlwi r11,r3,24 ; cmplwi cr6,r11,0 ; beq cr6,0x82b2f2b4 -- only the low byte is tested.
    if ((ctx.r3.u32 & 0xFF) == 0) {
      ok = false;
      break;
    }

    // Republish. Everything here is re-read after the call, because the callee wrote through the
    // request block at slot-20 and may have moved the index.
    const uint32_t fresh_index = REX_LOAD_U32(index_at);        // lwz r11,0(r30)
    const uint32_t word0 = REX_LOAD_U32(slot + 0);              // lwz r10,0(r31)
    const uint32_t word4 = REX_LOAD_U32(slot + 4);              // lwz r9,4(r31)
    const uint64_t next_generation = static_cast<uint64_t>(fresh_index) + 1;  // addi r11,r11,1
    REX_STORE_U32(slot + kSlotCursor, 0);                       // stw r26,16(r31)
    REX_STORE_U32(slot + kSlotGeneration,
                  static_cast<uint32_t>(next_generation));      // stw r11,20(r31)
    REX_STORE_U8(slot + kSlotFlag, 0);                          // stb r26,36(r31)
    REX_STORE_U32(slot + 12, word0);                            // stw r10,12(r31)
    REX_STORE_U32(slot + 32, word4);                            // stw r9,32(r31)
  }

  if (ok) {
    // lbz r11,1088(r27) ; bne cr6,0x82b2f2a4 -- already latched: the secondary set is skipped
    // entirely, latch and all, and the function returns 1.
    if (REX_LOAD_U8(self + kSecondaryLatch) == 0) {
      // lbz r11,1089(r27) ; beq cr6,0x82b2f29c -- a zero count still sets the latch.
      uint32_t i = 0;
      if (REX_LOAD_U8(self + kSecondaryCount) != 0) {
        // loc_82B2F238: pointers r30/r29/r31 advancing by 4/24/60.
        do {
          const uint32_t walk = self + kSecondaryWalk + kSecondaryWalkStride * i;      // r30
          const uint32_t gains = self + kSecondaryGains + kSecondaryGainStride * i;    // r29
          const uint32_t slot = self + kSecondarySlots + kSlotStride * i;              // r31

          ctx.fpscr.disableFlushModeUnconditional();
          const double shared = LoadSingle(base, self + kSecondaryShared);      // lfs f0,1084(r27)
          const double per_entry = LoadSingle(base, walk + kSecondaryGainOffset);  // lfs f13,-8(r30)
          StoreSingle(base, gains - 4, per_entry);  // stfs f13,-4(r29) -- not negated here
          StoreSingle(base, gains + 0, shared);     // stfs f0,0(r29)

          // lwz r11,4(r30) ; addi r4,r11,2
          const uint32_t index = REX_LOAD_U32(walk + kSecondaryIndexOffset);
          const uint64_t kind = static_cast<uint64_t>(index) + kSecondaryKindBias;
          // addi r3,r31,-20 ; bl 0x82b3d668 -- the same gate-1 call.
          skate3::audio::GuestCall(ctx, base, sub_82B3D668,
                                   static_cast<uint64_t>(slot + kSlotRequest), kind);
          // clrlwi r11,r3,24 ; beq cr6,0x82b2f2b4
          if ((ctx.r3.u32 & 0xFF) == 0) {
            ok = false;
            break;
          }

          // lwzu r11,4(r30) -- the SAME address the r4 index came from, read again after the
          // call. The update to r30 that `lwzu` folds in is the loop's own advance.
          const uint32_t fresh_index = REX_LOAD_U32(walk + kSecondaryIndexOffset);
          i++;                                                        // addi r28,r28,1
          REX_STORE_U32(slot + kSlotCursor, 0);                       // stw r26,16(r31)
          REX_STORE_U8(slot + kSlotFlag, 0);                          // stb r26,36(r31)
          const uint32_t word4 = REX_LOAD_U32(slot + 4);              // lwz r10,4(r31)
          const uint32_t word0 = REX_LOAD_U32(slot + 0);              // lwz r9,0(r31)
          // No +1 here, unlike the primary set: the raw index becomes the generation.
          REX_STORE_U32(slot + kSlotGeneration, fresh_index);         // stw r11,20(r31)
          REX_STORE_U32(slot + 32, word4);                            // stw r10,32(r31)
          REX_STORE_U32(slot + 12, word0);                            // stw r9,12(r31)
          // lbz r8,1089(r27) ; cmpw cr6,r28,r8 ; blt -- the count is RELOADED every iteration.
        } while (static_cast<int32_t>(i) <
                 static_cast<int32_t>(REX_LOAD_U8(self + kSecondaryCount)));
      }
      // loc_82B2F29C: li r11,1 ; stb r11,1088(r27) -- reached on success only.
      if (ok) REX_STORE_U8(self + kSecondaryLatch, 1);
    }
  }

  // loc_82B2F2A4 (li r3,1) and loc_82B2F2B4 (li r3,0) share this epilogue verbatim.
  ctx.r3.s64 = ok ? 1 : 0;
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,144
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.f31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveF31);  // lfd f31,-64(r1)
  ctx.r26.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR26);  // b 0x82f52160 -> ld r26,-56(r1)
  ctx.r27.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR27);  //                 ld r27,-48(r1)
  ctx.r28.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR28);  //                 ld r28,-40(r1)
  ctx.r29.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR29);  //                 ld r29,-32(r1)
  ctx.r30.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR30);  //                 ld r30,-24(r1)
  ctx.r31.u64 = REX_LOAD_U64(ctx.r1.u32 - kSaveR31);  //                 ld r31,-16(r1)
  ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 - kLinkSlot); //                 lwz r12,-8(r1)
  ctx.lr = ctx.r12.u64;                               //                 mtlr r12
}

// Never called: the SKATE3_PORT macro skips the shadow branch for a gate-labelled port. Kept so
// the file has the same shape as every other port. The write set this WOULD declare -- and why
// it is enumerable even though the gate blocks it -- is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B2F150
SKATE3_PORT(82B2F150, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
