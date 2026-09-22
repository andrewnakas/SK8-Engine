#include "skate3_loader_overlay.h"
#include "skate3_native_render.h"

#include "native/skate3_native_diag.h"
#include "native/skate3_native_entity.h"
#include "native/skate3_native_guest_read.h"
#include "native/skate3_native_lw.h"
#include "native/skate3_native_palette.h"
#include "skate3_crash_report.h"
#include "skate3_image_watch.h"
#include "skate3_alloc_counter.h"
#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#if defined(__SWITCH__)
// Declared rather than pulled in from <switch.h>: libnx puts Thread, Event,
// Handle, Mutex and Result in the global namespace, and this file already has
// all five names from the runtime and the renderer. One syscall is not worth
// that fight. The signature is libnx's, and it is C.
extern "C" void svcSleepThread(int64_t nano);
#endif

#include <algorithm>
#include <atomic>
#include <sched.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace skate3::frame_advance {
// Executions of the patched guest simulation clamp; see skate3_frame_advance.cpp.
uint64_t TakeClampCalls();
}  // namespace skate3::frame_advance

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_BOOL(skate3_native_render, true, "Skate 3",
                    "Enable the Skate 3 data-driven native renderer hook layer")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(skate3_native_render_log_interval, 0, "Skate 3",
                     "Frames between native-render hook liveness log lines (0 = off)")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(bool, skate3_native_render_scene_perf_log);
REXCVAR_DECLARE(bool, skate3_diagnostics);
REXCVAR_DECLARE(bool, skate3_native_render_scene_occlusion_cull_guest);
REXCVAR_DECLARE(int32_t, skate3_native_render_guest_static_refresh);
REXCVAR_DECLARE(int32_t, skate3_native_render_lw_refresh);
REXCVAR_DECLARE(int32_t, skate3_guest_spin_yield);
REXCVAR_DECLARE(int32_t, skate3_job_scan_backoff);
REXCVAR_DECLARE(int32_t, skate3_job_scan_backoff_yields);
REXCVAR_DECLARE(int32_t, skate3_job_scan_backoff_sleep_us);
REXCVAR_DECLARE(bool, skate3_job_scan_stats);
REXCVAR_DECLARE(bool, skate3_guest_spin_measure);
REXCVAR_DECLARE(bool, skate3_map_erase_probe);
REXCVAR_DECLARE(bool, skate3_map_erase_fix);

namespace skate3::native_render {
// Set by the find hook when it neutralised a miss, read by the erase hook to
// undo the size decrement. Thread-local: the erase runs on the load thread and
// the find is called from nowhere else in that window.
thread_local bool g_map_erase_missed = false;
}  // namespace skate3::native_render
REXCVAR_DEFINE_BOOL(skate3_d3d_ring_check, false, "Skate 3",
                    "Diagnostic: watch the guest D3D command-ring write pointer at every "
                    "deferred render-state flush (D3D::SetPending_RenderStates). The pointer at "
                    "device+0x30 is read-modify-written by guest code with no null check, no "
                    "range check and no synchronization. NOTE device+0x34 is a MOVING WATERMARK, "
                    "not the end of the buffer - the guest legitimately writes past it, so do "
                    "not treat that as corruption (an earlier version of this check did, and "
                    "fired constantly on healthy sessions). Logs an implausible pointer, and "
                    "whether a second thread ever touches the ring. Two loads per flush.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(skate3_guest_fps_cap, 0.0, "Skate 3",
                      "Pace the guest render loop to this frame rate (0 = uncapped). The "
                      "guest produces frames at irregular 2-9 ms intervals; the display "
                      "(especially with G-Sync/VRR, which follows present times directly) "
                      "turns that variance into visible irregular judder that no content "
                      "smoothing can fix. An even cap a few fps below the display refresh "
                      "(e.g. 140 on a 144 Hz panel) is the standard VRR recipe: every "
                      "frame arrives on a steady beat. Pacing is an absolute-deadline "
                      "sleep to the target, with skate3_guest_fps_cap_spin_us of spin "
                      "on the tail.")
    .range(0.0, 1000.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(skate3_guest_fps_cap_auto, true, "Skate 3",
                    "Derive the guest frame cap from the display the window is on: "
                    "cap a safety margin below the refresh rate (4 fps or 5%, "
                    "whichever is larger; see rex::ui::Window::AutoFrameCapHz), "
                    "the VRR recipe above, without hand-tuning per monitor. Above "
                    "the display refresh the extra frames cannot be shown anyway; "
                    "refreshes beat-sample the frame stream and steady motion "
                    "judders (measured: a mathematically perfect synthetic pan "
                    "judders at 330 fps on a 144 Hz panel and is smooth capped "
                    "below it). The margin must also absorb swap-to-present "
                    "jitter: presents run with tearing allowed, so a present "
                    "landing inside the panel's minimum refresh period tears even "
                    "under VRR. Overrides skate3_guest_fps_cap while the display "
                    "refresh is known.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(
    skate3_guest_fps_cap_spin_us, 300, "Skate 3",
    "How long the frame cap spins at the end of its wait, in microseconds.\n"
    "\n"
    "This was a hard-coded 2000 us. Measured on a Galaxy S23 FE during "
    "gameplay, that put sched_yield at 14.4% of the guest render thread's "
    "cycles - an eighth of every frame - while a scheduler trace of the same "
    "session showed the emulated command processor runnable with no core for "
    "7.8 of 50 seconds. The spin holds a performance core to do nothing while "
    "the thread the next frame is waiting for is queued behind it.\n"
    "\n"
    "The long window bought nothing: Android gives a thread 50 us of timer "
    "slack by default, so a multi-millisecond sleep already lands inside a few "
    "hundred microseconds of its deadline and the rest was pure yielding. "
    "0 disables the spin "
    "entirely and sleeps the whole way, which is fine under vsync - the cap is "
    "there to stop the guest running AHEAD of the panel, not to hit a deadline "
    "to the microsecond. Raise it if frames start landing late on a device "
    "whose scheduler wakes threads slowly.")
    .range(0, 4000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::native_render {
namespace {

// One per-mesh submission. kind 0 = RenderMesh (dynamic entities: a = the
// MeshContext, b = VertexProgramState, c = dynitem index+1). kind 1 =
// SceneRenderView sort-list entry (a = MeshContext, b = list offset from the
// view, c = view). kind 2 = world-path capture (skinned / model-space prop:
// a = MeshContext, b = submitting view, c = dynitem index+1). kind 3 =
// quad-list DrawVertices capture (a = synthetic key, c = dynitem index+1).
using RenderMeshRecord = skate3::native_scene::SubmitRecord;


std::mutex g_mutex;
std::vector<RenderMeshRecord> g_current_frame;
uint64_t g_frame_index = 0;
std::atomic<bool> g_announced{false};

bool Enabled() { return REXCVAR_GET(skate3_native_render); }

// ---- Guest D3D command-ring watch -----------------------------------------
// D3D::SetPending_RenderStates (sub_82B83C48) sits on the path where one face
// of the map-load crash landed: the guest walks the command ring from
// device+0x30 and writes PM4 type-0 packets through it with no null check, no
// range check and no synchronization. The crash itself turned out to be
// emulated-draw suppression corrupting guest state across a load (see
// skate3_native_render_scene_menu_unsuppress); this watch is kept because it is
// the only visibility into the ring if it ever misbehaves again.
//
// The loads here are raw REX_LOAD_U32 rather than the guarded GuestTryLoadU32:
// the guest performs the identical loads three instructions later, so a fault
// here is a fault that was going to happen anyway - only now the crash reporter
// describes it. Guarding would cost a sigsetjmp per state flush, and this runs
// several times per Clear.
std::atomic<uint32_t> g_ring_dev{0};
std::atomic<uint32_t> g_ring_prev_write{0};
std::atomic<uint32_t> g_ring_prev_end{0};
std::atomic<uint64_t> g_ring_prev_tid{0};
std::atomic<bool> g_ring_first_logged{false};
std::atomic<bool> g_ring_multi_thread_logged{false};
std::atomic<int64_t> g_ring_last_bad_ns{0};

void CheckD3DRing(uint8_t* base, uint32_t dev, uint64_t mask, uint32_t bank, uint32_t shadow) {
  if (!REXCVAR_GET(skate3_d3d_ring_check)) {
    return;
  }
  const uint64_t tid =
      uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  if (dev < 0x10000) {
    REXLOG_ERROR("d3d-ring: DEVICE pointer implausible dev={:08X} mask={:016X} bank={} tid={:X}",
                 dev, mask, bank, tid);
    return;
  }
  const uint32_t write = REX_LOAD_U32(dev + 48);
  const uint32_t end = REX_LOAD_U32(dev + 52);
  const uint32_t prev_write = g_ring_prev_write.exchange(write, std::memory_order_relaxed);
  const uint32_t prev_end = g_ring_prev_end.exchange(end, std::memory_order_relaxed);
  const uint64_t prev_tid = g_ring_prev_tid.exchange(tid, std::memory_order_relaxed);
  const uint32_t prev_dev = g_ring_dev.exchange(dev, std::memory_order_relaxed);

  if (!g_ring_first_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("d3d-ring: first flush dev={:08X} write={:08X} end={:08X} bank={} tid={:X}", dev,
                write, end, bank, tid);
  }

  // The one fact that decides "two guest threads share the ring" versus "one
  // thread computed a bad pointer". Once is enough - it is a property of the
  // session, not of the moment.
  if (prev_tid != 0 && prev_tid != tid && prev_dev == dev &&
      !g_ring_multi_thread_logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN(
        "d3d-ring: SECOND THREAD on device {:08X} - this tid={:X} previous tid={:X} "
        "(write={:08X} prev_write={:08X})",
        dev, tid, prev_tid, write, prev_write);
  }

  // Only genuinely impossible values. `write > end` is NOT one of them: 0x34 is
  // a watermark the guest crosses in normal operation.
  const bool bad = write < 0x10000 || end < 0x10000 || (write & 3u) != 0 || (end & 3u) != 0;
  if (!bad) {
    return;
  }
  // Rate-limited: once the ring is wrong every subsequent flush is wrong too.
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  const int64_t last = g_ring_last_bad_ns.load(std::memory_order_relaxed);
  if (last != 0 && now_ns - last < 1'000'000'000) {
    return;
  }
  g_ring_last_bad_ns.store(now_ns, std::memory_order_relaxed);
  REXLOG_ERROR(
      "d3d-ring: CORRUPT dev={:08X} write={:08X} end={:08X} | prev dev={:08X} write={:08X} "
      "end={:08X} tid={:X} | mask={:016X} bank={} shadow={:08X} tid={:X}",
      dev, write, end, prev_dev, prev_write, prev_end, prev_tid, mask, bank, shadow, tid);
}


void OnRenderMesh(uint8_t* base, uint32_t mesh_context, uint32_t vertex_program_state,
                  bool drew_inside) {
  const uint32_t dyn = skate3::native_scene::CaptureDynamicState(
      base, mesh_context, /*world_path=*/false, drew_inside);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({0, mesh_context, vertex_program_state, dyn});
}


// SceneRenderView draw-list renderer sub_827FAF50(view, sort_vec, first, count):
// sort_vec points at an eastl vector whose [0] is the entry array; entries are
// 8 bytes {u32 sort_key, MeshContext*}.
//
// Skinned entries and rigid MODEL-SPACE props (vending machines and other
// movables that never reach RenderMesh) are captured HERE, before the
// dispatcher draws the list. The captures are transform-pending; the
// post-draw fixup attaches the palette / world matrix at whichever draw
// eventually consumes the mesh's buffers.
void OnSceneDrawList(uint8_t* base, uint32_t view, uint32_t sort_vec, uint32_t first,
                     uint32_t count) {
  if (count == 0 || count > 100000) {
    return;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return;
  }
  // b = which of the view's sort lists this came from (sort_vec - view), so
  // the scene builder can select the primary opaque list (+20160).
  const uint32_t list_offset = sort_vec - view;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = entries + (first + i) * 8;
    const uint32_t mesh_context = REX_LOAD_U32(entry + 4);
    if (mesh_context == 0) {
      continue;
    }
    g_current_frame.push_back({1, mesh_context, list_offset, view});
    const uint32_t dyn =
        skate3::native_scene::CaptureDynamicState(base, mesh_context, /*world_path=*/true);
    if (dyn != 0) {
      // b = the submitting view: shadow-cascade views submit their own
      // contexts for the same NPCs; rendering those creates ghost
      // duplicates (torso-less: their deferred skin passes never run).
      g_current_frame.push_back({2, mesh_context, view, dyn});
    }
  }
}

// ---- Guest-side occlusion dispatch filter ---------------------------------
// The native renderer suppresses the guest's emulated draws, so for world
// statics the sorted-list dispatch below (material setup, command-packet
// building) produces nothing anyone consumes - yet it is the guest render
// thread's dominant per-item cost. For MeshContexts the render-side
// occlusion cull proved hidden last frame (published by RenderScene), the
// hook compacts the entry segment before invoking the guest dispatcher and
// restores it afterwards. Capture above has already recorded every entry,
// so scene.items, the shadow caster caches, and all state capture stay
// complete; the only guest work skipped is packet-building for draws that
// were both suppressed and occlusion-culled anyway. Guest render thread
// only.
struct OcclDispatchFilter {
  uint64_t stamp = ~0ull;          // g_frame_index of the ctx snapshot
  std::vector<uint32_t> ctxs;      // sorted culled-ctx snapshot
  std::vector<uint8_t> saved;      // original segment bytes for restore
  uint32_t saved_addr = 0;
  uint32_t saved_bytes = 0;
  bool active = false;             // a filtered dispatch is in flight
};
OcclDispatchFilter g_occl_filter;

// Compacts entries[first..first+count) in place, dropping culled ctxs, and
// returns the kept count. Returns `count` unchanged (nothing saved) when
// filtering is off, stale, empty, or nothing matched.
uint32_t FilterSceneDrawList(uint8_t* base, uint32_t sort_vec, uint32_t first,
                             uint32_t count) {
  OcclDispatchFilter& f = g_occl_filter;
  if (f.active || count == 0 || count > 100000) {
    return count;
  }

  // Whole-list throttle. The reasoning is the same as the per-item cull above,
  // taken to its conclusion: if nothing consumes the packets this dispatch
  // builds, a machine that cannot afford to build them can build them less
  // often. Dropping every entry is exactly the path the cull already takes
  // when it happens to prove them all hidden, so there is no new mechanism
  // here - only a different reason to reach it.
  //
  // Capture ran before this, every frame, so the native renderer still draws
  // the complete world; what is skipped is guest work whose output is thrown
  // away. Frame 0 of each period always dispatches, so anything that depends
  // on the guest walking its own list still happens regularly.
  if (const int32_t period = REXCVAR_GET(skate3_native_render_guest_static_refresh);
      period > 1 && (g_frame_index % uint64_t(period)) != 0) {
    skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
    const uint32_t entries = REX_LOAD_U32(sort_vec);
    if (entries != 0) {
      const uint32_t seg = entries + first * 8;
      // Saved and restored like the cull's own path: the guest's list must be
      // exactly as it left it, even though nothing here rewrites it.
      f.saved.assign(base + seg, base + seg + size_t(count) * 8);
      f.active = true;
      f.saved_addr = seg;
      f.saved_bytes = count * 8;
      skate3::native_scene::AddGuestOcclSkipped(count);
      return 0;
    }
  }

  if (!REXCVAR_GET(skate3_native_render_scene_occlusion_cull_guest)) {
    return count;
  }
  if (f.stamp != g_frame_index) {
    f.stamp = g_frame_index;
    skate3::native_scene::CopyOcclusionCulledCtxs(f.ctxs);
  }
  if (f.ctxs.empty()) {
    return count;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  const uint32_t entries = REX_LOAD_U32(sort_vec);
  if (entries == 0) {
    return count;
  }
  const uint32_t seg = entries + first * 8;
  f.saved.assign(base + seg, base + seg + size_t(count) * 8);
  uint32_t kept = 0;
  uint32_t skipped = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* src = f.saved.data() + size_t(i) * 8;
    uint32_t ctx_be;
    std::memcpy(&ctx_be, src + 4, 4);
    const uint32_t mesh_context = __builtin_bswap32(ctx_be);
    if (mesh_context != 0 &&
        std::binary_search(f.ctxs.begin(), f.ctxs.end(), mesh_context)) {
      ++skipped;
      continue;
    }
    if (kept != i) {
      std::memcpy(base + seg + size_t(kept) * 8, src, 8);
    }
    ++kept;
  }
  if (skipped == 0) {
    f.saved.clear();
    return count;
  }
  f.active = true;
  f.saved_addr = seg;
  f.saved_bytes = count * 8;
  skate3::native_scene::AddGuestOcclSkipped(skipped);
  return kept;
}

void RestoreSceneDrawList(uint8_t* base) {
  OcclDispatchFilter& f = g_occl_filter;
  if (!f.active) {
    return;
  }
  std::memcpy(base + f.saved_addr, f.saved.data(), f.saved_bytes);
  f.active = false;
}

// Non-indexed cloth patch draws (see native_scene::CaptureClothDraw). The
// synthetic "context" key is the dynamic buffer object, stable per garment.
void OnClothDraw(uint8_t* base, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
  uint32_t key = 0;
  const uint32_t dyn = skate3::native_scene::CaptureClothDraw(base, r4, r5, r6, r7, &key);
  if (dyn == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_frame.push_back({3, key, 0, dyn});
}

// Precise guest frame pacing (see skate3_guest_fps_cap): called on the guest
// render thread at the swap boundary. Absolute-schedule pacing (target +=
// interval) so sleep jitter never accumulates; resyncs when the guest falls
// more than one interval behind (loads, hitches).
void PaceGuestFrame() {
  double cap = REXCVAR_GET(skate3_guest_fps_cap);
  if (REXCVAR_GET(skate3_guest_fps_cap_auto)) {
    // Refresh-derived cap (see the cvar). Falls through to the explicit cap
    // while the platform hasn't reported a refresh rate.
    const double auto_cap = double(rex::ui::Window::AutoFrameCapHz(
        rex::ui::Window::CachedDisplayRefreshHz()));
    if (auto_cap > 0.0) {
      cap = auto_cap;
    }
  }
  // What the pacer is actually doing, logged whenever it changes - so a
  // report carries its own pacing configuration, and so flipping Framerate
  // Cap in the settings menu leaves both configurations in the same log with
  // the [pace] lines around them. Reading this against the display rate is
  // what distinguishes "the emulation is slow" from "the cap does not match
  // what the display will present" - see display_presentable_refresh_cap_hz.
  {
    static double s_logged_cap = -1.0;
    if (cap != s_logged_cap) {
      s_logged_cap = cap;
      const float refresh_hz = rex::ui::Window::CachedDisplayRefreshHz();
      // WARN, not INFO. The phone builds ship at log_level=warn, and this is
      // the only line that says whether the Framerate Cap row did anything at
      // all - which is the first question asked of every report about it. It
      // fires once per change, so it costs nothing to leave visible.
      if (cap >= 1.0) {
        REXLOG_WARN("[pace] guest frame cap is now {:.0f} fps (auto={}, display presents at {:.0f} Hz)",
                    cap, REXCVAR_GET(skate3_guest_fps_cap_auto) ? "on" : "off", refresh_hz);
      } else {
        REXLOG_WARN("[pace] guest frame cap is now OFF (auto={}, display presents at {:.0f} Hz)",
                    REXCVAR_GET(skate3_guest_fps_cap_auto) ? "on" : "off", refresh_hz);
      }
    }
  }
  static std::chrono::steady_clock::time_point s_next{};
  if (cap < 1.0) {
    s_next = {};
    return;
  }
  const auto interval =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / cap));
  const auto now = std::chrono::steady_clock::now();
  if (s_next.time_since_epoch().count() == 0 || now > s_next + interval) {
    s_next = now + interval;
    return;
  }
  // Sleep to the deadline; spin only the last few hundred microseconds.
  //
  // The spin window was 2 ms against a 16.6 ms period - an eighth of every
  // frame spent in a yield loop on the guest render thread. A simpleperf
  // profile of that thread during gameplay put `sched_yield` at 14.4% of its
  // cycles, all of it from here, while a Perfetto trace of the same session
  // showed the emulated command processor RUNNABLE WITH NO CORE for 7.8 of 50
  // seconds. The two facts are the same fact: this loop holds a big core to do
  // nothing while the thread the next frame waits on is queued behind it.
  //
  // Both the old code and this one sleep once and then spin the tail, so the
  // only question is how long the tail has to be. Android sets a 50 us timer
  // slack per thread by default and a sleep of a few milliseconds lands well
  // inside a few hundred microseconds of its deadline, so 2000 us of runway
  // was never buying accuracy - it was 2000 us of yielding. And the accuracy
  // barely matters here: under vsync the presenter blocks on the panel anyway,
  // and this cap exists to stop the guest running AHEAD of the display, not to
  // hit a deadline to the microsecond. Raise the cvar on a device whose
  // scheduler wakes threads late.
  const auto spin_window =
      std::chrono::microseconds(REXCVAR_GET(skate3_guest_fps_cap_spin_us));
  const auto wake_at = s_next - spin_window;
  if (std::chrono::steady_clock::now() < wake_at) {
    std::this_thread::sleep_until(wake_at);
  }
  if (spin_window.count() > 0) {
    while (std::chrono::steady_clock::now() < s_next) {
      std::this_thread::yield();
    }
  }
  s_next += interval;
}

// Delivered-frame pacing summary. The guest render thread is the only caller,
// so the window state needs no synchronization. This measures what the player
// actually sees - the interval between swaps - rather than the cost of any one
// subsystem, which is what makes it the number to compare across builds.
void ReportPacing() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_prev{};
  static Clock::time_point s_window_start{};
  static std::vector<double> s_intervals_ms;
  // Guest ticks at the top of the window, so the line can also say how fast
  // the GUEST's clock is running. See where it is printed.
  static uint64_t s_window_start_guest_ticks = 0;

  // This used to run unconditionally, which was defensible while the line it
  // produces was always printed. It is not defensible now that the shipped log
  // level is warn and [pace] writes at info: without this gate every player
  // would time every frame, grow a 1800-entry vector, and sort a copy of it
  // every thirty seconds, to format a string that is then dropped.
  // The perf log is enough to ask for pacing: this line is the only measure
  // of SMOOTHNESS the port has, and skate3_diagnostics also raises the whole
  // process to info - which on this console turns on the guest driver's own
  // printing and changes the very intervals being sampled. The sampling here
  // is a double per frame and one sort per thirty seconds.
  if (!REXCVAR_GET(skate3_diagnostics) &&
      !REXCVAR_GET(skate3_native_render_scene_perf_log)) {
    if (s_prev.time_since_epoch().count() != 0) {
      // Drop the window rather than keep it: the next sample after the switch
      // is flipped back on would otherwise be an interval spanning however long
      // diagnostics were off, and land in p95 as a hitch that never happened.
      s_prev = {};
      s_intervals_ms.clear();
      s_intervals_ms.shrink_to_fit();
    }
    return;
  }

  const auto now = Clock::now();
  if (s_prev.time_since_epoch().count() == 0) {
    s_prev = now;
    s_window_start = now;
    s_window_start_guest_ticks = rex::chrono::Clock::QueryGuestTickCount();
    s_intervals_ms.reserve(4096);
    return;
  }
  s_intervals_ms.push_back(std::chrono::duration<double, std::milli>(now - s_prev).count());
  s_prev = now;

  const auto elapsed = now - s_window_start;
  if (elapsed < std::chrono::seconds(30) || s_intervals_ms.empty()) {
    return;
  }
  std::vector<double> sorted = s_intervals_ms;
  std::sort(sorted.begin(), sorted.end());
  const auto pct = [&sorted](double p) {
    const size_t i = std::min(sorted.size() - 1,
                              size_t(p * double(sorted.size() - 1) + 0.5));
    return sorted[i];
  };
  const double secs = std::chrono::duration<double>(elapsed).count();
  // How fast the GUEST's clock ran over the same window, as a multiple of real
  // time. The guest timebase is mftb, which is QueryGuestTickCount at a fixed
  // 50 MHz, and it is what the title's own frame delta is computed from - so
  // this is the difference between "the game is slow" and "the game thinks
  // less time passed than did". 1.00 means the clock is honest and a
  // half-speed game is the title's own timestep, not ours; anything else is
  // a clock bug and is the whole answer. Free: two reads per thirty seconds.
  const uint64_t guest_ticks_now = rex::chrono::Clock::QueryGuestTickCount();
  const double guest_secs =
      double(guest_ticks_now - s_window_start_guest_ticks) /
      double(rex::chrono::Clock::guest_tick_frequency());
  // Warn: one line per thirty seconds, and the only measure of SMOOTHNESS
  // this port has - p50 against p95 against max is the difference between
  // "slow" and "juddering". At info it has never once reached a Switch log,
  // because raising that console to info turns on the guest driver's own
  // printing and changes the pacing being measured.
  // clamp= is executions of the guest's own per-frame time clamp over the same
  // window (see skate3_frame_advance.cpp). Compare it to frames: about one per
  // frame means that code IS the game's frame timing and patching it is worth
  // doing; ZERO means it never runs and every theory built on it is wrong.
  REXLOG_WARN(
      "[pace] {:.0f}s: frames={} fps={:.1f} p50={:.1f}ms p95={:.1f}ms max={:.1f}ms "
      "guest_clock={:.3f}x clamp={}",
      secs, sorted.size(), double(sorted.size()) / secs, pct(0.50), pct(0.95), sorted.back(),
      secs > 0.0 ? guest_secs / secs : 0.0, skate3::frame_advance::TakeClampCalls());
  s_intervals_ms.clear();
  s_window_start = now;
  s_window_start_guest_ticks = guest_ticks_now;
}

void OnFrameEnd(uint8_t* base) {
  // Everything this thread allocates outside BuildFrameScene is the GAME's own
  // per-frame code, and it was landing in kOther with nothing to say about it.
  // Set here because this is the one function guaranteed to run on the guest
  // render thread every frame; one thread-local byte store.
  skate3::alloc_counter::SetThreadDefaultPhase(
      skate3::alloc_counter::Phase::kGuestThread);
  PaceGuestFrame();
  ReportPacing();
  // EMULATED-mode guest frame breakdown (emulated gameplay once regressed
  // from 140 to 66 fps while native stayed at cap; the native-scene perf
  // line only prints while the native renderer is active, so emulated
  // stretches had no perf visibility at all). Logs every
  // ~600 guest frames while the native scene is OFF: whole-frame time (the
  // fps), time spent inside BuildFrameScene, and the per-frame draw/upload
  // hook traffic. If frame_avg is ~15 ms but build/hooks are small, the
  // cost is in the emulated GPU/CP pipeline, not our guest-thread code.
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_bd_prev{};
  static uint64_t s_bd_frames = 0;
  static double s_bd_frame_ns = 0, s_bd_frame_max = 0;
  static double s_bd_build_ns = 0, s_bd_build_max = 0;
  static uint64_t s_bd_draws0 = 0;
  const bool emu_profile = !skate3::native_scene::Enabled() &&
                           REXCVAR_GET(skate3_native_render_scene_perf_log);
  const auto bd_now = Clock::now();
  if (emu_profile && s_bd_prev.time_since_epoch().count() != 0) {
    const double dt =
        std::chrono::duration<double, std::nano>(bd_now - s_bd_prev).count();
    s_bd_frame_ns += dt;
    s_bd_frame_max = std::max(s_bd_frame_max, dt);
  }
  s_bd_prev = bd_now;

  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_frame_index;
  const size_t mesh_count = g_current_frame.size();

  const auto bd_build0 = Clock::now();
  skate3::native_scene::BuildFrameScene(base, g_current_frame.data(),
                                        g_current_frame.size());
  if (emu_profile) {
    const double bns = std::chrono::duration<double, std::nano>(Clock::now() -
                                                                bd_build0)
                           .count();
    s_bd_build_ns += bns;
    s_bd_build_max = std::max(s_bd_build_max, bns);
    if (++s_bd_frames >= 600) {
      const uint64_t draws = skate3::native_scene::DrawSequence();
      // Quantum probe: a measured 1 ms sleep + the kernel's current timer
      // resolution. frame avg ~15 ms with build ~0 and the GPU at idle
      // wattage is the signature of every pacing sleep rounding up to the
      // 15.625 ms default Windows quantum; sleep1ms ~15.6 here proves it,
      // ~1-2 ms refutes it.
      const auto sp0 = Clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const double sleep_ms =
          std::chrono::duration<double, std::milli>(Clock::now() - sp0).count();
      double timer_res_ms = -1.0;
#if defined(_WIN32)
      {
        using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
        static const auto query = reinterpret_cast<NtQueryTimerResolutionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
        if (query != nullptr) {
          ULONG min_res = 0, max_res = 0, cur_res = 0;
          if (query(&min_res, &max_res, &cur_res) == 0) {
            timer_res_ms = double(cur_res) / 10000.0;  // 100 ns units
          }
        }
      }
#endif
      REXLOG_INFO(
          "native-render EMULATED breakdown: frame avg={:.2f}/max={:.2f}ms "
          "build avg={:.3f}/max={:.3f}ms draws/frame={} sleep1ms={:.2f}ms "
          "timer_res={:.2f}ms (600-frame window)",
          s_bd_frame_ns / s_bd_frames / 1e6, s_bd_frame_max / 1e6,
          s_bd_build_ns / s_bd_frames / 1e6, s_bd_build_max / 1e6,
          (draws - s_bd_draws0) / s_bd_frames, sleep_ms, timer_res_ms);
      s_bd_frames = 0;
      s_bd_frame_ns = s_bd_frame_max = s_bd_build_ns = s_bd_build_max = 0;
      s_bd_draws0 = draws;
    }
  } else {
    s_bd_frames = 0;
    s_bd_frame_ns = s_bd_frame_max = s_bd_build_ns = s_bd_build_max = 0;
    s_bd_draws0 = skate3::native_scene::DrawSequence();
  }

  const int32_t log_interval = REXCVAR_GET(skate3_native_render_log_interval);
  if (log_interval > 0 && g_frame_index % static_cast<uint64_t>(log_interval) == 0) {
    REXLOG_INFO("native-render frame={} meshes={} snapshot_done={}", g_frame_index,
                mesh_count, skate3::native_scene::SnapshotWritten());
  }

  skate3::native_scene::OnCaptureFrameEnd(base, g_frame_index,
                                           g_current_frame);

  g_current_frame.clear();
}

}  // namespace

void Install() {
  if (!Enabled()) {
    return;
  }
  if (!g_announced.exchange(true)) {
    REXLOG_INFO(
        "native-render hook layer enabled");
  }
  skate3::native_scene::Install();
}

}  // namespace skate3::native_render

// "RenderMesh" per-visible-mesh submission for dynamic entities (characters,
// props). Actual convention (verified via recompiled code + snapshot):
// r3 = MeshContext*, r4 = renderengine::VertexProgramState*.
//
// The dynamic state snapshot must be taken AFTER the original call: the
// mesh's VS constants (instance world matrix, bone palette) are only flushed
// through D3D::SetPending_AluConstants from inside DrawIndexedVertices
// (sub_82B7AD68), i.e. during the RenderMesh body. Capturing on entry reads
// the PREVIOUS draw's constants and renders every dynamic entity with the
// previous entity's transform/palette.
// Deferred (multi-pass) meshes draw nothing inside the call, detected via
// the draw sequence counter so their transforms are left for the post-draw
// fixup instead of being read from a stale constant bank.
// LivingWorld census spawners: (this, spawn request) -> entity, or 0 for
// "nothing spawned".
//
// Returning 0 is the game's OWN result when a census declines to spawn - it is
// what Free Skate at population level 0 produces - so every caller already
// handles it. That is why this is a spawn hook and not a draw filter: an
// entity that never exists costs no collision, no voice, no engine noise, no
// hair and no LivingWorld update slot, and on a phone that CPU cost is the
// larger half of what a crowd is worth. Entities that already exist are left
// alone and walk off by themselves.
//
// The values are latched at boot (see AmbientNpcsAtBoot), so a world cannot be
// half-populated by a mid-session toggle.
extern "C" REX_FUNC(sub_82E22F30) {  // LWPedestrianCensusMan
  if (!skate3::native_scene::AmbientNpcsAtBoot()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82E22F30(ctx, base);
}

extern "C" REX_FUNC(sub_82C36300) {  // vehicle census
  if (!skate3::native_scene::AmbientNpcsAtBoot()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82C36300(ctx, base);
}

extern "C" REX_FUNC(sub_82C4D440) {  // movable street props
  if (!skate3::native_scene::MovablePropsAtBoot()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82C4D440(ctx, base);
}

extern "C" REX_FUNC(sub_82795AD8) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t mesh_context = ctx.r3.u32;
  const uint32_t vertex_program_state = ctx.r4.u32;
  const uint64_t draws_before = skate3::native_scene::DrawSequence();
  __imp__sub_82795AD8(ctx, base);
  if (enabled) {
    const bool drew_inside = skate3::native_scene::DrawSequence() != draws_before;
    skate3::native_render::OnRenderMesh(base, mesh_context, vertex_program_state,
                                        drew_inside);
  }
}

// SceneRenderView sorted draw-list renderer (world geometry):
// sub_827FAF50(r3 = SceneRenderView*, r4 = eastl vector of 8-byte
// {sort_key, MeshContext*} entries, r5 = first, r6 = count). Called from
// SceneRenderView::Render (82 7FB158) for each of the view's key lists.
extern "C" REX_FUNC(sub_827FAF50) {
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnSceneDrawList(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                           ctx.r6.u32);
    // Guest-side occlusion dispatch filter: capture above saw every entry;
    // the dispatcher only gets the ones the occlusion cull did not prove
    // hidden. The segment is restored after the call (the list object
    // outlives this dispatch).
    const uint32_t kept = skate3::native_render::FilterSceneDrawList(
        base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (kept != ctx.r6.u32) {
      ctx.r6.u64 = kept;
      __imp__sub_827FAF50(ctx, base);
      skate3::native_render::RestoreSceneDrawList(base);
      return;
    }
  }
  __imp__sub_827FAF50(ctx, base);
}


namespace {
// Frame time accounting, read by LogFrameBudget below.
std::atomic<uint64_t> g_frame_outside_swap_us{0};
std::atomic<uint64_t> g_frame_hooks_us{0};
std::atomic<uint64_t> g_frame_inner_us{0};
std::atomic<uint64_t> g_frame_count{0};
}  // namespace

// Guest D3D Swap: frame boundary.
//
// Also where the frame time is measured. The guest render thread completes one
// frame a second on this port while the command processor happily executes 70
// batches a second and the graphics submission itself accounts for 30 ms of
// that second - so the time is inside the guest's own swap, and this says how
// much of it. Split three ways: the runtime's swap handling, whatever the hook
// layer does at frame end, and the rest of the guest's frame.
extern "C" REX_FUNC(sub_82B82E08) {
  const auto swap_enter = std::chrono::steady_clock::now();
  {
    static std::atomic<int64_t> last_exit_ns{0};
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(swap_enter.time_since_epoch()).count();
    // Swap ENTRY to swap ENTRY, so this is the whole frame period - including
    // the time inside swap, which is reported separately below. It is not the
    // time outside swap, whatever the name suggests, and reading it as a
    // sibling of the swap figure rather than its parent will have you looking
    // for a frame's worth of work that is not there.
    const int64_t last = last_exit_ns.exchange(now_ns, std::memory_order_relaxed);
    if (last != 0) {
      g_frame_outside_swap_us.fetch_add(uint64_t((now_ns - last) / 1000), std::memory_order_relaxed);
    }
  }
  // Install point for the guest fault reporter: this runs on the guest render
  // thread every frame whatever else is switched off, and by the first Swap the
  // runtime's own fault handlers (MMIO write watches, the guarded-read
  // recovery armed by the capture hooks earlier in the same frame) have all
  // registered - so the reporter lands LAST on the chain and they keep first
  // refusal. Idempotent; not gated on Enabled() so a --no-skate3_native_render
  // session still reports its crashes.
  skate3::crash_report::EnsureInstalled(base);
  // Liveness for the hang watchdog: this is the guest's own frame boundary, so
  // it stops exactly when the game stops producing frames.
  skate3::crash_report::Heartbeat();
  // Drain the static-image write watch's trap records (logged here, off the
  // fault handler) and re-arm its hot pages for the next frame.
  skate3::image_watch::FlushPending();
  // Publish the guest base for the level picker, which reads the frontend's
  // selection cursor so it can walk menus with feedback instead of pressing
  // 'down' a fixed number of times.
  skate3::SetLoaderGuestBase(base);
  if (skate3::native_render::Enabled()) {
    skate3::native_render::OnFrameEnd(base);
  }
  const auto hooks_done = std::chrono::steady_clock::now();
  g_frame_hooks_us.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(hooks_done - swap_enter)
                   .count()),
      std::memory_order_relaxed);
  g_frame_count.fetch_add(1, std::memory_order_relaxed);
  const auto inner_begin = std::chrono::steady_clock::now();
  __imp__sub_82B82E08(ctx, base);
  g_frame_inner_us.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - inner_begin)
                   .count()),
      std::memory_order_relaxed);
}

// cProcessArenaAsset::RegisterTexture(cAssetList*, cAssetID,
// renderengine::Texture*, rw::Resource&): r4 = 64-bit asset guid,
// r5 = texture object.
extern "C" REX_FUNC(sub_82C9A618) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnRegisterTexture(ctx.r4.u64, ctx.r5.u32);
  }
  __imp__sub_82C9A618(ctx, base);
}

// ---- palette snapshot hooks (native/skate3_native_palette.h). Each fires
// POST-call, when m_matrices holds the settled packed upload palette.

// cModelInstance::PackAndMultiplyMatricesForUpload(Matrix44& localToWorld):
// post-call, m_matrices (+0x14) holds exactly the packed 4x3 column-major
// upload palette the VS consumes. r3 = the cModelInstance.
extern "C" REX_FUNC(sub_827E5B30) {
  const uint32_t instance = ctx.r3.u32;
  __imp__sub_827E5B30(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnPackPalette(base, instance);
  }
}

// Sk8::UpdateBoneTransforms(cModelInstance* parts, uint count, Matrix44*
// srcPalette, uint** remaps, Matrix44& out): r3 is an ARRAY of
// cModelInstance part records (stride 0x28): the player skater's per-piece
// palette source (its parts never reach the pack function above).
extern "C" REX_FUNC(sub_827A52C8) {
  const uint32_t parts = ctx.r3.u32;
  const uint32_t count = ctx.r4.u32;
  __imp__sub_827A52C8(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnBoneTransforms(base, parts, count);
  }
}

// Sk8::cLivingWorldPresEntity::Update: post-call, this+528 holds the
// entity's evaluated spawn/distance fade opacity (x = alpha), this+16 the
// current LOD index. Feeds the LW entity store (per-instance ctx ->
// alpha/identity, the serving path).
extern "C" REX_FUNC(sub_827C1188) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1188(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_lw::OnLwEntityTick(base, entity);
  }
}

// Sk8::SkaterPresEntity::StartJobs, bracketed as a pack owner:
// UpdateBoneTransforms calls inside stamp their snapshots with this entity.
extern "C" REX_FUNC(sub_827825B0) {
  const uint32_t prev_owner = skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_827825B0(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Sk8::SkaterPresEntity::DoubleBuffer(garment index): runs once per
// garment per COMPLETED cloth sim tick (the job's output memcpy + buffer
// flip). The identity store's deformed-VB freshness signal: garment-table
// fields persist after the sim stops, so this is the only per-tick proof
// the garment is really CPU-simulated right now.
extern "C" REX_FUNC(sub_82783038) {
  const uint32_t skater = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  __imp__sub_82783038(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnRopaDoubleBuffer(base, skater, index);
  }
}

// Sk8::SkaterPresEntity::EndJobs: calls PackAndMultiplyMatricesForUpload
// per instance with the tick's final locomotion (the body pose of record).
// Bracketed as a pack owner so those snapshots carry the entity.
extern "C" REX_FUNC(sub_82782818) {
  const uint32_t prev_owner =
      skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_82782818(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Skater-family virtual UBT+Pack driver (vtable slot +16): the remaining
// palette-write path (UpdateBoneTransforms + PackAndMultiply outside the
// StartJobs/EndJobs pair). Bracketed as a pack owner.
extern "C" REX_FUNC(sub_82785778) {
  const uint32_t prev_owner =
      skate3::native_palette::ExchangePackOwner(ctx.r3.u32);
  __imp__sub_82785778(ctx, base);
  skate3::native_palette::ExchangePackOwner(prev_owner);
}

// Sk8::RenderPresentation::AddEntityToRenderViews / RmvEntityFrmRenderViews
// - r4 is the PresentationEntity being (de)registered: the identity store's
// scene-membership/lifetime signal (native/skate3_native_entity.h).
extern "C" REX_FUNC(sub_827A6C50) {
  const uint32_t entity = ctx.r4.u32;
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnEntityViewAdd(entity);
  }
  __imp__sub_827A6C50(ctx, base);
}

extern "C" REX_FUNC(sub_827A6CE8) {
  const uint32_t entity = ctx.r4.u32;
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnEntityViewRemove(entity);
  }
  __imp__sub_827A6CE8(ctx, base);
}

// Sk8::PresentationEntity::BindConstants (base override): post-call, every
// MeshContext of the entity's current LOD has just received its constant
// param POINTERS (world hash 0x7DDF552B -> &entity+416, etc.; the game
// binds by pointer and dereferences at draw). Every subclass bind calls
// this base, so the exit sees every renderable presentation entity with
// its full ctx set: the ctx -> entity identity write point.
extern "C" REX_FUNC(sub_827A6658) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827A6658(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindConstants(base, entity);
  }
}

// Derived BindConstants overrides: class tags for the identity store. The
// most-derived override returns last (each calls its parent first), so the
// last tag to land is the entity's concrete class.
extern "C" REX_FUNC(sub_82783D68) {  // Sk8::SkaterPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82783D68(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kSkater);
  }
}

extern "C" REX_FUNC(sub_82785260) {  // ColorizedSkaterPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82785260(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kColorized);
  }
}

extern "C" REX_FUNC(sub_82793F70) {  // CACPresEntity (untransposed world)
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82793F70(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kCac);
  }
}

extern "C" REX_FUNC(sub_82785528) {  // unnamed skater-layout class
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_82785528(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kSkaterAux);
  }
}

extern "C" REX_FUNC(sub_827C1720) {  // cLivingWorldPresEntity
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1720(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_entity::OnBindClass(
        entity, skate3::native_entity::EntClass::kLivingWorld);
  }
}

// pegasus::tRModelData::Fixup(void* model, rw::core::arena::ArenaIterator*)
// - the rw-arena LOAD-time pointer resolve, fired once per model while its
// arena streams in. (The `Unfix(void*, SizeAndAlignment*)` atoms are the
// SAVE/size path; hooking tROptiMeshData::Unfix never fired during loads.)
// Post-call the model's mesh table is live: queue its meshes for the
// prewarm decode workers. This is the EARLY prewarm source; it fires
// throughout the load's disk-streaming phase, hours of decode headroom
// before the final-seconds AddRenderInstance activation burst. The atoms
// dispatch indirectly through the recomp function table, which resolves to
// this override at link time like any other reference.
extern "C" REX_FUNC(sub_82963510) {
  const uint32_t model = ctx.r3.u32;
  __imp__sub_82963510(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnModelFixup(base, model);
  }
}

// Sk8::Challenge::PhotoReplayController::Update(float): runs once per guest
// frame while a photo-mission's photo editor is up (pick-a-photo +
// depth-of-field / saturation / brightness / contrast controls). Heartbeat
// for the native scene's photo-editor yield: the editor's effects are the
// game's own postfx chain, which only the emulated path executes. Reached
// via the recomp function table (virtual dispatch), so the override fires
// like any direct call.
extern "C" REX_FUNC(sub_825623F0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoReplayUpdate();
  }
  __imp__sub_825623F0(ctx, base);
}

// Sk8::FE::FrontEndState_Replay2::TakePhoto(): fires once when the player
// takes a photo (replay editor / photo mission Select). The game then
// renders the shot into the 1152x640 PostFX screenshot target, resolves it,
// and ScreenshotBackEnd::GrabScreenshot CPU-reads the resolved guest memory
// to JPEG-encode it, which is all zeros unless resolve readback is forced
// (the invisible-final-photo bug: an F11 capture showed the grab texture
// 0x04911000 memory fully zero). Arms the photo-grab readback window.
extern "C" REX_FUNC(sub_826147C8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnTakePhoto();
  }
  __imp__sub_826147C8(ctx, base);
}

// Sk8::BE::ScreenshotBackEnd::GrabScreenshot(bool), the actual grab: the
// game CPU-reads the resolved screenshot target from guest memory and
// JPEG-encodes it. Fires in EVERY grab flow; the photo-mission Select
// confirm does NOT go through FrontEndState_Replay2::TakePhoto (a logged
// full photo-mission run never opened the card compose window), so this is
// the canonical shutter heartbeat. Right after the grab
// the FE composes the framed display card (white border / caption / logo)
// over the JPEG texture in a ONE-SHOT RTT pass; the card compose window
// (UpdatePhotoGrabWindow) keys off this timestamp so that pass executes and
// its resolve lands in CPU guest memory for the native 2D decoder.
extern "C" REX_FUNC(sub_824FD550) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnTakePhoto();
  }
  __imp__sub_824FD550(ctx, base);
  // Post-call: the grab has CPU-read the shot; the card compose that
  // follows reads already-copied memory. Event-closes the shutter burst.
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoGrabDone();
  }
}

// Sk8::BE::ScreenshotBackEnd grab REQUEST (sets the grab params + arms the
// request flag; called by the FE photo flows 1-2 frames BEFORE the game
// renders the shot + card-composite frame sequence whose resolves
// GrabScreenshot/OnScreenShot then CPU-read). This is the only PREDICTIVE
// shutter signal; the GrabScreenshot hook above fires AFTER those frames
// already rendered (suppressed). Arms the shutter burst: suppression lifted
// + full small-resolve readbacks for ~1.5 s so the card-build's inputs are
// real (a logged control run showed the composite CPU copies land exactly
// in this window; every post-hoc window/bound missed them).
extern "C" REX_FUNC(sub_824FD4B0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnPhotoGrabRequest();
  }
  __imp__sub_824FD4B0(ctx, base);
}

namespace skate3::native_scene {
// Fault-guarded guest read (defined in skate3_native_scene.cpp).
bool GuestTryLoadU32(uint8_t* base, uint32_t addr, uint32_t* out);
}  // namespace skate3::native_scene

REXCVAR_DEFINE_BOOL(
    skate3_autoexposure_pin, true, "Skate 3",
    "Pin the game's auto-exposure at its per-zone maximum. The game "
    "adapts world exposure from a GPU luminance measurement it reads "
    "back from memory; with resolve readbacks disabled (the native "
    "renderer's standard configuration) that measurement reads empty and "
    "exposure settles at the maximum anyway - but windows that "
    "temporarily enable readbacks (the photo shutter) can leak one real "
    "measurement of a bright frame, after which the frozen measurement "
    "drags exposure to the minimum clamp permanently (the stuck world "
    "darkening after photo missions). Pinning removes the dependence on "
    "measurement availability. Turn OFF only when running with full "
    "resolve readbacks enabled, where live per-frame measurements make "
    "the game's own adaptation behave as on console.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// World auto-exposure evaluator (per frame): reads the GPU luminance
// measurement (sum / area of a resolved measurement surface), adapts the
// scene exposure toward the target luminance and clamps it to the
// per-zone [min, max] from the tuning block at [this+2468]+120..132
// (target, min, max, rate). Without resolve readbacks the measurement
// reads empty and exposure settles at the maximum; a readback window
// that briefly lets one bright measurement through (the photo shutter
// burst) leaves a frozen too-bright measurement that drags exposure to
// the minimum clamp permanently. The hook re-pins the adapted exposure
// (the evaluator's internal state and both published outputs, honoring
// the same ownership gates the game checks) to the per-zone maximum.
extern "C" REX_FUNC(sub_827F0D00) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_827F0D00(ctx, base);
  if (!REXCVAR_GET(skate3_autoexposure_pin) || self < 0x10000) {
    return;
  }
  uint32_t aux = 0, inst_a = 0, inst_b = 0, max_bits = 0;
  if (!skate3::native_scene::GuestTryLoadU32(base, self + 2468, &aux) ||
      aux < 0x10000 ||
      !skate3::native_scene::GuestTryLoadU32(base, aux + 128, &max_bits) ||
      !skate3::native_scene::GuestTryLoadU32(base, self + 2484, &inst_a) ||
      !skate3::native_scene::GuestTryLoadU32(base, self + 2500, &inst_b)) {
    return;
  }
  float max_expo;
  std::memcpy(&max_expo, &max_bits, sizeof(max_expo));
  if (!(max_expo > 0.0f) || max_expo > 16.0f) {
    return;
  }
  skate3::native_scene::GuestReadRecoveryScope guest_read_recovery(base);
  uint32_t prev_bits = REX_LOAD_U32(self + 3240);
  float prev;
  std::memcpy(&prev, &prev_bits, sizeof(prev));
  static std::atomic<int64_t> s_last_log_ns{-1};
  if (prev > 0.0f && prev < max_expo * 0.98f) {
    const int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const int64_t last = s_last_log_ns.load(std::memory_order_relaxed);
    if (last < 0 || now_ns - last > 60'000'000'000) {
      s_last_log_ns.store(now_ns, std::memory_order_relaxed);
      REXLOG_INFO(
          "native-render: auto-exposure re-pinned at zone max {:.3f} "
          "(adaptation had moved it to {:.3f}; luminance measurement "
          "unavailable without resolve readbacks)",
          max_expo, prev);
    }
  }
  REX_STORE_U32(self + 3240, max_bits);
  REX_STORE_U32(self + 3392, max_bits);
  const uint32_t own_a = REX_LOAD_U8(self + 3397);
  const uint32_t own_b = REX_LOAD_U8(self + 3396);
  if (own_a == 0 && inst_a >= 0x10000) {
    REX_STORE_U32(inst_a + 48, max_bits);
  }
  if (own_b == 0 && inst_b >= 0x10000) {
    REX_STORE_U32(inst_b + 56, max_bits);
  }
}

// The title's screen manager loop, and the reason the port sits where it does.
//
// The guest main thread enters sub_82965C90 during startup and never leaves:
// it switches on a state word at +96 and asks a loader object at +16 whether
// the screen it wants is ready. Everything else on the thread - the per-frame
// wait a thread dump catches it in - is the tail of this loop. So the dump
// looks identical whether the machine is advancing normally or stuck waiting
// for a screen that will never load, and the only thing that tells them apart
// is the state word itself.
//
// The loop is entered once, so this records where the object lives and lets a
// host thread read it. Nothing is written and nothing is inferred: the state,
// the current and pending screens, and the two callback slots, exactly as the
// guest left them.
namespace {

std::atomic<uint32_t> g_screen_mgr_obj{0};
std::atomic<uint8_t*> g_screen_mgr_base{nullptr};

// The counter the load is waiting for, and who is still ticking it.
//
// sub_82481A98 is the game's "advance this counter by one" helper, shared by
// every subsystem that publishes a heartbeat. Three of those heartbeats gate
// the end of a load; one of them stops. Watching the helper splits the two
// explanations that a frozen counter allows and that nothing else can tell
// apart: either the callers stopped calling, which is a fault in the game's
// own logic, or they are still calling and the increment is not landing,
// which would be a fault in how this port implements the atomic.
std::atomic<uint32_t> g_tick_watch_addr{0};
std::atomic<uint32_t> g_tick_watch_calls{0};
std::atomic<uint32_t> g_tick_watch_lr{0};

}  // namespace

// The job manager's "pick the next job" scan, and by a wide margin the most
// expensive thing the guest does on this port.
//
// A call trace of the front end put 75% of all guest cycles inside this one
// function: 9,447 calls burning 59.3 million cycles, about 6,278 each. It walks
// an array of [this+16] eight-byte entries hunting for the best candidate, so
// its cost is entirely the length of that list - and a list that grows because
// nothing is draining it would look exactly like the slowdown here, which
// starts fast and degrades. Recording the length says whether that is what is
// happening.
namespace {

std::atomic<uint64_t> g_jobscan_calls{0};
std::atomic<uint64_t> g_jobscan_entries{0};
std::atomic<uint32_t> g_jobscan_max{0};
constexpr size_t kJobCallerSlots = 6;
std::atomic<uint32_t> g_jobcaller_lr[kJobCallerSlots] = {};
std::atomic<uint64_t> g_jobcaller_hits[kJobCallerSlots] = {};
std::atomic<uint64_t> g_jobcaller_other{0};
std::atomic<uint64_t> g_jobwait_calls{0};
std::atomic<uint32_t> g_jobwait_timeout{0xFFFFFFFFu};
std::atomic<uint32_t> g_jobwait_handle{0};
std::atomic<uint32_t> g_jobmgr_vt100{0};
std::atomic<uint32_t> g_jobmgr_vt96{0};
std::atomic<uint64_t> g_jobscan_worked{0};
std::atomic<bool> g_jobscan_walked{false};
uint32_t g_jobscan_stack[7] = {0};
std::atomic<uint64_t> g_jobscan_yields{0};
std::atomic<uint64_t> g_jobscan_sleeps{0};

// The worker thread's own call. It is the one caller that already does the
// right thing - scan, and if there was nothing, take a real 1 ms kernel wait -
// so it must not be paced twice.
constexpr uint32_t kJobWorkerScanLr = 0x8290B788u;

// Consecutive empty scans on THIS thread. Per-thread because the whole point is
// how long this particular waiter has been getting nothing; a shared counter
// would let a busy thread reset a starving one.
thread_local uint32_t tl_jobscan_misses = 0;

// Give up the core after an empty scan.
//
// Three guest functions wait for a job by calling the scan in a loop with
// nothing else in the loop body - sub_82596DB8 (the main thread's per-frame
// flush), sub_824741F0 and sub_8290AE38. On the console a spinning thread cost
// its SMT sibling little and the loops were paced by instructions the
// recompiler cannot emit. Here each one holds a whole core, at the one priority
// where Horizon time-slices, against the workers that would end its wait.
void JobScanBackOff() {
#if defined(__SWITCH__)
  const int32_t mode = REXCVAR_GET(skate3_job_scan_backoff);
  if (mode <= 0) {
    return;
  }
  ++tl_jobscan_misses;
  if (mode == 1) {
    // Offer the core to a sibling already queued on it.
    svcSleepThread(0);
  } else if (mode == 2 ||
             tl_jobscan_misses <= uint32_t(REXCVAR_GET(skate3_job_scan_backoff_yields))) {
    // -1 is YieldType_WithCoreMigration: the kernel may pull a runnable thread
    // from another core's queue onto this one. That is the case that matters,
    // because the thread this waiter needs is on a different core by design.
    svcSleepThread(-1);
  } else {
    // A yield with nothing to yield to returns immediately and the spin
    // continues at full speed. Sleeping is the only thing that actually idles
    // the core, and an idle core is what lets the scheduler move work here.
    const int32_t us = REXCVAR_GET(skate3_job_scan_backoff_sleep_us);
    svcSleepThread(int64_t(us) * 1000);
    g_jobscan_sleeps.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_jobscan_yields.fetch_add(1, std::memory_order_relaxed);
#endif
}

}  // namespace

extern "C" REX_FUNC(sub_8290AFA0) {
  const uint32_t self = ctx.r3.u32;
  if (self >= 0x1000u && self < 0xC0000000u) {
    if (g_jobmgr_vt100.load(std::memory_order_relaxed) == 0) {
      const auto rd = [&](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, base + a, sizeof(v));
        return __builtin_bswap32(v);
      };
      const uint32_t vt = rd(self);
      if (vt >= 0x82000000u && vt < 0x84000000u) {
        g_jobmgr_vt96.store(rd(vt + 96), std::memory_order_relaxed);
        g_jobmgr_vt100.store(rd(vt + 100), std::memory_order_relaxed);
      }
    }
    uint32_t be = 0;
    std::memcpy(&be, base + self + 16, sizeof(be));
    const uint32_t count = __builtin_bswap32(be);
    if (count < 0x100000u) {
      g_jobscan_calls.fetch_add(1, std::memory_order_relaxed);
      g_jobscan_entries.fetch_add(count, std::memory_order_relaxed);
      uint32_t prev = g_jobscan_max.load(std::memory_order_relaxed);
      while (count > prev &&
             !g_jobscan_max.compare_exchange_weak(prev, count, std::memory_order_relaxed)) {
      }
    }
  }
  // Who actually drives this. The worker loop reaches its wait only ~500 times
  // a second while the scan runs ~140,000 times a second, so the worker cannot
  // be making most of these calls - something else is calling the job manager
  // in a tight loop, and a histogram of return addresses names it.
  //
  // It named them - sub_82596DB8, sub_824741F0 and sub_8290AE38 - and is off by
  // default now. This runs at 140 kHz and the answer is already known.
  const bool stats = REXCVAR_GET(skate3_job_scan_stats);
  if (stats) {
    const uint32_t lr = uint32_t(ctx.lr);
    bool placed = false;
    for (size_t i = 0; i < kJobCallerSlots; ++i) {
      const uint32_t seen = g_jobcaller_lr[i].load(std::memory_order_relaxed);
      if (seen == lr) {
        g_jobcaller_hits[i].fetch_add(1, std::memory_order_relaxed);
        placed = true;
        break;
      }
      if (seen == 0) {
        uint32_t expected_zero = 0;
        if (g_jobcaller_lr[i].compare_exchange_strong(expected_zero, lr,
                                                      std::memory_order_relaxed)) {
          g_jobcaller_hits[i].fetch_add(1, std::memory_order_relaxed);
          placed = true;
          break;
        }
      }
    }
    if (!placed) {
      g_jobcaller_other.fetch_add(1, std::memory_order_relaxed);
    }
  }

  bool expected = false;
  if (stats && g_jobscan_walked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    const auto be32 = [&](uint32_t addr) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, base + addr, sizeof(v));
      return __builtin_bswap32(v);
    };
    size_t depth = 0;
    g_jobscan_stack[depth++] = uint32_t(ctx.lr);
    uint32_t frame = ctx.r1.u32;
    while (depth < std::size(g_jobscan_stack)) {
      if (frame == 0 || (frame & 3) != 0 || frame < 0x10000u) {
        break;
      }
      const uint32_t next = be32(frame);
      if (next <= frame || (next & 3) != 0) {
        break;
      }
      for (const uint32_t at : {next - 8, next + 4}) {
        const uint32_t link = be32(at);
        if (link >= 0x82000000u && link < 0x84000000u) {
          g_jobscan_stack[depth++] = link;
          break;
        }
      }
      frame = next;
    }
  }
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_8290AFA0(ctx, base);
  // The worker loop above this is "did you do any work? then go round again,
  // otherwise sleep". A worker that never sleeps is being told it did work
  // every time, so the answer it gives back is the thing to count.
  if ((ctx.r3.u32 & 0xFF) != 0) {
    g_jobscan_worked.fetch_add(1, std::memory_order_relaxed);
    tl_jobscan_misses = 0;
    return;
  }
  // Nothing to run. Every caller but the worker is about to ask again
  // immediately, so this is the moment to let go of the core.
  if (caller != kJobWorkerScanLr) {
    JobScanBackOff();
  }
}

// The wait the job worker is supposed to take when there is no work.
//
// It answers "no work" 99.9% of the time and still never sleeps, so the wait
// itself is returning immediately. sub_82EDFEC0(handle, timeout) is the guest's
// generic wait; the worker calls it from one place, so its return address
// identifies the call and r4 is the timeout it asked for. A timeout of zero is
// a poll, and a poll in that loop is the busy-wait eating the console.
extern "C" REX_FUNC(sub_82EDFEC0) {
  if (uint32_t(ctx.lr) == 0x8290B7B4u) {
    g_jobwait_calls.fetch_add(1, std::memory_order_relaxed);
    g_jobwait_timeout.store(ctx.r4.u32, std::memory_order_relaxed);
    g_jobwait_handle.store(ctx.r3.u32, std::memory_order_relaxed);
  }
  __imp__sub_82EDFEC0(ctx, base);
}

// The subsystem update that was ticking counter A, and stopped.
//
// It is a virtual method, so the vtable that reaches it lives in the title's
// data and nothing in the recompiled code names its caller. Walking the guest
// back chain at the moment it runs is the only way to see who drives it, and
// once is enough: the answer does not change between frames. The chain layout
// is the crash reporter's, which explains both return-address slots.
namespace {

std::atomic<bool> g_ticker_walked{false};
std::atomic<uint32_t> g_ticker_calls{0};
uint32_t g_ticker_stack[7] = {0};

}  // namespace

// The predicate that decides whether the screen update runs at all.
//
// sub_826D7708(this, arg) reduces to: pass `arg` straight back unless a state
// word at this+68 reads 18; at 18, answer yes if `arg` was already yes, else
// defer to a sub-object's own query and a flag at +4908. The manager calls it
// every iteration and skips the update - and therefore the counter the load is
// waiting on - whenever it answers no. Recording its inputs and its answer says
// which of those terms is the one that changed.
namespace {

struct GateSample {
  uint32_t self, arg, state, sub, result;
};
std::atomic<uint32_t> g_gate_calls{0};
std::atomic<uint32_t> g_gate_yes{0};
GateSample g_gate_last{};

}  // namespace

// The ring drainer. sub_826D6098 is the manager's vt[28]: it walks the two
// rings hanging off [mgr+128] and retires entries from each. Nothing calls it
// directly - it is dispatched through the manager's vtable - so the question
// is simply whether anything still calls it once the queue fills, and from
// where. The back chain is captured once; the answer does not change.
namespace {

std::atomic<uint32_t> g_drain_calls{0};
std::atomic<bool> g_drain_walked{false};
uint32_t g_drain_stack[6] = {0};

}  // namespace

extern "C" REX_FUNC(sub_826D6098) {
  g_drain_calls.fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (g_drain_walked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    const auto be32 = [&](uint32_t addr) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, base + addr, sizeof(v));
      return __builtin_bswap32(v);
    };
    size_t depth = 0;
    g_drain_stack[depth++] = uint32_t(ctx.lr);
    uint32_t frame = ctx.r1.u32;
    while (depth < std::size(g_drain_stack)) {
      if (frame == 0 || (frame & 3) != 0 || frame < 0x10000u) {
        break;
      }
      const uint32_t next = be32(frame);
      if (next <= frame || (next & 3) != 0) {
        break;
      }
      for (const uint32_t at : {next - 8, next + 4}) {
        const uint32_t link = be32(at);
        if (link >= 0x82000000u && link < 0x84000000u) {
          g_drain_stack[depth++] = link;
          break;
        }
      }
      frame = next;
    }
  }
  __imp__sub_826D6098(ctx, base);
}

extern "C" REX_FUNC(sub_826D7708) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t arg = uint32_t(ctx.r4.u32) & 0xFFu;
  __imp__sub_826D7708(ctx, base);
  const uint32_t result = ctx.r3.u32 & 0xFFu;
  const auto be32 = [&](uint32_t addr) -> uint32_t {
    uint32_t v = 0;
    std::memcpy(&v, base + addr, sizeof(v));
    return __builtin_bswap32(v);
  };
  g_gate_calls.fetch_add(1, std::memory_order_relaxed);
  if (result != 0) {
    g_gate_yes.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t sub = (self >= 0x1000u && self < 0xC0000000u) ? be32(self + 4) : 0;
  g_gate_last = GateSample{self, arg,
                           (self >= 0x1000u && self < 0xC0000000u) ? be32(self + 68) : 0, sub,
                           result};
}

extern "C" REX_FUNC(sub_826D78B0) {
  g_ticker_calls.fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (g_ticker_walked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    const auto be32 = [&](uint32_t addr) -> uint32_t {
      uint32_t v = 0;
      std::memcpy(&v, base + addr, sizeof(v));
      return __builtin_bswap32(v);
    };
    size_t depth = 0;
    g_ticker_stack[depth++] = uint32_t(ctx.lr);
    uint32_t frame = ctx.r1.u32;
    while (depth < std::size(g_ticker_stack)) {
      if (frame == 0 || (frame & 3) != 0 || frame < 0x10000u) {
        break;
      }
      const uint32_t next = be32(frame);
      if (next <= frame || (next & 3) != 0) {
        break;
      }
      for (const uint32_t at : {next - 8, next + 4}) {
        const uint32_t link = be32(at);
        if (link >= 0x82000000u && link < 0x84000000u) {
          g_ticker_stack[depth++] = link;
          break;
        }
      }
      frame = next;
    }
  }
  __imp__sub_826D78B0(ctx, base);
}

extern "C" REX_FUNC(sub_82481A98) {
  const uint32_t watched = g_tick_watch_addr.load(std::memory_order_relaxed);
  if (watched != 0 && ctx.r3.u32 + 48u == watched) {
    g_tick_watch_calls.fetch_add(1, std::memory_order_relaxed);
    g_tick_watch_lr.store(uint32_t(ctx.lr), std::memory_order_relaxed);
  }
  __imp__sub_82481A98(ctx, base);
}

extern "C" REX_FUNC(sub_82965C90) {
  g_screen_mgr_base.store(base, std::memory_order_relaxed);
  g_screen_mgr_obj.store(ctx.r3.u32, std::memory_order_release);
  __imp__sub_82965C90(ctx, base);
}

namespace skate3::native_render {

void LogJobScan() {
  const uint64_t n = g_jobscan_calls.load(std::memory_order_relaxed);
  if (n == 0) {
    return;
  }
  static uint64_t last_n = 0;
  const uint64_t worked = g_jobscan_worked.load(std::memory_order_relaxed);
  REXLOG_WARN("[jobs] scans={} (+{}) said-did-work={} ({:.1f}%) list={} | from {:08X} {:08X}", n,
              n - last_n, worked, 100.0 * double(worked) / double(n),
              g_jobscan_entries.load(std::memory_order_relaxed) / n, g_jobscan_stack[0],
              g_jobscan_stack[1]);
  {
    std::string callers;
    for (size_t i = 0; i < kJobCallerSlots; ++i) {
      const uint32_t lr = g_jobcaller_lr[i].load(std::memory_order_relaxed);
      if (lr == 0) {
        break;
      }
      callers += fmt::format("{:08X}={} ", lr, g_jobcaller_hits[i].load(std::memory_order_relaxed));
    }
    REXLOG_WARN("[jobs] scan callers: {}(other={})", callers,
                g_jobcaller_other.load(std::memory_order_relaxed));
  }
  REXLOG_WARN("[jobs] backoff yields={} sleeps={} | {:.0f} scans/s over the interval",
              g_jobscan_yields.load(std::memory_order_relaxed),
              g_jobscan_sleeps.load(std::memory_order_relaxed), double(n - last_n) / 5.0);
  REXLOG_WARN("[jobs] worker waits={} last timeout={} handle={:08X} | vt96={:08X} vt100={:08X}",
              g_jobwait_calls.load(std::memory_order_relaxed),
              int32_t(g_jobwait_timeout.load(std::memory_order_relaxed)),
              g_jobwait_handle.load(std::memory_order_relaxed),
              g_jobmgr_vt96.load(std::memory_order_relaxed),
              g_jobmgr_vt100.load(std::memory_order_relaxed));
  last_n = n;
}

void LogFrameBudget() {
  const uint64_t n = g_frame_count.load(std::memory_order_relaxed);
  if (n == 0) {
    return;
  }
  // Averages hide a frame time that is climbing, which is the whole point
  // here, so report the interval since the last line as well.
  static uint64_t last_n = 0, last_inner = 0, last_hooks = 0, last_outside = 0;
  const uint64_t inner = g_frame_inner_us.load(std::memory_order_relaxed);
  const uint64_t hooks = g_frame_hooks_us.load(std::memory_order_relaxed);
  const uint64_t outside = g_frame_outside_swap_us.load(std::memory_order_relaxed);
  const uint64_t dn = n - last_n;
  // "frame" rather than "rest" for the last one: it is the whole frame period.
  REXLOG_WARN("[frame] {} frames | since last: {} frames, swap {} us, hooks {} us, frame {} us",
              n, dn, dn ? (inner - last_inner) / dn : 0, dn ? (hooks - last_hooks) / dn : 0,
              dn ? (outside - last_outside) / dn : 0);
  last_n = n;
  last_inner = inner;
  last_hooks = hooks;
  last_outside = outside;
}

void LogScreenManagerState() {
  const uint32_t obj = g_screen_mgr_obj.load(std::memory_order_acquire);
  uint8_t* const guest_base = g_screen_mgr_base.load(std::memory_order_relaxed);
  if (obj == 0 || guest_base == nullptr) {
    return;
  }
  // Guest memory is never unmapped on this platform, so a plain read of a
  // pointer the guest itself handed us cannot fault.
  const auto read = [&](uint32_t offset) {
    uint32_t be = 0;
    std::memcpy(&be, guest_base + obj + offset, sizeof(be));
    return __builtin_bswap32(be);
  };

  const uint32_t state = read(96);
  const uint32_t current = read(8);
  const uint32_t pending = read(12);
  const uint32_t deferred = read(84);
  const uint32_t callback = read(88);

  // Report a change immediately and otherwise stay quiet for a while: a state
  // that is advancing normally would bury the log, and a state that is stuck
  // only needs saying often enough to show it is still stuck.
  // The three counters the loader waits on before it will report the load
  // finished. sub_826D8A28 snapshots each one, then sleeps in 1 ms steps until
  // it has advanced - "let the render pipeline get a frame through". The main
  // thread's state 3 is waiting for exactly that. Reading them says which of
  // the three stopped ticking, which no thread dump can show: the loader is
  // asleep in all three cases and looks identical.
  //
  // Addresses as the guest computes them, from two fixed globals in the image.
  const auto read_at = [&](uint32_t addr) -> uint32_t {
    // Only the loaded image and the guest heap are certain to be committed,
    // and an uncommitted read is fatal on this platform rather than merely
    // wrong. Anything outside those reads as zero.
    if (addr < 0x1000u || addr >= 0xC0000000u) {
      return 0;
    }
    uint32_t be = 0;
    std::memcpy(&be, guest_base + addr, sizeof(be));
    return __builtin_bswap32(be);
  };
  uint32_t tick[3] = {0, 0, 0};
  if (const uint32_t root = read_at(0x83083BCCu); root != 0) {
    if (const uint32_t a = read_at(root + 144); a != 0) {
      tick[0] = read_at(a + 72);
      g_tick_watch_addr.store(a + 72, std::memory_order_relaxed);
    }
    if (const uint32_t b = read_at(root + 148); b != 0) {
      tick[1] = read_at(b + 136);
    }
  }
  if (const uint32_t c = read_at(0x83083C38u); c != 0) {
    tick[2] = read_at(c + 56);
  }

  static uint32_t last_tick[3] = {0, 0, 0};
  const bool ticking = std::memcmp(last_tick, tick, sizeof(tick)) != 0;
  std::memcpy(last_tick, tick, sizeof(tick));

  static uint32_t last[5] = {0xFFFFFFFFu, 0, 0, 0, 0};
  static int quiet = 0;
  const uint32_t now[5] = {state, current, pending, deferred, callback};
  const bool changed = std::memcmp(last, now, sizeof(now)) != 0;
  if (!changed && !ticking && ++quiet < 6) {
    return;
  }
  std::memcpy(last, now, sizeof(now));
  quiet = 0;
  REXLOG_WARN(
      "[fe-mgr] state={} current={:08X} pending={:08X} deferred={:08X} callback={:08X}"
      " load-ticks={}/{}/{} {} | A: calls={} last-lr={:08X} upd={}{}",
      state, current, pending, deferred, callback, tick[0], tick[1], tick[2],
      ticking ? "advancing" : "FROZEN",
      g_tick_watch_calls.load(std::memory_order_relaxed),
      g_tick_watch_lr.load(std::memory_order_relaxed),
      g_ticker_calls.load(std::memory_order_relaxed), changed ? "" : "  (unchanged)");
  // Name the two virtual functions this hinges on. The screen manager only
  // runs the update that ticks A when screen->vt[24](x) says yes, and stops
  // saying yes the moment the load begins; the loader's own vt[24] runs
  // instead. Both live in vtables in the title's data, so nothing offline can
  // name them - but the objects are right here, and their tables are readable.
  {
    // Re-read whenever the screen changes, not once: skipping the intro movie
    // reaches the same stall through a different screen, and the whole point is
    // to name the functions the manager is actually calling at the time.
    static uint32_t named_for = 0;
    if (current != 0 && current != named_for) {
      named_for = current;
      const uint32_t screen_vt = read_at(current);
      const uint32_t loader = read_at(obj + 16);
      const uint32_t loader_vt = loader ? read_at(loader) : 0;
      REXLOG_WARN(
          "[fe-mgr] screen {:08X} vtable {:08X}: +24={:08X} +28={:08X} +36={:08X}",
          current, screen_vt, read_at(screen_vt + 24), read_at(screen_vt + 28),
          read_at(screen_vt + 36));
      REXLOG_WARN(
          "[fe-mgr] loader {:08X} vtable {:08X}: +24={:08X} +28={:08X} +60={:08X} +64={:08X}",
          loader, loader_vt, read_at(loader_vt + 24), read_at(loader_vt + 28),
          read_at(loader_vt + 60), read_at(loader_vt + 64));
      // The manager's own vt[24] is what actually decides each iteration: the
      // screen's gate turns out to pass its answer straight through, because
      // the screen's sub-object is null. Naming it is the last unknown.
      const uint32_t mgr_vt = read_at(obj);
      REXLOG_WARN("[fe-mgr] manager {:08X} vtable {:08X}: +24={:08X} +28={:08X}", obj, mgr_vt,
                  read_at(mgr_vt + 24), read_at(mgr_vt + 28));
      // The worker's own fields say whether it still holds the job: +56 is the
      // function slot the worker clears when it finishes, which is exactly what
      // "is it done" reports, and +4 is the event the main thread sleeps on.
      REXLOG_WARN("[fe-mgr] worker: wake-event={:08X} quit={} fn={:08X} arg={:08X} main-waits-on={:08X}",
                  read_at(loader + 48), read_at(loader + 52) & 0xFFu, read_at(loader + 56),
                  read_at(loader + 60), read_at(loader + 4));
    }
  }

  // The ring the manager allocates from, and whether anything is retiring it.
  {
    const uint32_t owner = read_at(obj + 128);
    const uint32_t ring0 = owner ? read_at(owner + 24) : 0;
    const uint32_t ring1 = owner ? read_at(owner + 28) : 0;
    REXLOG_WARN(
        "[fe-ring] drain-calls={} from {:08X} {:08X} {:08X} | ring0={:08X} head={} tail={} |"
        " ring1={:08X} head={} tail={}",
        g_drain_calls.load(std::memory_order_relaxed), g_drain_stack[0], g_drain_stack[1],
        g_drain_stack[2], ring0, ring0 ? read_at(ring0 + 12004) : 0,
        ring0 ? read_at(ring0 + 12008) : 0, ring1, ring1 ? read_at(ring1 + 12004) : 0,
        ring1 ? read_at(ring1 + 12008) : 0);
  }

  REXLOG_WARN("[fe-gate] calls={} yes={} | this={:08X} arg={} state={} sub={:08X} -> {}",
              g_gate_calls.load(std::memory_order_relaxed),
              g_gate_yes.load(std::memory_order_relaxed), g_gate_last.self, g_gate_last.arg,
              g_gate_last.state, g_gate_last.sub, g_gate_last.result);
  if (g_ticker_walked.load(std::memory_order_acquire)) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      REXLOG_WARN(
          "[fe-mgr] gate calls={} yes={} | last: this={:08X} arg={} state={} sub={:08X} -> {}",
          g_gate_calls.load(std::memory_order_relaxed),
          g_gate_yes.load(std::memory_order_relaxed), g_gate_last.self, g_gate_last.arg,
          g_gate_last.state, g_gate_last.sub, g_gate_last.result);
      REXLOG_WARN("[fe-mgr] the update that ticks A is driven from: {:08X} {:08X} {:08X} {:08X}"
                  " {:08X} {:08X} {:08X}",
                  g_ticker_stack[0], g_ticker_stack[1], g_ticker_stack[2], g_ticker_stack[3],
                  g_ticker_stack[4], g_ticker_stack[5], g_ticker_stack[6]);
    }
  }
}

}  // namespace skate3::native_render

// rw::movie::MovieDecoder::Decode(int, VideoRenderable**,
// SubtitleRenderable*): fires per decoded FMV frame while any movie plays
// (boot intro logos and all other rw::movie playback). Heartbeat for the
// native scene's FMV yield: the video frame is CPU-decoded into a texture
// and reaches the screen through the game's postfx chain + swap without a
// capturable 2D draw, so only the emulated path can show it.
extern "C" REX_FUNC(sub_82A92DC8) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnMovieDecode();
  }
  __imp__sub_82A92DC8(ctx, base);
}

// VideoRenderer_RwTexture::Render(VideoRenderable*, int): fills the three
// YUV plane textures (members this+12/+124/+68) with the decoded FMV frame
// via Texture::Lock/FillTextureData/Unlock. Post-call, the planes hold the
// finished frame: publish their fetch words for the native FMV blit.
extern "C" REX_FUNC(sub_8263C498) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8263C498(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnMovieFrame(base, self);
  }
}

// Sk8::WorldPresentation::AddRenderInstance(pegasus::tInstance*): the world
// registry add, fired per placed instance while a map loads (r4 = tInstance).
// The prewarm's primary mesh source: the instance's tRModelData mesh table
// is walked (validated offsets) and every optimesh queued for the
// loading-screen decode.
extern "C" REX_FUNC(sub_82791290) {
  const uint32_t instance = ctx.r4.u32;
  __imp__sub_82791290(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnAddRenderInstance(base, instance);
  }
}

// D3D::SetPending_AluConstants(device, u64 dirty_group_mask, bank, ptr):
// bank 0x4000 = vertex constants. Called from inside the Draw* functions;
// ptr is the device's positional constant shadow bank.
extern "C" REX_FUNC(sub_82B83FE0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnVsConstantUpload(base, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32,
                                             ctx.r3.u32);
  }
  __imp__sub_82B83FE0(ctx, base);
}

// D3DDevice_SetIndices(device, ib) / D3DDevice_SetStreamSource(device,
// stream, vb, offset, stride): track the currently bound guest buffers so
// draws can be matched back to captured skinned items.
extern "C" REX_FUNC(sub_82B79190) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetIndices(ctx.r4.u32);
  }
  __imp__sub_82B79190(ctx, base);
}

extern "C" REX_FUNC(sub_82B78FF0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetStreamSource(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                            ctx.r7.u32);
  }
  __imp__sub_82B78FF0(ctx, base);
}

// D3DDevice_DrawIndexedVertices: post-call, the draw's VS constants are now
// in the shadow bank; refresh any pending skinned item bound to these
// buffers (deferred multi-pass meshes only draw here).
extern "C" REX_FUNC(sub_82B7AD68) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7AD68(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 0, r4, r5, r6, r7);
  }
}

// D3DDevice_DrawVertices, non-indexed draw path: cloth-simulated garments
// (captured live as world-space quad items) and character shadow proxies.
extern "C" REX_FUNC(sub_82B7A970) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B7A970(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 1, r4, r5, r6, r7);
    skate3::native_render::OnClothDraw(base, r4, r5, r6, r7);
  }
}

// ---- 2D / APT (Flash-converted HUD) reconnaissance hooks -----------------
// Every HUD/menu 2D element is a Flash SWF converted to EA APT, rendered by
// Sk8::FE::AptRenderingIntegration through the same guest D3D draw functions
// hooked above. These brackets tag draws issued inside the 2D pass so the
// recorder can capture and the future native 2D pass can replay them.

// Sk8::FE::FrontEndManager::Render2D(unsigned int): the game's whole 2D
// pass (FE movies + HUD).
extern "C" REX_FUNC(sub_825D9168) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(0, true);
  __imp__sub_825D9168(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(0, false);
}

// Sk8::FE::AptMovieIntegration::Render(unsigned int): one APT movie.
extern "C" REX_FUNC(sub_825D67D8) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(1, true);
  __imp__sub_825D67D8(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(1, false);
}

// Sk8::FE::AptRenderingIntegration::DrawRenderingUnit(void*, AptRenderInfo
// const*): one APT display-list element (texture quad / vector shape).
extern "C" REX_FUNC(sub_825D4490) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(2, true);
  __imp__sub_825D4490(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(2, false);
}

// Sk8::FE::AptRenderingIntegration::UpdateRenderToTexture(unsigned int):
// in gameplay this renders the whole HUD into a screen-sized overlay
// texture at true screen coordinates (the game composites it later through
// the suppressed emulated pass). Diagnostic bracket only.
extern "C" REX_FUNC(sub_825D4E50) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(3, true);
  __imp__sub_825D4E50(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(3, false);
}

// Sk8::Render::cFont::DrawstringLocal<char> / <unsigned short>: the glyph
// text emitter (trick names, scores). Text can flush OUTSIDE the APT
// brackets, so it gets its own bit.
extern "C" REX_FUNC(sub_82808388) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(4, true);
  __imp__sub_82808388(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(4, false);
}

extern "C" REX_FUNC(sub_82808708) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(4, true);
  __imp__sub_82808708(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(4, false);
}

// Sk8::Render::SimpleDraw::DrawParameters::Draw: the game's immediate-mode
// quad/tri utility (chase arrows, in-world guide markers/beams, debug
// draws). Bottoms out in BeginVertices like the APT path.
extern "C" REX_FUNC(sub_82804168) {
  const bool enabled = skate3::native_render::Enabled();
  if (enabled) skate3::native_scene::On2dPhase(5, true);
  __imp__sub_82804168(ctx, base);
  if (enabled) skate3::native_scene::On2dPhase(5, false);
}

// D3DDevice_SetPixelShader / SetVertexShader (r4 = guest shader object):
// recorded per draw to group the 2D stream by shader variant.
extern "C" REX_FUNC(sub_82B7F408) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetShader(true, ctx.r4.u32);
  }
  __imp__sub_82B7F408(ctx, base);
}

extern "C" REX_FUNC(sub_82B7F150) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetShader(false, ctx.r4.u32);
  }
  __imp__sub_82B7F150(ctx, base);
}

// D3D::SetPending_RenderStates(device, u64 dirty mask, bank, ptr): the
// render-state shadow bank (blend/depth state for 2D draws lives here).
extern "C" REX_FUNC(sub_82B83C48) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnRenderStateUpload(ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
  }
  // Last look at the ring before the guest walks it; see CheckD3DRing.
  skate3::native_render::CheckD3DRing(base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_82B83C48(ctx, base);
}

// D3DDevice_SetViewport(device, D3DVIEWPORT*) / SetScissorRect(device,
// RECT*): recorded per draw (render-to-texture APT passes and mask rects).
extern "C" REX_FUNC(sub_82B74310) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetViewport(base, ctx.r4.u32);
  }
  __imp__sub_82B74310(ctx, base);
}

extern "C" REX_FUNC(sub_82B769C0) {
  if (skate3::native_render::Enabled()) {
    skate3::native_scene::OnSetScissor(base, ctx.r4.u32);
  }
  __imp__sub_82B769C0(ctx, base);
}

// D3DDevice_BeginVertices: inline (write-through-ring) vertex path; the
// CPU writes computed vertices directly. Post-call r3 = guest write pointer.
extern "C" REX_FUNC(sub_82B79FC0) {
  const bool enabled = skate3::native_render::Enabled();
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  __imp__sub_82B79FC0(ctx, base);
  if (enabled) {
    skate3::native_scene::OnDrawDone(base, 2, r4, r5, r6, ctx.r3.u32 != 0 ? ctx.r3.u32 : r7);
  }
}

// The loop AROUND the spin-wait. Timing it answers the question the profiler
// could not: the sampler says the render thread is inside this 48% of the
// time, but a share of samples is not a duration - it cannot say whether the
// thread is waiting 50 ms of a 122 ms frame or spending the same share of a
// frame it would have taken anyway. This measures the wall time and how often
// the wait is entered, which is what decides whether it is worth attacking.
extern "C" REX_FUNC(sub_82B755C0) {
  if (!REXCVAR_GET(skate3_guest_spin_measure)) {
    __imp__sub_82B755C0(ctx, base);
    return;
  }
  static std::atomic<uint64_t> ns{0};
  static std::atomic<uint64_t> calls{0};
  static std::atomic<uint64_t> last_report_ns{0};
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_82B755C0(ctx, base);
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t took =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  const uint64_t total = ns.fetch_add(took, std::memory_order_relaxed) + took;
  const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now_ns =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1.time_since_epoch()).count());
  uint64_t last = last_report_ns.load(std::memory_order_relaxed);
  if (now_ns - last > 5000000000ull &&
      last_report_ns.compare_exchange_strong(last, now_ns, std::memory_order_relaxed)) {
    REXLOG_WARN("guest-wait: {} calls, {:.1f} ms total, {:.3f} ms each, {:.1f} ms/s",
                n, double(total) / 1e6, double(total) / double(n) / 1e6,
                double(total) / 1e6 / 5.0);
    ns.store(0, std::memory_order_relaxed);
    calls.store(0, std::memory_order_relaxed);
  }
}

// The guest's spin-wait body, and by a wide margin the most expensive guest
// function on a slow device: a sampling profile of the render thread put
// sub_82B76080 at 35% and its calling loop sub_82B755C0 at 13%, stable across
// four windows of ~15,000 samples. Together, roughly half the render thread.
//
// It is a poll - read a timestamp, subtract, compare against 5000, return
// "keep waiting" - and on the console it is paced. The Xbox 360 wrote the wait
// as `cctpl` (drop this SMT thread's priority so its sibling gets the core),
// thirty-two `db16cyc` (sixteen cycles of delay each), then `cctpm`. The
// recompiler emits none of those three: they have no x86/ARM equivalent and
// they carry no architectural state, so the loop that was throttled on the
// console runs flat out here, burning a core and the memory bandwidth that the
// threads it is waiting FOR need in order to finish.
//
// Giving the wait back its pacing is the point. Default 0 keeps today's
// behaviour so this cannot regress a device that is already fast.
extern "C" REX_FUNC(sub_82B76080) {
  if (const int32_t mode = REXCVAR_GET(skate3_guest_spin_yield); mode > 0) {
    if (mode == 1) {
      // The console's own pacing, approximately: a pipeline hint rather than a
      // trip through the scheduler. Cheapest, and it cannot lose the thread's
      // timeslice while it holds anything.
      for (int i = 0; i < 32; ++i) {
#if defined(__clang__)
        __builtin_arm_yield();
#else
        // The same AArch64 YIELD hint. Written out because devkitA64's
        // arm_acle.h does not carry the __yield intrinsic.
        __asm__ __volatile__("yield" ::: "memory");
#endif
      }
    } else if (mode == 4) {
      // Horizon's yield-with-migration: hand the core over AND let the kernel
      // pull the thread this one is waiting for across from another core. On
      // three cores that is the only yield with anything to yield to; mode 3's
      // 100 us sleep measured worse here, because this wait returns in a
      // microsecond on this port rather than the 43 ms it takes on a phone.
#if defined(__SWITCH__)
      svcSleepThread(-1);
#else
      sched_yield();
#endif
    } else if (mode == 2) {
      // Offer the core to anything else runnable on it. Note this does NOT
      // idle the core: with nothing else queued, sched_yield returns straight
      // away and the spin continues at full speed. Measured as a wash, which
      // is exactly what that implies.
      sched_yield();
    } else {
      // Actually stop burning the core. The thread this loop waits on is
      // saturated on another core, and a sibling spinning flat out costs it
      // memory bandwidth, shared cache and - on a tablet already sitting at
      // 48 C - power budget it could otherwise spend on clocks. The wait is
      // ~43 ms, so sleeping at 100 us granularity cannot meaningfully delay
      // noticing that it ended.
#if defined(__SWITCH__)
      // The syscall directly rather than nanosleep: one less libc layer on the
      // path, and this is the only sleep in the frame's critical path.
      svcSleepThread(100000);
#else
      struct timespec ts = {0, 100000};
      nanosleep(&ts, nullptr);
#endif
    }
  }
  __imp__sub_82B76080(ctx, base);
}

// Sk8::cLivingWorldPresEntityManager::Update - the ambient world's whole sim
// tick: pedestrians and traffic, every entity, every frame.
//
// Skipping it on non-refresh frames is the single largest lever on a device
// that cannot keep up, because it is measurably what separates a menu from
// gameplay. The Galaxy Tab A7 Lite holds 55-57 fps in the menus and collapses
// to 6-7 in the world, on the same renderer and the same GPU - and the GPU is
// idle in both (wait 0.00 ms of a 155 ms frame), so the entire difference is
// the guest CPU simulating the crowd.
//
// The skater, the board and the physics do not come through here, so they keep
// running at full rate; what stutters is the pedestrians' own animation. Both
// the vtable thunk at 0x827BC9A0 and any direct dispatch land on this
// function, so hooking it here catches the subsystem in one place.
extern "C" REX_FUNC(sub_827BC9A8) {
  // Instrumented: three controlled runs (throttle off, every 2nd, every 4th)
  // all measured p50 116.5 ms, so either this never fires or the crowd is not
  // the cost here. Counting says which.
  static std::atomic<uint64_t> calls{0};
  static std::atomic<uint64_t> skips{0};
  const uint64_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  bool skipped = false;
  if (const int32_t period = REXCVAR_GET(skate3_native_render_lw_refresh);
      period > 1 && skate3::native_render::Enabled() &&
      (skate3::native_render::g_frame_index % uint64_t(period)) != 0) {
    skips.fetch_add(1, std::memory_order_relaxed);
    skipped = true;
  }
  if ((n % 2000) == 0) {
    REXLOG_WARN("lw-throttle: {} calls, {} skipped", n, skips.load());
  }
  if (skipped) {
    return;
  }
  __imp__sub_827BC9A8(ctx, base);
}

// LivingWorld batch pack writer (unnamed; called per entity per sim tick
// from the tail of cLivingWorldPresEntityManager::Update), the LOD-
// pedestrian skinning-palette source: fused UpdateBoneTransforms+Pack
// writing the concatenated per-mesh 4x3 palettes into
// cModelInstance.m_matrices. r3 = the cLivingWorldPresEntity-derived
// entity; post-call m_matrices holds the packed rows.
extern "C" REX_FUNC(sub_827C1D38) {
  const uint32_t entity = ctx.r3.u32;
  __imp__sub_827C1D38(ctx, base);
  if (skate3::native_render::Enabled()) {
    skate3::native_palette::OnLwPack(base, entity);
  }
}


// ---- map-erase miss probe (the crash returning to the stock world) ---------
//
// Returning from a DLC map faults writing guest 0xFFFFFFFF on the LOAD thread,
// deterministically, with guest lr = 0x82C95ECC - the return address of the
// call to sub_82C9E4F8 inside sub_82C95E18. Reading the recompiled code says
// what the pair is doing:
//
//   sub_82C9E4F8(out=&r1[80], map=r28+24880, key=&r1[152])   // hash find
//   r10 = out[0]; r8 = out[1];                                // node, bucket
//   r9 = *(r10 + 168)                                         // node->next
//   if (r9 == 0) do { r9 = *(r11 += 4); } while (r9 == 0);    // scan buckets
//
// It is an ERASE. On a MISS, sub_82C9E4F8 returns the end sentinel
// (buckets + bucket_count*4) rather than a node, the caller dereferences it
// unchecked, and that bucket scan then walks 4 bytes at a time off the end of
// the array until it hits unmapped memory. So the fault is the symptom; the
// bug is an erase of a key that is not in the map.
//
// This does not fix it - it says WHICH key, on which map, so the erase can be
// matched to what the title is tearing down. Rate-limited hard: the find is
// hot and only the misses are interesting.
extern "C" REX_FUNC(sub_82C9E4F8) {
  const uint32_t out = ctx.r3.u32;
  const uint32_t map = ctx.r4.u32;
  const uint32_t key_ptr = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82C9E4F8(ctx, base);
  if (!REXCVAR_GET(skate3_map_erase_probe)) {
    return;
  }
  // Only the erase call site; every other caller of this find is uninteresting.
  if (caller != 0x82C95ECC) {
    return;
  }
  const uint32_t buckets = REX_LOAD_U32(map + 4);
  const uint32_t count = REX_LOAD_U32(map + 8);
  const uint32_t node = REX_LOAD_U32(out + 0);
  const uint32_t end_slot = buckets + count * 4u;
  if (node != end_slot && REX_LOAD_U32(out + 4) != end_slot) {
    return;  // found: the erase is well formed
  }
  static std::atomic<uint32_t> s_hits{0};
  const uint32_t n = s_hits.fetch_add(1, std::memory_order_relaxed) + 1;

  // ---- Make the erase a no-op instead of a fault -------------------------
  //
  // Erasing a key that is not in the map is a no-op by definition, but this
  // caller does not check: it takes the find's "not found" marker (-1) as a
  // node and dereferences it at +168. What the caller then does, read out of
  // the recompiled source at sub_82C95E18+0xB4:
  //
  //   r10 = out[0]; r8 = out[1];
  //   r9 = *(r10+168);  if (r9 == 0) do { r9 = *(r11 += 4); } while (!r9);
  //   r11 = *(r8);  if (r11 == r10) *(r8) = *(r10+168);   // unlink
  //   if (r10 != *(map+40)) { *(map+32) -= 1; *(r10+0) = *(map+28);
  //                           *(map+28) = r10; }          // FREE-LIST PUSH
  //   *(map+12) -= 1;                                     // size--
  //
  // So hand it a result that walks that path harmlessly:
  //   out[0] = *(map+40), the map's own end sentinel. The `r10 != *(map+40)`
  //     test then SKIPS the free-list push - which is the dangerous half,
  //     because pushing a node we invented onto the game's free list would
  //     corrupt it later, far from here.
  //   out[1] = a scratch pair below the guest stack pointer, holding that
  //     same sentinel. The unlink's one store therefore lands in scratch and
  //     the real bucket array is never touched. There are no calls between
  //     the return and that store, so stack below r1 is safe for it.
  //   scratch[1] = 1, so if *(sentinel+168) happens to be zero the bucket
  //     scan terminates on the very next word instead of running away.
  //
  // That leaves exactly one side effect: `size--` at the end, which the
  // sub_82C95E18 hook below puts back.
  if (!REXCVAR_GET(skate3_map_erase_fix)) {
    if (n <= 8 || (n % 512) == 0) {
      REXLOG_WARN(
          "[map-erase] MISS #{} (fix OFF): key={:08X}{:08X} map={:08X} "
          "node={:08X} end={:08X} - the caller will now walk off the array",
          n, REX_LOAD_U32(key_ptr), REX_LOAD_U32(key_ptr + 4), map, node,
          end_slot);
    }
    return;
  }
  const uint32_t sentinel = REX_LOAD_U32(map + 40);
  const uint32_t scratch = ctx.r1.u32 - 256;
  if (sentinel == 0 || scratch < 0x1000) {
    return;  // nothing safe to hand back; leave the original behaviour
  }
  REX_STORE_U32(scratch + 0, sentinel);
  REX_STORE_U32(scratch + 4, 1);
  REX_STORE_U32(out + 0, sentinel);
  REX_STORE_U32(out + 4, scratch);
  skate3::native_render::g_map_erase_missed = true;
  if (n <= 8 || (n % 512) == 0) {
    REXLOG_WARN(
        "[map-erase] MISS #{} neutralised: key={:08X}{:08X} map={:08X} "
        "sentinel={:08X} scratch={:08X}",
        n, REX_LOAD_U32(key_ptr), REX_LOAD_U32(key_ptr + 4), map, sentinel,
        scratch);
  }
}

// The erase itself. Its tail decrements the map's size unconditionally, so a
// miss that the find hook above neutralised would still leave the count one
// too low. Put it back.
extern "C" REX_FUNC(sub_82C95E18) {
  const uint32_t map = ctx.r3.u32 + 24880;
  const bool guard = REXCVAR_GET(skate3_map_erase_fix);
  const uint32_t size_before = guard ? REX_LOAD_U32(map + 12) : 0;
  if (guard) {
    skate3::native_render::g_map_erase_missed = false;
  }
  __imp__sub_82C95E18(ctx, base);
  if (guard && skate3::native_render::g_map_erase_missed) {
    skate3::native_render::g_map_erase_missed = false;
    REX_STORE_U32(map + 12, size_before);
  }
}
