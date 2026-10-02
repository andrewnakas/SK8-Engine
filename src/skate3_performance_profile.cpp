#include "skate3_performance_profile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

REXCVAR_DEFINE_STRING(
    skate3_performance_profile, "", "Skate 3",
    "Apply a video preset at startup: 'potato' (every frame the machine has - no "
    "shadows, no effects, no HDR, half the original draw distance), 'performance', "
    "'balanced', 'quality' or 'ultra'. Empty (the default) leaves the settings alone, except on "
    "a first run, when one is picked from the hardware ('deck' is an alias for performance). "
    "A preset only fills in the settings you have not named yourself: anything set "
    "explicitly on the command line, or in ios_args.txt, wins over it.");

namespace skate3 {

void ApplyRequestedPerformanceProfile() {
  const std::string requested = REXCVAR_GET(skate3_performance_profile);
  if (requested.empty() || requested == "none") {
    return;
  }
  if (rex::ui::ApplyGraphicsPresetByName(requested)) {
    return;
  }

  // Say which names would have worked. A misspelled preset that silently does
  // nothing looks exactly like a preset that applied and changed nothing, and
  // telling those apart by watching frame times is the guessing this exists to
  // remove.
  std::string valid;
  for (const std::string& name : rex::ui::GraphicsPresetNames()) {
    valid += valid.empty() ? "" : ", ";
    valid += name;
  }
  REXLOG_WARN("skate3_performance_profile='{}' is not a preset; nothing applied. Try one of: {}",
              requested, valid);
}

}  // namespace skate3

namespace skate3::perf {
namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string ReadFirstLine(const char* path) {
  std::ifstream file(path);
  std::string line;
  if (file && std::getline(file, line)) {
    return ToLower(line);
  }
  return {};
}

// Every cvar named on the command line, as `--name` or `--name=value`.
//
// There is no cvar API for "was this set explicitly": HasNonDefaultValue only
// compares against the default, so `--skate3_native_render_scene_msaa=4` on a
// flag that already defaults to 4 is indistinguishable from silence - and that
// is exactly the case a preset must not overwrite. So read the real command
// line instead of inferring it.
const std::vector<std::string>& CommandLineFlagNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    std::vector<std::string> args;
#if defined(_WIN32)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
      for (int i = 0; i < argc; ++i) {
        std::wstring wide(argv[i]);
        args.emplace_back(wide.begin(), wide.end());
      }
      LocalFree(argv);
    }
#elif defined(__APPLE__)
    // argv is null-terminated, so _NSGetArgc is not needed.
    if (char*** argv = _NSGetArgv(); argv && *argv) {
      for (char** it = *argv; *it; ++it) {
        args.emplace_back(*it);
      }
    }
#else
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    for (char c; cmdline.get(c);) {
      if (c == '\0') {
        if (!arg.empty()) {
          args.push_back(arg);
          arg.clear();
        }
      } else {
        arg.push_back(c);
      }
    }
    if (!arg.empty()) {
      args.push_back(arg);
    }
#endif
    for (const std::string& value : args) {
      if (value.rfind("--", 0) != 0) {
        continue;
      }
      std::string name = value.substr(2);
      if (const size_t eq = name.find('='); eq != std::string::npos) {
        name.resize(eq);
      }
      if (!name.empty()) {
        out.push_back(ToLower(std::move(name)));
      }
    }
    return out;
  }();
  return names;
}

bool NamedOnCommandLine(const char* name) {
  const auto& names = CommandLineFlagNames();
  return std::find(names.begin(), names.end(), ToLower(name)) != names.end();
}

bool EnvSet(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' && std::string(value) != "0";
}

// A Steam Deck, by the two things it says about itself before any graphics
// device exists. Valve sets SteamDeck=1 in the game session; the DMI names are
// what identifies the hardware when the game is launched outside Steam.
bool LooksLikeSteamDeck() {
  if (EnvSet("SteamDeck")) {
    return true;
  }
#if defined(__linux__)
  static constexpr std::array<const char*, 2> kDmiPaths = {
      "/sys/devices/virtual/dmi/id/product_name",
      "/sys/devices/virtual/dmi/id/board_name",
  };
  for (const char* path : kDmiPaths) {
    const std::string value = ReadFirstLine(path);
    // Jupiter is the LCD Deck, Galileo the OLED.
    if (value.find("jupiter") != std::string::npos ||
        value.find("galileo") != std::string::npos) {
      return true;
    }
  }
#endif
  return false;
}

// Whether this machine has a GPU that is not the one wired to the chipset.
//
// The preset has to be chosen before Vulkan exists - it runs while paths are
// still being resolved - so the graphics API cannot be asked, and the DRM
// nodes are read instead. A GPU's PCI address is the tell: an integrated part
// hangs off bus 00 (0000:00:02.0 here), while a discrete card sits on a bus of
// its own behind a bridge (0000:01:00.0). Vendor IDs cannot do this job - AMD
// ships both APUs and discrete cards under one id.
//
// Unknown counts as "has a discrete GPU", so anything unrecognised keeps the
// full picture rather than being quietly downgraded.
bool HasDiscreteGpu() {
#if defined(__linux__)
  std::error_code ec;
  std::filesystem::directory_iterator it("/sys/class/drm", ec);
  if (ec) {
    return true;
  }
  bool saw_any_gpu = false;
  for (const auto& entry : it) {
    const std::string name = entry.path().filename().string();
    // cardN only - skip the cardN-HDMI-A-1 connector nodes.
    if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) {
      continue;
    }
    std::error_code link_ec;
    const auto device = std::filesystem::canonical(entry.path() / "device", link_ec);
    if (link_ec) {
      continue;
    }
    // Display controllers only (class 0x03xxxx); a render-only node is not a
    // GPU anyone presents from.
    const std::string cls = ReadFirstLine((entry.path() / "device/class").c_str());
    if (cls.rfind("0x03", 0) != 0) {
      continue;
    }
    saw_any_gpu = true;
    const std::string pci = device.filename().string();  // e.g. 0000:01:00.0
    if (pci.size() >= 8 && pci.compare(5, 2, "00") != 0) {
      return true;
    }
  }
  // No DRM nodes at all means this probe did not work; do not downgrade on it.
  return !saw_any_gpu;
#else
  return true;
#endif
}

}  // namespace

Profile DetectProfile() {
  // A Steam Deck, or any machine with no discrete GPU, gets the Performance
  // preset: measured on an integrated Intel UHD at 1280x800 the old desktop
  // defaults ran at 7.4 fps and the handheld bundle at 36. Anything not
  // recognised is treated as a desktop, so a fast machine is never downgraded.
  if (LooksLikeSteamDeck() || !HasDiscreteGpu()) {
    return Profile::kHandheld;
  }
  return Profile::kDesktop;
}

std::string_view ProfileName(Profile profile) {
  // A desktop starts on Balanced: the console's own draw distance, which is
  // what the phones stream with, rather than Quality's doubled radius - four
  // times the world area to stream, and the hitches at speed that come with
  // it. Quality and Ultra are one row away in the settings screen.
  return profile == Profile::kHandheld ? "performance" : "balanced";
}

namespace {

// The phones' low-end tier, for a handheld or a machine with no discrete GPU.
// Only first-run defaults: anything named on the command line is left alone.
void ApplyLowEndTier() {
  struct Default {
    const char* name;
    const char* value;
  };
  static constexpr Default kLowEnd[] = {
      // Drop the top mip of anything sizeable and a second from the giants:
      // 16x less texture decode on exactly the textures that fill the store.
      {"skate3_native_render_scene_tex_base_mip_px", "256"},
      {"skate3_native_render_scene_tex_base_mip2_px", "1024"},
      // Build the suppressed static packets and simulate the crowd every
      // other frame (Tab A7 Lite: 6 fps -> 56 in the world).
      {"skate3_native_render_guest_static_refresh", "2"},
      {"skate3_native_render_lw_refresh", "2"},
      // Spin less against the threads being waited on.
      {"gpu_idle_spin_iterations", "32"},
      {"rtl_critical_section_max_spin", "256"},
  };
  for (const Default& d : kLowEnd) {
    if (!NamedOnCommandLine(d.name)) {
      rex::cvar::SetFlagByName(d.name, d.value);
    }
  }
  REXLOG_INFO("Skate 3 low-end tier applied (mip drop, half-rate crowd and static packets)");
}

}  // namespace

namespace {

bool ApplyNamed(std::string requested) {
  requested = ToLower(std::move(requested));
  if (requested == "deck") {
    requested = "performance";
  }
  if (requested.empty() || requested == "auto" || requested == "none") {
    return false;
  }
  return rex::ui::ApplyGraphicsPresetByName(requested);
}

}  // namespace

bool ApplyRequestedProfile(bool first_run) {
  if (ApplyNamed(REXCVAR_GET(skate3_performance_profile))) {
    return true;
  }
  // Only a FIRST run picks from the hardware. On every later launch the
  // player's saved settings are the answer.
  if (!first_run) {
    return false;
  }
  const Profile detected = DetectProfile();
  REXLOG_INFO("Skate 3 video preset: no saved settings; detected '{}' for this machine",
              ProfileName(detected));
  if (detected == Profile::kHandheld) {
    ApplyLowEndTier();
  }
  return rex::ui::ApplyGraphicsPresetByName(ProfileName(detected));
}

void InstallLiveProfileSwitch() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  rex::cvar::RegisterChangeCallback(
      "skate3_performance_profile",
      [](std::string_view, std::string_view value) { ApplyNamed(std::string(value)); });
}

DeckModel DetectSteamDeckModel() {
#if defined(__linux__)
  static constexpr std::array<const char*, 2> kDmiPaths = {
      "/sys/devices/virtual/dmi/id/product_name",
      "/sys/devices/virtual/dmi/id/board_name",
  };
  for (const char* path : kDmiPaths) {
    const std::string value = ReadFirstLine(path);
    if (value.find("galileo") != std::string::npos) {
      return DeckModel::kOled;
    }
    if (value.find("jupiter") != std::string::npos) {
      return DeckModel::kLcd;
    }
  }
#endif
  // SteamDeck=1 without a readable board name: it is a Deck, and the LCD's
  // 60Hz is the safe guess - capping an OLED at 60 costs frames, while letting
  // an LCD try for 90 asks the panel for a rate it does not have.
  if (EnvSet("SteamDeck")) {
    return DeckModel::kLcd;
  }
  return DeckModel::kNotDeck;
}

bool ApplyDeckDefaultsOnce(const std::filesystem::path& user_data_root) {
  const DeckModel model = DetectSteamDeckModel();
  if (model == DeckModel::kNotDeck) {
    return false;
  }
  std::error_code ec;
  const auto marker = user_data_root / ".deck-defaults-applied";
  if (std::filesystem::exists(marker, ec)) {
    return false;
  }
  rex::ui::ApplyGraphicsPresetByName("performance");
  ApplyLowEndTier();
  // The panel's own refresh rate as the frame cap: a steady 60 on the LCD
  // reads better than an unsteady 75, and the OLED's 90 is real headroom.
  const double refresh = model == DeckModel::kOled ? 90.0 : 60.0;
  if (!rex::cvar::HasNonDefaultValue("skate3_guest_fps_cap")) {
    rex::cvar::SetFlagByName("skate3_guest_fps_cap", std::to_string(refresh));
    rex::cvar::SetFlagByName("skate3_guest_fps_cap_auto", "false");
  }
  REXLOG_INFO("Steam Deck ({}) detected: performance preset applied, frame cap {} Hz",
              model == DeckModel::kOled ? "OLED / Galileo" : "LCD / Jupiter", refresh);
  std::filesystem::create_directories(user_data_root, ec);
  std::ofstream(marker) << "The handheld preset and this panel's frame cap were applied once.\n"
                           "Delete this file to have them applied again.\n";
  return true;
}

}  // namespace skate3::perf
