// Where the audio mixer's CPU goes.
//
// [cpu] measured RwAudioCore Dac at 40-73% of one core in the career world,
// with all three cores saturated and the simulation starved for them. The
// thread's structure, from thread dumps (0x82B21D80 entry, 0x82B21B20 wait):
//
//   sub_82B219E8  finish one Dac update
//     sub_82B44858  voice graph per-block pass
//       sub_82B444C0  node mixer fill -> sub_82B43FB8 resample
//       sub_82B238A8  voice gain ramp  -> sub_82B42C98 ramp writer
//
// Every function below is one the Skate 3 Rust engine's skate-audio-core crate
// ports one-to-one and names (andrewnakas/skate-3-rust-engine, branch
// audio/retail-exact-player-sound), most with recorded bit-exact vectors. Each
// is timed with two tick reads (inside its port hook, skate3_audio_port.h) so
// the report names which bodies hold the time. Times are INCLUSIVE:
// the graph pass contains the kernels it calls.
//
// Off unless skate3_audio_profile is set; the wrappers then cost one flag test.

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(skate3_audio_profile, false, "Skate 3",
                    "Time the audio mixer's functions and report the heaviest every 30 s "
                    "as [audio-prof].")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// name, what it is (from skate-audio-core)
#define SKATE3_AUDIO_PROFILED(X)                         \
  X(82B219E8, "dac.update")                              \
  X(82B48530, "system.run_frame")                        \
  X(82B3C2D0, "dac.budget_prepass")                      \
  X(82B44858, "graph.block_pass")                        \
  X(82B444C0, "graph.node_fill")                         \
  X(82B238A8, "gains.voice_ramp")                        \
  X(82B43FB8, "dsp.resample")                            \
  X(82B2DBA8, "pitch.resample_block")                    \
  X(82B42C98, "dsp.ramp_writer_b")                       \
  X(82B427D8, "dsp.ramp_writer_a")                       \
  X(82B43340, "dsp.ramp_sine")                           \
  X(824531C8, "dsp.sine4")                               \
  X(82B3C098, "dsp.gain_ramp_copy")                      \
  X(82B44D18, "dsp.gain_ramp_acc")                       \
  X(82B3BED8, "dsp.scale")                               \
  X(82B44B20, "dsp.scale_acc")                           \
  X(82B3CF58, "dsp.scale_add")                           \
  X(82B43AF8, "dsp.biquad")                              \
  X(82B389A0, "dsp.allpass")                             \
  X(82B399D0, "stage.one_pole")                          \
  X(82B22678, "dsp.clip")                                \
  X(82B3D0A8, "crossfade.blend")                         \
  X(82B426D0, "routing.scatter")                         \
  X(82B468C0, "routing.gather")                          \
  X(82B46810, "routing.downmix")                         \
  X(82B29AF0, "gains.matrix")                            \
  X(82B23B50, "gains.channel_ramp")                      \
  X(82B298E0, "gains.matrix_ramp")                       \
  X(82B31838, "bus.source_mix")                          \
  X(82B305C0, "bus.descriptor_mix")                      \
  X(82B460A0, "spatial.mix_matrix")                      \
  X(82B27E20, "filters.lowpass")                         \
  X(82B26568, "filters.highpass")                        \
  X(82B26740, "filters.shelf")                           \
  X(82B43D78, "filters.shelf_coeffs")                    \
  X(82B34E08, "mix.flush")                               \
  X(82B3C668, "mix.fold")                                \
  X(82B443F8, "mix.advance_clear")                       \
  X(82B3CA60, "stream.deliver")                          \
  X(82B34278, "sndplayer.process")                       \
  X(82B3DD90, "ring.window")                             \
  X(82B21F58, "output.route")                            \
  X(82B21D98, "output.interleave")                       \
  X(82B20E18, "output.clamp")                            \
  X(82B46B30, "interleave.six")

namespace {

enum : int {
#define SKATE3_ENUM(addr, name) kFn_##addr,
  SKATE3_AUDIO_PROFILED(SKATE3_ENUM)
#undef SKATE3_ENUM
  kFnCount
};

const char* const kNames[kFnCount] = {
#define SKATE3_NAME(addr, name) name,
    SKATE3_AUDIO_PROFILED(SKATE3_NAME)
#undef SKATE3_NAME
};
const char* const kAddrs[kFnCount] = {
#define SKATE3_ADDR(addr, name) #addr,
    SKATE3_AUDIO_PROFILED(SKATE3_ADDR)
#undef SKATE3_ADDR
};

std::atomic<uint64_t> g_ticks[kFnCount];
std::atomic<uint32_t> g_calls[kFnCount];

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

void MaybeReport() {
  static uint64_t window_start = 0;
  const uint64_t now = Now();
  if (window_start == 0) {
    window_start = now;
    return;
  }
  const double secs = double(now - window_start) / TickFreq();
  if (secs < 30.0) {
    return;
  }
  window_start = now;
  struct Row {
    int i;
    uint64_t ticks;
    uint32_t calls;
  };
  std::vector<Row> rows;
  for (int i = 0; i < kFnCount; ++i) {
    rows.push_back({i, g_ticks[i].exchange(0, std::memory_order_relaxed),
                    g_calls[i].exchange(0, std::memory_order_relaxed)});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ticks > b.ticks; });
  std::string out;
  for (const auto& r : rows) {
    if (!r.calls) {
      continue;
    }
    // Share of one core, and microseconds per call.
    const double core_pct = 100.0 * double(r.ticks) / TickFreq() / secs;
    const double us = 1e6 * double(r.ticks) / TickFreq() / r.calls;
    out += fmt::format(" {}({})={:.1f}%/{}c/{:.1f}us", kNames[r.i], kAddrs[r.i], core_pct,
                       uint32_t(r.calls / secs), us);
  }
  REXLOG_WARN("[audio-prof] {:.0f}s inclusive, % of one core / calls per s / us per call:{}",
              secs, out);
}

}  // namespace

// Every profiled function is now a native port (skate3_audio_port.h), so the port hook owns
// its symbol and does the timing through these; a second strong definition here would not
// link. The timing is inclusive and covers whichever body ran, native or original.
namespace skate3::audio {

bool ProfileOn() {
  return REXCVAR_GET(skate3_audio_profile);
}

uint64_t ProfileNow() {
  return Now();
}

int ProfileSlot(uint32_t addr) {
  for (int i = 0; i < kFnCount; ++i) {
    if (uint32_t(std::strtoul(kAddrs[i], nullptr, 16)) == addr) {
      return i;
    }
  }
  return -1;
}

void ProfileRecord(int slot, uint64_t ticks) {
  g_ticks[slot].fetch_add(ticks, std::memory_order_relaxed);
  g_calls[slot].fetch_add(1, std::memory_order_relaxed);
  if (slot == kFn_82B219E8) {
    MaybeReport();
  }
}

}  // namespace skate3::audio
