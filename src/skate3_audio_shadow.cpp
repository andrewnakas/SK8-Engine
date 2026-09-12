/**
 * @file        skate3_audio_shadow.cpp
 * @brief       Differential verification for native audio replacements
 *
 * See the header for why the lifted body runs first and stays authoritative.
 */
#include "skate3_audio_shadow.h"

#include <atomic>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    skate3_audio_shadow, false, "Skate 3",
    "Run native audio replacements alongside the original recompiled code and report "
    "any divergence.\n"
    "\n"
    "The original stays authoritative, so this cannot change what the game does; it "
    "doubles the work on the functions it covers. Leave it on through a play session to "
    "check a native implementation against real inputs rather than hand-written ones.");

REXCVAR_DEFINE_UINT32(
    skate3_audio_shadow_reports, 16, "Skate 3",
    "How many divergences to report per function before going quiet.");

namespace skate3::audio {
namespace {

// The window is compared twice per call; audio functions run at 187.5 Hz, so this is
// deliberately small. A function needing more than this is the wrong shape for a
// window-based diff and should be compared on its output buffer instead.
constexpr uint32_t kMaxWatch = 64 * 1024;

std::atomic<uint64_t> g_reports{0};

bool RegistersEqual(const PPCContext& a, const PPCContext& b) {
  return std::memcmp(&a, &b, sizeof(PPCContext)) == 0;
}

// Name the first differing byte range, which is far more useful than "they differ".
size_t FirstDiff(const uint8_t* a, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) return i;
  }
  return n;
}

}  // namespace

bool ShadowEnabled() {
  static const bool armed = REXCVAR_GET(skate3_audio_shadow);
  return armed;
}

bool ShadowCompare(PPCContext& ctx, uint8_t* base, PPCFunc* native, PPCFunc* lifted,
                   uint32_t watch_addr, uint32_t watch_len, const char* name) {
  if (watch_len > kMaxWatch) watch_len = kMaxWatch;
  uint8_t* watch = (watch_addr && watch_len) ? base + watch_addr : nullptr;

  const PPCContext entry = ctx;
  std::vector<uint8_t> mem_entry;
  if (watch) {
    mem_entry.assign(watch, watch + watch_len);
  }

  // 1. The lifted body, for real.
  lifted(ctx, base);
  const PPCContext after_lifted = ctx;
  std::vector<uint8_t> mem_lifted;
  if (watch) {
    mem_lifted.assign(watch, watch + watch_len);
    std::memcpy(watch, mem_entry.data(), watch_len);
  }

  // 2. The native body, against a rewound copy.
  ctx = entry;
  native(ctx, base);
  const bool regs_ok = RegistersEqual(ctx, after_lifted);
  bool mem_ok = true;
  size_t at = 0;
  if (watch) {
    at = FirstDiff(watch, mem_lifted.data(), watch_len);
    mem_ok = at == watch_len;
  }

  // 3. Restore the authoritative result, whatever the native code did.
  ctx = after_lifted;
  if (watch) {
    std::memcpy(watch, mem_lifted.data(), watch_len);
  }

  if (regs_ok && mem_ok) {
    return true;
  }
  if (g_reports.fetch_add(1, std::memory_order_relaxed) <
      REXCVAR_GET(skate3_audio_shadow_reports)) {
    if (!regs_ok && !mem_ok) {
      REXLOG_ERROR("skate3-audio-shadow: {} diverges in registers and at {:08X}+{}",
                   name, watch_addr, at);
    } else if (!regs_ok) {
      REXLOG_ERROR("skate3-audio-shadow: {} diverges in registers", name);
    } else {
      REXLOG_ERROR("skate3-audio-shadow: {} diverges in memory at {:08X}+{} "
                   "(lifted {:02X}, native {:02X})",
                   name, watch_addr, at, mem_lifted[at], watch[at]);
    }
  }
  return false;
}

}  // namespace skate3::audio
