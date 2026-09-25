// Where the simulation's CPU goes in the career world.
//
// University runs its simulation at 11-28 frames a second against a 60 Hz
// timer, which is the slow motion: Simulation::Update (sub_8285C928, Main
// XThread) waits in SimUpdate1 (sub_82859E70) on the three Job Manager threads.
// [cpu] says those threads are busy but not WHAT they run.
//
// Wrapped here: SimUpdate1 itself, every function it calls directly (read off
// the recompiled body), and the physics functions the Skate 3 Rust engine
// names (andrewnakas/skate-3-rust-engine, crates/skate-core/src/physics: Horus
// constraint iteration, Island::Step_Solver2, RigidBody::DynamicUpdate, the
// narrowphase pair queries, the world query). Each one's time is split by the
// thread that ran it, so the report says both which body is heavy and whether
// it runs on Main or on the job threads. Times are INCLUSIVE.
//
// Off unless skate3_phys_profile is set; the wrappers then cost one flag test.

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_phys_profile, false, "Skate 3",
                    "Time SimUpdate1, its callees and the physics solver, split by thread, and "
                    "report the heaviest every 30 s as [phys-prof].")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#define SKATE3_PHYS_PROFILED(X)                        \
  X(82859E70, "sim.update1")                           \
  X(82E09B70, "u1.82E09B70")                           \
  X(82DC2FB8, "u1.82DC2FB8")                           \
  X(82763EE0, "u1.82763EE0")                           \
  X(82763E00, "u1.82763E00")                           \
  X(82DD9A78, "u1.82DD9A78")                           \
  X(828A80F8, "u1.828A80F8")                           \
  X(8288BAF0, "u1.8288BAF0")                           \
  X(82883738, "u1.82883738")                           \
  X(8287F630, "u1.8287F630")                           \
  X(82865E68, "u1.82865E68")                           \
  X(82764480, "u1.82764480")                           \
  X(82763B08, "island.step_solver2")                   \
  X(8275EB10, "u1.8275EB10")                           \
  X(8275EA20, "u1.8275EA20")                           \
  X(826BE140, "u1.826BE140")                           \
  X(826BE020, "u1.826BE020")                           \
  X(826BDB50, "u1.826BDB50")                           \
  X(826B04C8, "u1.826B04C8")                           \
  X(82696030, "u1.82696030")                           \
  X(82597338, "u1.82597338")                           \
  X(82596DB8, "job.wait_a")                            \
  X(82596C10, "u1.82596C10")                           \
  X(82596AF0, "u1.82596AF0")                           \
  X(82595298, "u1.82595298")                           \
  X(82588380, "u1.82588380")                           \
  X(8245A7E8, "u1.8245A7E8")                           \
  X(82DC35E0, "phys.job_setup")                        \
  X(82AE27D0, "horus.constraint_iter")                 \
  X(82AE6590, "rigid.dynamic_update")                  \
  X(82AD43A8, "narrow.gp_pair")                        \
  X(82AD3CD8, "narrow.prim_pair")                      \
  X(8277A508, "narrow.pair_caller")                    \
  X(8277B720, "world.query")                           \
  X(824741F0, "job.wait_b")                            \
  X(8290AE38, "job.wait_c")

namespace {

enum : int {
#define SKATE3_ENUM(addr, name) kPhys_##addr,
  SKATE3_PHYS_PROFILED(SKATE3_ENUM)
#undef SKATE3_ENUM
  kPhysCount
};

const char* const kNames[kPhysCount] = {
#define SKATE3_NAME(addr, name) name,
    SKATE3_PHYS_PROFILED(SKATE3_NAME)
#undef SKATE3_NAME
};
const char* const kAddrs[kPhysCount] = {
#define SKATE3_ADDR(addr, name) #addr,
    SKATE3_PHYS_PROFILED(SKATE3_ADDR)
#undef SKATE3_ADDR
};

// Threads are numbered on first use; the last slot collects any beyond the table.
constexpr int kMaxThreads = 16;
std::mutex g_thread_mutex;
std::string g_thread_names[kMaxThreads];
int g_thread_count = 0;
thread_local int t_thread_slot = -1;

std::atomic<uint64_t> g_ticks[kPhysCount][kMaxThreads];
std::atomic<uint32_t> g_calls[kPhysCount];

inline uint64_t Now() {
#if defined(__SWITCH__)
  return armGetSystemTick();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline double TickFreq() {
#if defined(__SWITCH__)
  return double(armGetSystemTickFreq());
#else
  return double(std::chrono::steady_clock::period::den) /
         double(std::chrono::steady_clock::period::num);
#endif
}

int ThreadSlot() {
  if (t_thread_slot >= 0) {
    return t_thread_slot;
  }
  std::string name = "?";
  if (auto* thread = rex::system::XThread::GetCurrentThread()) {
    name = thread->thread_name();
  }
  std::lock_guard<std::mutex> lock(g_thread_mutex);
  if (g_thread_count < kMaxThreads) {
    g_thread_names[g_thread_count] = name + "/" + std::to_string(g_thread_count);
    t_thread_slot = g_thread_count++;
  } else {
    t_thread_slot = kMaxThreads - 1;
  }
  return t_thread_slot;
}

void ReporterMain() {
  constexpr int kWindowSeconds = 30;
  uint64_t window_start = Now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(kWindowSeconds));
    const uint64_t now = Now();
    const double secs = double(now - window_start) / TickFreq();
    window_start = now;
    struct Row {
      int i;
      uint64_t ticks;
      uint32_t calls;
      int top_thread;
      uint64_t top_ticks;
      int threads;
    };
    std::vector<Row> rows;
    for (int i = 0; i < kPhysCount; ++i) {
      Row r{i, 0, g_calls[i].exchange(0, std::memory_order_relaxed), -1, 0, 0};
      for (int t = 0; t < kMaxThreads; ++t) {
        const uint64_t v = g_ticks[i][t].exchange(0, std::memory_order_relaxed);
        if (!v) {
          continue;
        }
        r.ticks += v;
        r.threads++;
        if (v > r.top_ticks) {
          r.top_ticks = v;
          r.top_thread = t;
        }
      }
      if (r.calls) {
        rows.push_back(r);
      }
    }
    if (rows.empty()) {
      continue;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ticks > b.ticks; });
    std::string names[kMaxThreads];
    {
      std::lock_guard<std::mutex> lock(g_thread_mutex);
      for (int t = 0; t < g_thread_count; ++t) {
        names[t] = g_thread_names[t];
      }
    }
    // Share of one core, calls per second, microseconds per call, and the thread that
    // spent the most of it (with its share of the function, and how many threads ran it).
    std::string out;
    for (const auto& r : rows) {
      const double core_pct = 100.0 * double(r.ticks) / TickFreq() / secs;
      const double us = 1e6 * double(r.ticks) / TickFreq() / r.calls;
      const double top_share = r.ticks ? 100.0 * double(r.top_ticks) / double(r.ticks) : 0.0;
      out += fmt::format(" {}({})={:.1f}%/{}c/{:.1f}us@{}:{:.0f}%x{}", kNames[r.i], kAddrs[r.i],
                         core_pct, uint32_t(r.calls / secs), us,
                         r.top_thread >= 0 ? names[r.top_thread] : "?", top_share, r.threads);
    }
    REXLOG_WARN("[phys-prof] {:.0f}s inclusive, % of one core / calls per s / us per call "
                "@ heaviest thread:share x threads:{}",
                secs, out);
  }
}

inline bool ProfileOn() {
  if (!REXCVAR_GET(skate3_phys_profile)) {
    return false;
  }
  static std::once_flag once;
  std::call_once(once, [] { std::thread(ReporterMain).detach(); });
  return true;
}

}  // namespace

#define SKATE3_WRAP(addr, name)                                                          \
  extern "C" REX_FUNC(sub_##addr) {                                                      \
    if (!ProfileOn()) {                                                                  \
      __imp__sub_##addr(ctx, base);                                                      \
      return;                                                                            \
    }                                                                                    \
    const int slot = ThreadSlot();                                                       \
    const uint64_t t0 = Now();                                                           \
    __imp__sub_##addr(ctx, base);                                                        \
    g_ticks[kPhys_##addr][slot].fetch_add(Now() - t0, std::memory_order_relaxed);        \
    g_calls[kPhys_##addr].fetch_add(1, std::memory_order_relaxed);                       \
  }
SKATE3_PHYS_PROFILED(SKATE3_WRAP)
#undef SKATE3_WRAP
