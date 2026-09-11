/**
 * @file        skate3_input_script.cpp
 * @brief       Drive the pad from a timeline file, and capture frames along the way
 *
 * Unattended sessions need more than the demo path's button taps: skating needs held
 * sticks, and tricks are stick flicks. This overrides the XamInputGetState import in the
 * executable -- the SDK implementation lives in librexruntime.so and stays reachable
 * through dlsym(RTLD_NEXT) -- lets the real input system fill the state, then replaces
 * player one's gamepad with the script's current step while the script runs.
 *
 * Script, one step per line:
 *
 *   <duration_ms> [a b x y lb rb l3 r3 start back up down left right] [lx= ly= rx= ry= lt= rt=]
 *
 * Sticks run -100..100 with up and right positive, triggers 0..100. A step holds exactly
 * that pad state for its duration; anything it does not name is neutral. `#` starts a
 * comment. `@mark NAME` logs a marker when the timeline reaches it, `@capture NAME` also
 * saves the guest output frame as a PPM in skate3_input_capture_dir.
 *
 * Bails are counted from the game's own decision, not guessed from input:
 * PhysicalPlayerHiLOD::IsWipeoutRequested (sub_82DB9100 -- the name and the r3 reading come
 * from sk8-engine-linux's trick pipeline) returns nonzero when the game puts a skater down.
 * Board state cannot see a bail, because the skater stays on the board.
 *
 * The script starts skate3_input_script_settle_ms after gameplay is first reached. A
 * controller thread handles the start, markers, captures and the bail log. The hook only indexes a
 * table built before the script starts, in integer arithmetic, because it runs on a guest
 * thread inside a guest call -- see skate3_audio_dump.cpp for what host float work there
 * can do.
 */
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/presenter.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_STRING(skate3_input_script, "", "Skate 3",
                      "Drive player one's pad from this timeline file once gameplay is reached. "
                      "Format in src/skate3_input_script.cpp. Empty disables it.");
REXCVAR_DEFINE_INT32(skate3_input_script_settle_ms, 3000, "Skate 3",
                     "Wait this long after gameplay is first reached before starting "
                     "skate3_input_script.")
    .range(0, 120000);
REXCVAR_DEFINE_STRING(skate3_input_capture_dir, "", "Skate 3",
                      "Write @capture frames from skate3_input_script here, as PPM. Empty "
                      "disables capture.");
REXCVAR_DEFINE_INT32(skate3_input_capture_every_ms, 0, "Skate 3",
                     "Also capture a frame this often while the script runs. 0 disables.")
    .range(0, 600000);

namespace {

constexpr const char* kImport = "__imp__XamInputGetState";
constexpr uint32_t kFlagGamepad = 1;

struct Step {
  int64_t start_ms;
  int64_t end_ms;
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t lx, ly, rx, ry;
};

struct Event {
  int64_t at_ms;
  bool capture;
  std::string name;
};

// Written once by the controller thread before g_start_ms is published, then read-only.
std::vector<Step> g_steps;
std::vector<Event> g_events;
int64_t g_total_ms = 0;

std::atomic<int64_t> g_start_ms{-1};
std::atomic<bool> g_done{false};
std::atomic<size_t> g_cursor{0};
std::atomic<uint64_t> g_input_polls{0};

// Wipeout observation, updated from guest threads with integer work only.
constexpr int64_t kWipeoutGapMs = 1500;
std::atomic<uint64_t> g_wipeout_polls{0};
std::atomic<uint64_t> g_wipeout_true_polls{0};
std::atomic<uint64_t> g_wipeout_events{0};
std::atomic<int64_t> g_last_wipeout_true_ms{-1000000};

// Which player each wipeout belonged to. The function is polled per physical player and
// these worlds are populated, so an unfiltered count means "someone fell" - a pedestrian
// going down next to the skater already got a bail attributed to an input that did not
// cause one. The object arrives in r3 on entry; the local skater's is learned by matching
// a wipeout against its frame, and is not derivable from here.
constexpr size_t kMaxWipeoutPlayers = 8;
std::atomic<uint32_t> g_wipeout_player[kMaxWipeoutPlayers]{};
std::atomic<uint64_t> g_wipeout_player_events[kMaxWipeoutPlayers]{};
std::atomic<uint32_t> g_last_wipeout_object{0};

void RecordWipeoutPlayer(uint32_t object) {
  g_last_wipeout_object.store(object, std::memory_order_relaxed);
  for (size_t i = 0; i < kMaxWipeoutPlayers; i++) {
    uint32_t slot = g_wipeout_player[i].load(std::memory_order_relaxed);
    if (slot == 0) {
      uint32_t expected = 0;
      if (!g_wipeout_player[i].compare_exchange_strong(expected, object,
                                                       std::memory_order_relaxed)) {
        slot = expected;
      } else {
        slot = object;
      }
    }
    if (slot == object) {
      g_wipeout_player_events[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct ButtonName {
  const char* name;
  uint16_t bit;
};
constexpr ButtonName kButtons[] = {
    {"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004},  {"right", 0x0008},
    {"start", 0x0010}, {"back", 0x0020}, {"l3", 0x0040},    {"r3", 0x0080},
    {"lb", 0x0100},    {"rb", 0x0200},   {"a", 0x1000},     {"b", 0x2000},
    {"x", 0x4000},     {"y", 0x8000},
};

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ParseScript(const std::string& path, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open";
    return false;
  }
  int64_t t = 0;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    lineno++;
    if (const size_t hash = line.find('#'); hash != std::string::npos) {
      line.resize(hash);
    }
    std::istringstream ss(line);
    std::string tok;
    if (!(ss >> tok)) {
      continue;
    }
    if (tok == "@mark" || tok == "@capture") {
      std::string name;
      if (!(ss >> name)) {
        error = "line " + std::to_string(lineno) + ": " + tok + " needs a name";
        return false;
      }
      g_events.push_back({t, tok == "@capture", name});
      continue;
    }
    int64_t duration = 0;
    try {
      duration = std::stoll(tok);
    } catch (...) {
      duration = 0;
    }
    if (duration <= 0) {
      error = "line " + std::to_string(lineno) + ": expected a positive duration, got '" + tok + "'";
      return false;
    }
    Step step{t, t + duration, 0, 0, 0, 0, 0, 0, 0};
    while (ss >> tok) {
      if (const size_t eq = tok.find('='); eq != std::string::npos) {
        const std::string key = tok.substr(0, eq);
        int value = 0;
        try {
          value = std::stoi(tok.substr(eq + 1));
        } catch (...) {
          error = "line " + std::to_string(lineno) + ": bad value in '" + tok + "'";
          return false;
        }
        if (key == "lx" || key == "ly" || key == "rx" || key == "ry") {
          const int16_t axis = static_cast<int16_t>(ClampInt(value, -100, 100) * 32767 / 100);
          (key == "lx" ? step.lx : key == "ly" ? step.ly : key == "rx" ? step.rx : step.ry) = axis;
        } else if (key == "lt" || key == "rt") {
          const uint8_t trigger = static_cast<uint8_t>(ClampInt(value, 0, 100) * 255 / 100);
          (key == "lt" ? step.left_trigger : step.right_trigger) = trigger;
        } else {
          error = "line " + std::to_string(lineno) + ": unknown axis '" + key + "'";
          return false;
        }
        continue;
      }
      bool found = false;
      for (const ButtonName& b : kButtons) {
        if (tok == b.name) {
          step.buttons |= b.bit;
          found = true;
        }
      }
      if (!found) {
        error = "line " + std::to_string(lineno) + ": unknown button '" + tok + "'";
        return false;
      }
    }
    g_steps.push_back(step);
    t += duration;
  }
  g_total_ms = t;
  if (g_steps.empty()) {
    error = "no steps";
    return false;
  }
  return true;
}

// Saves the latest guest output frame. Runs on the controller thread, never on a guest one.
void CaptureFrame(const std::string& dir, const std::string& name, int64_t t) {
  if (dir.empty()) {
    return;
  }
  auto* kernel = REX_KERNEL_STATE();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* graphics =
      runtime ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system()) : nullptr;
  rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
  rex::ui::RawImage image;
  if (!presenter || !presenter->CaptureGuestOutput(image) || image.width == 0 ||
      image.height == 0) {
    REXLOG_WARN("input script: capture {} at t={} ms failed - no guest output to read", name, t);
    return;
  }
  char file[512];
  std::snprintf(file, sizeof(file), "%s/%06lld_%s.ppm", dir.c_str(), static_cast<long long>(t),
                name.c_str());
  std::FILE* out = std::fopen(file, "wb");
  if (!out) {
    REXLOG_WARN("input script: cannot write {}", file);
    return;
  }
  std::fprintf(out, "P6\n%u %u\n255\n", image.width, image.height);
  std::vector<uint8_t> row(size_t(image.width) * 3);
  for (uint32_t y = 0; y < image.height; y++) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < image.width; x++) {
      std::memcpy(&row[size_t(x) * 3], src + size_t(x) * 4, 3);
    }
    std::fwrite(row.data(), 1, row.size(), out);
  }
  std::fclose(out);
  REXLOG_INFO("input script: captured {} ({}x{})", file, image.width, image.height);
}

void ControllerMain() {
  const std::string path = REXCVAR_GET(skate3_input_script);
  if (path.empty()) {
    return;
  }
  std::string error;
  if (!ParseScript(path, error)) {
    REXLOG_ERROR("input script: '{}' rejected - {}", path, error);
    return;
  }
  const int64_t settle = REXCVAR_GET(skate3_input_script_settle_ms);
  const std::string capture_dir = REXCVAR_GET(skate3_input_capture_dir);
  const int64_t every = REXCVAR_GET(skate3_input_capture_every_ms);
  REXLOG_INFO("input script: loaded '{}' - {} steps, {} markers, {} ms long", path,
              g_steps.size(), g_events.size(), g_total_ms);

  while (rex::kernel::guest_presence::GameplayContextValue() != 1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const int64_t start = NowMs() + settle;
  g_start_ms.store(start, std::memory_order_relaxed);
  REXLOG_INFO("input script: gameplay reached; starting in {} ms", settle);

  size_t next_event = 0;
  int64_t next_periodic = 0;
  int periodic = 0;
  uint64_t logged_wipeouts = g_wipeout_events.load(std::memory_order_relaxed);
  // The last marker passed, so a bail names what it followed instead of leaving the
  // timestamps to be matched by hand.
  std::string last_mark = "start";
  int64_t last_mark_ms = 0;
  const uint64_t polls_at_start = g_input_polls.load(std::memory_order_relaxed);
  bool rate_logged = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int64_t t = NowMs() - start;
    if (t < 0) {
      continue;
    }
    while (next_event < g_events.size() && g_events[next_event].at_ms <= t) {
      const Event& ev = g_events[next_event++];
      REXLOG_INFO("input script: t={} ms {} {}", t, ev.capture ? "capture" : "mark", ev.name);
      last_mark = ev.name;
      last_mark_ms = ev.at_ms;
      if (ev.capture) {
        CaptureFrame(capture_dir, ev.name, t);
      }
    }
    if (!rate_logged && t >= 2000) {
      rate_logged = true;
      REXLOG_INFO("input script: {} pad polls in the first 2000 ms",
                  g_input_polls.load(std::memory_order_relaxed) - polls_at_start);
    }
    const uint64_t wipeouts = g_wipeout_events.load(std::memory_order_relaxed);
    if (wipeouts != logged_wipeouts) {
      logged_wipeouts = wipeouts;
      REXLOG_INFO("input script: t={} ms WIPEOUT #{} - {} ms after '{}' (player {:08X})", t,
                  wipeouts, t - last_mark_ms, last_mark,
                  g_last_wipeout_object.load(std::memory_order_relaxed));
      CaptureFrame(capture_dir, "wipeout" + std::to_string(wipeouts), t);
    }
    if (every > 0 && t >= next_periodic) {
      char name[32];
      std::snprintf(name, sizeof(name), "every%03d", periodic++);
      CaptureFrame(capture_dir, name, t);
      next_periodic += every;
    }
    if (t >= g_total_ms) {
      g_done.store(true, std::memory_order_relaxed);
      REXLOG_INFO("input script: complete after {} ms - {} wipeouts ({} true of {} "
                  "IsWipeoutRequested polls)",
                  g_total_ms, g_wipeout_events.load(std::memory_order_relaxed),
                  g_wipeout_true_polls.load(std::memory_order_relaxed),
                  g_wipeout_polls.load(std::memory_order_relaxed));
      for (size_t i = 0; i < kMaxWipeoutPlayers; i++) {
        const uint32_t object = g_wipeout_player[i].load(std::memory_order_relaxed);
        if (object == 0) break;
        REXLOG_INFO("input script:   player {:08X}: {} wipeouts", object,
                    g_wipeout_player_events[i].load(std::memory_order_relaxed));
      }
      CaptureFrame(capture_dir, "end", t);
      return;
    }
  }
}

PPCFunc* ResolveOriginal() {
  auto* original = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, kImport));
  if (original == nullptr || original == &__imp__XamInputGetState) {
    const char* why = dlerror();
    REXLOG_ERROR("input script: cannot resolve the runtime's {} ({})", kImport,
                 why ? why : "resolved to this override");
    std::abort();
  }
  return original;
}

void StoreU16(uint8_t* base, uint32_t addr, uint16_t v) {
  REX_STORE_U8(addr, static_cast<uint8_t>(v >> 8));
  REX_STORE_U8(addr + 1, static_cast<uint8_t>(v));
}

}  // namespace

extern "C" REX_FUNC(__imp__XamInputGetState) {
  static PPCFunc* const original = ResolveOriginal();
  static std::once_flag started;
  std::call_once(started, [] { std::thread(ControllerMain).detach(); });

  const uint32_t user = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t state = ctx.r5.u32;
  original(ctx, base);
  g_input_polls.fetch_add(1, std::memory_order_relaxed);

  const int64_t start = g_start_ms.load(std::memory_order_relaxed);
  if (start < 0 || g_done.load(std::memory_order_relaxed) || state == 0) {
    return;
  }
  if ((user & 0xFF) != 0 && (user & 0xFF) != 0xFF) {
    return;  // player one only
  }
  if ((flags & 0xFF) != 0 && (flags & kFlagGamepad) == 0) {
    return;  // a query for some other device type
  }
  const int64_t t = NowMs() - start;
  if (t < 0 || t >= g_total_ms) {
    return;
  }
  size_t i = g_cursor.load(std::memory_order_relaxed);
  while (i + 1 < g_steps.size() && g_steps[i].end_ms <= t) {
    i++;
  }
  g_cursor.store(i, std::memory_order_relaxed);
  const Step& s = g_steps[i];

  // No pad plugged in: present a connected one, so the script works unattended either way.
  if (ctx.r3.u32 != 0) {
    for (uint32_t k = 0; k < 16; k++) {
      REX_STORE_U8(state + k, 0);
    }
    ctx.r3.u64 = 0;
  }
  // A new packet number per step, so a game that skips unchanged packets sees each one.
  REX_STORE_U32(state, 0x5C000001u + static_cast<uint32_t>(i) * 2);
  StoreU16(base, state + 4, s.buttons);
  REX_STORE_U8(state + 6, s.left_trigger);
  REX_STORE_U8(state + 7, s.right_trigger);
  StoreU16(base, state + 8, static_cast<uint16_t>(s.lx));
  StoreU16(base, state + 10, static_cast<uint16_t>(s.ly));
  StoreU16(base, state + 12, static_cast<uint16_t>(s.rx));
  StoreU16(base, state + 14, static_cast<uint16_t>(s.ry));
}

// PhysicalPlayerHiLOD::IsWipeoutRequested. Polled per physical player several times a
// frame, and true on many consecutive frames for one fall, so true polls closer than
// kWipeoutGapMs to the previous true poll belong to the same bail.
extern "C" REX_FUNC(sub_82DB9100) {
  const uint32_t player = ctx.r3.u32;  // the object, before the call overwrites r3
  __imp__sub_82DB9100(ctx, base);
  g_wipeout_polls.fetch_add(1, std::memory_order_relaxed);
  if ((ctx.r3.u32 & 0xFFu) == 0) {
    return;
  }
  g_wipeout_true_polls.fetch_add(1, std::memory_order_relaxed);
  const int64_t now = NowMs();
  const int64_t previous = g_last_wipeout_true_ms.exchange(now, std::memory_order_relaxed);
  if (now - previous >= kWipeoutGapMs) {
    RecordWipeoutPlayer(player);
    g_wipeout_events.fetch_add(1, std::memory_order_relaxed);
  }
}
