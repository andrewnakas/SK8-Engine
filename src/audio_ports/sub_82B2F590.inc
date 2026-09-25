// sub_82B2F590  role: recommit a filter/envelope object when its state word says 1 -- rebuild the ramp (sub_82B2FE00), remap it through the table (sub_82B2FEA8) and resample the six bands (sub_82B2F2C8) according to which of the three parameters moved, turn the bands into six weights, then either rearm the voice table (sub_82B2FF88) or rewrite the per-voice records, and publish all four parameters with the state word set to 2   lifted: skate3_recomp.67.cpp:42949 (332 lines)
// STATUS: verified
// calls/session: boot 25287  play 29949  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2F590.md
namespace port_82B2F590 {

// r3 = the object. Nothing in docs/rw_audio_structs.h names it, so the offsets stay plain. Three
// parameters each have a published copy, and the state word gates the whole function.
constexpr uint32_t kParam52 = 52;         // f32, floored against a rodata minimum for the weights
constexpr uint32_t kGuard = 56;           // u32, poisoned with 0x7FF7FFF1 (a NaN) on the rate path
constexpr uint32_t kParam60 = 60;         // f32, the rate sub_82B2FE00 clamps
constexpr uint32_t kParam68 = 68;         // f32, sub_82B2F2C8's "current level"
constexpr uint32_t kPublishedBlend = 348;  // f32[4]: +348 copies +1100, +352 copies +52,
                                           //         +356 copies +60, +360 copies +68
constexpr uint32_t kPublished52 = 352;
constexpr uint32_t kPublished60 = 356;
constexpr uint32_t kPublished68 = 360;
constexpr uint32_t kPublishedBytes = 16;  // [348, 364)
constexpr uint32_t kRamp = 364;           // f32[6], sub_82B2FE00's output, sub_82B2FEA8's input
constexpr uint32_t kPoints = 388;         // s32[6], sub_82B2FEA8's output
constexpr uint32_t kBands = 436;          // f32[6], sub_82B2F2C8's output
constexpr uint32_t kWeights = 460;        // f32[6], (1 - band) * (1 - floor/param52)
constexpr uint32_t kBlockBytes = 24;      // six singles: kRamp, kPoints, kBands and kWeights
constexpr uint32_t kReady = 1088;         // u8, zero selects sub_82B2FF88 over the record loop
constexpr uint32_t kVoices = 1089;        // u8, the record loop's bound, RE-READ every iteration
constexpr uint32_t kState = 1092;         // s32, 1 = dirty (the only value this runs for), left 2
constexpr uint32_t kBlend = 1100;         // f32, handed to sub_82B2FEA8 and sub_82B2F2C8 in f1

// The per-voice records the loop rewrites: 60 bytes each from +140. Each iteration reads +24/+28 and
// writes +36, +40, +44, +56 and the byte at +60 -- which is the NEXT record's first byte.
constexpr uint32_t kRecordArray = 140;
constexpr uint32_t kRecordStride = 60;
constexpr uint32_t kRecordFirstStore = 36;   // the lowest offset the loop writes in a record
constexpr uint32_t kRecordLastStore = 60;    // the highest, inclusive (the stbu byte)
constexpr uint32_t kWordArray = 1068;        // lwzu r8,4(r9) from +1068, so +1072 is the first read

// 0x7FF7FFF1: lis r11,32759 ; ori r10,r11,65521, so ((32759 & 0xFFFF) << 16) | 65521.
constexpr uint32_t kGuardWord = 0x7FF7FFF1u;

constexpr uint32_t kFrameBytes = 128;  // stwu r1,-128(r1)
constexpr uint32_t kScratch = 80;      // stfs f0,80(r1): the single sub_82B2FE00 clamps in place

// Two rodata singles, each computed as ((imm & 0xFFFF) << 16) + offset, never read off by eye:
//   0x822F8A5C  lis -32208 ; addi -31232 ; lfs 1116  -- the floor +52 is clamped up to. The same
//               constant pool sub_82B2F2C8 reads at +1120 and sub_82B298E0 at +480.
//   0x8231A844  lis -32206 ; lfs -22460             -- 1.0f, the word four other ports load.
constexpr uint32_t kFloorSingle = 0x822F8A5C;
constexpr uint32_t kOneSingle = 0x8231A844;

// What the four callees write, mirrored from their own ported Windows() rather than re-derived.
// sub_82B2FE00 writes r3+0 (4 bytes, here this function's own frame) and r4+0 (24).
// sub_82B2FEA8 writes r5+0 (24) and returns false itself when r3+1096 reads 0.
// sub_82B2F2C8 writes r4+0 (24) always, and when the singles at +68 and +360 differ also six
//   reservation records and six envelope words:
constexpr uint32_t kTableOffset = 1096;      // sub_82B2FEA8's kTableOffset
constexpr uint32_t kEnvelopeBase = 480;      // sub_82B2F2C8's kEnvelopeBase/kEnvelopeStride
constexpr uint32_t kEnvelopeStride = 36;
constexpr uint32_t kQueueBase = 720;         // sub_82B2F2C8's kQueueBase/kQueueStride
constexpr uint32_t kQueueStride = 180;
constexpr uint32_t kQueues = 2;
constexpr uint32_t kQueueRecords = 3;
constexpr uint32_t kRecStride = 60;
constexpr uint32_t kRecLimitCopy = 12;
constexpr uint32_t kRecPublished = 25;       // +12 .. +36 inclusive
constexpr uint32_t kOutputs = 6;
// sub_82B2FF88 (a leaf, outside the audio closure) writes +1060 .. +1083 and the byte at +1089,
// which of them depending on the byte at +42; the union is declared.
constexpr uint32_t kVoiceBlock = 1060;
constexpr uint32_t kVoiceBlockBytes = 24;
constexpr uint32_t kVoiceKind = 42;  // u8, read by sub_82B2FF88 to pick 1, 2 or 3 bands

// lfs: the 32-bit word reinterpreted as a single, widened to double.
inline double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return static_cast<double>(temp.f32);
}

// stfs: the double narrowed to single, stored as its 32-bit word.
inline void StoreSingle(uint8_t* base, uint32_t addr, double value) {
  PPCRegister temp{};
  temp.f32 = static_cast<float>(value);
  REX_STORE_U32(addr, temp.u32);
}

// fsubs / fmuls / fdivs: the double operation rounded to single, as the lifted line does it.
inline double SubSingle(double a, double b) { return static_cast<double>(static_cast<float>(a - b)); }
inline double MulSingle(double a, double b) { return static_cast<double>(static_cast<float>(a * b)); }
inline double DivSingle(double a, double b) { return static_cast<double>(static_cast<float>(a / b)); }

// fcmpu + beq on two singles, from the stored words: a NaN on either side is never equal, and the
// two zeroes are. Copied from sub_82B2F2C8.inc's SingleEqual (same aggregator, lower address, but
// copied so this file reads on its own) -- if that changes, this must change with it.
inline bool SingleEqual(uint32_t a, uint32_t b) {
  const bool a_nan = (a & 0x7F800000u) == 0x7F800000u && (a & 0x007FFFFFu) != 0u;
  const bool b_nan = (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0u;
  if (a_nan || b_nan) return false;
  if ((a & 0x7FFFFFFFu) == 0u && (b & 0x7FFFFFFFu) == 0u) return true;
  return a == b;
}

// [a, a+alen) and [b, b+blen) overlap, modulo 2^32.
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return (b - a) < alen || (a - b) < blen;
}

REX_FUNC(Native) {
  const uint32_t object = ctx.r3.u32;  // mr r31,r3

  // __savegprlr_29 spills r29-r31 and the tail restores them; the harness compares that range.
  const PPCRegister saved_gpr[3] = {ctx.r29, ctx.r30, ctx.r31};

  // stwu r1,-128(r1). Reproduced -- most ports skip their own frame -- because sub_82B2FE00 is
  // handed a pointer at r1+80 and clamps it in place, and because sub_82B2F2C8 and sub_82B2FF88
  // both use the red zone below the LIVE r1 for their own scratch.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;
  const uint32_t frame = ctx.r1.u32;

  const int32_t state = static_cast<int32_t>(REX_LOAD_U32(object + kState));  // lwz r11,1092(r3)
  // cmpwi cr6,r11,1 ; bne cr6,0x82b2f790 -- anything but 1 leaves the object untouched
  if (state == 1) {
    bool retuned = false;  // r29, zeroed at entry (mr r29,r30); only the rate path sets it

    ctx.fpscr.disableFlushModeUnconditional();
    const double rate = LoadSingle(base, object + kParam60);            // lfs f0,60(r3)
    const double rate_published = LoadSingle(base, object + kPublished60);  // lfs f13,356(r3)

    // fcmpu cr6,f0,f13 ; beq cr6,0x82b2f614 -- unordered is not equal, so a NaN takes this path
    if (!(rate == rate_published)) {
      StoreSingle(base, frame + kScratch, rate);  // stfs f0,80(r1)
      // bl 0x82b2fe00 -- clamps the single at r3 and fills the six-point ramp at r4
      skate3::audio::GuestCall(ctx, base, sub_82B2FE00, frame + kScratch, object + kRamp);

      ctx.fpscr.disableFlushModeUnconditional();
      const double clamped = LoadSingle(base, frame + kScratch);  // lfs f0,80(r1), after the call
      StoreSingle(base, object + kParam60, clamped);              // stfs f0,60(r31)
      REX_STORE_U32(object + kGuard, kGuardWord);                 // stw r10,56(r31)
      ctx.f1.f64 = LoadSingle(base, object + kBlend);             // lfs f1,1100(r31)
      // bl 0x82b2fea8. The original does NOT reload r4: sub_82B2FE00 leaves it holding +364. Set
      // explicitly here rather than depend on a volatile register surviving a call that may itself
      // be promoted -- same value either way.
      skate3::audio::GuestCall(ctx, base, sub_82B2FEA8, object, object + kRamp, object + kPoints);

      ctx.fpscr.disableFlushModeUnconditional();
      ctx.f1.f64 = LoadSingle(base, object + kBlend);  // lfs f1,1100(r31), reloaded as lifted
      ctx.r6.u64 = object + kRamp;                     // mr r6,r4 -- still +364 from above
      // bl 0x82b2f2c8 -- r5 is left holding +388 and that callee never reads it
      skate3::audio::GuestCall(ctx, base, sub_82B2F2C8, object, object + kBands);
      retuned = true;  // li r29,1
    } else {
      // loc_82B2F614
      ctx.fpscr.disableFlushModeUnconditional();
      const double blend_published = LoadSingle(base, object + kPublishedBlend);  // lfs f0,348(r31)
      const double blend = LoadSingle(base, object + kBlend);                     // lfs f1,1100(r31)
      // fcmpu cr6,f1,f0 ; beq cr6,0x82b2f650
      if (!(blend == blend_published)) {
        ctx.f1.f64 = blend;  // f1 still holds the +1100 single loaded for the compare
        skate3::audio::GuestCall(ctx, base, sub_82B2FEA8, object, object + kRamp, object + kPoints);

        ctx.fpscr.disableFlushModeUnconditional();
        ctx.f1.f64 = LoadSingle(base, object + kBlend);  // lfs f1,1100(r31), reloaded as lifted
        ctx.r6.u64 = object + kRamp;                     // mr r6,r4
        skate3::audio::GuestCall(ctx, base, sub_82B2F2C8, object, object + kBands);
        REX_STORE_U8(object + kReady, 0);                // stb r30,1088(r31)
      } else {
        // loc_82B2F650
        ctx.fpscr.disableFlushModeUnconditional();
        const double level = LoadSingle(base, object + kParam68);               // lfs f0,68(r31)
        const double level_published = LoadSingle(base, object + kPublished68);  // lfs f13,360(r31)
        // fcmpu cr6,f0,f13 ; beq cr6,0x82b2f670
        if (!(level == level_published)) {
          ctx.r6.u64 = object + kRamp;  // addi r6,r31,364
          // f1 is NOT reloaded on this path: it still holds the +1100 single from the compare above
          ctx.f1.f64 = blend;
          skate3::audio::GuestCall(ctx, base, sub_82B2F2C8, object, object + kBands);
        }
      }
    }

    // loc_82B2F670: the six weights. Every load precedes every store in the lifted body.
    ctx.fpscr.disableFlushModeUnconditional();
    double param = LoadSingle(base, object + kParam52);  // lfs f0,52(r31)
    const double floor_value = LoadSingle(base, kFloorSingle);  // lfs f13,1116(r10)
    // fcmpu cr6,f0,f13 ; bgt cr6,0x82b2f68c ; fmr f0,f13 -- unordered is not >, so a NaN is replaced
    if (!(param > floor_value)) param = floor_value;
    ctx.fpscr.disableFlushModeUnconditional();
    const double ratio = DivSingle(floor_value, param);  // fdivs f13,f13,f0
    double band[kOutputs];
    for (uint32_t k = 0; k < 4; k++) {  // lfs f12,436 ; lfs f11,440 ; lfs f10,444 ; lfs f9,448
      band[k] = LoadSingle(base, object + kBands + 4 * k);
    }
    const double one = LoadSingle(base, kOneSingle);          // lfs f0,-22460(r11)
    band[4] = LoadSingle(base, object + kBands + 16);         // lfs f8,452(r31)
    const double rest0 = SubSingle(one, band[0]);             // fsubs f7,f0,f12
    band[5] = LoadSingle(base, object + kBands + 20);         // lfs f6,456(r31)
    const double rest1 = SubSingle(one, band[1]);             // fsubs f5,f0,f11
    const double rest2 = SubSingle(one, band[2]);             // fsubs f4,f0,f10
    const double rest3 = SubSingle(one, band[3]);             // fsubs f3,f0,f9
    const double depth = SubSingle(one, ratio);               // fsubs f2,f0,f13
    const double rest4 = SubSingle(one, band[4]);             // fsubs f1,f0,f8
    const double rest5 = SubSingle(one, band[5]);             // fsubs f0,f0,f6
    StoreSingle(base, object + kWeights + 0, MulSingle(rest0, depth));   // stfs f13,460(r31)
    StoreSingle(base, object + kWeights + 4, MulSingle(rest1, depth));   // stfs f12,464(r31)
    StoreSingle(base, object + kWeights + 8, MulSingle(rest2, depth));   // stfs f11,468(r31)
    StoreSingle(base, object + kWeights + 12, MulSingle(rest3, depth));  // stfs f10,472(r31)
    StoreSingle(base, object + kWeights + 16, MulSingle(rest4, depth));  // stfs f9,476(r31)
    StoreSingle(base, object + kWeights + 20, MulSingle(rest5, depth));  // stfs f8,480(r31)

    // lbz r10,1088(r31) ; cmplwi cr6,r10,0 ; bne cr6,0x82b2f714
    if (REX_LOAD_U8(object + kReady) == 0) {
      skate3::audio::GuestCall(ctx, base, sub_82B2FF88, object);  // bl 0x82b2ff88
    } else {
      // loc_82B2F714: clrlwi r11,r29,24 ; beq cr6 -- only the rate path reaches the loop, and only
      // when the voice count is non-zero.
      const bool have_voices = REX_LOAD_U8(object + kVoices) != 0;  // lbz r11,1089(r31)
      if (retuned && have_voices) {
        uint32_t words = object + kWordArray;    // addi r9,r31,1068
        uint32_t record = object + kRecordArray;  // addi r11,r31,140
        int32_t index = 0;                        // mr r10,r30
        do {                                      // loc_82B2F738
          const uint32_t from24 = REX_LOAD_U32(record + 24);  // lwz r7,24(r11)
          index += 1;                                         // addi r10,r10,1
          words += 4;                                         // lwzu r8,4(r9)
          const uint32_t word = REX_LOAD_U32(words);
          const uint32_t from28 = REX_LOAD_U32(record + 28);  // lwz r6,28(r11)
          REX_STORE_U32(record + 40, 0u);                     // stw r30,40(r11)
          REX_STORE_U32(record + 36, from24);                 // stw r7,36(r11)
          REX_STORE_U32(record + 44, word);                   // stw r8,44(r11)
          REX_STORE_U32(record + 56, from28);                 // stw r6,56(r11)
          record += kRecordStride;                            // stbu r30,60(r11)
          REX_STORE_U8(record, 0u);                           // (lands on the next record's +0)
          // lbz r5,1089(r31) -- re-read every iteration ; cmpw cr6,r10,r5 ; blt cr6
        } while (index < static_cast<int32_t>(REX_LOAD_U8(object + kVoices)));
      }
    }

    // loc_82B2F768: publish all four parameters, with the state word stored between two of the
    // loads, exactly where the lifted body puts it.
    ctx.fpscr.disableFlushModeUnconditional();
    const double final52 = LoadSingle(base, object + kParam52);  // lfs f0,52(r31)
    const double final60 = LoadSingle(base, object + kParam60);  // lfs f13,60(r31)
    const double final68 = LoadSingle(base, object + kParam68);  // lfs f12,68(r31)
    REX_STORE_U32(object + kState, 2u);                          // li r11,2 ; stw r11,1092(r31)
    const double final_blend = LoadSingle(base, object + kBlend);  // lfs f11,1100(r31)
    StoreSingle(base, object + kPublished52, final52);            // stfs f0,352(r31)
    StoreSingle(base, object + kPublished60, final60);            // stfs f13,356(r31)
    StoreSingle(base, object + kPublished68, final68);            // stfs f12,360(r31)
    StoreSingle(base, object + kPublishedBlend, final_blend);     // stfs f11,348(r31)
  }

  // loc_82B2F790: addi r1,r1,128 ; b 0x82f5216c (__restgprlr_29)
  ctx.r1.u32 = entry_sp;
  ctx.r29 = saved_gpr[0];
  ctx.r30 = saved_gpr[1];
  ctx.r31 = saved_gpr[2];
}

// Which of the four paths runs is decided entirely by memory readable at entry: the state word, then
// the singles at +60/+356, +1100/+348 and +68/+360, and finally the byte at +1088 -- and none of the
// four callees writes any of those (the highest object byte sub_82B2F2C8 touches is +1056, and
// sub_82B2FE00 / sub_82B2FEA8 stay inside +364 .. +411). So the write set is enumerable, and it is
// declared per path rather than as one union:
//
//   state != 1                      nothing at all
//   +60 != +356   (the rate path)   +56/+60, sub_82B2FE00's +364, sub_82B2FEA8's +388,
//                                   sub_82B2F2C8's, the weights, then EITHER sub_82B2FF88's block
//                                   or the record loop, then the state word and the four copies
//   +1100 != +348                   as above without +56/+60 and without +364, plus the +1088 byte,
//                                   and sub_82B2FF88 rather than the loop (that store forces it)
//   +68 != +360                     sub_82B2F2C8's only, the weights, sub_82B2FF88 when +1088 is 0
//   all three equal                 the weights, sub_82B2FF88 when +1088 is 0, the tail
//
// Not declared: the clamped single at r1+80, and the red-zone scratch of sub_82B2F2C8 (r1-80) and
// sub_82B2FF88 (r1-16) -- each is its own function's frame.
//
// Returns false when the record loop's bound could itself be written (it cannot, but the check is
// mechanical rather than a paragraph), when the table pointer sub_82B2FEA8 sweeps is null -- mirroring
// that callee's own Windows() -- or when a record store would land on the state word or a published
// copy, which would make the tail's own stores unpredictable.
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t object = ctx.r3.u32;
  if (object == 0) return false;  // the original would store through null

  spec.read(object + kState, 4);
  if (static_cast<int32_t>(REX_LOAD_U32(object + kState)) != 1) return true;  // nothing runs

  const uint32_t rate = REX_LOAD_U32(object + kParam60);
  const uint32_t rate_published = REX_LOAD_U32(object + kPublished60);
  const uint32_t blend = REX_LOAD_U32(object + kBlend);
  const uint32_t blend_published = REX_LOAD_U32(object + kPublishedBlend);
  const uint32_t level = REX_LOAD_U32(object + kParam68);
  const uint32_t level_published = REX_LOAD_U32(object + kPublished68);
  const bool rate_moved = !SingleEqual(rate, rate_published);
  const bool blend_moved = !rate_moved && !SingleEqual(blend, blend_published);
  const bool level_moved = !rate_moved && !blend_moved && !SingleEqual(level, level_published);

  // sub_82B2FEA8 runs on the first two paths, and declines to be replayed itself when the table
  // pointer is null; mirror that rather than compare half a call.
  if ((rate_moved || blend_moved) && REX_LOAD_U32(object + kTableOffset) == 0) return false;

  if (rate_moved) {
    spec.write(object + kGuard, 8);            // +56 and +60, contiguous
    spec.write(object + kRamp, kBlockBytes);   // sub_82B2FE00's r4
  }
  if (rate_moved || blend_moved) {
    spec.write(object + kPoints, kBlockBytes);  // sub_82B2FEA8's r5
  }
  if (blend_moved) {
    spec.write(object + kReady, 1);  // stb r30,1088(r31)
  }
  const bool resampled = rate_moved || blend_moved || level_moved;
  if (resampled) {
    // sub_82B2F2C8: its r4 always; the six records and six envelope words only when +68 and +360
    // differ. Its own Windows() also bails when r4 overlaps +68, +360, the sizes at +388 or a
    // record's first two words -- r4 is +436 here, which is disjoint from all of them by
    // construction, so that exit cannot be reached from this caller.
    spec.write(object + kBands, kBlockBytes);
    if (!SingleEqual(level, level_published)) {
      for (uint32_t q = 0; q < 2; q++) {    // kQueues
        for (uint32_t r = 0; r < 3; r++) {  // kQueueRecords
          const uint32_t rec = object + kQueueBase + kQueueStride * q + kRecStride * r;
          spec.write(rec + kRecLimitCopy, kRecPublished);
          spec.read(rec, 8);
        }
      }
      for (uint32_t i = 0; i < 6; i++) {  // kOutputs
        spec.write(object + kEnvelopeBase + kEnvelopeStride * (i + 1), 4);
      }
      // sub_82B2F2C8's "pending sizes" cursor is object + 392 - 4, which is exactly the six words
      // sub_82B2FEA8 writes at +388.
      spec.read(object + kPoints, 4 * kQueues * kQueueRecords);
    }
    spec.read(object + kRamp, kBlockBytes);  // sub_82B2F2C8's r6, and sub_82B2FEA8's r4
  }

  spec.write(object + kWeights, kBlockBytes);  // the six weights, on every path that gets here
  spec.write(object + kState, 4);
  spec.write(object + kPublishedBlend, kPublishedBytes);

  // Reads recorded for replay: the three parameters and their published copies, the blend single,
  // the two flag bytes, the bands the weights come from and the two rodata singles. The rodata and
  // tables the four callees read -- sub_82B2FEA8's 1652-entry table above all -- are recorded by
  // their own hooks, not duplicated here.
  spec.read(object + kParam52, 20);  // +52 .. +71: +52, +60 and +68 (and +56, which is not read)
  spec.read(object + kPublishedBlend, kPublishedBytes);
  spec.read(object + kBlend, 4);
  spec.read(object + kTableOffset, 4);
  spec.read(object + kBands, kBlockBytes);
  spec.read(object + kReady, 2);
  spec.read(kFloorSingle, 4);
  spec.read(kOneSingle, 4);
  // The callees' rodata, for replay only, copied from their own window builders: sub_82B2FE00's
  // pool words and singles, sub_82B2FEA8's pool and its 1652-entry table, sub_82B2F2C8's edges, knot
  // table, rates, headroom and zero, and sub_82B2FF88's rates and candidates. 3 of 300 recorded calls
  // stopped at sub_82B2FE00's +468 before this.
  spec.read(0x822F87C8u, 24);   // +456 .. +479: sub_82B2FEA8's three singles, sub_82B2FE00's +468..+479
  spec.read(0x82060C50u, 4);
  spec.read(0x8208824Cu, 4);
  spec.read(0x822249B4u, 4);
  spec.read(0x820ED57Cu, 4);
  spec.read(0x82099280u, 4);
  const uint32_t table = REX_LOAD_U32(object + kTableOffset);
  if (table != 0 && uint64_t(table) + 1652ull * 4 <= 0x100000000ull) spec.read(table, 1652 * 4);
  spec.read(0x821161A0u, 12);
  spec.read(0x821161B0u, 216);
  spec.read(0x822F8A50u, 20);   // +1104 .. +1123: sub_82B2FF88's three and sub_82B2F2C8's +1120
  spec.read(0x822F8A64u, 4);    // sub_82B2F2C8's +1124
  spec.read(0x82063A48u, 4);
  spec.read(0x82165A10u, 4);
  spec.read(0x82116288u, 4);
  spec.read(0x821161ACu, 4);
  spec.read(0x820BA9D0u, 4);
  spec.read(0x8209975Cu, 4);

  // The +1088 byte decides the last block. On the blend path this function has just stored zero into
  // it, so sub_82B2FF88 is certain; otherwise the entry value decides.
  const bool ready = !blend_moved && REX_LOAD_U8(object + kReady) != 0;
  if (!ready) {
    // sub_82B2FF88's union: +1060 .. +1083 and the byte at +1089. Declared as one span through
    // +1089, four unwritten bytes included -- they compare equal, and +1088 inside it is read before
    // that call, not after.
    spec.write(object + kVoiceBlock, kVoiceBlockBytes + 6);
    spec.read(object + kVoiceKind, 1);
    spec.read(object + kBlend, 4);
    return true;
  }

  const uint32_t voices = REX_LOAD_U8(object + kVoices);
  if (!rate_moved || voices == 0) return true;  // the loop needs r29 == 1 and a non-zero count

  // The loop's bound is re-read every iteration, so it has to be unwritable by the loop itself. The
  // five stores of record i sit at +36/+40/+44/+56 and the byte at +60 from 140 + 60i; none of those
  // residues can reach +1089, but check it rather than argue it. The state word and the published
  // copies are written by the tail AFTER the loop, so a record landing on them is fine; a record
  // landing on +1089 is not.
  for (uint32_t i = 0; i < voices && i < 256; i++) {  // a u8 count: 255 records at most
    const uint32_t rec = object + kRecordArray + kRecordStride * i;
    if (Overlaps(rec + 36, 4, object + kVoices, 1)) return false;
    if (Overlaps(rec + 40, 4, object + kVoices, 1)) return false;
    if (Overlaps(rec + 44, 4, object + kVoices, 1)) return false;
    if (Overlaps(rec + 56, 4, object + kVoices, 1)) return false;
    if (Overlaps(rec + kRecordLastStore, 1, object + kVoices, 1)) return false;
  }
  // One span over all of it: from the first store of record 0 to the last of record n-1. The gaps
  // are not written and compare equal; at stride 60 and up to 255 records this is at most 15 KB, so
  // it stays inside the budget where 255 separate spans could not.
  const uint32_t first = object + kRecordArray + kRecordFirstStore;
  const uint32_t last = object + kRecordArray + kRecordStride * (voices - 1) + kRecordLastStore;
  spec.write(first, last + 1 - first);
  spec.read(object + kRecordArray, kRecordStride * voices);
  spec.read(object + kWordArray + 4, 4 * voices);
  spec.read(object + kVoices, 1);
  return true;
}

}  // namespace port_82B2F590
SKATE3_PORT(82B2F590, skate3::audio::kPortVerified, skate3::audio::kReturnNone)
