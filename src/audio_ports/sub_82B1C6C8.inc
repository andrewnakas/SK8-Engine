// sub_82B1C6C8  role: step a cursor -- accumulate signed per-step weights until the sum passes the clock's remainder mod 100, then publish base+index   lifted: skate3_recomp.66.cpp:64600 (96 lines)
// STATUS: verified
// calls/session: boot 1994  play 2382  map ?          thread: RwAudioCore Dac
// note: probe/ports/notes/sub_82B1C6C8.md
namespace port_82B1C6C8 {

// r3 = the cursor record. Nothing in docs/rw_audio_structs.h names it.
constexpr uint32_t kSource = 0;      // lwz r10,0(r30)  -- the record holding the weight table
constexpr uint32_t kBaseIndex = 4;   // lwz r10,4(r30)  -- added to the chosen index
constexpr uint32_t kStepCount = 8;   // lwz r9,8(r30)   -- int32 table length; the loop bound
constexpr uint32_t kResult = 12;     // stw r11,12(r30) -- the only store, and the return value
constexpr uint32_t kEnable = 16;     // lwz r11,16(r3)  -- int32; zero short-circuits everything
constexpr uint32_t kRecordBytes = 20;

constexpr uint32_t kTableOffset = 16;  // addi r8,r10,16 -- the weights start 16 bytes into source

// A cap on the declared READ span for the weight table only. The loop itself is bounded by the
// int32 count the original reloads; this keeps the window builder's read declaration finite.
constexpr uint32_t kMaxSteps = 4096;

// sub_82B1F360's six-word global counter, which this call advances and then reads back through r3.
// Its own port derives the address the same way: lis -31993 ; addi r11,r11,30192.
constexpr uint32_t kClock = 0x830775F0;
constexpr uint32_t kClockBytes = 24;

// lis r11,20971 ; ori r8,r11,34079 -- ((20971 & 0xFFFF) << 16) + 0x851F, computed, not read by
// eye. The usual unsigned divide-by-100 reciprocal.
constexpr uint32_t kReciprocal = 0x51EB851Fu;

// rlwinm rX,rY,SH,MB,ME, written out because the lifted form rotates the low word into both halves
// of a 64-bit value before masking and the two are only equal for small operands.
inline uint64_t Rlwinm(uint64_t value, uint32_t sh, uint64_t mask) {
  const uint64_t lo = value & 0xFFFFFFFFu;
  return __builtin_rotateleft64(lo | (lo << 32), sh) & mask;
}

inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
}

REX_FUNC(Native) {
  const uint32_t record = ctx.r3.u32;  // mr r30,r3
  // The original spills r30 and r31 into its red zone and restores them on the way out, so its
  // exit values ARE its entry values; the harness compares r13-r31.
  const PPCRegister saved_r30 = ctx.r30;
  const PPCRegister saved_r31 = ctx.r31;
  // The stwu is NOT reproduced: sub_82B1F360 is a pure leaf that touches no stack at all, so
  // nothing of its own lands below this function's r1.

  // cmpwi cr6,r11,0 ; beq 0x82b1c758 -- a disabled record is read back untouched, with no call.
  if (int32_t(REX_LOAD_U32(record + kEnable)) != 0) {
    // bl 0x82b1f360 -- advance the global software clock and take its running total in r3. Its
    // 24-byte global is declared as a write window, so the harness rewinds the counter between the
    // two runs; without that the native run would read an already-advanced value.
    skate3::audio::GuestCall(ctx, base, sub_82B1F360);
    const uint64_t now = ctx.r3.u64;

    const int32_t steps = int32_t(REX_LOAD_U32(record + kStepCount));  // lwz r9,8(r30)
    const uint32_t source = REX_LOAD_U32(record + kSource);            // lwz r10,0(r30)
    // mulhwu r7,r3,r8 ; rlwinm r6,r7,27,5,31 ; mulli r5,r6,100 ; subf r9,r5,r3 -- now/100 by the
    // reciprocal, then the remainder. The compare below reads only the low word, but the subf is
    // 64-bit on the FULL counter value, so the intermediate stays 64-bit.
    const uint64_t high = (uint64_t(uint32_t(now)) * uint64_t(kReciprocal)) >> 32;
    const uint64_t quotient = Rlwinm(high, 27, 0x7FFFFFFull);
    const uint64_t scaled = uint64_t(int64_t(quotient * 100ull));  // mulli r5,r6,100
    const uint64_t remainder = now - scaled;                       // subf r9,r5,r3

    // ble cr6,0x82b1c758 -- the count tested here is the one read BEFORE the division, and it is
    // tested signed, so a negative count skips the loop too.
    if (steps > 0) {
      const uint32_t table = source + kTableOffset;  // addi r8,r10,16
      int64_t index = 0;                             // li r11,0
      int64_t total = 0;                             // li r31,0
      for (;;) {                                     // loc_82B1C724
        // lbzx r10,r8,r11 ; extsb r10,r10 ; add r31,r10,r31 -- a SIGNED byte, summed in 64 bits.
        total += int8_t(REX_LOAD_U8(table + uint32_t(index)));
        // cmplw cr6,r31,r9 ; bgt 0x82b1c74c -- UNSIGNED, and only on the low words.
        if (uint32_t(total) > uint32_t(remainder)) {
          // loc_82B1C74C: add r11,r10,r11 is 64-bit; only the store truncates.
          const uint32_t base_index = REX_LOAD_U32(record + kBaseIndex);  // lwz r10,4(r30)
          REX_STORE_U32(record + kResult,
                        uint32_t(uint64_t(base_index) + uint64_t(index)));  // stw r11,12(r30)
          break;
        }
        // lwz r10,8(r30) -- the bound is reloaded on every iteration.
        const int32_t limit = int32_t(REX_LOAD_U32(record + kStepCount));
        index += 1;                             // addi r11,r11,1
        if (int32_t(index) >= limit) break;     // cmpw cr6,r11,r10 ; blt
      }
    }
  }

  // loc_82B1C758 -- the result word is reloaded from memory on every path, including the paths
  // that never wrote it.
  ctx.r3.u64 = REX_LOAD_U32(record + kResult);  // lwz r3,12(r30)
  ctx.r30 = saved_r30;
  ctx.r31 = saved_r31;
}

bool Windows(PPCContext& ctx, uint8_t* base, skate3::audio::PortSpec& spec) {
  const uint32_t record = ctx.r3.u32;
  if (record == 0) return false;
  spec.read(record, kRecordBytes);  // +0, +4, +8, +12, +16

  // The enable word is read before anything happens, so this path is predicted, not unioned. It
  // makes no call and no store -- only the reload of the result word into r3.
  if (int32_t(REX_LOAD_U32(record + kEnable)) == 0) return true;

  // Every other field is reloaded AFTER sub_82B1F360 ran. If the record overlapped its counter,
  // those reloads would not be entry state.
  if (Overlaps(record, kRecordBytes, kClock, kClockBytes)) return false;

  spec.write(kClock, kClockBytes);  // sub_82B1F360's whole global
  spec.read(kClock, kClockBytes);
  // stw r11,12(r30) is conditional on the weight sum overrunning the remainder. Declared
  // unconditionally: a superset, and a word the call leaves alone compares equal anyway.
  spec.write(record + kResult, 4);

  const int32_t steps = int32_t(REX_LOAD_U32(record + kStepCount));
  if (steps > 0) {
    const uint32_t source = REX_LOAD_U32(record + kSource);
    const uint64_t table = uint64_t(source) + kTableOffset;
    // The weights are read, never written, so a truncated span costs the recorded vector some
    // context and nothing else. Capped so the read declaration cannot run away on a corrupt count.
    const uint32_t len = uint32_t(steps) < kMaxSteps ? uint32_t(steps) : kMaxSteps;
    if (table + len <= 0x100000000ull) spec.read(uint32_t(table), len);
  }
  return true;
}

}  // namespace port_82B1C6C8
SKATE3_PORT(82B1C6C8, skate3::audio::kPortVerified, skate3::audio::kReturnR3)
