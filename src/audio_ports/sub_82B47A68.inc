// sub_82B47A68  role: acquire a 32-byte slot in the manager's table for key r6 at priority f1 -- bump the slot already holding that key, else claim the first free one, else steal the lowest-priority slot, but only if its priority is below both the request and a pool ceiling, and only after calling the victim's owner back through the pointer parked in the slot   lifted: skate3_recomp.68.cpp:34985 (438 lines)
// STATUS: gate-1 (the victim's release callback is a function pointer read out of the slot: sub_82B47A68 indirect call, `bctrl` at 0x82B47D04)
// calls/session: boot 22  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B47A68.md
namespace port_82B47A68 {

// Nothing in docs/rw_audio_structs.h names this table, so the names below are local to this site
// and the offsets are plain.
constexpr uint32_t kClock = 0;    // manager+0 -> the object whose +8 double is stamped into a slot
constexpr uint32_t kSlots = 4;    // manager+4 -> the slot array; RE-READ before every walk
constexpr uint32_t kCount = 48;   // u8 slot count
constexpr uint32_t kClosed = 49;  // u8; nonzero means the table refuses every request

constexpr uint32_t kSlotBytes = 32;
constexpr uint32_t kStamp = 0;    // f64, copied from clock+8 whenever the slot is (re)claimed
constexpr uint32_t kScore = 8;    // f32, the priority the slot was claimed at
constexpr uint32_t kRelease = 12; // u32, the r5 argument; called through ctr when the slot is stolen
constexpr uint32_t kKey = 16;     // u32, the r6 argument; matched on, and the callback's own r3
constexpr uint32_t kUses = 24;    // u16, incremented on every acquire
constexpr uint32_t kInUse = 26;   // u8

constexpr uint32_t kClockStamp = 8;  // clock+8, f64

// Both constants computed as ((lis_imm & 0xFFFF) << 16) + offset, never read off by eye.
// lis r11,-32208 -> 0x82300000 ; addi r10,r11,-31232 ; lfd f13,2112(r10)
constexpr uint32_t kTieBreakInit = 0x822F8E40;
// lis r11,-32241 -> 0x820F0000 ; lfs f13,-10884(r11)
constexpr uint32_t kScoreCeiling = 0x820ED57C;

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

double LoadDouble(uint8_t* base, uint32_t addr) {  // lfd, read back as f64
  PPCRegister temp{};
  temp.u64 = REX_LOAD_U64(addr);
  return temp.f64;
}

// The running minimum: f0, f13 and r31 in the original.
struct Search {
  double best_score;  // f0, seeded with the REQUESTED priority, so nothing at or above it can win
  double best_stamp;  // f13, seeded from the pool double at kTieBreakInit
  uint64_t best;      // r31, zero until some slot beats the request
};

// One slot of the minimum search. The original holds five copies of this: the walk is unrolled four
// wide and followed by a scalar tail, and the five bodies are the same test sequence instruction
// for instruction (only the register scheduling differs -- there are no stores and no calls in any
// of them). The split is `rlwinm r10,r11,30,2,31` on count-4, +1, then `rlwinm r7,r10,2,0,29`, i.e.
// the unrolled half covers slots 0..count-count%4-1 and the tail the rest, so the union is exactly
// the ascending walk, in order. Hence one function, called from two counted loops below.
void ConsiderSlot(PPCContext& ctx, uint8_t* base, Search& search, uint64_t slot) {
  ctx.fpscr.disableFlushModeUnconditional();
  const double score = LoadSingle(base, uint32_t(slot) + kScore);  // lfs f12,8(r11)
  // fcmpu cr6,f12,f0 ; bge -- STRICTLY less, so an unordered compare (either side NaN) loses.
  if (score < search.best_score) {
    search.best_stamp = LoadDouble(base, uint32_t(slot) + kStamp);  // lfd f13,0(r11)
    search.best_score = score;                                      // fmr f0,f12
    search.best = slot;                                             // mr r31,r11
    return;
  }
  ctx.fpscr.disableFlushModeUnconditional();
  // fcmpu cr6,f12,f0 ; bne -- the tie-break runs only on an exact equality.
  if (score == search.best_score) {
    const double stamp = LoadDouble(base, uint32_t(slot) + kStamp);  // lfd f12,0(r11)
    // fcmpu cr6,f12,f13 ; bge -- the older stamp wins a tie.
    if (stamp < search.best_stamp) {
      search.best_stamp = stamp;  // fmr f13,f12
      search.best = slot;         // mr r31,r11
    }
  }
}

REX_FUNC(Native) {
  // __savegprlr_28 spills r28-r31 and the link register, and `stfd f31,-48(r1)` spills f31, all at
  // negative offsets from the ENTRY r1 -- that is, inside the 128 bytes the stwu below claims, so
  // none of it is a window. The harness compares r13-r31 and f14-f31, so the entry values are kept
  // here in locals and put back in the epilogue instead of being spilled and reloaded.
  const PPCRegister saved_r28 = ctx.r28;
  const PPCRegister saved_r29 = ctx.r29;
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  const PPCRegister saved_f31 = ctx.f31;
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;
  ctx.fpscr.disableFlushModeUnconditional();  // stfd f31,-48(r1)

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-128(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t manager = ctx.r3.u32;    // mr r30,r3
  const double requested = ctx.f1.f64;    // fmr f31,f1
  ctx.f31.f64 = requested;
  const uint32_t release = ctx.r5.u32;    // mr r28,r5 -- parked in the slot at +12
  const uint32_t key = ctx.r6.u32;        // mr r29,r6

  uint64_t result = 0;  // loc_82B47D44: li r3,0 -- every refusal lands here

  // lbz r11,49(r3) ; cmplwi cr6,r11,0 ; bne cr6,0x82b47d44
  if (REX_LOAD_U8(manager + kClosed) == 0) {
    // lbz r8,48(r3) -- a zero-extended byte, so every `cmpw` against it below is sign-agnostic.
    const uint32_t count = REX_LOAD_U8(manager + kCount);
    bool handled = false;

    // loc_82B47AA8 -- first walk: an in-use slot that already holds this key just gets its use
    // count bumped, and IS the result. cmpwi cr6,r8,0 ; ble skips the walk on an empty table.
    if (int32_t(count) > 0) {
      uint64_t slot = REX_LOAD_U32(manager + kSlots);  // lwz r3,4(r3)
      for (uint32_t i = 0; i < count; i++) {           // addi r10,r10,1 ; cmpw r10,r8 ; blt
        // lbz r11,26(r3) ; beq -- a free slot is not a match even if its key survived.
        if (REX_LOAD_U8(uint32_t(slot) + kInUse) != 0) {
          const uint32_t held = REX_LOAD_U32(uint32_t(slot) + kKey);  // lwz r11,16(r3)
          // cmplwi cr6,r11,0 ; beq -- a zero key never matches, so a zero r6 never matches either.
          // cmplw cr6,r11,r29 ; beq cr6,0x82b47b60
          if (held != 0 && held == key) {
            // loc_82B47B60
            const uint32_t uses = REX_LOAD_U16(uint32_t(slot) + kUses);  // lhz r11,24(r3)
            REX_STORE_U16(uint32_t(slot) + kUses, uint16_t(uses + 1));   // sth r10,24(r3)
            result = slot;  // r3 is still the walk pointer: the entry itself is the result
            handled = true;
            break;
          }
        }
        slot += kSlotBytes;  // addi r3,r3,32
      }
    }

    // loc_82B47AE8 -- second walk: the first slot whose in-use byte is clear is claimed outright.
    // The array base is RE-READ; the first walk consumed r3.
    if (!handled && int32_t(count) > 0) {
      uint64_t slot = REX_LOAD_U32(manager + kSlots);  // lwz r3,4(r30)
      for (uint32_t i = 0; i < count; i++) {           // addi r11,r11,1 ; cmpw r11,r8 ; blt
        // lbz r10,26(r3) ; cmplwi cr6,r10,0 ; beq cr6,0x82b47b78
        if (REX_LOAD_U8(uint32_t(slot) + kInUse) == 0) {
          // loc_82B47B78 -- the claim. This store order differs from the steal path's below (+12
          // before +26 before +16 there, +12/+26/+16 here) and both are kept as lifted.
          const uint32_t entry = uint32_t(slot);
          const uint32_t uses = REX_LOAD_U16(entry + kUses);  // lhz r11,24(r3)
          ctx.fpscr.disableFlushModeUnconditional();
          StoreSingle(base, entry + kScore, requested);           // stfs f31,8(r3)
          REX_STORE_U32(entry + kRelease, release);               // stw r28,12(r3)
          REX_STORE_U8(entry + kInUse, 1);                        // li r10,1 ; stb r10,26(r3)
          REX_STORE_U32(entry + kKey, key);                       // stw r29,16(r3)
          REX_STORE_U16(entry + kUses, uint16_t(uses + 1));       // sth r9,24(r3)
          const uint32_t clock = REX_LOAD_U32(manager + kClock);  // lwz r7,0(r30)
          // lfd f0,8(r7) ; stfd f0,0(r3) -- a bit copy through an FPR, reproduced as bits.
          REX_STORE_U64(entry + kStamp, REX_LOAD_U64(clock + kClockStamp));
          result = slot;
          handled = true;
          break;
        }
        slot += kSlotBytes;  // addi r3,r3,32
      }
    }

    // loc_82B47B04 -- no free slot: find the lowest-priority slot, tie broken by the oldest stamp.
    if (!handled) {
      ctx.fpscr.disableFlushModeUnconditional();
      Search search;
      search.best_score = requested;                         // fmr f0,f31
      search.best = 0;                                       // li r31,0
      search.best_stamp = LoadDouble(base, kTieBreakInit);   // lfd f13,2112(r10)
      uint64_t scanned = 0;                                  // li r7,0

      // cmpwi cr6,r8,4 ; blt cr6,0x82b47c88 -- fewer than four slots skips the unrolled half.
      if (int32_t(count) >= 4) {
        const uint64_t array = REX_LOAD_U32(manager + kSlots);  // lwz r9,4(r30)
        // rlwinm r10,r11,30,2,31 is srwi by 2 on count-4; addi r10,r10,1 is the trip count.
        const uint64_t groups = uint64_t(uint32_t(count - 4) >> 2) + 1;
        // rlwinm r7,r10,2,0,29 -- slots covered by the unrolled half, zero-extended.
        scanned = (groups << 2) & 0xFFFFFFFCull;
        for (uint64_t g = 0; g < groups; g++) {  // mtctr r10 ; bdnz 0x82b47b44
          // r11 runs at array+32+128g and r9 at array+72+128g; the four slots it tests are
          // -24(r11)/-32(r9)/0(r9)/32(r9) for the scores, i.e. array + 128g + {0,32,64,96}.
          for (uint32_t k = 0; k < 4; k++)
            ConsiderSlot(ctx, base, search, array + (g * 4 + k) * kSlotBytes);
        }
      }

      // loc_82B47C88 -- cmpw cr6,r7,r8 ; bge cr6,0x82b47ce0
      if (int32_t(uint32_t(scanned)) < int32_t(count)) {
        // lwz r10,4(r30) -- the array base is read a SECOND time here; nothing in the search
        // stores, so it can only differ if another thread moved the array mid-walk.
        const uint64_t array = REX_LOAD_U32(manager + kSlots);
        // rlwinm r11,r7,5,0,26 ; add r11,r11,r10 -- the shift is masked to 32 bits and the add is
        // 64-bit on zero-extended halves; with a u8 count neither can overflow.
        const uint64_t first = ((scanned << 5) & 0xFFFFFFE0ull) + array;
        const uint32_t remaining = count - uint32_t(scanned);  // subf r9,r7,r8
        for (uint32_t i = 0; i < remaining; i++)               // mtctr r9 ; bdnz 0x82b47ca4
          ConsiderSlot(ctx, base, search, first + uint64_t(i) * kSlotBytes);
      }

      // loc_82B47CE0 -- fcmpu cr6,f0,f31 ; bge cr6,0x82b47d44: nothing beat the request (or a NaN
      // left the compare unordered), so refuse. Then fcmpu cr6,f0,f13 ; bge: the winner must also
      // sit under the pool ceiling. The ceiling load happens only if the first test passed, which
      // the short circuit preserves.
      ctx.fpscr.disableFlushModeUnconditional();
      if (search.best_score < requested &&
          search.best_score < LoadSingle(base, kScoreCeiling)) {
        const uint32_t victim = uint32_t(search.best);
        ctx.ctr.u64 = REX_LOAD_U32(victim + kRelease);  // lwz r11,12(r31) ; mtctr r11
        ctx.r3.u64 = REX_LOAD_U32(victim + kKey);       // lwz r3,16(r31)
        ctx.lr = 0x82B47D08;                            // bctrl
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);

        // loc_82B47D08 -- the same claim as the free-slot path, in a DIFFERENT store order.
        const uint32_t uses = REX_LOAD_U16(victim + kUses);  // lhz r11,24(r31)
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, victim + kScore, requested);           // stfs f31,8(r31)
        REX_STORE_U32(victim + kKey, key);                       // stw r29,16(r31)
        REX_STORE_U32(victim + kRelease, release);               // stw r28,12(r31)
        REX_STORE_U8(victim + kInUse, 1);                        // li r10,1 ; stb r10,26(r31)
        REX_STORE_U16(victim + kUses, uint16_t(uses + 1));       // sth r8,24(r31)
        const uint32_t clock = REX_LOAD_U32(manager + kClock);   // lwz r6,0(r30)
        // lfd f0,8(r6) ; stfd f0,0(r31)
        REX_STORE_U64(victim + kStamp, REX_LOAD_U64(clock + kClockStamp));
        result = search.best;  // mr r3,r31, issued before the last two stores
      }
    }
  }

  ctx.r3.u64 = result;
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,128
  ctx.fpscr.disableFlushModeUnconditional();
  ctx.f31 = saved_f31;  // lfd f31,-48(r1)
  ctx.r28 = saved_r28;  // __restgprlr_28
  ctx.r29 = saved_r29;
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
  ctx.lr = entry_lr;
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B47A68
SKATE3_PORT(82B47A68, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
