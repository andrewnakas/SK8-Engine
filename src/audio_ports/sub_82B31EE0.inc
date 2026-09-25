// sub_82B31EE0  role: pump one stream's segment ring -- retire finished slots from the retire index, republish the object's rate and its two time bounds (or the idle sentinel when nothing is queued), then walk the ring from +469 driving each active slot through its state machine (1 -> prepare, 2 -> submit/refill/wrap through three different callees, 3 -> wrapped), stopping when the ring closes, a slot goes inactive, a callee refuses, or the callees' shared counter passes 8192   lifted: skate3_recomp.67.cpp:49140 (608 lines)
// STATUS: gate-1 (RtlEnterCriticalSection / RtlLeaveCriticalSection at depth 2, reached through the submit callees via sub_82EC1378; census closure is 198 functions wide)
// calls/session: boot 206045  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B31EE0.md
namespace port_82B31EE0 {

// The object (r3/r31). Nothing in docs/rw_audio_structs.h names this ~476-byte record; the names
// here are local to this site. sub_82B32870 in this tree writes +432..+477 of what looks like the
// same record, which is the only cross-check available.
constexpr uint32_t kClock = 8;          // u32 -> object whose f64 at +8 is the current time
constexpr uint32_t kOwner = 12;         // u32 -> owner; its u8 at +71 == 2 parks this call
constexpr uint32_t kSentinelWord = 48;  // u32, always set to kSentinel here
constexpr uint32_t kCurrentRate = 52;   // f32, republished from +424 on every call
constexpr uint32_t kStartTime = 56;     // f64
constexpr uint32_t kEndTime = 64;       // f64
constexpr uint32_t kRecords = 96;       // u32 -> array of 80-byte records, same index as the ring
constexpr uint32_t kSelectorFlag = 113; // u8 at object + 16*u8[+473] + 113; nonzero parks the walk
constexpr uint32_t kRate = 424;         // f32, the rate this call publishes
constexpr uint32_t kDivisor = 428;      // f32, divides both sample counts into seconds
constexpr uint32_t kStartSamples = 432; // s32
constexpr uint32_t kEndSamples = 436;   // s32
constexpr uint32_t kDefaultSource = 440;// u32 -> an f32 copied into +424 when the gate below is 0
constexpr uint32_t kRefreshGate = 444;  // u32 -> u32; zero means "refresh +424 from +440"
constexpr uint32_t kTableOffset = 464;  // u16, the ring's byte offset INSIDE the object
constexpr uint32_t kRetireIndex = 468;  // u8, advanced past every state-4 slot
constexpr uint32_t kFirstActive = 469;  // u8, the index both walks start from and stop at
constexpr uint32_t kRingSize = 470;     // u8, the index the ring wraps to zero at
constexpr uint32_t kSelector = 473;     // u8, scales by 16 into the kSelectorFlag address

constexpr uint32_t kOwnerState = 71;  // u8 on the owner
constexpr uint32_t kOwnerRate = 56;   // f32 on the owner, copied into the record's sink

// A ring slot, 48 bytes.
constexpr uint32_t kSlotTime = 0;   // f64
constexpr uint32_t kSlotWork = 20;  // u32
constexpr uint32_t kSlotEnd = 24;   // u32
constexpr uint32_t kState = 46;     // u8: 0 empty, 1 -> 2 -> 3 in flight, 4 finished

// An 80-byte record at u32[object+96] + 80*index.
constexpr uint32_t kRecordCursor = 20;  // u32, compared against the slot's +24 and +20
constexpr uint32_t kRecordSink = 36;    // u32 -> object whose f32 at +8 takes the owner's rate
constexpr uint32_t kRecordKind = 72;    // u8, 3 selects the time fixup

// Constants, each ((lis_imm & 0xFFFF) << 16) + offset, computed and not read off by eye.
// lis r11,-32208 -> 0x82300000 ; addi r9,r11,-31232 -> 0x822F8600 ; lfd 256(r9)
constexpr uint32_t kFarFuture = 0x822F8700;
// the same 0x822F8600 base ; lfd f31,4192(r10)
constexpr uint32_t kTimeBump = 0x822F9660;
// lis r10,32759 -> 0x7FF70000 ; ori r8,r10,65521
constexpr uint32_t kSentinel = 0x7FF7FFF1;

constexpr uint32_t kFrameBytes = 176;  // stwu r1,-176(r1)
constexpr uint32_t kFlagSlot = 80;     // addi r6,r1,80 -- sub_82B33970's out byte
constexpr uint32_t kCounterSlot = 88;  // addi r5,r1,88 -- the callees' shared counter, and the
                                       // int-to-double spill earlier in the same call

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

double LoadDouble(uint8_t* base, uint32_t addr) {  // lfd
  PPCRegister temp{};
  temp.u64 = REX_LOAD_U64(addr);
  return temp.f64;
}

void StoreDouble(uint8_t* base, uint32_t addr, double value) {  // stfd
  PPCRegister temp{};
  temp.f64 = value;
  REX_STORE_U64(addr, temp.u64);
}

// rotlwi r10,r11,1 ; add r11,r11,r10 ; rlwinm r11,r11,4,0,27 ; add r11,r11,r9 ; add r30,r11,r31 --
// a 48-byte stride: 3*index, then scaled by 16 and masked to 32 bits, then the ring's own offset
// inside the object, then the object. Two other sites spell the doubling `rlwinm ,1,23,30` and
// `rlwinm ,1,0,30`; for the byte-masked index every caller passes, all three are 2*index.
uint64_t SlotAt(uint64_t object, uint64_t table, uint32_t index) {
  const uint64_t tripled = uint64_t(index) + uint64_t(__builtin_rotateleft32(index, 1));
  return ((uint64_t(uint32_t(tripled) << 4)) & 0xFFFFFFF0ull) + table + object;
}

// rlwinm r11,r27,2,22,29 ; add r11,r29,r11 ; rlwinm r11,r11,4,0,27 ; add r28,r11,r10 --
// an 80-byte stride off u32[object+96]: 4*index + index, scaled by 16.
uint64_t RecordAt(uint64_t records, uint32_t index) {
  const uint64_t fived = uint64_t(index) + ((uint64_t(index) << 2) & 0x3FCull);
  return ((uint64_t(uint32_t(fived) << 4)) & 0xFFFFFFF0ull) + records;
}

// addi r11,r11,1 ; clrlwi r10,r11,24 ; subf r6,r10,r9 ; subfic r5,r6,0 ; subfe r3,r4,r4 ;
// and r27,r3,r10 -- increment the byte index and wrap to zero when it reaches u8[object+470]. The
// subfic/subfe pair turns "the low word of limit-index is zero" into an all-ones-or-zero mask and
// the `and` applies it, so this is a branchless `++i == limit ? 0 : i`.
uint32_t NextIndex(uint32_t index, uint32_t limit) {
  const uint32_t bumped = (index + 1) & 0xFF;
  return uint32_t(limit - bumped) == 0 ? 0 : bumped;
}

// lbz ; cmpwi 4 ; beq -> 0 ; cmpwi 0 ; li 1 ; bne -> 1 ; else 0. The state byte is zero-extended,
// so the signed compares are the unsigned ones.
bool SlotActive(uint32_t state) { return state != 4 && state != 0; }

// lbz r11,473(r31) ; rotlwi r11,r11,4 ; add r10,r11,r31 ; lbz r9,113(r10) -- re-read at both sites
// that test it, so it is a function and not a cached value.
uint32_t SelectorFlag(uint8_t* base, uint32_t object) {
  const uint32_t selector = REX_LOAD_U8(object + kSelector);
  const uint64_t scaled = uint64_t(__builtin_rotateleft32(selector, 4)) + uint64_t(object);
  return REX_LOAD_U8(uint32_t(scaled) + kSelectorFlag);
}

// Everything between the owner-state test and loc_82B3230C. Every `return` here is that label: the
// function stores nothing on the way out and leaves r3 as whatever the last call left.
void Pump(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t frame) {
  // bl 0x82b32480 -- r3 is still the object, and r4-r10 are still the caller's, as in the original.
  skate3::audio::GuestCall(ctx, base, sub_82B32480, object);

  // Retire: while the slot at the retire index reads state 4, release it and step the index on.
  {
    uint32_t index = REX_LOAD_U8(object + kRetireIndex);    // lbz r11,468(r31)
    uint32_t table = REX_LOAD_U16(object + kTableOffset);   // lhz r9,464(r31)
    // lbz r9,46(r10) ; cmplwi cr6,r9,4 ; bne cr6,0x82b31f8c
    while (REX_LOAD_U8(uint32_t(SlotAt(object, table, index)) + kState) == 4) {
      // loc_82B31F34 -- the index is re-read for the call, and again after it, because the callee
      // may move it.
      skate3::audio::GuestCall(ctx, base, sub_82B33F98, object, REX_LOAD_U8(object + kRetireIndex));
      const uint32_t bumped = REX_LOAD_U8(object + kRetireIndex);  // lbz r11,468(r31)
      const uint32_t ring = REX_LOAD_U8(object + kRingSize);       // lbz r9,470(r31)
      index = NextIndex(bumped, ring);
      REX_STORE_U8(object + kRetireIndex, uint8_t(index));         // stb r4,468(r31)
      table = REX_LOAD_U16(object + kTableOffset);                 // lhz r9,464(r31) -- re-read
    }
  }

  // loc_82B31F8C -- the walk starts at the first-active index.
  uint32_t index = REX_LOAD_U8(object + kFirstActive);  // lbz r11,469(r31) ; mr r27,r11
  uint64_t slot = SlotAt(object, REX_LOAD_U16(object + kTableOffset), index);

  // loc_82B31FC8 -- cmplwi cr6,r11,0 ; bne cr6,0x82b32028
  if (!SlotActive(REX_LOAD_U8(uint32_t(slot) + kState))) {
    // Nothing in flight: publish the idle bounds and leave.
    const uint32_t gate = REX_LOAD_U32(object + kRefreshGate);  // lwz r11,444(r31)
    // lwz r10,0(r11) ; cmplwi cr6,r10,0 ; bne cr6,0x82b31ff0
    if (REX_LOAD_U32(gate) == 0) {
      const uint32_t source = REX_LOAD_U32(object + kDefaultSource);  // lwz r11,440(r31)
      ctx.fpscr.disableFlushModeUnconditional();
      // lfs f0,0(r11) ; stfs f0,424(r31) -- a single round trip, not a bit copy.
      StoreSingle(base, object + kRate, LoadSingle(base, source));
    }
    // loc_82B31FF0
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, object + kCurrentRate, LoadSingle(base, object + kRate));  // lfs 424 ; stfs 52
    REX_STORE_U32(object + kSentinelWord, kSentinel);      // stw r8,48(r31)
    const uint64_t far_bits = REX_LOAD_U64(kFarFuture);    // lfd f0,256(r9)
    REX_STORE_U64(object + kEndTime, far_bits);            // stfd f0,64(r31)
    REX_STORE_U64(object + kStartTime, far_bits);          // stfd f0,56(r31)
    return;
  }

  // loc_82B32028 -- something is in flight: publish the rate and the two bounds in seconds.
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, object + kCurrentRate, LoadSingle(base, object + kRate));  // lfs 424 ; stfs 52
  REX_STORE_U32(object + kSentinelWord, kSentinel);        // stw r10,48(r31)
  const double divisor = LoadSingle(base, object + kDivisor);  // lfs f13,428(r31)
  // extsw ; std r8,88(r1) ; lfd f12,88(r1) ; fcfid -- the int-to-double path spills through this
  // call's OWN frame, the same slot the counter below uses, so neither is a window.
  REX_STORE_U64(frame + kCounterSlot,
                uint64_t(int64_t(int32_t(REX_LOAD_U32(object + kEndSamples)))));
  PPCRegister end_bits{};
  end_bits.u64 = REX_LOAD_U64(frame + kCounterSlot);
  REX_STORE_U64(frame + kCounterSlot,
                uint64_t(int64_t(int32_t(REX_LOAD_U32(object + kStartSamples)))));
  PPCRegister start_bits{};
  start_bits.u64 = REX_LOAD_U64(frame + kCounterSlot);
  const double end_samples = double(end_bits.s64);      // fcfid f10,f12
  const double start_samples = double(start_bits.s64);  // fcfid f9,f11
  // fdiv, not fdivs: both quotients keep double precision.
  StoreDouble(base, object + kEndTime, end_samples / divisor);      // stfd f8,64(r31)
  StoreDouble(base, object + kStartTime, start_samples / divisor);  // stfd f7,56(r31)

  // lwz r5,20(r30) ; cmpwi cr6,r5,0 ; bne cr6,0x82b32100
  if (REX_LOAD_U32(uint32_t(slot) + kSlotWork) == 0) {
    // loc_82B32094 -- step forward to the first slot that has work. The ring size and the table
    // offset are read ONCE here, unlike the retire loop above.
    const uint32_t start_index = index & 0xFF;                  // clrlwi r11,r27,24 ; mr r7,r11
    const uint32_t ring = REX_LOAD_U8(object + kRingSize);      // lbz r9,470(r31)
    const uint32_t table = REX_LOAD_U16(object + kTableOffset); // lhz r8,464(r31)
    for (;;) {
      index = NextIndex(index & 0xFF, ring);
      slot = SlotAt(object, table, index);
      // cmplw cr6,r11,r7 ; beq cr6,0x82b3230c -- the slot address is formed before the test.
      if (index == start_index) return;
      const uint32_t state = REX_LOAD_U8(uint32_t(slot) + kState);  // lbz r10,46(r30)
      if (!SlotActive(state)) return;
      // lwz r10,20(r30) ; cmpwi cr6,r10,0 ; beq cr6,0x82b32094
      if (REX_LOAD_U32(uint32_t(slot) + kSlotWork) != 0) break;
    }
  }

  // loc_82B32100
  REX_STORE_U32(frame + kCounterSlot, 0);  // stw r11,88(r1) -- the counter the callees advance
  ctx.fpscr.disableFlushModeUnconditional();
  const double time_bump = LoadDouble(base, kTimeBump);  // lfd f31,4192(r10)
  const double far_future = LoadDouble(base, kFarFuture);  // lfd f30,256(r10)

  for (;;) {
    // loc_82B32120
    if (!SlotActive(REX_LOAD_U8(uint32_t(slot) + kState))) return;
    if (SelectorFlag(base, object) != 0) return;

    const uint32_t masked = index & 0xFF;                        // clrlwi r29,r27,24
    const uint32_t records = REX_LOAD_U32(object + kRecords);    // lwz r10,96(r31)
    const uint64_t record = RecordAt(records, masked);           // r28
    const uint32_t sink = REX_LOAD_U32(uint32_t(record) + kRecordSink);  // lwz r11,36(r28)
    // cmplwi cr6,r11,0 ; beq cr6,0x82b32190
    if (sink != 0) {
      const uint32_t owner = REX_LOAD_U32(object + kOwner);  // lwz r10,12(r31)
      ctx.fpscr.disableFlushModeUnconditional();
      // lfs f0,56(r10) ; stfs f0,8(r11)
      StoreSingle(base, sink + 8, LoadSingle(base, owner + kOwnerRate));
    }

    // loc_82B32190 -- state 1: prepare, then promote to 2.
    if (REX_LOAD_U8(uint32_t(slot) + kState) == 1) {
      skate3::audio::GuestCall(ctx, base, sub_82B33D40, object, masked, frame + kCounterSlot);
      if ((ctx.r3.u32 & 0xFF) == 0) return;  // clrlwi r11,r3,24 ; cmplwi ; beq
      REX_STORE_U8(uint32_t(slot) + kState, 2);  // stb r25,46(r30)
      // Four nested tests, all of which must hold before the slot's timestamp is seeded.
      if (REX_LOAD_U8(uint32_t(record) + kRecordKind) == 3) {  // lbz r11,72(r28) ; cmplwi 3
        ctx.fpscr.disableFlushModeUnconditional();
        // lfd f0,0(r30) ; fcmpu cr6,f0,f30 ; bne -- an unordered compare skips, as `==` does.
        if (LoadDouble(base, uint32_t(slot) + kSlotTime) == far_future) {
          // lbz r11,469(r31) ; cmplw cr6,r29,r11 ; bne
          if (masked == REX_LOAD_U8(object + kFirstActive)) {
            const uint32_t clock = REX_LOAD_U32(object + kClock);  // lwz r11,8(r31)
            // lfd f0,8(r11) ; fadd f13,f0,f31 ; stfd f13,0(r30) -- a DOUBLE add.
            StoreDouble(base, uint32_t(slot) + kSlotTime,
                        LoadDouble(base, clock + 8) + time_bump);
          }
        }
      }
    }

    // loc_82B321F0 -- state 2: one of three callees, chosen by comparing the record's cursor with
    // the slot's two markers. `advance` is false on the paths that jump straight to the counter
    // test without stepping the ring.
    bool advance = true;
    if (REX_LOAD_U8(uint32_t(slot) + kState) == 2 && SelectorFlag(base, object) == 0) {
      const uint32_t cursor = REX_LOAD_U32(uint32_t(record) + kRecordCursor);  // lwz r11,20(r28)
      const uint32_t at_end = REX_LOAD_U32(uint32_t(slot) + kSlotEnd);         // lwz r10,24(r30)
      // cmpw cr6,r11,r10 ; bne cr6,0x82b32244 -- SIGNED compares throughout this dispatch.
      if (int32_t(cursor) == int32_t(at_end)) {
        skate3::audio::GuestCall(ctx, base, sub_82B33870, object, masked, frame + kCounterSlot);
        if ((ctx.r3.u32 & 0xFF) == 0) return;
        advance = false;  // b 0x82b32300
      } else {
        // loc_82B32244
        const uint32_t at_work = REX_LOAD_U32(uint32_t(slot) + kSlotWork);  // lwz r10,20(r30)
        if (int32_t(cursor) == int32_t(at_work)) {
          skate3::audio::GuestCall(ctx, base, sub_82B33970, object, masked, frame + kCounterSlot,
                                   frame + kFlagSlot);
          if ((ctx.r3.u32 & 0xFF) == 0) return;
          // lbz r10,80(r1) ; cmplwi cr6,r10,0 ; beq cr6,0x82b32300 -- the callee's out byte says
          // whether the slot wrapped.
          if (REX_LOAD_U8(frame + kFlagSlot) == 0) {
            advance = false;
          } else {
            REX_STORE_U8(uint32_t(slot) + kState, 3);  // stb r26,46(r30)
          }
        } else {
          // loc_82B322A0 -- li r7,0 ; li r6,0 ; bl 0x82b332d0
          skate3::audio::GuestCall(ctx, base, sub_82B332D0, object, masked, frame + kCounterSlot, 0,
                                   0);
          if ((ctx.r3.u32 & 0xFF) == 0) return;
          advance = false;  // b 0x82b32300
        }
      }
    }

    if (advance) {
      // loc_82B322BC, and the tail of the state-3 path, which are the same two byte reads in the
      // opposite order and the same ring step.
      const uint32_t ring = REX_LOAD_U8(object + kRingSize);    // lbz r10,470(r31)
      const uint32_t first = REX_LOAD_U8(object + kFirstActive);// lbz r9,469(r31)
      index = NextIndex(masked, ring);
      // loc_82B322E4 -- cmplw cr6,r11,r9 ; beq cr6,0x82b3230c: the ring has closed.
      if (index == first) return;
      slot = SlotAt(object, REX_LOAD_U16(object + kTableOffset), index);
    }

    // loc_82B32300 -- lwz r11,88(r1) ; cmplwi cr6,r11,8192 ; ble cr6,0x82b32120
    if (REX_LOAD_U32(frame + kCounterSlot) > 8192) return;
  }
}

REX_FUNC(Native) {
  // __savegprlr_25 spills r25-r31 and the link register, and two `stfd`s spill f30 and f31, all at
  // negative offsets from the ENTRY r1, i.e. inside the 176 bytes the stwu claims: not windows.
  // Everything the original keeps in r25-r31 and f30-f31 is a local here, so no callee-saved
  // register is touched and the epilogue has nothing to put back.
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;
  ctx.fpscr.disableFlushModeUnconditional();  // stfd f30,-80(r1)

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-176(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;                        // mr r31,r3
  const uint32_t owner = REX_LOAD_U32(object + kOwner);      // lwz r11,12(r3)
  // lbz r10,71(r11) ; cmplwi cr6,r10,2 ; beq cr6,0x82b3230c
  if (REX_LOAD_U8(owner + kOwnerState) != 2) {
    Pump(ctx, base, object, frame);
  }

  // loc_82B3230C
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;      // addi r1,r1,176
  ctx.fpscr.disableFlushModeUnconditional();  // lfd f30,-80(r1) ; lfd f31,-72(r1)
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

}  // namespace port_82B31EE0
SKATE3_PORT(82B31EE0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
