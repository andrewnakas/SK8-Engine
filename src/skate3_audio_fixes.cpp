/**
 * @file        skate3_audio_fixes.cpp
 * @brief       Guest hooks for the rw::audio::core command-queue race
 *
 * The AYN Thor (Snapdragon 8 Gen 2) filled its log with
 *
 *   [cpu] Call to invalid or unregistered function at guest address 0xFFFDFFFF
 *         (guest lr=0x82B3CD64) | r3=00000006 r10=00000000 r11=FFFDFFFF
 *
 * at a steady 187.5 calls a second - which is 48000/256, one per Xbox audio
 * quantum - for twenty-three minutes, and then died writing to guest
 * 0x70000000 on the render thread.
 *
 * Reading the dump register-by-register: `lr` is inside sub_82B3CD38, which
 * builds an audio decoder from a format descriptor:
 *
 *   lwz r11,0(r3)   ; r3 = the FormatDesc
 *   mtctr r11
 *   bctrl           ; desc->GetSize(channels, &size)
 *
 * r3 at the fault is 6 only because the recompiled code has already loaded the
 * NEXT argument into it; the descriptor pointer is r10, and r10 is 0 in every
 * occurrence in every report. The guest reads *(0+0), which is the low guest
 * page holding 0xFFFDFFFF, and calls that.
 *
 * The descriptor is NULL because the lookup that produced it missed. Its
 * callers key the System's format registry by a four-character codec tag taken
 * from a table indexed by a byte on the player object:
 *
 *   sub_82B29018  key = table_8210A310[*(this+352)]   (only 0 and 1 are valid:
 *                                                      'P6L0', 'PFN0')
 *   sub_82B33D40  key = table_82119870[codec id]
 *
 * Every format the game ships, including those two, is registered at startup,
 * so a miss means the INDEX BYTE IS GARBAGE, not that a format is missing.
 *
 * That byte is written by sub_82B28B78 (the SetFormat command handler) out of
 * a 20-byte command record. Records are appended to the System's command
 * queue - buffer *(sys+48), write offset *(sys+204) - and run later on the
 * audio thread by sub_82B48530, which holds the System lock while it does it.
 *
 * sub_82B28A00, the append, takes NO lock. Worse, it publishes the new write
 * offset before it stores the record:
 *
 *   lwz  r9,204(r11)     ; offset
 *   stw  r7,204(r11)     ; PUBLISH offset + 20   <-- first
 *   stw  r6,0(r10)       ; then the handler
 *   stw  r3,4(r10)       ; then the object
 *   stfs f0,8(r10)       ; then the payload
 *
 * A consumer that observes the advanced offset before those stores land runs a
 * record built from whatever the buffer held - typically a stale heap pointer
 * like 0x40C21870, which read as a float and truncated to an integer gives the
 * small values (6, 5, 1) the reports show. The format byte then lands outside
 * {0,1}, the lookup misses forever, and the player retries every audio frame.
 *
 * On the in-order Xenon the window between those stores is a few cycles and
 * the two threads were on the same die. On an out-of-order ARM64 core at
 * 3.2 GHz it is wide enough to hit repeatedly. The recompiler honours the
 * PowerPC barriers (lwsync becomes a seq_cst fence) - there simply are none on
 * this path to honour.
 *
 * sub_82B28A00 is not the only unlocked appender - sub_82B1E458,
 * sub_82B1E4F8, sub_82B1E780 and sub_82B1E8D8 all append the same way with no
 * lock, and any of them racing the drain corrupts the same queue. Only
 * sub_82B1DCD0 among the producers takes the lock. All five are wrapped below.
 * (sub_82B1EBA0 also appends, but it is the one-shot setup path behind an
 * "already initialised" flag, and it reaches its System through neither of the
 * two member paths, so it is left alone.)
 *
 * Nesting is safe: the guest critical section is recursive - see
 * RtlEnterCriticalSection_entry, which increments recursion_count when the
 * owning thread re-enters - and these hooks take the same lock the drain
 * already takes, so no new lock ordering is introduced.
 *
 * The hooks below:
 *   - the five appenders run under the System lock, exactly the lock the drain
 *     takes, which serialises them against it and against each other. The
 *     acquire NEVER blocks: these run on the game's own threads, and parking
 *     one behind an audio worker that Android stopped mid-callback would trade
 *     a glitch for a freeze. If the lock cannot be had, the append proceeds
 *     exactly as it does today.
 *   - sub_82B3CD38 refuses a NULL descriptor instead of calling through it.
 *     Its callers all handle a 0 return (sub_82B29018 stores 255 at +344 and
 *     reports failure), so the guest takes its own error path. If this one
 *     ever fires the log names the format byte, which says whether the race is
 *     really closed.
 */

#include <atomic>
#include <cstdint>
#include <thread>

#include <rex/logging.h>

#include "generated/skate3_init.h"

namespace {

// System object layout, from sub_82B48530 (the queue drain) and sub_82B47E68
// (the constructor).
constexpr uint32_t kSysLockFn = 84;    // void (*)() - null on every build seen
constexpr uint32_t kSysUnlockFn = 88;  // unused: we only ever take the critical section
constexpr uint32_t kSysCritSec = 96;   // RTL_CRITICAL_SECTION*, the real lock

// PacketPlayer, from sub_82B28B78 (the SetFormat handler).
constexpr uint32_t kPlayerSystem = 8;      // System*
constexpr uint32_t kPlayerRate = 340;      // float
constexpr uint32_t kPlayerChannels = 351;  // u8
constexpr uint32_t kPlayerFormat = 352;    // u8, indexes the tag table below
constexpr uint32_t kFormatTagTable = 0x8210A310;
constexpr uint32_t kFormatTagCount = 2;  // 'P6L0', 'PFN0'

// The return address inside sub_82B29018, i.e. "this call came from
// PacketPlayer::CreateDecoder", which is the only caller whose object fields
// are worth printing.
constexpr uint32_t kCreateDecoderReturn = 0x82B290AC;

// How many times to try for the lock before giving up and appending without
// it. The append is a dozen stores and the drain holds the lock only for the
// commands already queued, so a handful of attempts is far more than a live
// holder ever needs.
//
// This was 256. The uncontended case takes the lock on the first attempt and
// never yields, so that number only ever showed up under real contention -
// but there it is 256 yields, and each yield is a syscall, on one of the
// game's own threads. Falling through unlocked is safe by design (see above),
// so spinning long enough to outlast a stalled holder buys nothing: it just
// makes the bad case more expensive than the fallback it is avoiding.
constexpr int kLockAttempts = 16;

// Take the System lock WITHOUT ever blocking, and say whether we got it.
//
// Blocking here would be a new way to freeze the whole game. These appends run
// on the game's own threads, which never touched this lock before; if the
// audio worker is stopped while holding it - which is exactly what happens
// when Android suspends the app mid-callback - a blocking acquire would park
// the game thread behind a holder that is not running. Falling through
// unlocked is precisely today's behaviour, so the worst case is the bug we
// are fixing, not a hang.
//
// The custom lock at +84 is null on this title. If some build ever sets it we
// cannot try-acquire a guest function, so leave that path alone rather than
// risk blocking on it.
bool SystemTryLock(PPCContext& __restrict ctx, uint8_t* base, uint32_t sys) {
  if (REX_LOAD_U32(sys + kSysLockFn) != 0) {
    return false;
  }
  const uint32_t cs = REX_LOAD_U32(sys + kSysCritSec);
  if (cs == 0) {
    return false;
  }
  for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
    ctx.r3.u64 = cs;
    __imp__RtlTryEnterCriticalSection(ctx, base);
    if (ctx.r3.u32 != 0) {
      return true;  // recursion is handled inside; one release balances this
    }
    std::this_thread::yield();
  }
  return false;
}

void SystemUnlock(PPCContext& __restrict ctx, uint8_t* base, uint32_t sys) {
  ctx.r3.u64 = REX_LOAD_U32(sys + kSysCritSec);
  __imp__RtlLeaveCriticalSection(ctx, base);
}

// Geometric, like the invalid-call trap in the dispatcher: if this fires at
// all it fires every audio frame, and a synchronous log write per frame is
// worse than the fault it reports.
bool ShouldLog(std::atomic<uint64_t>& counter, uint64_t* out_n) {
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  *out_n = n + 1;
  return n < 8 || (n & (n - 1)) == 0;
}

std::atomic<uint64_t> g_null_desc{0};

// Volatile guest state the lock helpers may disturb. RtlEnterCriticalSection
// is a host implementation that only writes r3, but the +84/+88 function
// pointers - null in this build, honoured anyway - would run guest code, and
// guest code clobbers the whole volatile set.
struct VolatileState {
  uint64_t r[8];  // r3..r10
  uint64_t f[13];  // f1..f13
  uint64_t lr;
  uint64_t ctr;

  explicit VolatileState(const PPCContext& ctx) {
    r[0] = ctx.r3.u64; r[1] = ctx.r4.u64; r[2] = ctx.r5.u64; r[3] = ctx.r6.u64;
    r[4] = ctx.r7.u64; r[5] = ctx.r8.u64; r[6] = ctx.r9.u64; r[7] = ctx.r10.u64;
    f[0] = ctx.f1.u64; f[1] = ctx.f2.u64; f[2] = ctx.f3.u64; f[3] = ctx.f4.u64;
    f[4] = ctx.f5.u64; f[5] = ctx.f6.u64; f[6] = ctx.f7.u64; f[7] = ctx.f8.u64;
    f[8] = ctx.f9.u64; f[9] = ctx.f10.u64; f[10] = ctx.f11.u64; f[11] = ctx.f12.u64;
    f[12] = ctx.f13.u64;
    lr = ctx.lr;
    ctr = ctx.ctr.u64;
  }

  void Restore(PPCContext& ctx) const {
    ctx.r3.u64 = r[0]; ctx.r4.u64 = r[1]; ctx.r5.u64 = r[2]; ctx.r6.u64 = r[3];
    ctx.r7.u64 = r[4]; ctx.r8.u64 = r[5]; ctx.r9.u64 = r[6]; ctx.r10.u64 = r[7];
    ctx.f1.u64 = f[0]; ctx.f2.u64 = f[1]; ctx.f3.u64 = f[2]; ctx.f4.u64 = f[3];
    ctx.f5.u64 = f[4]; ctx.f6.u64 = f[5]; ctx.f7.u64 = f[6]; ctx.f8.u64 = f[7];
    ctx.f9.u64 = f[8]; ctx.f10.u64 = f[9]; ctx.f11.u64 = f[10]; ctx.f12.u64 = f[11];
    ctx.f13.u64 = f[12];
    ctx.lr = lr;
    ctx.ctr.u64 = ctr;
  }
};

// System* at *(this + 8).
inline uint32_t SysFromMember8(uint8_t* base, uint32_t self) {
  return self != 0 ? REX_LOAD_U32(self + kPlayerSystem) : 0;
}

// System* at *(*(this + 4) + 16), which is how sub_82B1E458 reaches it.
inline uint32_t SysFromOwner16(uint8_t* base, uint32_t self) {
  if (self == 0) return 0;
  const uint32_t owner = REX_LOAD_U32(self + 4);
  return owner != 0 ? REX_LOAD_U32(owner + 16) : 0;
}

}  // namespace

// Take the System lock, run the original with its arguments intact, release,
// and hand back the original's return value.
//
// Each appender reaches its System differently - most from *(this+8),
// sub_82B1E458 from *(*(this+4)+16) - so the caller resolves it. A zero
// anywhere along the way means there is no queue to protect, and the hook
// steps aside rather than guessing.
//
// Written out one by one rather than generated from a macro because
// tools/gen_hooked_funcs.sh finds hooks by grepping for the literal
// REX_FUNC(sub_...) and __imp__sub_... spellings; a macro would hide them and
// the override would silently not happen.
namespace {

void RunUnderSystemLock(PPCContext& __restrict ctx, uint8_t* base, uint32_t sys,
                        PPCFunc* original) {
  if (sys == 0) {
    original(ctx, base);
    return;
  }
  const VolatileState saved(ctx);
  const bool locked = SystemTryLock(ctx, base, sys);
  saved.Restore(ctx);

  original(ctx, base);

  if (locked) {
    const VolatileState returned(ctx);
    SystemUnlock(ctx, base, sys);
    returned.Restore(ctx);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_82B28A00) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B28A00);
}

extern "C" REX_FUNC(sub_82B1E4F8) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B1E4F8);
}

extern "C" REX_FUNC(sub_82B1E780) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B1E780);
}

extern "C" REX_FUNC(sub_82B1E8D8) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B1E8D8);
}

extern "C" REX_FUNC(sub_82B1E458) {
  RunUnderSystemLock(ctx, base, SysFromOwner16(base, ctx.r3.u32), __imp__sub_82B1E458);
}

// rw::audio::core decoder factory: r3 = FormatDesc, r4 = channels, r5 = rate.
// A NULL descriptor here means a format lookup missed; calling through it
// reads guest address 0 and dispatches to whatever that page holds.
extern "C" REX_FUNC(sub_82B3CD38) {
  if (ctx.r3.u32 != 0) {
    __imp__sub_82B3CD38(ctx, base);
    return;
  }

  uint64_t n = 0;
  if (ShouldLog(g_null_desc, &n)) {
    const uint32_t lr = uint32_t(ctx.lr);
    if (lr == kCreateDecoderReturn && ctx.r31.u32 != 0) {
      // PacketPlayer::CreateDecoder: r31 is the player, so the index byte that
      // produced the miss can be printed. Anything but 0 or 1 is the race.
      const uint32_t player = ctx.r31.u32;
      const uint32_t format = REX_LOAD_U8(player + kPlayerFormat);
      const uint32_t channels = REX_LOAD_U8(player + kPlayerChannels);
      const uint32_t rate_bits = REX_LOAD_U32(player + kPlayerRate);
      float rate = 0.0f;
      __builtin_memcpy(&rate, &rate_bits, sizeof(rate));
      uint32_t tag = 0;
      if (format < kFormatTagCount) {
        tag = REX_LOAD_U32(kFormatTagTable + format * 4);
      }
      REXLOG_ERROR(
          "skate3-audio: decoder requested with a NULL format descriptor "
          "(occurrence {}), from PacketPlayer::CreateDecoder player={:08X}: "
          "format byte={} (valid: 0-{}), tag={:08X}, channels={}, rate={} - "
          "returning failure instead of calling through guest address 0",
          n, player, format, kFormatTagCount - 1, tag, channels, rate);
    } else {
      REXLOG_ERROR(
          "skate3-audio: decoder requested with a NULL format descriptor "
          "(occurrence {}), guest lr={:08X}, channels={}, rate={:08X} - "
          "returning failure instead of calling through guest address 0",
          n, lr, ctx.r4.u32, ctx.r5.u32);
    }
  }

  ctx.r3.u64 = 0;
}
