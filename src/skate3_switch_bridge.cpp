/**
 * @file        skate3_switch_bridge.cpp
 * @brief       Console-specific glue: clocks and memory reporting.
 */

#include "skate3_switch_bridge.h"

#if defined(__SWITCH__)

#include <switch.h>

#include <chrono>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/main_switch.h>

REXCVAR_DEFINE_STRING(
    switch_overclock, "off", "Switch",
    "Run the console faster than Nintendo intended: off, cpu, cpu+gpu, or max. "
    "The processor is what holds this game back, so 'cpu' is the setting that "
    "buys frames - 1785 MHz against a stock 1020. This is not free. The console "
    "draws more power, the fan works harder, and in handheld mode the battery "
    "goes considerably sooner; sustained use runs the chip hotter than any "
    "retail game will. It is off by default for those reasons, and a system "
    "module like sys-clk does the same job with better thermal supervision.")
    .allowed({"off", "cpu", "cpu+gpu", "max"});

REXCVAR_DEFINE_INT32(
    switch_memory_log_interval_s, 30, "Switch",
    "Seconds between memory lines in the log (0 disables). The budget on a "
    "retail console is tight enough that knowing the headroom is worth a line "
    "twice a minute.");

namespace skate3::switch_bridge {

namespace {

// Stock rates, which are what "off" restores. The GPU's stock rate differs
// between handheld and docked; 768 MHz is the docked figure and the system
// re-derives it on the next mode change anyway.
// Explicit clock overrides, in MHz, 0 = use the built-in boost value below.
// Exposed because "overclock the GPU" should not need a rebuild, and because
// the right number is a property of the console's cooling and silicon rather
// than of this program. Above 921 MHz on the GPU is beyond Nintendo's own
// maximum: it is what the sys-clk tools call an overclock, it runs hot, and an
// unstable one shows up as a hang or a graphical fault rather than damage.
// Lower it if the console becomes unstable.
REXCVAR_DEFINE_INT32(switch_cpu_clock_mhz, 0, "Switch",
                     "CPU clock in MHz when switch_overclock is on (0 = 1785, the built-in "
                     "boost). Stock is 1020.")
    .range(0, 2400);
REXCVAR_DEFINE_INT32(switch_gpu_clock_mhz, 0, "Switch",
                     "GPU clock in MHz when switch_overclock raises the GPU (0 = 921, which is "
                     "Nintendo's own docked maximum). Higher is a real overclock: 998, 1075 and "
                     "1267 are the usual steps, they run hot, and instability shows as a hang or "
                     "a graphical fault. Handheld will throttle before docked does.")
    .range(0, 1600);
REXCVAR_DEFINE_INT32(switch_emc_clock_mhz, 0, "Switch",
                     "Memory clock in MHz when switch_overclock is 'max' (0 = 1600). Stock is "
                     "1331.")
    .range(0, 2133);

constexpr uint32_t kCpuStockHz = 1020000000;
constexpr uint32_t kCpuBoostHz = 1785000000;
constexpr uint32_t kGpuStockHz = 768000000;
constexpr uint32_t kGpuBoostHz = 921000000;
constexpr uint32_t kEmcStockHz = 1331200000;
constexpr uint32_t kEmcBoostHz = 1600000000;

bool clkrst_ready_ = false;
bool clocks_raised_ = false;

bool SetModuleRate(PcvModuleId module, uint32_t hz, const char* name) {
  ClkrstSession session = {};
  if (R_FAILED(clkrstOpenSession(&session, module, 3))) {
    REXLOG_WARN("switch_overclock: could not open a {} clock session", name);
    return false;
  }
  const Result rc = clkrstSetClockRate(&session, hz);
  clkrstCloseSession(&session);
  if (R_FAILED(rc)) {
    REXLOG_WARN("switch_overclock: {} would not accept {} MHz (rc=0x{:x})", name, hz / 1000000,
                rc);
    return false;
  }
  REXLOG_WARN("switch_overclock: {} set to {} MHz", name, hz / 1000000);
  return true;
}

}  // namespace

void ApplyClocks() {
  const std::string& mode = REXCVAR_GET(switch_overclock);
  if (mode == "off") {
    return;
  }

  // clkrst replaced pcv in firmware 8.0.0, which is old enough by now that
  // supporting both is not worth the second code path - but say so rather than
  // failing silently on a console that cannot do it.
  if (hosversionBefore(8, 0, 0)) {
    REXLOG_WARN("switch_overclock: needs firmware 8.0.0 or newer; leaving the clocks alone");
    return;
  }
  if (R_FAILED(clkrstInitialize())) {
    REXLOG_WARN("switch_overclock: clkrst is unavailable; leaving the clocks alone");
    return;
  }
  clkrst_ready_ = true;

  REXLOG_WARN(
      "switch_overclock={}: running above the stock clocks. Expect more heat, more fan, and "
      "noticeably less battery in handheld mode.",
      mode);

  // A cvar in MHz wins over the compiled-in boost rate.
  const auto pick = [](int32_t mhz, uint32_t fallback_hz) -> uint32_t {
    return mhz > 0 ? uint32_t(mhz) * 1000000u : fallback_hz;
  };
  bool any = SetModuleRate(PcvModuleId_CpuBus,
                           pick(REXCVAR_GET(switch_cpu_clock_mhz), kCpuBoostHz), "CPU");
  if (mode == "cpu+gpu" || mode == "max") {
    any |= SetModuleRate(PcvModuleId_GPU, pick(REXCVAR_GET(switch_gpu_clock_mhz), kGpuBoostHz),
                         "GPU");
  }
  if (mode == "max") {
    // Memory bandwidth is shared with the display controller, and this is the
    // one of the three most likely to be refused outright.
    any |= SetModuleRate(PcvModuleId_EMC, pick(REXCVAR_GET(switch_emc_clock_mhz), kEmcBoostHz),
                         "memory");
  }
  clocks_raised_ = any;
}

void RestoreClocks() {
  if (!clkrst_ready_) {
    return;
  }
  if (clocks_raised_) {
    SetModuleRate(PcvModuleId_CpuBus, kCpuStockHz, "CPU");
    SetModuleRate(PcvModuleId_GPU, kGpuStockHz, "GPU");
    SetModuleRate(PcvModuleId_EMC, kEmcStockHz, "memory");
    clocks_raised_ = false;
  }
  clkrstExit();
  clkrst_ready_ = false;
}

void LogMemoryPeriodically() {
  const int32_t interval = REXCVAR_GET(switch_memory_log_interval_s);
  if (interval <= 0) {
    return;
  }
  static std::chrono::steady_clock::time_point last;
  const auto now = std::chrono::steady_clock::now();
  if (last.time_since_epoch().count() &&
      now - last < std::chrono::seconds(interval)) {
    return;
  }
  last = now;

  const uint64_t used = rex::SwitchUsedMemory();
  const uint64_t total = rex::SwitchTotalMemory();
  const uint64_t free_bytes = total > used ? total - used : 0;
  REXLOG_INFO("[mem] {} MiB used of {} MiB, {} MiB free", used >> 20, total >> 20,
              free_bytes >> 20);
}

}  // namespace skate3::switch_bridge

#else

// Kept compilable off-console so the header can be included unconditionally.
namespace skate3::switch_bridge {
void ApplyClocks() {}
void RestoreClocks() {}
void LogMemoryPeriodically() {}
}  // namespace skate3::switch_bridge

#endif  // __SWITCH__
