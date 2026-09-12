/**
 * @file        skate3_audio_kernel_census.cpp
 * @brief       Counting-only hooks on the audio-thread VMX128 kernels
 *
 * Phase 3 screened sixteen vector kernels and gate 1 closed across the whole subtree, but the
 * guest tracer records *first call* breadth, not frequency. `REQUEUE` (`sub_82B48B28`) passed
 * all three gates and was then never called once, which cost a native body and two sessions to
 * discover. These kernels are 200 to 2,300 lines each, so the same mistake would cost ten times
 * as much.
 *
 * So: count first, port second. Each hook forwards straight to the original and does nothing
 * else, so this is safe to leave compiled in; it is cvar-gated so it costs nothing when off.
 *
 * Note for whoever adds the next TU here: `CMakeLists.txt` carries an **explicit source list**
 * (around line 256), not a glob. A new file is silently ignored until it is listed there, and
 * `ninja` still exits 0 — so the first build of this file compiled nothing, flipped no symbols,
 * and looked exactly like success. Check for the object file or the symbol, never the exit code.
 * Adding a file also forces a reconfigure plus ~261 build steps, not the usual 3-4 s relink.
 *
 * `sub_82B4FD40` is deliberately absent. It is already hooked by `skate3_audio_probe.cpp`, and a
 * second definition would be a duplicate symbol — its comment also identifies it as the VMX128
 * memcpy filling XMA input contexts, so it is a block copy rather than DSP anyway.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_BOOL(
    skate3_audio_kernel_windows, false, "Skate 3",
    "Log sub_82B50380's address registers at entry, to size its shadow windows.\n"
    "\n"
    "It writes 111 stores through 14 distinct base/offset register pairs, and the shadow harness\n"
    "drops windows past 64 KB in total - dropping the offending window and every one after it -\n"
    "while an uncovered native write is never rewound and so reaches the live game. Whether it\n"
    "can be ported safely is therefore a question about these spans, and this answers it with\n"
    "numbers rather than an inference, the way BUFPAIR's buffer lengths were answered.");

REXCVAR_DEFINE_BOOL(
    skate3_audio_kernel_census, false, "Skate 3",
    "Count calls to the audio-thread VMX128 kernels and log the totals every 10 s.\n"
    "\n"
    "Breadth is not frequency: the guest tracer says these sixteen kernels each ran at least "
    "once, and says nothing about whether any is hot enough to be worth porting. This answers "
    "that before a 1,284-line hand translation is written rather than after.");

namespace {

constexpr const char* kKernelNames[] = {
    "sub_82B22898", "sub_82B50380", "sub_82B42C98", "sub_82B399D0", "sub_82B44D18",
    "sub_82B3C098", "sub_82B3D0A8", "sub_824531C8", "sub_82B389A0", "sub_82B427D8",
    "sub_82B44B20", "sub_82B373C8", "sub_82B3CF58", "sub_82B3BED8", "sub_82B238A8",
};
constexpr size_t kKernelCount = sizeof(kKernelNames) / sizeof(kKernelNames[0]);
std::atomic<uint64_t> g_calls[kKernelCount]{};
std::atomic<bool> g_reporter_started{false};

bool CensusEnabled() {
  static const bool on = REXCVAR_GET(skate3_audio_kernel_census);
  return on;
}

void ReporterMain() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    uint64_t total = 0;
    for (size_t i = 0; i < kKernelCount; i++) {
      total += g_calls[i].load(std::memory_order_relaxed);
    }
    if (total == 0) {
      continue;
    }
    for (size_t i = 0; i < kKernelCount; i++) {
      const uint64_t n = g_calls[i].load(std::memory_order_relaxed);
      if (n != 0) {
        REXLOG_INFO("skate3-kernel-census: {} calls={}", kKernelNames[i], n);
      }
    }
    REXLOG_INFO("skate3-kernel-census: {} kernels of {} called, {} calls total",
                [&] {
                  size_t c = 0;
                  for (size_t i = 0; i < kKernelCount; i++) {
                    if (g_calls[i].load(std::memory_order_relaxed) != 0) c++;
                  }
                  return c;
                }(),
                kKernelCount, total);
  }
}

bool WindowProbeEnabled() {
  static const bool on = REXCVAR_GET(skate3_audio_kernel_windows);
  return on;
}

std::atomic<uint64_t> g_window_logged{0};

/// The registers sub_82B50380's store addresses are built from, per the lifted body:
/// `ea = r11`, `r11 + {r3,r4,r6,r7,r29,r30,r31}`, `r7 + {...}`, `(r10 + r9) & ~0xF`, `2 + r10`.
/// Logged raw rather than reduced here: which register is the base and which the offset is not
/// obvious from the body, so the spans get computed offline from real values.
void LogWindowRegisters(const PPCContext& ctx) {
  if (!WindowProbeEnabled()) {
    return;
  }
  const uint64_t n = g_window_logged.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 24 && (n % 50000) != 0) {
    return;
  }
  REXLOG_INFO("skate3-kernel-windows: sub_82B50380 #{} r3={:08X} r4={:08X} r5={:08X} r6={:08X} "
              "r7={:08X} r8={:08X} r9={:08X} r10={:08X} r11={:08X} r27={:08X} r28={:08X} "
              "r29={:08X} r30={:08X} r31={:08X} r1={:08X}",
              n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
              ctx.r9.u32, ctx.r10.u32, ctx.r11.u32, ctx.r27.u32, ctx.r28.u32, ctx.r29.u32,
              ctx.r30.u32, ctx.r31.u32, ctx.r1.u32);
}

void Count(size_t index) {
  if (!CensusEnabled()) {
    return;
  }
  g_calls[index].fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (g_reporter_started.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
    std::thread(ReporterMain).detach();
  }
}

}  // namespace

#define SKATE3_CENSUS(index, name)                \
  extern "C" REX_FUNC(name) {                     \
    Count(index);                                 \
    __imp__##name(ctx, base);                     \
  }

SKATE3_CENSUS(0, sub_82B22898)

// Hand-written rather than SKATE3_CENSUS(1, ...), because this one also measures its own
// window spans. Phase 3's first port target: 254,917 calls, leaf, 111 observable stores.
extern "C" REX_FUNC(sub_82B50380) {
  Count(1);
  LogWindowRegisters(ctx);
  __imp__sub_82B50380(ctx, base);
}
SKATE3_CENSUS(2, sub_82B42C98)
SKATE3_CENSUS(3, sub_82B399D0)
SKATE3_CENSUS(4, sub_82B44D18)
SKATE3_CENSUS(5, sub_82B3C098)
SKATE3_CENSUS(6, sub_82B3D0A8)
SKATE3_CENSUS(7, sub_824531C8)
SKATE3_CENSUS(8, sub_82B389A0)
SKATE3_CENSUS(9, sub_82B427D8)
SKATE3_CENSUS(10, sub_82B44B20)
SKATE3_CENSUS(11, sub_82B373C8)
SKATE3_CENSUS(12, sub_82B3CF58)
SKATE3_CENSUS(13, sub_82B3BED8)
SKATE3_CENSUS(14, sub_82B238A8)
