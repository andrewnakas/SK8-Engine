#include "skate3_live_cvars.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

// Changing one cvar on a phone costs a push_args.sh, a force-stop, a relaunch,
// and then several minutes of navigating menus back to the place the thing
// being measured actually happens. Across a dozen A/B levers that is hours of
// somebody's evening, and it is why levers with a written, tested
// implementation have sat at their defaults for months without a number.
//
// The SDK already has a runtime setter, rex::cvar::SetFlagByName, reachable
// from the console overlay - which is bound to the Backtick key and has no
// gamepad chord, so on a phone it cannot be opened at all. The cvar existing
// and the cvar being reachable are different things; that distinction has
// already cost this project four shipped features nobody could turn on.
//
// So: watch a file. `adb push` it and the change lands within a second, in the
// middle of a run, with no relaunch and no menu navigation.
//
//   adb shell "echo skate3_guest_spin_yield=1 > \
//     /sdcard/Android/data/com.nakas.skate3/files/user/live_cvars.txt"
//
// Only cvars that are genuinely hot - read fresh each time they are used -
// respond mid-frame. A kRequiresRestart cvar will be set and will do nothing
// visible, which is why the applied value is logged with its lifecycle rather
// than silently accepted.
REXCVAR_DEFINE_BOOL(skate3_live_cvars, false, "Skate 3",
                    "Watch user/live_cvars.txt and apply 'name=value' lines to the running "
                    "game. For A/B measurement on a device, where a relaunch costs minutes of "
                    "navigating back to the thing being measured.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(skate3_live_cvars_poll_ms, 500, "Skate 3",
                     "How often to stat the live cvar file, milliseconds.")
    .range(50, 10000);

REXCVAR_DECLARE(std::string, user_data_root);

namespace skate3::live_cvars {
namespace {

std::atomic<bool> g_installed{false};

// Applied values are remembered so an unchanged line is not re-applied and
// re-logged every poll: the file is rewritten wholesale by `adb push`, so its
// mtime moves even when only one of ten lines actually changed.
void ApplyFile(const std::filesystem::path& path,
               std::unordered_map<std::string, std::string>& applied) {
  std::ifstream in(path);
  if (!in) {
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (const auto cr = line.find('\r'); cr != std::string::npos) {
      line.erase(cr);
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    const auto eq = line.find('=', first);
    if (eq == std::string::npos) {
      REXLOG_WARN("[live-cvars] ignoring '{}': expected name=value", line);
      continue;
    }
    std::string name = line.substr(first, eq - first);
    std::string value = line.substr(eq + 1);
    // Trim both halves; a trailing space on a bool is not a bool.
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    const auto vfirst = value.find_first_not_of(" \t");
    value = vfirst == std::string::npos ? std::string() : value.substr(vfirst);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    if (name.empty()) {
      continue;
    }
    if (const auto seen = applied.find(name); seen != applied.end() && seen->second == value) {
      continue;
    }
    if (rex::cvar::SetFlagByName(name, value)) {
      applied[name] = value;
      // WARN, not INFO: phones ship at log_level=warn, and a confirmation
      // nobody can see is indistinguishable from the code never having run.
      REXLOG_WARN("[live-cvars] {} = {}", name, value);
    } else {
      applied[name] = value;  // do not retry a bad name every poll
      REXLOG_WARN("[live-cvars] unknown cvar '{}' - not applied", name);
    }
  }
}

}  // namespace

void Install() {
  if (!REXCVAR_GET(skate3_live_cvars)) {
    return;
  }
  if (g_installed.exchange(true)) {
    return;
  }

  std::filesystem::path root = REXCVAR_GET(user_data_root);
  if (root.empty()) {
    REXLOG_WARN("[live-cvars] no user_data_root; not watching");
    return;
  }
  const std::filesystem::path path = root / "live_cvars.txt";
  REXLOG_WARN("[live-cvars] watching {}", path.string());

  std::thread([path] {
    rex::thread::set_current_thread_name("live_cvars");
    std::unordered_map<std::string, std::string> applied;
    std::filesystem::file_time_type last{};
    for (;;) {
      std::error_code ec;
      const auto stamp = std::filesystem::last_write_time(path, ec);
      if (!ec && stamp != last) {
        last = stamp;
        ApplyFile(path, applied);
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(REXCVAR_GET(skate3_live_cvars_poll_ms)));
    }
  }).detach();
}

}  // namespace skate3::live_cvars
