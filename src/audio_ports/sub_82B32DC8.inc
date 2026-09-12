// sub_82B32DC8  role: append one queue request to a stream's segment ring -- claim the free slot at the write index and its parallel 80-byte record, seed both from the request, run the two setup callees, then (for kinds 1 and 2) find the request's pool in the global list at 0x830BDEDC, acquire a voice for it through sub_82B47A68, copy the request's name into a fresh allocation from the holder's allocator, and schedule one or three callbacks through sub_82EC0E08; on any refusal give the slot back, otherwise advance the write index. Returns u16[request+44] on every path   lifted: skate3_recomp.67.cpp:51317 (624 lines)
// STATUS: gate-1 (the name buffer is allocated through the holder's vtable: sub_82B32DC8 indirect call, `bctrl` at 0x82B33090; sub_82B47A68 and sub_82EC0E08 are unreplayable besides)
// calls/session: boot 1932  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B32DC8.md
namespace port_82B32DC8 {

// The request record (r3/r28).
constexpr uint32_t kRequestStream = 4;    // u32 -> the stream this appends to
constexpr uint32_t kRequestStart = 8;     // f64 -> slot+0, a bit copy
constexpr uint32_t kRequestTime = 16;     // f64 -> record+0, a bit copy
constexpr uint32_t kRequestSpan = 24;     // f64, tested against the pool double and scaled by slot+16
constexpr uint32_t kRequestOffset = 32;   // u32
constexpr uint32_t kRequestDetail = 36;   // u32 -> a pointer whose u8[0]==1 selects its +12
constexpr uint32_t kRequestOwner = 40;    // u32, matched against +44 of each pool in the list
constexpr uint32_t kRequestResult = 44;   // u16 -> THE RETURN VALUE, on all three exits
constexpr uint32_t kRequestFlags = 46;    // u8 -> record+75
constexpr uint32_t kRequestLevel = 48;    // f32 -> stream+448, slot+12, stream+452
constexpr uint32_t kRequestName = 56;     // NUL-terminated, in the request itself

// The stream (r30) -- the same object sub_82B31EE0 pumps.
constexpr uint32_t kHolder = 8;           // u32 -> object whose +36 is the allocator
constexpr uint32_t kOwner = 12;           // u32 -> owner; its f32 at +56 is the acquire priority
constexpr uint32_t kRecords = 96;         // u32 -> array of 80-byte records
constexpr uint32_t kPendingSlot = 444;    // u32 -> u32 counter, decremented once per call
constexpr uint32_t kLevelEntry = 448;     // f32, written before the slot is even checked
constexpr uint32_t kLevelCommitted = 452; // f32, the same value again once the append sticks
constexpr uint32_t kTableOffset = 464;    // u16, the ring's byte offset INSIDE the stream
constexpr uint32_t kWriteIndex = 467;     // u8, the slot this call claims; advanced on success
constexpr uint32_t kRingSize = 470;       // u8, the index the ring wraps to zero at

constexpr uint32_t kOwnerRate = 56;       // f32 on the owner
constexpr uint32_t kHolderAllocator = 36; // u32 on the holder
constexpr uint32_t kAllocSlot = 4;        // the allocator vtable's allocate entry

// A ring slot, 48 bytes (offsets shared with sub_82B31EE0's reading of the same ring).
constexpr uint32_t kSlotStart = 0;    // f64
constexpr uint32_t kSlotCounter = 8;  // u32, cleared here
constexpr uint32_t kSlotLevel = 12;   // f32
constexpr uint32_t kSlotRate = 16;    // f32
constexpr uint32_t kSlotQueued = 20;  // u32
constexpr uint32_t kSlotCursor = 24;  // u32, its SIGN gates three separate decisions
constexpr uint32_t kState = 46;       // u8: 0 free, 1 claimed
constexpr uint32_t kSlotMode = 47;    // u8

// The 80-byte record at u32[stream+96] + 80*index.
constexpr uint32_t kRecordTime = 0;      // f64
constexpr uint32_t kRecordStride = 12;   // s32, added to the time for the repeat callbacks
constexpr uint32_t kRecordFirst = 16;    // s32
constexpr uint32_t kRecordNameBuf = 28;  // u32, the allocation this call makes
constexpr uint32_t kRecordPool = 32;     // u32, the list match (or zero)
constexpr uint32_t kRecordVoice = 36;    // u32, sub_82B47A68's slot
constexpr uint32_t kRecordPosition = 40; // u32, from the voice slot's +20
constexpr uint32_t kRecordHandle = 44;   // u32, the FIRST sub_82EC0E08 result only
constexpr uint32_t kRecordBegin = 48;    // u32
constexpr uint32_t kRecordEnd = 52;      // u32
constexpr uint32_t kRecordBias = 64;     // u32
constexpr uint32_t kRecordKind = 73;     // u8: 1 and 2 take the long path, 2 adds two range tests
constexpr uint32_t kRecordFlags = 75;    // u8, from request+46
constexpr uint32_t kRecordReady = 76;    // u8, read twice: picks the callback, then gates the shortcut

// Constants, each ((lis_imm & 0xFFFF) << 16) + offset, computed and not read off by eye.
// lis r11,-32208 -> 0x82300000 ; addi r10,r11,-31232 -> 0x822F8600 ; lfd f0,256(r10). The same pool
// double port_82B31EE0 calls kFarFuture; copied locally rather than referenced across ports.
constexpr uint32_t kFarFuture = 0x822F8700;
// lis r11,-32077 -> 0x82B30000 ; addi r24,r11,7728 / 7824 -- two guest CODE addresses, handed to
// sub_82EC0E08 as its r7.
constexpr uint32_t kCallbackReady = 0x82B31E90;  // u8[record+76] nonzero
constexpr uint32_t kCallbackPlain = 0x82B31E30;  // u8[record+76] zero
// the same 0x82B30000 base ; addi r5,r11,8992 -- the release callback sub_82B47A68 parks at slot+12.
constexpr uint32_t kReleaseCallback = 0x82B32320;
// lis r10,-32238 -> 0x82120000 ; addi r5,r10,-26420 -- the allocation tag string.
constexpr uint32_t kAllocTag = 0x821198CC;
// lis r11,-31988 -> 0x830C0000 ; lwz r11,-8484(r11) -- four bytes below the list head
// port_82B4FAF8 calls kSpareHead, in the same global block.
constexpr uint32_t kPoolListHead = 0x830BDEDC;

constexpr uint32_t kFrameBytes = 176;  // stwu r1,-176(r1)
constexpr uint32_t kSpillA = 80;       // the fctiwz/fctidz/fcfid spill slot, this call's own frame
constexpr uint32_t kSpillB = 88;       // the second one, used only by the repeat loop

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

// fctiwz: NaN gives 0x0000000080000000 (so the low word reads 0x80000000), past INT_MAX saturates,
// and the whole doubleword is what the following stfd writes.
int64_t FctiwzBits(double value) {
  return std::isnan(value) ? int64_t(0x80000000U)
         : (value > double(INT_MAX))
             ? int64_t(INT_MAX)
             : int64_t(simde_mm_cvttsd_si32(simde_mm_load_sd(&value)));
}

// fctidz: NaN gives INT64_MIN, past LLONG_MAX saturates.
int64_t FctidzBits(double value) {
  return std::isnan(value) ? int64_t(0x8000000000000000ULL)
         : (value > double(LLONG_MAX))
             ? LLONG_MAX
             : int64_t(simde_mm_cvttsd_si64(simde_mm_load_sd(&value)));
}

// rotlwi r10,r11,1 ; add ; rlwinm r10,r8,4,0,27 ; add r10,r9,r10 ; add r26,r10,r30 -- the 48-byte
// slot stride, the same arithmetic port_82B31EE0 spells out (copied, not referenced).
uint64_t SlotAt(uint64_t stream, uint64_t table, uint32_t index) {
  const uint64_t tripled = uint64_t(index) + uint64_t(__builtin_rotateleft32(index, 1));
  return ((uint64_t(uint32_t(tripled) << 4)) & 0xFFFFFFF0ull) + table + stream;
}

// rlwinm r10,r11,2,0,29 ; add r11,r11,r10 ; rlwinm r11,r11,4,0,27 ; add r31,r11,r9 -- the 80-byte
// record stride off u32[stream+96].
uint64_t RecordAt(uint64_t records, uint32_t index) {
  const uint64_t fived = uint64_t(index) + ((uint64_t(index) << 2) & 0xFFFFFFFCull);
  return ((uint64_t(uint32_t(fived) << 4)) & 0xFFFFFFF0ull) + records;
}

// addi r9,r11,1 ; clrlwi r8,r9,24 ; subf r7,r8,r10 ; subfic ; subfe r4,r5,r5 ; and r3,r4,r8 --
// increment the byte index and wrap to zero at u8[stream+470], branchlessly.
uint32_t NextIndex(uint32_t index, uint32_t limit) {
  const uint32_t bumped = (index + 1) & 0xFF;
  return uint32_t(limit - bumped) == 0 ? 0 : bumped;
}

// loc_82B330A0 -- hand the slot back. Reached from three refusals, always with r29 still zero.
void Abandon(uint8_t* base, uint32_t slot) {
  REX_STORE_U8(slot + kState, 0);       // stb r29,46(r26)
  REX_STORE_U32(slot + kSlotQueued, 0); // stw r29,20(r26)
}

// Everything from the claim to loc_82B331D4. Every `return` is an exit to loc_82B33204, which the
// caller below does; the three that give the slot back call Abandon() first, as the original does.
void Append(PPCContext& ctx, uint8_t* base, uint32_t request, uint32_t stream, uint32_t name,
            uint32_t holder, uint32_t index, uint32_t slot, uint32_t frame) {
  const uint32_t records = REX_LOAD_U32(stream + kRecords);  // lwz r9,96(r30)
  const uint32_t record = uint32_t(RecordAt(records, index));

  // The seed. Store order as lifted: the slot's level and counter, then the record's +40, then the
  // two doubles, then the flags byte, then the state, then five cleared words.
  const double level = LoadSingle(base, request + kRequestLevel);  // lfs f0,48(r3) -- a second read
  StoreSingle(base, slot + kSlotLevel, level);                     // stfs f0,12(r26)
  REX_STORE_U32(slot + kSlotCounter, 0);                           // stw r29,8(r26)
  REX_STORE_U32(record + kRecordPosition, 0);                      // stw r29,40(r31)
  // lfd f13,8(r28) ; stfd f13,0(r26) and lfd f12,16(r28) ; stfdx f12,r11,r9 -- bit copies. The
  // indexed store's effective address is r11+r9, which is exactly the record base.
  REX_STORE_U64(slot + kSlotStart, REX_LOAD_U64(request + kRequestStart));
  REX_STORE_U64(record + kRecordTime, REX_LOAD_U64(request + kRequestTime));
  REX_STORE_U8(record + kRecordFlags, REX_LOAD_U8(request + kRequestFlags));  // lbz 46 ; stb 75
  REX_STORE_U8(slot + kState, 1);        // stb r23,46(r26)
  REX_STORE_U32(record + 20, 0);         // stw r29,20(r31)
  REX_STORE_U32(record + 24, 0);         // stw r29,24(r31)
  REX_STORE_U32(record + kRecordVoice, 0);    // stw r29,36(r31)
  REX_STORE_U32(record + kRecordHandle, 0);   // stw r29,44(r31)
  REX_STORE_U32(record + kRecordNameBuf, 0);  // stw r29,28(r31)

  // bl 0x82b335a8 -- the write index is RE-READ for r4 here and at every later call.
  skate3::audio::GuestCall(ctx, base, sub_82B335A8, stream, REX_LOAD_U8(stream + kWriteIndex),
                           REX_LOAD_U32(request + kRequestOffset));

  // lbz r9,76(r31) ; cmplwi cr6,r9,0 ; bne -- which of the two guest callbacks gets scheduled.
  const uint32_t callback =
      REX_LOAD_U8(record + kRecordReady) != 0 ? kCallbackReady : kCallbackPlain;

  // loc_82B32EB0 -- a span past the pool sentinel, on a slot whose mode byte is clear, runs the
  // setup a second time with a different argument.
  ctx.fpscr.disableFlushModeUnconditional();
  const double span = LoadDouble(base, request + kRequestSpan);  // lfd f13,24(r28)
  const double far_future = LoadDouble(base, kFarFuture);        // lfd f0,256(r10)
  // fcmpu cr6,f13,f0 ; ble -- strictly greater, so an unordered compare skips.
  if (span > far_future && REX_LOAD_U8(slot + kSlotMode) == 0) {
    const uint32_t detail = REX_LOAD_U32(request + kRequestDetail);  // lwz r11,36(r28)
    uint64_t argument = 0;                                          // mr r5,r29
    // lbz r10,0(r11) ; cmplwi cr6,r10,1 ; bne cr6,0x82b32eec
    if (REX_LOAD_U8(detail) == 1) argument = uint64_t(detail) + 12;  // addi r5,r11,12
    skate3::audio::GuestCall(ctx, base, sub_82B335A8, stream, REX_LOAD_U8(stream + kWriteIndex),
                             argument);
  }

  // loc_82B32EF8 -- the admission test: the slot's queued count must exceed a truncated product.
  ctx.fpscr.disableFlushModeUnconditional();
  const double rate = LoadSingle(base, slot + kSlotRate);         // lfs f0,16(r26)
  const double span_again = LoadDouble(base, request + kRequestSpan);  // lfd f13,24(r28) -- re-read
  const double product = rate * span_again;                       // fmul -- a DOUBLE multiply
  REX_STORE_U64(frame + kSpillA, uint64_t(FctiwzBits(product)));  // fctiwz f11 ; stfd f11,80(r1)
  const uint32_t truncated = REX_LOAD_U32(frame + kSpillA + 4);   // lwz r9,84(r1) -- the LOW word
  uint64_t limit = 0;                                             // mr r6,r29
  // cmpwi cr6,r9,0 ; bgt cr6,0x82b32f20
  if (int32_t(truncated) > 0) {
    // loc_82B32F20 -- two branchless masks ANDed onto that word. `subfic`/`subfe` on (kind-2) is
    // all ones unless the kind IS 2; `subfc`/`subfe` against zero with the two sign bits is the
    // signed test (int32)cursor < 0.
    const uint32_t kind = REX_LOAD_U8(record + kRecordKind);   // lbz r10,73(r31)
    const uint32_t cursor = REX_LOAD_U32(slot + kSlotCursor);  // lwz r8,24(r26)
    if (kind != 2 && int32_t(cursor) < 0) limit = truncated;
  }
  // loc_82B32F50 -- lwz r11,20(r26) ; cmpw cr6,r11,r6 ; ble cr6,0x82b330a0. SIGNED, on low words.
  if (int32_t(REX_LOAD_U32(slot + kSlotQueued)) <= int32_t(uint32_t(limit))) {
    Abandon(base, slot);
    return;
  }

  // bl 0x82b33780
  skate3::audio::GuestCall(ctx, base, sub_82B33780, stream, REX_LOAD_U8(stream + kWriteIndex),
                           REX_LOAD_U32(request + kRequestDetail));

  // lbz r11,76(r31) ; cmplwi 1 ; bne ; lbz r11,0(r25) ; cmplwi 0 ; bne -- a ready record with an
  // EMPTY name needs no voice, no allocation and no callbacks: just the two offsets.
  if (REX_LOAD_U8(record + kRecordReady) == 1 && REX_LOAD_U8(name) == 0) {
    const uint64_t offset = REX_LOAD_U32(request + kRequestOffset);  // lwz r11,32(r28)
    const uint64_t bias = REX_LOAD_U32(record + kRecordBias);        // lwz r10,64(r31)
    const uint64_t stride = REX_LOAD_U32(record + kRecordStride);    // lwz r9,12(r31)
    const uint64_t begin = bias + offset;        // add r11,r10,r11 -- 64-bit, truncated by the stw
    REX_STORE_U32(record + kRecordBegin, uint32_t(begin));           // stw r11,48(r31)
    REX_STORE_U32(record + kRecordEnd, uint32_t(stride + begin));    // stw r10,52(r31)
  } else {
    // loc_82B32FA4 -- only kinds 1 and 2 go any further; anything else commits as it stands.
    const uint32_t kind = REX_LOAD_U8(record + kRecordKind);
    if (kind == 1 || kind == 2) {
      // loc_82B32FB8 -- walk the global list for the pool whose +44 matches the request's owner.
      // Each list link sits 12 bytes into its pool, so the candidate is node-12; the `subfic`/
      // `subfe`/`and` around it is a null guard that can never fire inside the loop.
      uint64_t found = 0;
      uint32_t node = REX_LOAD_U32(kPoolListHead);  // lwz r11,-8484(r11)
      if (node != 0) {
        const uint32_t wanted = REX_LOAD_U32(request + kRequestOwner);  // lwz r9,40(r28)
        for (;;) {
          const uint64_t candidate = uint64_t(node) - 12;  // addi r7,r11,-12 ; and r10,r6,r7
          // lwz r5,44(r10) ; cmplw cr6,r5,r9 ; beq cr6,0x82b32ff8
          if (REX_LOAD_U32(uint32_t(candidate) + 44) == wanted) {
            found = candidate;
            break;
          }
          node = REX_LOAD_U32(node);  // lwz r11,0(r11)
          // cmplwi cr6,r11,0 ; bne cr6,0x82b32fcc -- otherwise loc_82B32FF4 leaves found zero.
          if (node == 0) break;
        }
      }
      // loc_82B32FF8
      REX_STORE_U32(record + kRecordPool, uint32_t(found));  // stw r10,32(r31)

      // bl 0x82b47a68 -- r4 is NOT set by the original here: it is the leftover write index from
      // the call above, and sub_82B47A68 reads only r3, r5, r6 and f1. So the registers are set by
      // hand and GuestCall is given no arguments, which leaves r4 exactly as the original does.
      const uint32_t owner = REX_LOAD_U32(stream + kOwner);  // lwz r10,12(r30)
      ctx.fpscr.disableFlushModeUnconditional();
      ctx.f1.f64 = LoadSingle(base, owner + kOwnerRate);     // lfs f1,56(r10)
      ctx.r3.u64 = found;                                    // mr r3,r10
      ctx.r5.u64 = kReleaseCallback;                         // addi r5,r11,8992
      ctx.r6.u64 = stream;                                   // mr r6,r30
      skate3::audio::GuestCall(ctx, base, sub_82B47A68);
      const uint32_t voice = ctx.r3.u32;
      REX_STORE_U32(record + kRecordVoice, voice);  // stw r3,36(r31)
      // cmplwi cr6,r3,0 ; beq cr6,0x82b330a0
      if (voice == 0) {
        Abandon(base, slot);
        return;
      }
      // lwz r11,20(r3) ; stw r11,40(r31)
      REX_STORE_U32(record + kRecordPosition, REX_LOAD_U32(voice + 20));

      // lwz r10,24(r26) ; cmpwi cr6,r10,0 ; bge cr6,0x82b33044 -- a negative cursor on a slot whose
      // mode byte is set skips the name copy entirely (straight to loc_82B330C0).
      bool copy_name = true;
      if (int32_t(REX_LOAD_U32(slot + kSlotCursor)) < 0 &&
          REX_LOAD_U8(slot + kSlotMode) != 0) {
        copy_name = false;
      }
      if (copy_name) {
        // loc_82B33048 -- strlen, then allocate and copy. The byte count is re-derived through a
        // 32-bit truncation of (length), which is why it is spelled out.
        uint64_t scan = name;  // mr r11,r25
        for (;;) {
          const uint32_t byte = REX_LOAD_U8(uint32_t(scan));  // lbz r10,0(r11)
          scan += 1;                                          // addi r11,r11,1
          if (byte == 0) break;                               // cmplwi ; bne cr6,0x82b33048
        }
        const uint64_t span_bytes = scan - uint64_t(name);     // subf r11,r25,r11
        const uint64_t length = span_bytes - 1;                // addi r9,r11,-1
        const uint64_t bytes = uint64_t(uint32_t(length)) + 1; // rotlwi r11,r9,0 ; addi r27,r11,1

        const uint32_t allocator = REX_LOAD_U32(holder + kHolderAllocator);  // lwz r3,36(r27)
        const uint32_t vtable = REX_LOAD_U32(allocator);                     // lwz r10,0(r3)
        ctx.r3.u64 = allocator;
        ctx.r4.u64 = bytes;                            // mr r4,r27
        ctx.r5.u64 = kAllocTag;                        // addi r5,r10,-26420
        ctx.r6.s64 = 0;                                // li r6,0
        ctx.r7.s64 = 16;                               // li r7,16
        ctx.r8.s64 = 0;                                // li r8,0
        ctx.ctr.u64 = REX_LOAD_U32(vtable + kAllocSlot);  // lwz r9,4(r10) ; mtctr r9
        ctx.lr = 0x82B33094;                              // bctrl
        REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
        const uint32_t buffer = ctx.r3.u32;
        REX_STORE_U32(record + kRecordNameBuf, buffer);  // stw r3,28(r31)
        // cmplwi cr6,r3,0 ; bne cr6,0x82b330b4
        if (buffer == 0) {
          Abandon(base, slot);
          return;
        }
        // loc_82B330B4 -- bl 0x82edf460, memcpy(buffer, name, bytes)
        skate3::audio::GuestCall(ctx, base, sub_82EDF460, buffer, name, bytes);
      }

      // loc_82B330C0 -- schedule the first callback unless kind 2 with a non-negative cursor says
      // the cursor has not reached the record's first mark. The `xoris`/`addc`/`subfe` chain is
      // clang's branchless signed (int32)cursor >= (int32)first.
      uint64_t schedule = 1;  // mr r11,r23
      if (REX_LOAD_U8(record + kRecordKind) == 2) {
        const uint32_t cursor = REX_LOAD_U32(slot + kSlotCursor);  // lwz r10,24(r26)
        // cmpwi cr6,r10,0 ; blt cr6,0x82b330f4 -- a negative cursor leaves the flag set.
        if (int32_t(cursor) >= 0) {
          const uint32_t first = REX_LOAD_U32(record + kRecordFirst);  // lwz r9,16(r31)
          schedule = uint64_t(int32_t(cursor) >= int32_t(first));
        }
      }
      // loc_82B330F4 -- clrlwi r11,r11,24 ; cmplwi ; beq cr6,0x82b33134
      if ((schedule & 0xFF) != 0) {
        ctx.fpscr.disableFlushModeUnconditional();
        const double at = LoadDouble(base, record + kRecordTime);   // lfd f0,0(r31)
        const uint64_t bias = REX_LOAD_U32(record + kRecordBias);   // lwz r10,64(r31)
        REX_STORE_U64(frame + kSpillA, uint64_t(FctidzBits(at)));   // fctidz f13 ; stfd f13,80(r1)
        const uint64_t ticks = REX_LOAD_U64(frame + kSpillA);       // ld r11,80(r1)
        // bl 0x82ec0e08 -- add r6,r10,r11 is a 64-bit sum and r6 keeps all of it.
        skate3::audio::GuestCall(ctx, base, sub_82EC0E08, REX_LOAD_U32(record + kRecordPosition),
                                 name, 0, bias + ticks, callback, stream);
        REX_STORE_U32(record + kRecordHandle, ctx.r3.u32);  // stw r3,44(r31)
      }

      // loc_82B33134 -- lwz r11,24(r26) ; cmpwi cr6,r11,0 ; blt cr6,0x82b331d4
      if (int32_t(REX_LOAD_U32(slot + kSlotCursor)) >= 0) {
        uint64_t repeat = 1;  // mr r11,r23
        if (REX_LOAD_U8(record + kRecordKind) == 2) {
          const uint32_t first = REX_LOAD_U32(record + kRecordFirst);  // lwz r10,16(r31)
          const uint32_t queued = REX_LOAD_U32(slot + kSlotQueued);    // lwz r9,20(r26)
          // The same idiom the other way round: branchless (int32)first < (int32)queued.
          repeat = uint64_t(int32_t(first) < int32_t(queued));
        }
        // loc_82B3316C
        if ((repeat & 0xFF) != 0) {
          // li r29,2 ; addic. r29,r29,-1 ; bne -- exactly two more callbacks.
          for (uint32_t left = 2; left != 0; left--) {
            // loc_82B3317C
            const uint32_t stride = REX_LOAD_U32(record + kRecordStride);  // lwz r11,12(r31)
            ctx.fpscr.disableFlushModeUnconditional();
            const double at = LoadDouble(base, record + kRecordTime);      // lfd f0,0(r31)
            // extsw r10,r11 ; std r10,80(r1) ; lfd f13,80(r1) ; fcfid f12,f13
            REX_STORE_U64(frame + kSpillA, uint64_t(int64_t(int32_t(stride))));
            PPCRegister spill{};
            spill.u64 = REX_LOAD_U64(frame + kSpillA);
            const double widened = double(spill.s64);
            const double when = widened + at;  // fadd f11,f12,f0 -- a DOUBLE add
            REX_STORE_U64(frame + kSpillB, uint64_t(FctidzBits(when)));  // stfd f10,88(r1)
            const uint64_t ticks = REX_LOAD_U64(frame + kSpillB);        // ld r6,88(r1)
            skate3::audio::GuestCall(ctx, base, sub_82EC0E08,
                                     REX_LOAD_U32(record + kRecordPosition), name, 0, ticks,
                                     callback, stream);
            // lwz r9,44(r31) ; cmpwi cr6,r9,0 ; bne -- only the FIRST handle is kept, so a
            // scheduled repeat overwrites nothing.
            if (int32_t(REX_LOAD_U32(record + kRecordHandle)) == 0) {
              REX_STORE_U32(record + kRecordHandle, ctx.r3.u32);  // stw r3,44(r31)
            }
          }
        }
      }
    }
  }

  // loc_82B331D4 -- the append sticks: the slot stays claimed, the write index steps on, and the
  // request's level is republished a second time.
  REX_STORE_U8(slot + kState, 1);  // stb r23,46(r26) -- again; it was already 1 from the seed
  const uint32_t current = REX_LOAD_U8(stream + kWriteIndex);  // lbz r11,467(r30)
  const uint32_t ring = REX_LOAD_U8(stream + kRingSize);       // lbz r10,470(r30)
  REX_STORE_U8(stream + kWriteIndex, uint8_t(NextIndex(current, ring)));  // stb r3,467(r30)
  ctx.fpscr.disableFlushModeUnconditional();
  // lfs f0,48(r28) ; stfs f0,452(r30)
  StoreSingle(base, stream + kLevelCommitted, LoadSingle(base, request + kRequestLevel));
}

REX_FUNC(Native) {
  // __savegprlr_23 spills r23-r31 and the link register at negative offsets from the ENTRY r1, i.e.
  // inside the 176 bytes the stwu claims: not windows. Everything the original keeps in r23-r31 is
  // a local here, so no callee-saved register is touched and the epilogue has nothing to put back.
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-176(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t request = ctx.r3.u32;                              // mr r28,r3
  const uint32_t stream = REX_LOAD_U32(request + kRequestStream);   // lwz r30,4(r3)
  const uint32_t name = request + kRequestName;                     // addi r25,r3,56
  const uint32_t pending = REX_LOAD_U32(stream + kPendingSlot);     // lwz r10,444(r30)
  const uint32_t holder = REX_LOAD_U32(stream + kHolder);           // lwz r27,8(r30)
  // lwz r11,0(r10) ; addi r9,r11,-1 ; stw r9,0(r10) -- one fewer request outstanding.
  REX_STORE_U32(pending, REX_LOAD_U32(pending) - 1);

  ctx.fpscr.disableFlushModeUnconditional();
  const double level = LoadSingle(base, request + kRequestLevel);  // lfs f0,48(r3)
  const uint32_t index = REX_LOAD_U8(stream + kWriteIndex);        // lbz r11,467(r30)
  StoreSingle(base, stream + kLevelEntry, level);                  // stfs f0,448(r30)
  const uint32_t slot =
      uint32_t(SlotAt(stream, REX_LOAD_U16(stream + kTableOffset), index));  // r26

  // lbz r7,46(r26) ; cmplwi cr6,r7,0 ; bne cr6,0x82b33204 -- the write slot has to be free.
  if (REX_LOAD_U8(slot + kState) == 0) {
    Append(ctx, base, request, stream, name, holder, index, slot, frame);
  }

  // loc_82B33204 -- the result is the same halfword on all three exits.
  ctx.r3.u64 = REX_LOAD_U16(request + kRequestResult);  // lhz r3,44(r28)
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;                // addi r1,r1,176
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

}  // namespace port_82B32DC8
SKATE3_PORT(82B32DC8, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
