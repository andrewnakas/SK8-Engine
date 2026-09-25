// sub_82B305C0  role: when enabled, bump a counter in the buffer block and mix N source channels of the pair's back descriptor into N consecutive 1 KB destination runs -- a gain ramp (sub_82B44D18) for record modes 1 and 3, a flat scale (sub_82B44B20) otherwise -- keeping each source's last sample   lifted: skate3_recomp.67.cpp:45400 (269 lines)
// STATUS: thin -- verified (zero divergence) but on too few calls for its size: 0 comparable calls in the weaker profile, 100810 in the stronger, against 269 lifted lines -- never compared at all in the boot profile. Held out of default promotion by docs/promotion.md; shadows normally, never promoted.
// calls/session: boot 0  play 101160  map ?          thread: RwAudioCore Dac (census; never called in the traced boot)
// note: probe/ports/notes/sub_82B305C0.md
namespace port_82B305C0 {

// r3 = an object nothing in docs/rw_audio_structs.h names; the offsets stay plain.
constexpr uint32_t kRecord = 12;         // u32 -> a record whose byte at +72 picks the kernel
constexpr uint32_t kBuffer = 52;         // u32 -> the block holding the counter and the dest runs
constexpr uint32_t kCounterOffset = 64;  // u16: byte offset of the counter word inside kBuffer
constexpr uint32_t kEnabled = 66;        // u8: zero returns 1 with nothing written
constexpr uint32_t kLastSample = 68;     // f32[8]: +68 + 4*k, the store is at 4*(k + 17)
constexpr uint32_t kSourceFirst = 100;   // u8: first source channel, reloaded every iteration
constexpr uint32_t kDestFirst = 101;     // u8: first destination run, reloaded every iteration
constexpr uint32_t kChannels = 102;      // u8: channel count, reloaded every iteration
constexpr uint32_t kLastSampleSlots = 8; // kLastSample runs into kSourceFirst after eight singles
constexpr uint32_t kRecordMode = 72;     // u8 on *(r3+12): 1 and 3 ramp, anything else scales

// r4 = the two-entry descriptor pair; sub_82B29BE0.inc names +28 the source descriptor.
constexpr uint32_t kPairBack = 28;
// On a descriptor -- sub_82B29BE0.inc / sub_82B29AF0.inc's names, same arithmetic.
constexpr uint32_t kBufferBase = 4;      // u32  channel 0's float array
constexpr uint32_t kChannelStride = 14;  // u16  singles between one channel and the next
constexpr uint32_t kDescriptorBytes = 16;

// Rodata singles, each ((lis_imm & 0xFFFF) << 16) + offset -- no value is assumed, all are loaded.
constexpr uint32_t kGainStart = 0x8231A844;  // lis r10,-32206 ; lfs -22460  (modes 1 and other)
constexpr uint32_t kRampStep1 = 0x822F8A4C;  // lis r11,-32208 ; addi -31232 ; lfs 1100 (mode 1)
constexpr uint32_t kGainStart3 = 0x82165A10; // lis r10,-32234 ; lfs 23056   (mode 3)
constexpr uint32_t kRampStep3 = 0x822F87E0;  // lis r11,-32208 ; addi -31232 ; lfs 480 (mode 3)

constexpr uint32_t kFrameBytes = 128;        // stwu r1,-128(r1)
constexpr uint32_t kRunBytes = 1024;         // 256 singles, both kernels
constexpr uint32_t kLastInRun = 1020;        // lfs f0,1020(r27)
constexpr uint64_t kScaleCount = 256;        // li r6,256 -- sub_82B44B20's count

double LoadSingle(uint8_t* base, uint32_t addr) {  // lfs
  PPCRegister temp{};
  temp.u32 = REX_LOAD_U32(addr);
  return double(temp.f32);
}

void StoreSingle(uint8_t* base, uint32_t addr, double value) {  // stfs / stfsx
  PPCRegister temp{};
  temp.f32 = float(value);
  REX_STORE_U32(addr, temp.u32);
}

// mullw r7,r8,r9 ; rlwinm r11,r7,2,0,29 ; add r27,r11,r10 -- mirrored from sub_82B29BE0.inc's
// ChannelAddress: 64-bit product of two sign-extended words, 32-bit shift, 64-bit add.
inline uint64_t ChannelAddress(uint64_t stride, uint32_t index, uint64_t buffer_base) {
  const int64_t elements = int64_t(int32_t(uint32_t(stride))) * int64_t(int32_t(index));
  const uint64_t byte_offset = static_cast<uint32_t>(static_cast<uint32_t>(elements) << 2);
  return byte_offset + buffer_base;
}

inline bool Disjoint(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
  return a + alen <= b || b + blen <= a;
}

// The shared epilogue: li r3,1 ; addi r1,r1,128 ; b __restgprlr_27. The body never writes
// r27-r31 (locals throughout), so __restgprlr_27 has nothing to put back.
inline void Leave(PPCContext& ctx) {
  ctx.r3.s64 = 1;
  ctx.r1.s64 = ctx.r1.s64 + kFrameBytes;
}

REX_FUNC(Native) {
  const uint32_t self = ctx.r3.u32;  // mr r31,r3
  const uint32_t pair = ctx.r4.u32;

  // stwu r1,-128(r1). Reproduced: sub_82B44D18 spills to -112..-1 of its r1 and sub_82B44B20 to
  // -48(r1), so both callees' red zones sit where the original put them.
  const uint32_t entry_sp = ctx.r1.u32;
  REX_STORE_U32(entry_sp - kFrameBytes, entry_sp);
  ctx.r1.u32 = entry_sp - kFrameBytes;

  // lbz r11,66(r3) ; cmplwi cr6,r11,0 ; beq cr6,0x82b30784
  if (REX_LOAD_U8(self + kEnabled) == 0) {
    Leave(ctx);
    return;
  }

  const uint64_t buffer = REX_LOAD_U32(self + kBuffer);                  // lwz r11,52(r3)
  const uint32_t counter_offset = REX_LOAD_U16(self + kCounterOffset);   // lhz r10,64(r3)
  // lbz r9,101(r3) ; rotlwi r9,r9,10 -- a byte, so the 32-bit rotate is a plain * 1024.
  const uint64_t first_run = uint64_t(REX_LOAD_U8(self + kDestFirst)) << 10;
  // lwzx r8,r10,r11 ; addi r8,r8,1 ; stwx r8,r10,r11 -- the effective address is a 32-bit sum.
  const uint32_t counter = counter_offset + static_cast<uint32_t>(buffer);
  const uint64_t count_value = REX_LOAD_U32(counter);
  REX_STORE_U32(counter, static_cast<uint32_t>(count_value + 1));        // stwx r8,r10,r11
  uint64_t dst = first_run + buffer;                                     // add r28,r9,r11 (64-bit)

  // All three loaded AFTER the counter store, in this order.
  const uint32_t record = REX_LOAD_U32(self + kRecord);                  // lwz r7,12(r3)
  const uint32_t desc = REX_LOAD_U32(pair + kPairBack);                  // lwz r30,28(r4)
  const uint32_t mode = REX_LOAD_U8(record + kRecordMode);               // lbz r11,72(r7)

  // cmplwi cr6,r11,1 ; bne -> loc_82B30694 ; cmplwi cr6,r11,3 ; bne -> loc_82B3071C
  const bool ramp = (mode == 1) || (mode == 3);
  // lbz r11,102(r3/r31) ; cmplwi cr6,r11,0 ; beq cr6,0x82b30784 -- on all three paths.
  if (REX_LOAD_U8(self + kChannels) == 0) {
    Leave(ctx);
    return;
  }
  ctx.fpscr.disableFlushModeUnconditional();
  if (mode == 1) {
    ctx.f1.f64 = LoadSingle(base, kGainStart);   // lfs f1,-22460(r10)
    ctx.f2.f64 = LoadSingle(base, kRampStep1);   // lfs f2,1100(r9)
  } else if (mode == 3) {
    ctx.f1.f64 = LoadSingle(base, kGainStart3);  // lfs f1,23056(r10)
    ctx.f2.f64 = LoadSingle(base, kRampStep3);   // lfs f2,480(r9)
  } else {
    ctx.f1.f64 = LoadSingle(base, kGainStart);   // lfs f1,-22460(r11)
  }

  // loc_82B30634 / loc_82B306BC / loc_82B3072C -- the same loop around a different kernel.
  // f1 and f2 are loaded ONCE, before it, and the lifted body never reloads them: every later
  // iteration passes whatever the previous kernel left in those volatile registers. They are
  // therefore read off the live context here, never from a local. (Neither kernel writes them in
  // its lifted form, so today that is the same value each time.)
  uint64_t channel = 0;  // r29
  for (;;) {
    const uint64_t source_index = uint64_t(REX_LOAD_U8(self + kSourceFirst)) + channel;  // lbz r11,100(r31) ; add r8
    const uint64_t stride = REX_LOAD_U16(desc + kChannelStride);        // lhz r9,14(r30)
    const uint64_t source_base = REX_LOAD_U32(desc + kBufferBase);      // lwz r10,4(r30)
    const uint64_t src = ChannelAddress(stride, static_cast<uint32_t>(source_index), source_base);
    if (ramp) {
      // mr r3,r28 ; mr r4,r27 ; bl 0x82b44d18 -- f1/f2 as they stand
      skate3::audio::GuestCall(ctx, base, sub_82B44D18, dst, src);
    } else {
      // li r6,256 ; mr r3,r28 ; mr r4,r27 ; bl 0x82b44b20 -- the kernel never reads r5, so it is
      // left as it is rather than passed positionally.
      ctx.r6.s64 = static_cast<int64_t>(kScaleCount);
      skate3::audio::GuestCall(ctx, base, sub_82B44B20, dst, src);
    }
    const uint64_t dest_index = uint64_t(REX_LOAD_U8(self + kDestFirst)) + channel;  // lbz r11,101(r31) ; add r11,r11,r29
    ctx.fpscr.disableFlushModeUnconditional();
    const double last = LoadSingle(base, static_cast<uint32_t>(src) + kLastInRun);   // lfs f0,1020(r27)
    ctx.f0.f64 = last;  // left in f0 as lifted; neither kernel reads f0 before writing it
    dst = static_cast<uint64_t>(static_cast<int64_t>(dst) + kRunBytes);              // addi r28,r28,1024
    channel = static_cast<uint64_t>(static_cast<int64_t>(channel) + 1);              // addi r29,r29,1
    // addi r6,r11,17 ; rlwinm r5,r6,2,0,29 ; stfsx f0,r5,r31
    const uint32_t slot_offset = static_cast<uint32_t>(static_cast<uint32_t>(dest_index + 17) << 2);
    StoreSingle(base, slot_offset + self, last);
    // lbz r4,102(r31) ; cmplw cr6,r29,r4 ; blt -- the count is reloaded after the store.
    if (!(static_cast<uint32_t>(channel) < REX_LOAD_U8(self + kChannels))) break;
  }
  Leave(ctx);  // li r3,1
}

// The write set, from entry state. Every store address derives from words read at entry or
// before the loop; the guards make sure no declared store lands on a word that is re-read
// after it (the counter store precedes the reads of +12, pair+28 and record+72; every loop
// store precedes the reloads of +100..+102 and of the descriptor's base and stride).
bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t pair = ctx.r4.u32;
  if (self == 0) return false;
  spec.read(self + kEnabled, 1);
  if (REX_LOAD_U8(self + kEnabled) == 0) return true;  // li r3,1 and nothing written

  const uint64_t buffer = REX_LOAD_U32(self + kBuffer);
  const uint32_t counter = REX_LOAD_U16(self + kCounterOffset) + static_cast<uint32_t>(buffer);
  if (pair == 0) return false;
  const uint32_t record = REX_LOAD_U32(self + kRecord);
  if (!Disjoint(counter, 4, self + kRecord, 4) || !Disjoint(counter, 4, pair + kPairBack, 4) ||
      !Disjoint(counter, 4, record + kRecordMode, 1) ||
      !Disjoint(counter, 4, self + kSourceFirst, 3)) {
    return false;  // the counter store would change a word read after it
  }
  const uint32_t desc = REX_LOAD_U32(pair + kPairBack);
  const uint32_t mode = REX_LOAD_U8(record + kRecordMode);
  const uint32_t channels = REX_LOAD_U8(self + kChannels);
  spec.write(counter, 4);  // stwx r8,r10,r11
  spec.read(self + kRecord, 4);
  spec.read(self + kBuffer, 4);
  spec.read(self + kCounterOffset, 2);
  spec.read(pair + kPairBack, 4);
  spec.read(record + kRecordMode, 1);
  spec.read(self + kSourceFirst, 3);
  if (channels == 0) return true;
  if (desc == 0) return false;  // the loop reads the descriptor off null
  if (!Disjoint(counter, 4, desc + kBufferBase, kDescriptorBytes - kBufferBase)) return false;

  // The last-sample stores go to +68 + 4*(first + k); past eight they reach +100..+102, the
  // three control bytes the loop reloads, and the trip count stops deriving from entry state.
  const uint32_t dest_first = REX_LOAD_U8(self + kDestFirst);
  if (dest_first + channels > kLastSampleSlots) return false;
  const uint32_t last_begin = self + kLastSample + 4 * dest_first;
  const uint32_t last_len = 4 * channels;

  // The destination runs: r28 = buffer + 1024 * first, then + 1024 per channel. sub_82B44D18
  // writes [dst & ~0xF, +1024) (its Windows()), sub_82B44B20 with a count of 256 writes exactly
  // [dst, +1024) on either of its paths (its Windows()); consecutive runs abut, so one span.
  const uint64_t first_dst = buffer + (uint64_t(dest_first) << 10);
  const bool ramp = (mode == 1) || (mode == 3);
  const uint64_t run_begin = ramp ? (first_dst & ~uint64_t(0xF)) : first_dst;
  const uint64_t run_len = uint64_t(kRunBytes) * channels;
  if ((first_dst >> 32) != 0 || run_begin + run_len > 0x100000000ull) return false;

  const uint32_t control_addr[3] = {self + kSourceFirst, desc + kBufferBase, desc + kChannelStride};
  const uint32_t control_len[3] = {3, 4, 2};
  for (uint32_t k = 0; k < 3; k++) {
    if (!Disjoint(run_begin, run_len, control_addr[k], control_len[k])) return false;
    if (!Disjoint(last_begin, last_len, control_addr[k], control_len[k])) return false;
  }

  spec.write(last_begin, last_len);                                   // stfsx f0,r5,r31
  spec.write(static_cast<uint32_t>(run_begin), static_cast<uint32_t>(run_len));  // the kernels

  // Reads, for replay only: the descriptor, the source runs, and the rodata singles.
  spec.read(desc, kDescriptorBytes);
  const uint64_t stride = REX_LOAD_U16(desc + kChannelStride);
  const uint64_t source_base = REX_LOAD_U32(desc + kBufferBase);
  const uint32_t source_first = REX_LOAD_U8(self + kSourceFirst);
  for (uint32_t k = 0; k < channels && k < kLastSampleSlots; k++) {
    const uint32_t src = static_cast<uint32_t>(ChannelAddress(stride, source_first + k, source_base));
    const uint64_t end = uint64_t(src) + kRunBytes;
    spec.read(src, static_cast<uint32_t>((end > 0x100000000ull ? 0x100000000ull : end) - src));
  }
  spec.read(kGainStart, 4);
  spec.read(kRampStep1, 4);
  spec.read(kGainStart3, 4);
  spec.read(kRampStep3, 4);
  // sub_82B44D18's constant pool, as its own Windows() declares it, so a recorded vector carries
  // everything the ramp kernel reads. For replay only; reads cost nothing against the budget.
  spec.read(0x82257308u, 4);        // its kStepScale
  spec.read(0x820ED958u, 4);        // kRampSpan
  spec.read(0x82060C50u, 4);        // kLane2Scale
  spec.read(0x82063B08u, 4);        // kLane3Scale
  spec.read(0x8231BA20u, 16 * 8);   // kScale5..kScaleStep, then kScale1..kScale4
  return true;
}

}  // namespace port_82B305C0
SKATE3_PORT(82B305C0, skate3::audio::kPortVerifiedThin, skate3::audio::kReturnR3)
