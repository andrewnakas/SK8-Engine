/**
 * @file        skate3_audio_port.h
 * @brief       One macro per ported audio function: census, shadow compare, promotion
 *
 * A port file (recomp/src/audio_ports/sub_XXXXXXXX.inc) supplies two things inside
 * `namespace port_XXXXXXXX`:
 *
 *   REX_FUNC(Native)                                   the readable rewrite
 *   bool Windows(PPCContext&, uint8_t* base, PortSpec&) every byte the call writes, from entry
 *                                                        state; false when it cannot say
 *
 * and one line, `SKATE3_PORT(XXXXXXXX, kPortVerified, kReturnR3)`, which generates the hook:
 *
 *   count      every call, under skate3_audio_port_census, with the first four calls' entry
 *              registers and thread name -- so an armed function that is never compared still
 *              leaves evidence of being called (or not)
 *   shadow     under skate3_audio_shadow, unless the STATUS is a gate label: run the original,
 *              rewind, run Native, diff (skate3_audio_shadow.h)
 *   promote    under skate3_audio_native, only when STATUS is kPortVerified
 *   otherwise  the original
 *
 * The window builder is the only per-function intelligence; everything else is here once.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/skate3_init.h"
#include "skate3_audio_shadow.h"

namespace skate3::audio {

enum PortStatus : uint8_t {
  kPortPending = 0,   // written, not yet compared clean
  kPortVerified,      // zero divergence over a session; promotable
  kPortDivergent,     // compared and disagreed; kept for the record, never promoted
  kPortUncalled,      // passes the gates, the game never reached it
  kPortGate1,         // callees not replayable
  kPortGate2,         // write set not enumerable from entry state
  kPortGate3,         // output not deterministic
  kPortGate4,         // result not observable by the harness
};

/// Half the harness budget: a port declaring more than this runs the original and counts.
constexpr uint32_t kPortWatchCap = 32 * 1024;
constexpr size_t kPortMaxSpans = 32;

/// Filled per call by a port's Windows(): the write set, the read set, and which result
/// registers to compare.
struct PortSpec {
  ShadowWindow windows[kPortMaxSpans];
  size_t nwin = 0;
  ShadowWindow inputs[kPortMaxSpans];
  size_t nin = 0;
  ShadowResults returns{};
  bool overflowed = false;  // more spans than fit: the hook must skip, not truncate

  void write(uint32_t addr, uint32_t len) {
    if (len == 0) return;
    if (nwin < kPortMaxSpans) windows[nwin++] = {addr, len};
    else overflowed = true;
  }
  void read(uint32_t addr, uint32_t len) {
    if (len == 0) return;
    if (nin < kPortMaxSpans) inputs[nin++] = {addr, len};
  }
  uint32_t total() const {
    uint32_t t = 0;
    for (size_t i = 0; i < nwin; i++) t += windows[i].len;
    return t;
  }
};

/// Per-hook counters, registered with the census reporter on construction.
struct PortCounters {
  PortCounters(const char* function_name, PortStatus s);
  const char* name;
  PortStatus status;
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> dumped{0};
  std::atomic<uint64_t> skipped{0};
  std::atomic<uint64_t> oversize{0};
  std::atomic<uint64_t> native_runs{0};
  std::atomic<uint64_t> logged{0};
  std::atomic<const char*> thread{nullptr};
};

/// Counting-only slots for functions that have no port yet (skate3_audio_census_all.cpp).
struct CensusSlot {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> dumped{0};
  std::atomic<uint64_t> logged{0};
  std::atomic<const char*> thread{nullptr};
};
struct CensusTable {
  const char* const* names;
  CensusSlot* slots;
  size_t count;
};
void RegisterCensusTable(CensusTable& table);

/// Count a call; dump the entry registers and thread for the first four. Cvar-gated. Returns the
/// call's sequence number (1-based) whether or not the census is on.
uint64_t CensusHit(CensusTable& table, size_t index, const PPCContext& ctx);
uint64_t PortCensus(PortCounters& c, const PPCContext& ctx);

void PortSkip(PortCounters& c, ShadowStats& stats, const char* why);
void PortOversize(PortCounters& c, ShadowStats& stats, uint32_t total);
void PortNativeRun(PortCounters& c);
bool PortPromoted(PortStatus s);
constexpr bool PortShadowable(PortStatus s) {
  return s == kPortPending || s == kPortVerified || s == kPortDivergent || s == kPortUncalled;
}

/// Call a guest function from a native body with its arguments in r3.. on the LIVE context, so
/// a hooked callee behaves exactly as it would under the original caller (compared by its own
/// hook, or run natively once promoted). Volatile registers are clobbered as on the real call;
/// read the result from ctx.r3 (or f1) afterwards. Never call __imp__ directly from a port.
template <typename... Args>
inline void GuestCall(PPCContext& ctx, uint8_t* base, PPCFunc* fn, Args... args) {
  static_assert(sizeof...(Args) <= 8, "guest calls take at most eight register arguments");
  const uint64_t values[] = {static_cast<uint64_t>(args)..., 0};
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (size_t i = 0; i < sizeof...(Args); i++) regs[i]->u64 = values[i];
  fn(ctx, base);
}

}  // namespace skate3::audio

#define SKATE3_PORT_EX(ADDR, STATUS, RETURNS, SAMPLE_SHIFT)                                    \
  extern "C" REX_FUNC(sub_##ADDR) {                                                            \
    using namespace skate3::audio;                                                             \
    static ShadowStats stats("sub_" #ADDR);                                                    \
    static PortCounters counters("sub_" #ADDR, (STATUS));                                      \
    const uint64_t seq = PortCensus(counters, ctx);                                            \
    if (ShadowEnabled() && PortShadowable(STATUS)) {                                           \
      if (ShadowNestedReplay() ||                                                              \
          ((SAMPLE_SHIFT) != 0 && (seq & ((1ull << (SAMPLE_SHIFT)) - 1)) != 0)) {              \
        __imp__sub_##ADDR(ctx, base);                                                          \
        return;                                                                                \
      }                                                                                        \
      PortSpec spec;                                                                           \
      spec.returns = (RETURNS);                                                                \
      if (!port_##ADDR::Windows(ctx, base, spec) || spec.overflowed) {                         \
        PortSkip(counters, stats, spec.overflowed ? "too many spans" : "not enumerable");      \
        __imp__sub_##ADDR(ctx, base);                                                          \
        return;                                                                                \
      }                                                                                        \
      if (spec.total() > kPortWatchCap) {                                                      \
        PortOversize(counters, stats, spec.total());                                           \
        __imp__sub_##ADDR(ctx, base);                                                          \
        return;                                                                                \
      }                                                                                        \
      ShadowCompare(ctx, base, port_##ADDR::Native, __imp__sub_##ADDR, {spec.windows, spec.nwin}, \
                    {spec.inputs, spec.nin}, spec.returns, stats);                             \
      return;                                                                                  \
    }                                                                                          \
    if (PortPromoted(STATUS)) {                                                                \
      PortNativeRun(counters);                                                                 \
      port_##ADDR::Native(ctx, base);                                                          \
      return;                                                                                  \
    }                                                                                          \
    __imp__sub_##ADDR(ctx, base);                                                              \
  }

#define SKATE3_PORT(ADDR, STATUS, RETURNS) SKATE3_PORT_EX(ADDR, STATUS, RETURNS, 0)
