// sub_82B34278  role: render one block of a Dac stream from its 48-byte record ring -- with both restart flags set hand the whole call to sub_82B34108; otherwise skip records whose total length is zero (marking each state 4 and popping it with sub_82B349A8), bail out when the record's rate or channel count no longer matches what is published at +456/+42, honour a start timestamp by emitting that many frames of silence instead, then take the record's scratch off the owner's pointer, ask sub_82B23C10 how much of the stream is left, discard the record's pre-roll 256 frames at a time and render one block through sub_82B3CA60, latch the last sample of every channel into the table at +462, advance the running position at +432 (looping to +24 or ending the record when it reaches the total), and finally retire every ready 16-byte slot in the ring at +108, asking sub_82B23C10 again after each   lifted: skate3_recomp.67.cpp:54313 (1032 lines)
// STATUS: gate-1 (sub_82B3CA60 runs the stream's fill function through the pointer at its +20, and sub_82B3C9D8 under it has four more: sub_82B3CA60 indirect call, `bctrl` at 0x82B3CB80 and 0x82B3CCA0, depth 1)
// calls/session: boot 223568  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B34278.md
namespace port_82B34278 {

// The object (r3/r31), about 476 bytes. Nothing in docs/rw_audio_structs.h names it; every offset
// is named for what this function does with it.
constexpr uint32_t kOwner = 8;           // -> a holder whose [0] owns the scratch pointer
constexpr uint32_t kChannels = 42;       // u8  the published channel count
constexpr uint32_t kDecoder = 420;       // u32 the record's stream object, live only in this call
constexpr uint32_t kPublishedParam = 424;  // float, = the record's +12
constexpr uint32_t kPublishedRate = 428;   // float, = the record's +16
constexpr uint32_t kPosition = 432;      // u32 the running position inside the record
constexpr uint32_t kPublishedTotal = 436;  // u32, = the record's +20
constexpr uint32_t kCurrentRate = 456;   // float the published rate, the format-change test
constexpr uint32_t kBlockFrames = 460;   // u16 frames per block (sub_82B34268 writes this field)
constexpr uint32_t kTailTable = 462;     // u16 BYTE OFFSET from the object to the last-sample table
constexpr uint32_t kRecordRing = 464;    // u16 BYTE OFFSET from the object to the 48-byte records
constexpr uint32_t kMaxChannels = 466;   // u8  the ceiling on the tail-sample copy
constexpr uint32_t kRecordCursor = 469;  // u8  which record is current
constexpr uint32_t kSlotProducer = 473;  // u8  the slot the skip loop stops at
constexpr uint32_t kSlotConsumer = 474;  // u8  the slot being consumed, wraps at 20
constexpr uint32_t kProducedFlag = 471;  // u8  set to 1 once a block has been rendered
constexpr uint32_t kResumeFlag = 472;    // u8  with +471, sends the whole call to sub_82B34108

// The 16-byte slot ring: 20 slots, the first at object+108. The original addresses these as
// object + 16*i + {108, 112, 113} and forms the middle one as 16*(i+7), so the three fields are a
// slot's +0, +4 and +5.
constexpr uint32_t kSlotStride = 16;
constexpr uint32_t kSlotWrap = 20;    // addi r11,r11,-20 ; subfic ; subfe ; and -- a branchless wrap
constexpr uint32_t kSlotCount = 108;  // u32 frames delivered out of this slot
constexpr uint32_t kSlotId = 112;     // u8  the stream index sub_82B23C10 takes
constexpr uint32_t kSlotStatus = 113; // u8  1 ready, 2 retired

// A record, 48 bytes.
constexpr uint32_t kRecordStride = 48;      // rotlwi/add is 3*cursor, rlwinm ...,4,0,27 scales by 16
constexpr uint32_t kRecStart = 0;           // f64 start timestamp, zeroed once honoured
constexpr uint32_t kRecDecoder = 8;         // -> the stream object
constexpr uint32_t kRecParam = 12;          // float, published at object+424
constexpr uint32_t kRecRate = 16;           // float, the format-change test and pair+52
constexpr uint32_t kRecTotal = 20;          // u32 length; zero means "skip this record"
constexpr uint32_t kRecLoop = 24;           // s32 loop point, negative means "end the record"
constexpr uint32_t kRecPreroll = 28;        // u32 frames to discard before the first block
constexpr uint32_t kRecSpanA = 32;          // u32 \ summed into +432 when it is still zero
constexpr uint32_t kRecSpanB = 36;          // u32 /
constexpr uint32_t kRecScratchBytes = 44;   // u16 scratch wanted, rounded up to 128
constexpr uint32_t kRecState = 46;          // u8  0 and 4 are "not running"; only 2 and 3 render
constexpr uint32_t kRecChannels = 47;       // u8  channels of this record

// The descriptor pair (r4/r24).
constexpr uint32_t kPairTime = 16;      // f64 the current time, subtracted from the record's start
constexpr uint32_t kPairFirst = 28;     // the two buffer descriptors, swapped on every block
constexpr uint32_t kPairSecond = 32;
constexpr uint32_t kPairFormat = 40;    // -> a format object whose +12 is the source rate
constexpr uint32_t kPairFrames = 48;    // u32 frames this call produced
constexpr uint32_t kPairRate = 52;      // float
constexpr uint32_t kPairScale = 56;     // float seconds -> frames
constexpr uint32_t kPairChannels = 60;  // u8
constexpr uint32_t kFormatRate = 12;    // float, on the +40 object

// A buffer descriptor, the layout sub_82B3CA60 and sub_82B3D8E0 also read.
constexpr uint32_t kBufData = 4;     // u32 -> planar float32
constexpr uint32_t kBufFrames = 14;  // u16 floats between one channel's block and the next

constexpr uint32_t kScratchField = 32;  // on the holder: a downward-growing pointer
constexpr uint32_t kScratchAlign = 128; // addi +127 then rlwinm ...,0,0,24
constexpr uint32_t kChunkFrames = 256;  // the pre-roll discards at most this many per call
constexpr uint32_t kStateDone = 4;      // li r25,4
constexpr uint32_t kStatusReady = 1;
constexpr uint32_t kStatusRetired = 2;  // li r27,2, reusing the register the render count was in

// The rodata pool, each address ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye.
// lis r11,-32208 -> 0x82300000 ; addi r11,r11,-31232 -> 0x822F8600, the same pool base
// sub_82B3D8E0.inc reaches for its ramp constants.
constexpr uint32_t kPoolBase = 0x822F8600;
constexpr uint32_t kZeroDouble = kPoolBase + 256;   // lfd f12,256(r11) -- compared and stored back
constexpr uint32_t kDelayLimit = kPoolBase + 1096;  // lfs f13,1096(r11) -- the "too far ahead" bound

constexpr uint32_t kFrameBytes = 192;  // stwu r1,-192(r1)
constexpr uint32_t kFrame80 = 80;      // the fctidz spill slot, r1+80

// cmpw / cmpwi: a SIGNED compare of the low words, whatever the 64-bit registers hold above them.
inline int32_t Word(uint64_t value) { return static_cast<int32_t>(static_cast<uint32_t>(value)); }

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

// object + [object+464] + 48*[object+469]. Built four times in the original, always from the same
// three instructions (`rotlwi r10,r11,1 ; add r11,r11,r10 ; rlwinm r11,r11,4,0,27` is 48*cursor,
// exact because the cursor comes from an lbz); the two loads come in a different order at each site,
// which is unobservable -- nothing stores to either field in between.
uint32_t CurrentRecord(uint8_t* base, uint32_t object) {
  const uint64_t cursor = REX_LOAD_U8(object + kRecordCursor);
  const uint64_t table = REX_LOAD_U16(object + kRecordRing);
  return uint32_t(kRecordStride * cursor + table + object);
}

// `lbz +46 ; cmpwi 4 ; beq -> 0 ; cmpwi 0 ; li r11,1 ; bne -> 1` -- three times in the original.
// Both 0 and 4 mean "not running"; everything else does.
bool Running(uint8_t* base, uint32_t record) {
  const int32_t state = static_cast<int32_t>(static_cast<uint32_t>(REX_LOAD_U8(record + kRecState)));
  return state != 4 && state != 0;
}

// Take `bytes` rounded up to 128 off the holder's scratch pointer, leaving the entry value in
// `saved` and the reduced one in `now`. Both sites -- loc_82B3454C and loc_82B34810 -- are the same
// seven instructions.
void TakeScratch(PPCContext& ctx, uint8_t* base, uint32_t object, uint64_t bytes, uint64_t* saved,
                 uint64_t* now) {
  (void)ctx;
  const uint32_t owner = REX_LOAD_U32(object + kOwner);  // lwz r10,8(r31)
  const uint32_t holder = REX_LOAD_U32(owner);           // lwz r8,0(r10)
  // addi r9,r11,127 ; rlwinm r7,r9,0,0,24
  const uint64_t rounded = uint32_t((uint32_t(bytes) + kScratchAlign - 1) & ~(kScratchAlign - 1));
  *saved = REX_LOAD_U32(holder + kScratchField);               // lwz r21,32(r8)
  *now = *saved - rounded;                                     // subf r11,r7,r21
  REX_STORE_U32(holder + kScratchField, uint32_t(*now));       // stw r11,32(r8)
}

// loc_82B34974 -- the record's rate or channel count no longer matches what is published, so
// nothing is rendered: the block is empty and the record's format becomes the published one.
void PublishFormat(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t pair, uint32_t record) {
  REX_STORE_U32(pair + kPairFrames, 0);                                    // stw r22,48(r24)
  REX_STORE_U8(pair + kPairChannels, REX_LOAD_U8(record + kRecChannels));  // lbz r11,47 ; stb r11,60
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, pair + kPairRate, LoadSingle(base, record + kRecRate));     // lfs 16 ; stfs 52
  StoreSingle(base, object + kCurrentRate, LoadSingle(base, record + kRecRate));  // lfs 16 ; stfs 456
  REX_STORE_U8(object + kChannels, REX_LOAD_U8(record + kRecChannels));  // lbz r10,47 ; stb r10,42
}

// The epilogue all four returns share: `addi r1,r1,192` then __restgprlr_20, which reloads r20..r31
// and the link register from the frame this call is giving back.
void Restore(PPCContext& ctx, uint64_t entry_lr, const PPCRegister* saved) {
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,192
  PPCRegister* const slots[] = {&ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25,
                                &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (size_t i = 0; i < 12; i++) *slots[i] = saved[i];
  ctx.r12.u64 = entry_lr;  // lwz r12,-8(r1)
  ctx.lr = entry_lr;       // mtlr r12
}

REX_FUNC(Native) {
  // __savegprlr_20 spills r20..r31 and the link register at -8..-104 off the entry stack pointer --
  // inside the 192 bytes the stwu below claims -- and __restgprlr_20 reloads all of them. r20-r31
  // are compared, so the entry values are kept here rather than the spills reproduced.
  const PPCRegister saved[12] = {ctx.r20, ctx.r21, ctx.r22, ctx.r23, ctx.r24, ctx.r25,
                                 ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-192(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;  // mr r31,r3
  const uint32_t pair = ctx.r4.u32;    // mr r24,r4

  // lbz r11,472(r3) ; beq ; lbz r11,471(r3) ; beq -- both set hands the call over wholesale.
  if (REX_LOAD_U8(object + kResumeFlag) != 0 && REX_LOAD_U8(object + kProducedFlag) != 0) {
    // bl 0x82b34108 -- r3 is still the entry r3, and whatever it returns is this call's result.
    skate3::audio::GuestCall(ctx, base, sub_82B34108, object);
    Restore(ctx, entry_lr, saved);
    return;
  }

  // loc_82B342B0. Every register the tail reads is cleared here, so they are declared up front:
  // the `goto loc_82B34918` below jump out of nested scopes into this one.
  uint64_t skipped = 0;        // r23, frames the pre-roll discarded
  uint64_t produced = 0;       // r28, frames sub_82B3CA60 delivered
  uint64_t available = 0;      // r26, what sub_82B23C10 reported
  uint64_t leftover = 0;       // r6  at loc_82B34894
  uint64_t scratch_saved = 0;  // r21, the holder's entry scratch pointer
  uint64_t scratch_now = 0;    // r20, the reduced one

  REX_STORE_U8(object + kResumeFlag, 0);  // li r22,0 ; stb r22,472(r31)
  REX_STORE_U32(pair + kPairFrames, 0);   // stw r22,48(r24)
  REX_STORE_U32(object + kDecoder, 0);    // stw r22,420(r31)
  uint32_t record = CurrentRecord(base, object);

  // loc_82B34308 -- a record that is not running ends the call.
  if (!Running(base, record)) goto loc_82B34918;

  // lwz r11,20(r29) ; cmpwi cr6,r11,0 ; bne cr6,0x82b34380 -- SIGNED
  if (Word(REX_LOAD_U32(record + kRecTotal)) == 0) {
    do {
      // loc_82B34324 -- an empty record is retired and the ring stepped, as often as it takes.
      REX_STORE_U8(record + kRecState, kStateDone);                // stb r25,46(r29)
      skate3::audio::GuestCall(ctx, base, sub_82B349A8, object);   // mr r3,r31 ; bl 0x82b349a8
      record = CurrentRecord(base, object);
      if (!Running(base, record)) goto loc_82B34918;               // loc_82B34368
    } while (Word(REX_LOAD_U32(record + kRecTotal)) == 0);
  }

  // loc_82B34380 -- cmplwi 2 ; beq ; cmplwi 3 ; bne cr6,0x82b34918. Only these two states render.
  {
    const uint64_t state = REX_LOAD_U8(record + kRecState);
    if (state != 2 && state != 3) goto loc_82B34918;
  }

  // loc_82B34394 -- the format tests. fcmpu with `bne` takes the change path on NaN too.
  {
    ctx.fpscr.disableFlushModeUnconditional();
    const double rate = LoadSingle(base, record + kRecRate);           // lfs f0,16(r29)
    if (rate != LoadSingle(base, object + kCurrentRate)) {             // lfs f13,456(r31)
      PublishFormat(ctx, base, object, pair, record);
      ctx.r3.s64 = 1;  // loc_82B34998: li r3,1
      Restore(ctx, entry_lr, saved);
      return;
    }
    // lbz r11,42(r31) ; lbz r10,47(r29) ; cmplw cr6,r10,r11 ; bne cr6,0x82b34974 -- UNSIGNED
    if (REX_LOAD_U8(record + kRecChannels) != REX_LOAD_U8(object + kChannels)) {
      PublishFormat(ctx, base, object, pair, record);
      ctx.r3.s64 = 1;
      Restore(ctx, entry_lr, saved);
      return;
    }
  }

  // The consumer slot must be ready. When it is not, the cursor is stepped until it reaches the
  // producer or finds a non-empty slot.
  {
    const uint32_t slot =
        uint32_t(__builtin_rotateleft32(uint32_t(REX_LOAD_U8(object + kSlotConsumer)), 4) + object);
    // lbz r9,113(r10) ; cmplwi cr6,r9,0 ; bne cr6,0x82b34418
    if (REX_LOAD_U8(slot + kSlotStatus) == 0) {
      const uint64_t producer = REX_LOAD_U8(object + kSlotProducer);  // lbz r9,473(r31)
      for (;;) {
        // loc_82B343D0
        const uint64_t at = REX_LOAD_U8(object + kSlotConsumer);  // lbz r11,474(r31)
        // cmplw cr6,r11,r9 ; beq cr6,0x82b34418 -- caught up with the producer: stop.
        if (uint32_t(at) == uint32_t(producer)) break;
        // addi r11,r11,1 ; clrlwi ; addi r10,r11,-20 ; subfic ; subfe r7,r8,r8 ; and r6,r7,r11 --
        // a BRANCHLESS wrap: the mask is zero exactly when the incremented cursor is 20.
        const uint64_t next = uint8_t(uint32_t(at) + 1);
        const uint64_t wrapped = next == kSlotWrap ? 0 : next;
        REX_STORE_U8(object + kSlotConsumer, uint8_t(wrapped));  // stb r6,474(r31)
        const uint32_t stepped =
            uint32_t(__builtin_rotateleft32(uint32_t(uint8_t(wrapped)), 4) + object);
        // lbz r3,113(r4) ; cmplwi cr6,r3,0 ; beq cr6,0x82b343d0
        if (REX_LOAD_U8(stepped + kSlotStatus) != 0) break;
      }
    }
    // loc_82B34418 -- and it must be READY, not merely non-empty.
    const uint32_t ready =
        uint32_t(__builtin_rotateleft32(uint32_t(REX_LOAD_U8(object + kSlotConsumer)), 4) + object);
    if (REX_LOAD_U8(ready + kSlotStatus) != kStatusReady) goto loc_82B34918;
  }

  // The start timestamp. A record whose +0 equals the pool's zero double renders immediately;
  // otherwise the difference from the pair's current time is converted to frames of silence.
  {
    ctx.fpscr.disableFlushModeUnconditional();
    PPCRegister start{};
    start.u64 = REX_LOAD_U64(record + kRecStart);  // lfd f0,0(r29)
    PPCRegister pool{};
    pool.u64 = REX_LOAD_U64(kZeroDouble);  // lfd f12,256(r11)
    // fcmpu cr6,f0,f12 ; beq cr6,0x82b3454c
    if (start.f64 != pool.f64) {
      PPCRegister now{};
      now.u64 = REX_LOAD_U64(pair + kPairTime);           // lfd f13,16(r24)
      const double ahead = start.f64 - now.f64;           // fsub f0,f0,f13
      uint64_t delay = 0;                                 // r26
      bool have = false;                                  // r11
      // fcmpu cr6,f0,f12 ; ble cr6,0x82b34498
      if (ahead > pool.f64) {
        const uint32_t format = REX_LOAD_U32(pair + kPairFormat);      // lwz r10,40(r24)
        const double limit = LoadSingle(base, kDelayLimit);            // lfs f13,1096(r11)
        const double source = LoadSingle(base, format + kFormatRate);  // lfs f11,12(r10)
        const double scaled = double(float(source * ahead));           // fmul f10,f11,f0 ; frsp f0
        // fcmpu cr6,f0,f13 ; blt cr6,0x82b34480
        if (!(scaled < limit)) {
          // Too far ahead to schedule. `lwz r26,80(r1)` reads the fctidz spill slot this call has
          // not written, so r26 is whatever the frame held -- and it is dead, because `have` stays
          // false and the next test bails out. Reproduced as the load it is.
          delay = REX_LOAD_U32(frame + kFrame80);
        } else {
          // loc_82B34480
          ctx.fpscr.disableFlushModeUnconditional();
          const double per_second = LoadSingle(base, pair + kPairScale);  // lfs f13,56(r24)
          double frames = double(float(per_second * scaled));             // fmuls f11,f13,f0
          // fctidz f10,f11 -- NaN and anything above 2^63 exactly as the lifted line does; the
          // test is `>`, so 2^63 itself goes through cvttsd2si.
          const int64_t truncated = std::isnan(frames) ? int64_t(0x8000000000000000ULL)
                                    : (frames > double(LLONG_MAX))
                                        ? LLONG_MAX
                                        : simde_mm_cvttsd_si64(simde_mm_load_sd(&frames));
          REX_STORE_U64(frame + kFrame80, static_cast<uint64_t>(truncated));  // stfd f10,80(r1)
          delay = REX_LOAD_U32(frame + kFrame80 + 4);  // lwz r26,84(r1) -- the LOW word
          have = true;                                 // loc_82B3449C: li r11,1
        }
      } else {
        // loc_82B34498 -- the start time has already passed.
        delay = 0;    // mr r26,r22
        have = true;  // loc_82B3449C
      }

      // loc_82B344A0 -- clrlwi r11,r11,24 ; cmplwi cr6,r11,0 ; bne cr6,0x82b344b4
      if (!have) {
        REX_STORE_U32(object + kPosition, 0);  // stw r22,432(r31)
        goto loc_82B34918;
      }

      // loc_82B344B4 -- a non-zero delay is emitted as silence and the call ends here.
      if (delay != 0) {
        // lhz r11,460(r31) ; cmplw cr6,r26,r11 ; blt -- UNSIGNED clamp to one block
        const uint64_t block = REX_LOAD_U16(object + kBlockFrames);
        if (!(uint32_t(delay) < uint32_t(block))) delay = block;  // mr r26,r11
        // loc_82B344CC
        const uint64_t bytes = uint32_t(uint32_t(delay) << 2);     // rlwinm r27,r26,2,0,29
        const uint32_t desc = REX_LOAD_U32(pair + kPairSecond);    // lwz r28,32(r24)
        uint64_t lane = 0;                                         // mr r30,r22
        // lbz r11,47(r29) ; cmplwi cr6,r11,0 ; beq cr6,0x82b34514
        if (REX_LOAD_U8(record + kRecChannels) != 0) {
          do {
            // loc_82B344E4
            const uint64_t stride = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r28)
            const uint32_t data = REX_LOAD_U32(desc + kBufData);      // lwz r10,4(r28)
            // mullw r9,r11,r30 ; rlwinm r11,r9,2,0,29 ; add r3,r11,r10
            const int64_t product =
                static_cast<int64_t>(Word(stride)) * static_cast<int64_t>(Word(lane));
            const uint64_t at = uint64_t(uint32_t(uint32_t(product) << 2)) + data;
            skate3::audio::GuestCall(ctx, base, sub_82EE5E80, at, 0, bytes);  // memset(dst, 0, n)
            lane += 1;                                                       // addi r30,r30,1
            // lbz r8,47(r29) ; cmplw cr6,r30,r8 ; blt -- UNSIGNED, bound re-read every pass
          } while (uint32_t(lane) < REX_LOAD_U8(record + kRecChannels));
        }
        // loc_82B34514 -- publish the silent block and swap the descriptor pair.
        const uint32_t was_first = REX_LOAD_U32(pair + kPairFirst);    // lwz r11,28(r24)
        const uint32_t was_second = REX_LOAD_U32(pair + kPairSecond);  // lwz r10,32(r24)
        REX_STORE_U32(pair + kPairFrames, uint32_t(delay));            // stw r26,48(r24)
        REX_STORE_U32(pair + kPairSecond, was_first);                  // stw r11,32(r24)
        REX_STORE_U32(pair + kPairFirst, was_second);                  // stw r10,28(r24)
        REX_STORE_U8(pair + kPairChannels, REX_LOAD_U8(record + kRecChannels));  // lbz r9 ; stb r9,60
        ctx.fpscr.disableFlushModeUnconditional();
        StoreSingle(base, pair + kPairRate, LoadSingle(base, record + kRecRate));  // lfs 16 ; stfs 52
        REX_STORE_U32(object + kPosition, 0);  // stw r22,432(r31)
        ctx.r3.s64 = 1;                        // li r3,1, issued before the stores above
        Restore(ctx, entry_lr, saved);
        return;
      }

      // loc_82B34548 -- the delay is spent, so the start time is cleared with the pool's own zero.
      ctx.fpscr.disableFlushModeUnconditional();
      REX_STORE_U64(record + kRecStart, pool.u64);  // stfd f12,0(r29)
    }
  }

  // loc_82B3454C -- render. Everything from here to loc_82B34894 is one region; the tail reads
  // only the variables declared at the top of the function.
  {
    const uint64_t want = REX_LOAD_U16(record + kRecScratchBytes);  // lhz r11,44(r29)
    TakeScratch(ctx, base, object, want, &scratch_saved, &scratch_now);

    const uint32_t slot =
        uint32_t(__builtin_rotateleft32(uint32_t(REX_LOAD_U8(object + kSlotConsumer)), 4) + object);
    const uint32_t stream = REX_LOAD_U32(record + kRecDecoder);  // lwz r3,8(r29)
    REX_STORE_U32(object + kDecoder, stream);                    // stw r3,420(r31)
    // addi r6,r11,7 ; rlwinm r5,r6,4,0,27 ; lbzx r4,r5,r31 -- 16*(cursor+7) is this slot's +4.
    const uint64_t id = REX_LOAD_U8(slot + kSlotId);
    // bl 0x82b23c10 -- how much of stream `id` is still unconsumed.
    skate3::audio::GuestCall(ctx, base, sub_82B23C10, stream, id);
    available = ctx.r3.u64;                              // mr r26,r3
    produced = REX_LOAD_U32(record + kRecPreroll);       // lwz r28,28(r29)
    // cmpw cr6,r3,r28 ; bge ; mr r28,r3 -- SIGNED min, so `produced` is the pre-roll to discard
    if (Word(available) < Word(produced)) produced = available;

    // loc_82B345A0 -- render at most one block, and at most what is left after the pre-roll.
    const uint64_t block = REX_LOAD_U16(object + kBlockFrames);  // lhz r11,460(r31)
    const uint64_t rest = available - produced;                  // subf r10,r28,r26
    uint64_t render = block;                                     // mr r27,r11
    if (!(Word(block) < Word(rest))) render = rest;              // blt ; mr r27,r10

    // loc_82B345B8
    const uint32_t desc = REX_LOAD_U32(pair + kPairSecond);  // lwz r30,32(r24)
    // cmpwi cr6,r28,0 ; beq cr6,0x82b345f0 -- SIGNED
    if (Word(produced) != 0) {
      do {
        // loc_82B345C4 -- discard the pre-roll 256 frames at a time.
        uint64_t take = produced;                                              // mr r5,r28
        if (!(Word(produced) < int32_t(kChunkFrames))) take = kChunkFrames;     // li r5,256
        const uint32_t from = REX_LOAD_U32(object + kDecoder);                  // lwz r3,420(r31)
        produced -= take;                                                      // subf r28,r5,r28
        skate3::audio::GuestCall(ctx, base, sub_82B3CA60, from, desc, take);    // bl 0x82b3ca60
        skipped = ctx.r3.u64 + skipped;                                        // add r23,r3,r23
      } while (Word(produced) != 0);
    }

    // loc_82B345F0 -- the block itself.
    skate3::audio::GuestCall(ctx, base, sub_82B3CA60, REX_LOAD_U32(object + kDecoder), desc, render);
    produced = ctx.r3.u64;  // mr r28,r3 -- from here on this is what was delivered
    // li r27,2 -- the register the render count was in becomes the retired-slot status byte.

    // cmpwi cr6,r3,0 ; ble cr6,0x82b3473c -- SIGNED
    if (Word(produced) > 0) {
      REX_STORE_U8(object + kProducedFlag, 1);                     // li r10,1 ; stb r10,471(r31)
      const uint64_t cap = REX_LOAD_U8(object + kMaxChannels);     // lbz r11,466(r31)
      const uint64_t have = REX_LOAD_U8(record + kRecChannels);    // lbz r10,47(r29)
      // cmplw cr6,r10,r11 ; blt ; mr r10,r11 ; clrlwi r6,r10,24 -- UNSIGNED min, then a byte
      const uint64_t lanes = uint8_t(uint32_t(have) < uint32_t(cap) ? have : cap);
      // lhz r11,462(r31) ; add r5,r11,r31
      const uint32_t table = uint32_t(REX_LOAD_U16(object + kTailTable) + object);
      // The original writes this table from two loops: one unrolled by four, running
      // ((lanes-4)>>2)+1 times with the four channel indices spelled (r9, r11-1, r11, r11+1), and a
      // remainder loop for what is left -- which is entered directly when lanes < 4. Together they
      // cover channel 0 through lanes-1 exactly once each, in order, each sample reloading the
      // descriptor's stride and data pointer, so one loop is the same loads and the same stores in
      // the same order. (The remainder loop also clobbers r6, the channel count; loc_82B34784
      // recomputes it before anything reads it again.)
      for (uint64_t lane = 0; uint32_t(lane) < uint32_t(lanes); lane++) {
        const uint64_t stride = REX_LOAD_U16(desc + kBufFrames);  // lhz r8,14(r30)
        const uint32_t data = REX_LOAD_U32(desc + kBufData);      // lwz r7,4(r30)
        // mullw r8,r8,r9 ; add r8,r8,r28 ; rlwinm r8,r8,2,0,29 ; add r7,r8,r7
        const int64_t product =
            static_cast<int64_t>(Word(stride)) * static_cast<int64_t>(Word(lane));
        const uint64_t at =
            uint64_t(uint32_t(uint32_t(uint64_t(product) + produced) << 2)) + data;
        ctx.fpscr.disableFlushModeUnconditional();
        // lfs f0,-4(r7) ; stfs f0,4(r10) with r10 starting at the table less four: the LAST sample
        // of this channel's block lands at table + 4*lane.
        StoreSingle(base, uint32_t(table + 4 * uint32_t(lane)),
                    LoadSingle(base, uint32_t(at) - 4));
      }
    }

    // loc_82B3473C -- publish the block and swap the pair, the same swap the silence path does.
    const uint32_t was_second = REX_LOAD_U32(pair + kPairSecond);  // lwz r11,32(r24)
    const uint32_t was_first = REX_LOAD_U32(pair + kPairFirst);    // lwz r10,28(r24)
    REX_STORE_U32(pair + kPairFrames, uint32_t(produced));         // stw r28,48(r24)
    REX_STORE_U32(pair + kPairFirst, was_second);                  // stw r11,28(r24)
    REX_STORE_U32(pair + kPairSecond, was_first);                  // stw r10,32(r24)
    REX_STORE_U8(pair + kPairChannels, REX_LOAD_U8(record + kRecChannels));  // lbz r9 ; stb r9,60
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, pair + kPairRate, LoadSingle(base, record + kRecRate));  // lfs 16 ; stfs 52
    const double param = LoadSingle(base, record + kRecParam);       // lfs f13,12(r29)
    const uint64_t position = REX_LOAD_U32(object + kPosition);      // lwz r8,432(r31)
    StoreSingle(base, object + kPublishedParam, param);              // stfs f13,424(r31)
    // cmpwi cr6,r8,0 ; bne cr6,0x82b34784 -- SIGNED
    if (Word(position) == 0) {
      const uint64_t span_b = REX_LOAD_U32(record + kRecSpanB);  // lwz r11,36(r29)
      const uint64_t span_a = REX_LOAD_U32(record + kRecSpanA);  // lwz r10,32(r29)
      REX_STORE_U32(object + kPosition, uint32_t(span_b + span_a));  // add ; stw r11,432(r31)
    }

    // loc_82B34784 -- advance the position and the slot's own frame count.
    const uint64_t at_position = REX_LOAD_U32(object + kPosition);   // lwz r10,432(r31)
    const uint64_t delivered = produced + skipped;                   // add r11,r28,r23
    const uint64_t cursor = REX_LOAD_U8(object + kSlotConsumer);     // lbz r9,474(r31)
    const uint64_t unused = available - produced;                    // subf r8,r28,r26
    REX_STORE_U32(object + kPosition, uint32_t(delivered + at_position));  // add r7 ; stw r7,432
    leftover = unused - skipped;                                     // subf r6,r23,r8
    // rotlwi r11,r9,4 ; add r11,r11,r31 ; addi r10,r11,108 is dead, the load overwrites r10.
    const uint32_t live = uint32_t(__builtin_rotateleft32(uint32_t(cursor), 4) + object);
    ctx.fpscr.disableFlushModeUnconditional();
    StoreSingle(base, object + kPublishedRate, LoadSingle(base, record + kRecRate));  // 16 -> 428
    REX_STORE_U32(object + kPublishedTotal, REX_LOAD_U32(record + kRecTotal));        // 20 -> 436
    const uint64_t counted = REX_LOAD_U32(live + kSlotCount) + produced;  // lwz 108(r11) ; add
    REX_STORE_U32(live + kSlotCount, uint32_t(counted + skipped));        // add r4 ; stw 108(r11)

    // lwz r3,20(r29) ; lwz r11,432(r31) ; cmpw cr6,r11,r3 ; bne cr6,0x82b34894 -- SIGNED
    const uint64_t total = REX_LOAD_U32(record + kRecTotal);
    if (Word(REX_LOAD_U32(object + kPosition)) == Word(total)) {
      const uint64_t loop_point = REX_LOAD_U32(record + kRecLoop);  // lwz r11,24(r29)
      // cmpwi cr6,r11,0 ; blt cr6,0x82b347f0 -- SIGNED: a negative loop point ends the record
      if (Word(loop_point) >= 0) {
        REX_STORE_U32(object + kPosition, uint32_t(loop_point));  // stw r11,432(r31)
      } else {
        // loc_82B347F0 -- retire this record, hand the scratch back, and pull in the next one.
        REX_STORE_U8(record + kRecState, kStateDone);  // stb r25,46(r29)
        // lwz r11,420(r31) ; cmplwi cr6,r11,0 ; beq cr6,0x82b34810
        if (REX_LOAD_U32(object + kDecoder) != 0) {
          const uint32_t owner = REX_LOAD_U32(object + kOwner);  // lwz r11,8(r31)
          REX_STORE_U32(object + kDecoder, 0);                   // stw r22,420(r31)
          const uint32_t holder = REX_LOAD_U32(owner);           // lwz r10,0(r11)
          REX_STORE_U32(holder + kScratchField, uint32_t(scratch_saved));  // stw r21,32(r10)
        }
        // loc_82B34810
        skate3::audio::GuestCall(ctx, base, sub_82B349A8, object);  // mr r3,r31 ; bl 0x82b349a8
        const uint32_t next = CurrentRecord(base, object);
        // The `active` test again, then lwz r10,8(r11): both `beq` go to loc_82B34894, which is
        // where this region ends anyway.
        if (Running(base, next) && REX_LOAD_U32(next + kRecDecoder) != 0) {
          const uint64_t want_next = REX_LOAD_U16(next + kRecScratchBytes);  // lhz r10,44(r11)
          TakeScratch(ctx, base, object, want_next, &scratch_saved, &scratch_now);
          REX_STORE_U32(object + kDecoder, REX_LOAD_U32(next + kRecDecoder));  // lwz r4,8 ; stw 420
        }
      }
    }
  }

  // loc_82B34894 -- cmpwi cr6,r6,0 ; bne cr6,0x82b34918. `leftover` is what sub_82B23C10 reported
  // less everything this call consumed: anything left means the slot is not finished.
  if (Word(leftover) != 0) goto loc_82B34918;

loc_82B3489C: {
  // Retire the consumer slot and step the cursor, then ask sub_82B23C10 about the next slot; a
  // slot with nothing left is retired too, which is what the loop back here is for.
  const uint32_t slot =
      uint32_t(__builtin_rotateleft32(uint32_t(REX_LOAD_U8(object + kSlotConsumer)), 4) + object);
  // lbz r10,113(r11) ; cmplwi cr6,r10,1 ; bne cr6,0x82b34918
  if (REX_LOAD_U8(slot + kSlotStatus) != kStatusReady) goto loc_82B34918;
  REX_STORE_U8(slot + kSlotStatus, kStatusRetired);            // stb r27,113(r11)
  const uint32_t stream = REX_LOAD_U32(object + kDecoder);      // lwz r3,420(r31)
  // The cursor is stepped and stored BEFORE the `beq` on that stream pointer, which was compared
  // four instructions earlier.
  const uint64_t at = REX_LOAD_U8(object + kSlotConsumer);      // lbz r11,474(r31)
  const uint64_t next = uint8_t(uint32_t(at) + 1);              // addi r11,r11,1 ; clrlwi
  // addi r10,r11,-20 ; subfic ; subfe r8,r9,r9 ; and r11,r8,r11 -- the same branchless wrap
  const uint64_t wrapped = next == kSlotWrap ? 0 : next;
  REX_STORE_U8(object + kSlotConsumer, uint8_t(wrapped));      // stb r11,474(r31)
  if (stream == 0) goto loc_82B34910;                          // beq cr6,0x82b34910
  // rlwinm r10,r11,4,20,27 -- 16*cursor masked to 0xFF0, which for a cursor under 20 drops nothing.
  const uint32_t stepped = uint32_t((uint32_t(wrapped) << 4) & 0xFF0u) + object;
  // lbz r9,113(r10) ; cmplwi cr6,r9,1 ; bne cr6,0x82b34910
  if (REX_LOAD_U8(stepped + kSlotStatus) != kStatusReady) goto loc_82B34910;
  // addi r11,r11,7 ; rlwinm r10,r11,4,0,27 ; lbzx r4,r10,r31 -- that slot's +4
  const uint64_t id = REX_LOAD_U8(uint32_t(kSlotStride * (uint8_t(wrapped) + 7)) + object);
  skate3::audio::GuestCall(ctx, base, sub_82B23C10, stream, id);  // bl 0x82b23c10
  leftover = ctx.r3.u64;                                         // mr r6,r3
}

loc_82B34910:
  // cmpwi cr6,r6,0 ; beq cr6,0x82b3489c -- SIGNED
  if (Word(leftover) == 0) goto loc_82B3489C;

loc_82B34918:
  // lwz r11,420(r31) ; cmplwi cr6,r11,0 ; beq cr6,0x82b3493c
  if (REX_LOAD_U32(object + kDecoder) != 0) {
    // The compare on r20 is issued BEFORE the store, and the store happens either way.
    const bool restore_scratch = uint32_t(scratch_now) != 0;
    REX_STORE_U32(object + kDecoder, 0);  // stw r22,420(r31)
    if (restore_scratch) {
      const uint32_t owner = REX_LOAD_U32(object + kOwner);  // lwz r11,8(r31)
      const uint32_t holder = REX_LOAD_U32(owner);           // lwz r10,0(r11)
      REX_STORE_U32(holder + kScratchField, uint32_t(scratch_saved));  // stw r21,32(r10)
    }
  }

  // loc_82B3493C
  REX_STORE_U8(pair + kPairChannels, REX_LOAD_U8(object + kChannels));  // lbz r11,42 ; stb r11,60
  ctx.fpscr.disableFlushModeUnconditional();
  StoreSingle(base, pair + kPairRate, LoadSingle(base, object + kCurrentRate));  // 456 -> 52
  // cmpwi cr6,r28,0 ; bne cr6,0x82b34998 ; cmpwi cr6,r23,0 ; bne ; lhz r11,460(r31) ; cmplwi ; beq
  // -- 0 only when nothing was rendered, nothing was discarded, and the block size is non-zero.
  if (Word(produced) == 0 && Word(skipped) == 0 && REX_LOAD_U16(object + kBlockFrames) != 0) {
    ctx.r3.s64 = 0;  // li r3,0
  } else {
    ctx.r3.s64 = 1;  // loc_82B34998: li r3,1
  }
  Restore(ctx, entry_lr, saved);
}

// Never called: the macro skips the shadow branch for a gate-labelled port. Kept so the file has
// the same shape as every other port. The write set this WOULD declare is in the note.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  (void)ctx;
  (void)base;
  (void)spec;
  return false;
}

}  // namespace port_82B34278
SKATE3_PORT(82B34278, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
