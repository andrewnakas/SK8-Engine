// sub_82B2F8C0  role: run one block of a Dac object's surround output chain -- with the level at +52 at or below zero (or with a parameter changed and the state word at +1092 still 0) memset one 1024-byte block per channel and leave; otherwise take 3072 bytes off the owner's scratch pointer, wire the six mix-stage records at +700 and the up-to-three output-stage records at +144 to their two entry points (0x82B39FA0/0x82B3A038 and 0x82B38B68/0x82B61BB8) and that one scratch block, run all six mix stages and then one output stage per speaker group through sub_82B3D8E0, replicating the block channel 0 holds into every channel each group feeds, zeroing the last channel of a 6- or 8-channel layout, and finally hand the scratch pointer back   lifted: skate3_recomp.67.cpp:43461 (742 lines)
// STATUS: gate-1 (sub_82B3D8E0 hands every block to the mixer through the pointer at its object's +4 -> +0: sub_82B3D8E0 indirect call, `bctrl` at 0x82B3DA00 and 0x82B3DA8C, depth 1)
// calls/session: boot 30433  play ?  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B2F8C0.md
namespace port_82B2F8C0 {

// The object (r3/r31), about 1104 bytes. Nothing in docs/rw_audio_structs.h names it, so every
// offset below is named for what this one function does with it.
constexpr uint32_t kOwner = 8;          // -> a holder whose [0] owns the scratch pointer
constexpr uint32_t kLayout = 42;        // u8 channel count: 1, 2, 4, 6 or 8 are the shapes handled
constexpr uint32_t kLevel = 52;         // float; at or below zero the whole block is silence
constexpr uint32_t kParam60 = 60;       // float
constexpr uint32_t kParam68 = 68;       // float
constexpr uint32_t kParam1100 = 1100;   // float
constexpr uint32_t kParam1100Cached = 348;  // the last configured value of +1100
constexpr uint32_t kLevelCached = 352;      // ... of +52
constexpr uint32_t kParam60Cached = 356;    // ... of +60
constexpr uint32_t kParam68Cached = 360;    // ... of +68
constexpr uint32_t kOutCount = 1089;    // u8 output stages, re-read at the bottom of its loop
constexpr uint32_t kState = 1092;       // u32: 0 idle, 1 running, 3 -> sub_82B2F798 then 4

// The scratch block. The holder's +32 is a downward-growing pointer: 3072 bytes are taken here and
// the entry value is written back at loc_82B2FDD0, so the block lives exactly for this call.
constexpr uint32_t kScratchField = 32;
constexpr uint32_t kScratchBytes = 3072;  // addi r11,r26,-3072
constexpr uint32_t kScratchWords = 768;   // li r10,768 -- 3072/4, the same block measured in words

// The two stage families. Each "filter" is an object whose first two words are entry points, and
// each "record" is the object sub_82B3D8E0 takes in r3 (its +4 is the mixer whose +0 it calls
// through -- kObjMixer/kMixerEntry in sub_82B3D8E0.inc).
constexpr uint32_t kMixFilterBase = 484;   // six of them, 36 bytes apart
constexpr uint32_t kMixFilterStride = 36;
constexpr uint32_t kMixStages = 6;         // li r29,5 plus the one call before the loop
constexpr uint32_t kMixRecordBase = 700;   // addi r3,r31,700 ; addi r30,r31,760 ; ...
constexpr uint32_t kOutFilterBase = 72;    // addi r9,r31,72 -- up to three, 24 bytes apart
constexpr uint32_t kOutFilterStride = 24;
constexpr uint32_t kOutRecordBase = 144;   // addi r3,r31,144 / 204 / 264 in the tail
constexpr uint32_t kRecordStride = 60;
constexpr uint32_t kEntryProcess = 0;      // filter+0
constexpr uint32_t kEntryReset = 4;        // filter+4
constexpr uint32_t kRecFilter = 4;         // record+4 -> its filter object
constexpr uint32_t kRecScratch = 8;        // record+8 -> the shared scratch block
constexpr uint32_t kRecWords = 28;         // record+28 = 768

// The four entry points, each ((lis_imm & 0xFFFF) << 16) + offset -- computed, not read by eye,
// and each one checked against a DEFINE_REX_FUNC of that address in the lifted tree.
// lis r10,-32076 -> 0x82B40000 ; addi r8,r10,-24672
constexpr uint32_t kMixProcess = 0x82B39FA0;  // sub_82B39FA0, the mix dispatcher
// lis r9,-32076 ; addi r6,r9,-24520
constexpr uint32_t kMixReset = 0x82B3A038;    // sub_82B3A038, which just zeroes the gain at +32
// lis r6,-32076 ; addi r4,r6,-29848
constexpr uint32_t kOutProcess = 0x82B38B68;  // sub_82B38B68, the allpass-stage dispatcher
// lis r5,-32074 -> 0x82B60000 ; addi r6,r5,7096
constexpr uint32_t kOutReset = 0x82B61BB8;    // sub_82B61BB8, a bare `blr`

// lis r11,-32234 -> 0x82160000 ; lfs f13,23056(r11) -- the same rodata zero sub_82B2DAC8 loads.
constexpr uint32_t kZeroFloat = 0x82165A10;

// The buffer-descriptor pair (r4/r25): two descriptors that swap roles for the tail.
constexpr uint32_t kPairFirst = 28;
constexpr uint32_t kPairSecond = 32;

// A buffer descriptor, the layout sub_82B3D8E0 and sub_82B3CA60 also read.
constexpr uint32_t kBufData = 4;     // u32 -> the samples, four bytes each
constexpr uint32_t kBufFrames = 14;  // u16 words between one channel's block and the next

// Every memset and memcpy here is exactly 1024 bytes, i.e. one 256-word block. The channel stride
// is 4*frames, so this code only lines up when frames is 256; nothing checks that.
constexpr uint32_t kBlockBytes = 1024;

constexpr uint32_t kFrameBytes = 160;  // stwu r1,-160(r1)

double LoadSingle(uint8_t* base, uint32_t addr) {
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

// The byte offset of one channel's block. The original spells this six different ways --
// `rotlwi r11,r11,2`, `rotlwi r11,r11,3`, `rotlwi r11,r11,4`, `3*f` then `rlwinm ...,2,0,29` or
// `,3,0,28`, `5*f` then `rlwinm ...,2,0,29`, and `mullw` then `rlwinm ...,2,0,29` -- and they all
// agree: `frames` comes from an `lhz` so it is under 2^16, nothing can rotate into the low bits,
// and every product is already a multiple of 4 or 8, so the rlwinm masks drop nothing.
inline uint64_t ChannelOffset(uint64_t frames, uint64_t channel) {
  return uint32_t(uint32_t(frames) * uint32_t(channel) * 4u);
}

// The epilogue all three returns share: `addi r1,r1,160` then __restgprlr_23, which reloads
// r23..r31 and the link register from the frame this call is giving back.
void Restore(PPCContext& ctx, uint64_t entry_lr, const PPCRegister* saved) {
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;  // addi r1,r1,160
  PPCRegister* const slots[] = {&ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27,
                                &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (size_t i = 0; i < 9; i++) *slots[i] = saved[i];
  ctx.r12.u64 = entry_lr;  // lwz r12,-8(r1)
  ctx.lr = entry_lr;       // mtlr r12
}

// loc_82B2F918 and loc_82B2F9B4 are the same loop twice: zero one 1024-byte block per channel of
// the first descriptor's buffer. The count byte is re-read at the bottom of every pass.
void ClearChannels(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t desc) {
  uint64_t channel = 0;  // li r30,0
  // cmplwi cr6,r11,0 ; beq -- nothing to clear
  if (REX_LOAD_U8(object + kLayout) == 0) return;
  do {
    const uint64_t frames = REX_LOAD_U16(desc + kBufFrames);  // lhz r10,14(r28)
    const uint32_t data = REX_LOAD_U32(desc + kBufData);      // lwz r11,4(r28)
    // mullw r9,r10,r30 ; rlwinm r10,r9,2,0,29 ; add r3,r10,r11 -- a 64-bit sum of two
    // zero-extended halves, as lifted.
    skate3::audio::GuestCall(ctx, base, sub_82EE5E80, ChannelOffset(frames, channel) + data, 0,
                             kBlockBytes);  // bl 0x82ee5e80 -- memset(dst, 0, 1024)
    channel += 1;                           // addi r30,r30,1
    // lbz r8,42(r31) ; cmplw cr6,r30,r8 ; blt -- UNSIGNED, bound re-read every pass
  } while (uint32_t(channel) < REX_LOAD_U8(object + kLayout));
}

// Every `bl 0x82edf460` in the tail: copy the block at the buffer's channel 0 into `channel`.
// `src` is passed in because the original sometimes reuses a pointer loaded several instructions
// earlier and sometimes the one it just loaded for the destination; both are the same value,
// nothing here writes the descriptor.
void CopyChannel(PPCContext& ctx, uint8_t* base, uint32_t data, uint64_t frames, uint64_t channel,
                 uint32_t src) {
  skate3::audio::GuestCall(ctx, base, sub_82EDF460, ChannelOffset(frames, channel) + data, src,
                           kBlockBytes);  // bl 0x82edf460 -- memcpy(dst, src, 1024)
}

REX_FUNC(Native) {
  // __savegprlr_23 spills r23..r31 and the link register at -8..-80 off the entry stack pointer --
  // inside the 160 bytes the stwu below claims -- and __restgprlr_23 reloads all of them. r23-r31
  // are compared, so the entry values are kept here rather than the spills reproduced.
  const PPCRegister saved[9] = {ctx.r23, ctx.r24, ctx.r25, ctx.r26, ctx.r27,
                                ctx.r28, ctx.r29, ctx.r30, ctx.r31};
  const uint64_t entry_lr = ctx.lr;  // mflr r12
  ctx.r12.u64 = entry_lr;

  const uint32_t entry_sp = ctx.r1.u32;
  const uint32_t frame = entry_sp - kFrameBytes;  // stwu r1,-160(r1)
  REX_STORE_U32(frame, entry_sp);
  ctx.r1.u32 = frame;

  const uint32_t object = ctx.r3.u32;  // mr r31,r3
  const uint32_t pair = ctx.r4.u32;    // mr r25,r4

  // lwz r11,1092(r3) ; cmpwi cr6,r11,3 ; bne -- SIGNED, and exactly 3.
  if (int32_t(REX_LOAD_U32(object + kState)) == 3) {
    // bl 0x82b2f798 -- r3 is still the entry r3, so it takes this same object.
    skate3::audio::GuestCall(ctx, base, sub_82B2F798, object);
    REX_STORE_U32(object + kState, 4);  // li r11,4 ; stw r11,1092(r31)
  }

  // loc_82B2F8EC -- both descriptors are read HERE, before every store below, and every later use
  // is of these two values.
  ctx.fpscr.disableFlushModeUnconditional();
  const double level = LoadSingle(base, object + kLevel);    // lfs f0,52(r31)
  const uint32_t first = REX_LOAD_U32(pair + kPairFirst);    // lwz r28,28(r25)
  const uint32_t second = REX_LOAD_U32(pair + kPairSecond);  // lwz r27,32(r25)
  const double zero = LoadSingle(base, kZeroFloat);          // lfs f13,23056(r11)

  // fcmpu cr6,f0,f13 ; bgt cr6,0x82b2f95c -- a NaN level is unordered, so it takes this path too.
  if (!(level > zero)) {
    ClearChannels(ctx, base, object, first);
    REX_STORE_U32(object + kState, 0);  // li r11,0 ; stw r11,1092(r31)
    ctx.r3.s64 = 1;                     // li r3,1
    Restore(ctx, entry_lr, saved);
    return;
  }

  // loc_82B2F95C -- four parameter pairs, each `bne` jumping to loc_82B2F998. f0 still holds the
  // +52 value; the chain short-circuits exactly as the branches do.
  ctx.fpscr.disableFlushModeUnconditional();
  bool unchanged = false;
  if (level == LoadSingle(base, object + kLevelCached)) {  // lfs f13,352(r31) ; fcmpu ; bne
    const double b_now = LoadSingle(base, object + kParam60);
    const double b_was = LoadSingle(base, object + kParam60Cached);
    if (b_now == b_was) {
      const double c_now = LoadSingle(base, object + kParam68);
      const double c_was = LoadSingle(base, object + kParam68Cached);
      if (c_now == c_was) {
        const double d_now = LoadSingle(base, object + kParam1100);
        const double d_was = LoadSingle(base, object + kParam1100Cached);
        unchanged = (d_now == d_was);  // beq cr6,0x82b2fa08
      }
    }
  }

  if (!unchanged) {
    // loc_82B2F998 -- something was reconfigured. cmpwi cr6,r11,0 ; bne cr6,0x82b2fa00
    if (int32_t(REX_LOAD_U32(object + kState)) == 0) {
      ClearChannels(ctx, base, object, first);
      REX_STORE_U32(object + kState, 1);  // li r11,1 ; stw r11,1092(r31)
      // mr r3,r31 ; bl 0x82b2f590
      skate3::audio::GuestCall(ctx, base, sub_82B2F590, object);
      ctx.r3.s64 = 1;  // li r3,1
      Restore(ctx, entry_lr, saved);
      return;
    }
    // loc_82B2FA00
    REX_STORE_U32(object + kState, 1);  // li r11,1 ; stw r11,1092(r31)
  }

  // loc_82B2FA08 -- 3072 bytes come off the holder's scratch pointer and the same block is handed
  // to every stage record. The entry value is put back at loc_82B2FDD0.
  const uint32_t owner = REX_LOAD_U32(object + kOwner);                // lwz r11,8(r31)
  const uint32_t holder = REX_LOAD_U32(owner);                         // lwz r4,0(r11)
  const uint64_t scratch_top = REX_LOAD_U32(holder + kScratchField);   // lwz r26,32(r4)
  const uint64_t scratch = scratch_top - kScratchBytes;                // addi r11,r26,-3072
  REX_STORE_U32(holder + kScratchField, uint32_t(scratch));            // stw r11,32(r4)

  // 31 stores, one group of five per mix stage, in the original's own order. The addresses are all
  // distinct and nothing is read between them; only group 0 stores +28 before +8, a scheduling
  // artifact the original's register pressure produced.
  for (uint32_t stage = 0; stage < kMixStages; stage++) {
    const uint32_t filter = object + kMixFilterBase + stage * kMixFilterStride;
    const uint32_t record = object + kMixRecordBase + stage * kRecordStride;
    REX_STORE_U32(filter + kEntryProcess, kMixProcess);  // stw r8,484(r31) ; stw r5,520(r31) ; ...
    REX_STORE_U32(filter + kEntryReset, kMixReset);      // stw r6,488(r31) ; stw r3,524(r31) ; ...
    REX_STORE_U32(record + kRecFilter, filter);          // stw r7,704(r31) ; stw r9,764(r31) ; ...
    if (stage == 0) {
      REX_STORE_U32(record + kRecWords, kScratchWords);          // stw r10,728(r31)
      REX_STORE_U32(record + kRecScratch, uint32_t(scratch));    // stw r11,708(r31)
    } else {
      REX_STORE_U32(record + kRecScratch, uint32_t(scratch));    // stw r11,768(r31) ; ...
      REX_STORE_U32(record + kRecWords, kScratchWords);          // stw r10,788(r31) ; ...
    }
  }

  // lbz r6,1089(r31) ; cmplwi cr6,r6,0 ; beq cr6,0x82b2fb74
  if (REX_LOAD_U8(object + kOutCount) != 0) {
    uint32_t filter = object + kOutFilterBase;  // addi r9,r31,72
    // addi r8,r31,112 -- the original walks the records from 32 bytes BELOW kOutRecordBase and
    // stores at +36/+40/+60, which are this record's +4/+8/+28, the same three fields the mix
    // stages get. The last of the three is a `stwu`, which is what advances the walk.
    uint32_t record = object + kOutRecordBase;
    int32_t seen = 0;  // li r7,0
    do {
      REX_STORE_U32(filter + kEntryProcess, kOutProcess);        // stw r4,0(r9)
      seen += 1;                                                 // addi r7,r7,1
      REX_STORE_U32(filter + kEntryReset, kOutReset);            // stw r6,4(r9)
      REX_STORE_U32(record + kRecFilter, filter);                // stw r9,36(r8)
      filter += kOutFilterStride;                                // addi r9,r9,24
      REX_STORE_U32(record + kRecScratch, uint32_t(scratch));    // stw r11,40(r8)
      REX_STORE_U32(record + kRecWords, kScratchWords);          // stwu r10,60(r8)
      record += kRecordStride;
      // lbz r5,1089(r31) ; cmpw cr6,r7,r5 ; blt -- SIGNED, and the bound is re-read every pass.
    } while (seen < int32_t(REX_LOAD_U8(object + kOutCount)));
  }

  // loc_82B2FB74 -- stage 0 with flag 0, then stages 1..5 with flag 1. r3 for the first call is
  // object+700, materialised well above the branch by `addi r3,r31,700`.
  skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, object + kMixRecordBase, first, second, 0);
  // loc_82B2FB88 -- li r29,5 ; addic. r29,r29,-1 ; addi r30,r30,60 ; bne
  uint32_t mix_record = object + kMixRecordBase + kRecordStride;  // mr r3,r30, r30 = object+760
  for (int32_t left = 5; left != 0; left--) {
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, mix_record, first, second, 1);
    mix_record += kRecordStride;
  }

  // Swap the pair's two descriptors, so the output stages read what the mix stages just wrote. The
  // swap at loc_82B2FDD0 undoes this exactly, so the pair ends the call unchanged.
  const uint32_t was_first = REX_LOAD_U32(pair + kPairFirst);    // lwz r11,28(r25)
  const uint32_t was_second = REX_LOAD_U32(pair + kPairSecond);  // lwz r29,32(r25)
  REX_STORE_U32(pair + kPairSecond, was_first);                  // stw r11,32(r25)
  REX_STORE_U32(pair + kPairFirst, was_second);                  // stw r29,28(r25)

  const uint32_t layout = REX_LOAD_U8(object + kLayout);  // lbz r11,42(r31)
  // lwz r30,32(r25) -- RELOADED after the swap, so this is the descriptor that was at +28. Every
  // memcpy below reads its buffer, and every sub_82B3D8E0 call takes it as r5 with r29 as r4.
  const uint32_t desc = REX_LOAD_U32(pair + kPairSecond);
  const uint32_t other = was_second;  // mr r4,r29

  const uint32_t out0 = object + kOutRecordBase;                     // addi r3,r31,144
  const uint32_t out1 = object + kOutRecordBase + kRecordStride;     // addi r3,r31,204
  const uint32_t out2 = object + kOutRecordBase + 2 * kRecordStride; // addi r3,r31,264

  // cmplwi cr6,r11,1 ; beq cr6,0x82b2fbfc -- then cmplwi cr6,r11,2 ; bne cr6,0x82b2fc14
  if (layout == 1 || layout == 2) {
    if (layout == 2) {
      skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out1, other, desc, 0);
      const uint64_t frames = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
      const uint32_t data = REX_LOAD_U32(desc + kBufData);      // lwz r4,4(r30)
      CopyChannel(ctx, base, data, frames, 1, data);            // rotlwi r11,r11,2 -> channel 1
    }
    // loc_82B2FBFC
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out0, other, desc, 0);
  } else if (layout == 4) {
    // loc_82B2FC14 -- cmplwi cr6,r11,4 ; bne cr6,0x82b2fc94
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out1, other, desc, 0);
    const uint64_t frames_a = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
    const uint32_t held = REX_LOAD_U32(desc + kBufData);        // lwz r28,4(r30) -- kept in r28
    CopyChannel(ctx, base, held, frames_a, 1, held);            // rotlwi r11,r11,2 -> channel 1
    const uint64_t frames_b = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
    const uint32_t data_b = REX_LOAD_U32(desc + kBufData);      // lwz r9,4(r30)
    CopyChannel(ctx, base, data_b, frames_b, 3, held);          // 3*f then rlwinm ,2,0,29
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out0, other, desc, 0);
    const uint64_t frames_c = REX_LOAD_U16(desc + kBufFrames);  // lhz r9,14(r30)
    const uint32_t data_c = REX_LOAD_U32(desc + kBufData);      // lwz r4,4(r30)
    CopyChannel(ctx, base, data_c, frames_c, 2, data_c);        // rotlwi r11,r9,3 -> channel 2
  } else {
    // loc_82B2FC94 -- everything else, which is where 6 and 8 land.
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out2, other, desc, 0);
    const uint64_t frames_a = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
    const uint32_t held = REX_LOAD_U32(desc + kBufData);        // lwz r28,4(r30) -- kept in r28
    CopyChannel(ctx, base, held, frames_a, 2, held);            // rotlwi r11,r11,3 -> channel 2
    const uint64_t frames_b = REX_LOAD_U16(desc + kBufFrames);  // lhz r9,14(r30)
    const uint32_t data_b = REX_LOAD_U32(desc + kBufData);      // lwz r10,4(r30)
    CopyChannel(ctx, base, data_b, frames_b, 4, held);          // rotlwi r11,r9,4 -> channel 4
    // lbz r8,42(r31) ; cmplwi cr6,r8,8 ; bne -- the layout byte is RE-READ, not reused.
    if (REX_LOAD_U8(object + kLayout) == 8) {
      const uint64_t frames_c = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
      const uint32_t data_c = REX_LOAD_U32(desc + kBufData);      // lwz r10,4(r30)
      CopyChannel(ctx, base, data_c, frames_c, 6, held);          // 3*f then rlwinm ,3,0,28
    }
    // loc_82B2FD04
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out1, other, desc, 0);
    const uint64_t frames_d = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
    const uint32_t data_d = REX_LOAD_U32(desc + kBufData);      // lwz r4,4(r30)
    CopyChannel(ctx, base, data_d, frames_d, 1, data_d);        // rotlwi r11,r11,2 -> channel 1
    skate3::audio::GuestCall(ctx, base, sub_82B3D8E0, out0, other, desc, 0);
    const uint64_t frames_e = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
    // lwz r29,4(r30) -- this CLOBBERS the r4 argument register copy; it is dead from here on, and
    // the channel-5 copy below uses this same pointer as its source.
    const uint32_t held_e = REX_LOAD_U32(desc + kBufData);
    CopyChannel(ctx, base, held_e, frames_e, 3, held_e);  // 3*f then rlwinm ,2,0,29 -> channel 3
    // lbz r9,42(r31) ; cmplwi cr6,r9,8 ; bne -- re-read again.
    if (REX_LOAD_U8(object + kLayout) == 8) {
      const uint64_t frames_f = REX_LOAD_U16(desc + kBufFrames);  // lhz r11,14(r30)
      const uint32_t data_f = REX_LOAD_U32(desc + kBufData);      // lwz r10,4(r30)
      CopyChannel(ctx, base, data_f, frames_f, 5, held_e);        // 5*f then rlwinm ,2,0,29
    }
    // loc_82B2FD98 -- zero the layout's last channel: index 5 for six channels, 7 otherwise.
    // li r11,7 ; cmplwi cr6,r10,6 ; bne cr6,0x82b2fdac ; li r11,5
    const uint64_t last = (REX_LOAD_U8(object + kLayout) == 6) ? 5 : 7;
    const uint64_t frames_g = REX_LOAD_U16(desc + kBufFrames);  // lhz r9,14(r30)
    const uint32_t data_g = REX_LOAD_U32(desc + kBufData);      // lwz r10,4(r30)
    // clrlwi r8,r11,24 ; mullw r7,r8,r9 ; rlwinm r11,r7,2,0,29 ; add r3,r11,r10
    skate3::audio::GuestCall(ctx, base, sub_82EE5E80, ChannelOffset(frames_g, last) + data_g, 0,
                             kBlockBytes);  // bl 0x82ee5e80 -- memset(dst, 0, 1024)
  }

  // loc_82B2FDD0 -- swap the descriptors back, hand the scratch block back, and finish.
  const uint32_t now_second = REX_LOAD_U32(pair + kPairSecond);  // lwz r11,32(r25)
  const uint32_t now_first = REX_LOAD_U32(pair + kPairFirst);    // lwz r10,28(r25)
  REX_STORE_U32(pair + kPairFirst, now_second);                  // stw r11,28(r25)
  REX_STORE_U32(pair + kPairSecond, now_first);                  // stw r10,32(r25)
  // The holder is re-derived rather than reused: lwz r9,8(r31) ; lwz r8,0(r9).
  const uint32_t owner_again = REX_LOAD_U32(object + kOwner);
  const uint32_t holder_again = REX_LOAD_U32(owner_again);
  REX_STORE_U32(holder_again + kScratchField, uint32_t(scratch_top));  // stw r26,32(r8)
  // mr r3,r31 ; bl 0x82b2f590
  skate3::audio::GuestCall(ctx, base, sub_82B2F590, object);
  ctx.r3.s64 = 1;  // li r3,1
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

}  // namespace port_82B2F8C0
SKATE3_PORT(82B2F8C0, skate3::audio::kPortGate1, skate3::audio::kReturnR3)
