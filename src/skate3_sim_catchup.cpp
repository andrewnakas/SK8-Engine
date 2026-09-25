// Run one simulation step per timer tick, not one per main-loop iteration.
//
// Skate 3's simulation is paced by a wall-clock timer, not by frames. Its timer
// thread (sub_82966930) waits on a 60 Hz one-shot timer (the period comes from
// Simulation::OnRequestSetSimRate, sub_82857EC8, via the setter sub_82966910),
// and on every fire bumps a tick counter at +72 of the object *0x830842F4 and
// signals the main loop. The main loop then calls SimUpdate2 (sub_8285A3C0)
// ONCE, with bit 0 of its flags set, and SimUpdate2 advances exactly one fixed
// 1/60 s physics step (0x3C888889) per call.
//
// On the 360 that loop ran at 60 Hz, so one step per iteration was one step per
// tick. On a Switch the career world's main loop manages ~34 iterations a
// second with every core saturated, the ticks that land during an iteration
// coalesce into one wake-up, and the game runs at loop-rate/60 speed - the
// "slow motion" that no clock, vblank or clamp change could touch, because the
// timer itself measures 60.0 arms a second throughout.
//
// This runs SimUpdate2 once per 1/60 s of REAL time owed instead, capped - a
// fixed-timestep accumulator on the host clock. The cap is a ceiling on purpose: each
// extra step costs CPU on a machine with none to spare, and an uncapped
// catch-up after a load stall would spiral.
//
// Recovered with the timing notes from the Skate 3 Rust engine
// (crates/skate-game/src/physics/clock.rs), which names these functions.

#include "generated/skate3_init.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(skate3_sim_catchup_max, 1, "Skate 3",
                     "Most simulation steps (1/60 s each) the main loop may run per iteration "
                     "when timer ticks have piled up. 1 = the original one step per iteration, "
                     "which runs the game slow whenever the loop cannot hold 60.")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// lis r24,-31992 ; lwz r3,17140(r24) in the timer thread: the object whose
// +72 the timer increments on every fire.
constexpr uint32_t kTickOwnerPtr = 0x830842F4;
constexpr uint32_t kTickCountOffset = 72;

inline uint32_t GuestU32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  __builtin_memcpy(&v, base + addr, sizeof(v));
  return __builtin_bswap32(v);
}

bool ReadTickCount(uint8_t* base, uint32_t& out) {
  const uint32_t owner = GuestU32(base, kTickOwnerPtr);
  if (owner < 0x10000u) {
    return false;
  }
  out = GuestU32(base, owner + kTickCountOffset);
  return true;
}

// Main thread only: SimUpdate2 has a single caller, sub_8285CC28.
struct Stats {
  std::chrono::steady_clock::time_point window_start{};
  uint32_t calls = 0;
  uint32_t step_calls = 0;  // calls with bit 0 set
  uint32_t steps = 0;       // SimUpdate2 bodies actually run on those
  uint32_t dropped = 0;     // ticks beyond the cap, discarded
  uint32_t ticks_at_start = 0;
  bool have_ticks = false;
} g_stats;

bool g_have_last = false;
std::chrono::steady_clock::time_point g_last_step{};
double g_accum = 0.0;

void Report(uint8_t* base) {
  auto& s = g_stats;
  const auto now = std::chrono::steady_clock::now();
  uint32_t ticks = 0;
  const bool have = ReadTickCount(base, ticks);
  if (s.window_start.time_since_epoch().count() == 0) {
    s.window_start = now;
    s.ticks_at_start = ticks;
    s.have_ticks = have;
    return;
  }
  const double secs = std::chrono::duration<double>(now - s.window_start).count();
  if (secs < 30.0) {
    return;
  }
  REXLOG_WARN(
      "[sim] {:.0f}s: task={:.1f}/s task-with-frame={:.1f}/s sim-frames={:.1f}/s timer-ticks={} "
      "dropped={} catchup_max={}",
      secs, s.calls / secs, s.step_calls / secs, s.steps / secs,
      (have && s.have_ticks) ? fmt::format("{:.1f}/s", (ticks - s.ticks_at_start) / secs)
                             : std::string("n/a"),
      s.dropped, REXCVAR_GET(skate3_sim_catchup_max));
  s = Stats{};
  s.window_start = now;
  s.ticks_at_start = ticks;
  s.have_ticks = have;
}

}  // namespace

// SimUpdate2: counted only. Repeating it alone (camera publication, audio,
// per-second bookkeeping) measured 60 steps/s and changed nothing on screen,
// because the physics half - SimUpdate1, sub_82859E70 - still ran once.
extern "C" REX_FUNC(sub_8285A3C0) {
  if (ctx.r4.u32 & 1u) {
    ++g_stats.steps;
  }
  __imp__sub_8285A3C0(ctx, base);
}

// Simulation::Update, the Main XThread task (stack: 0x826D79F4 -> 0x8285CB94).
// It asks the frame source (vfunc 16 of this+12) for frame n+1, and when one
// is ready runs SimUpdate1 (physics, waits on the job threads) and then
// SimUpdate2, returning the flags word with bit 0 set. Re-run the whole task
// while real time is owed, and stop the moment it reports no frame ready, so
// a frame the game did not produce is never invented.
extern "C" REX_FUNC(sub_8285C928) {
  ++g_stats.calls;
  Report(base);

  const auto now = std::chrono::steady_clock::now();
  const int32_t cap = REXCVAR_GET(skate3_sim_catchup_max);
  uint32_t budget = 1;
  if (g_have_last && cap > 1) {
    constexpr double kStep = 1.0 / 60.0;
    g_accum += std::chrono::duration<double>(now - g_last_step).count();
    budget = uint32_t(std::clamp(g_accum / kStep, 1.0, double(cap)));
  }
  g_last_step = now;
  g_have_last = true;

  const uint64_t r3 = ctx.r3.u64;
  const uint64_t lr = ctx.lr;
  uint32_t stepped = 0;
  for (uint32_t i = 0; i < budget; ++i) {
    ctx.r3.u64 = r3;
    ctx.lr = lr;
    __imp__sub_8285C928(ctx, base);
    if (!(ctx.r3.u32 & 1u)) {
      break;  // no sim frame was ready: nothing more to catch up
    }
    ++stepped;
  }
  if (stepped) {
    ++g_stats.step_calls;
  }
  if (cap > 1) {
    constexpr double kStep = 1.0 / 60.0;
    g_accum -= std::max<uint32_t>(stepped, 1) * kStep;
    if (g_accum > kStep) {
      g_stats.dropped += uint32_t(g_accum / kStep);
      g_accum = std::fmod(g_accum, kStep);
    }
    g_accum = std::max(g_accum, -kStep);
  }
}
