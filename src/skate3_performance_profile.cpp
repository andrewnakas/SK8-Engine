#include "skate3_performance_profile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <string>
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

REXCVAR_DEFINE_STRING(skate3_performance_profile, "auto", "Skate 3",
                      "Video preset: 'quality' (desktop discrete GPU - supersampled, 4x MSAA, "
                      "every effect), 'balanced' (native resolution, effects kept), 'deck' "
                      "(handhelds and integrated GPUs), 'performance' (every frame that can be "
                      "had - no shadows, no bloom, no ambient occlusion), or 'auto' to pick from "
                      "the hardware on "
                      "the first run only. A preset only ever sets defaults - anything you "
                      "change in the settings screen is kept.")
    .allowed({"auto", "quality", "balanced", "deck", "performance"});

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
  if (LooksLikeSteamDeck()) {
    return Profile::kDeck;
  }
  // A machine with no discrete GPU wants the handheld preset too, and this is
  // the case that actually bites: measured on this laptop's integrated Intel
  // UHD at 1280x800, the quality preset runs at 7.4 fps and the deck preset at
  // 36. Recognising only Steam Decks left every mini-PC, non-Valve handheld
  // and integrated-graphics laptop on a preset their GPU cannot render.
  if (!HasDiscreteGpu()) {
    return Profile::kDeck;
  }
  // Otherwise treat it as a desktop. Guessing "slow" for a machine that is
  // not would quietly halve the picture on hardware that could hold it, and
  // the settings screen is one keypress away either direction.
  return Profile::kQuality;
}

std::string_view ProfileName(Profile profile) {
  switch (profile) {
    case Profile::kPerformance:
      return "performance";
    case Profile::kDeck:
      return "deck";
    case Profile::kBalanced:
      return "balanced";
    case Profile::kQuality:
      break;
  }
  return "quality";
}

void ApplyProfile(Profile profile) {
  // Every preset sets the SAME set of cvars, so switching between two of them
  // cannot leave a knob behind at the other one's value.
  struct Preset {
    const char* resolution_scale;
    const char* msaa;
    const char* draw_distance;
    const char* lod_distance;
    const char* ssao;
    const char* shafts;
    const char* bloom;
    const char* shadows;
    const char* shadow_pcss;
    const char* shadow_static_casters;
    const char* shadow_static_size;
  };

  static constexpr Preset kQuality = {"2", "4", "2.0", "2.0", "true",
                                      "true", "true", "true", "true", "true", "4096"};
  static constexpr Preset kBalanced = {"1", "4", "2.0", "2.0", "true",
                                       "true", "true", "true", "true", "true", "2048"};
  // The handheld preset. On this machine's integrated GPU (Intel RPL-P) at
  // 1280x800, each figure a median over 600-frame present-cadence windows of
  // real gameplay:
  //
  //   shipped defaults                             7.4 fps
  //   native resolution alone                     23.8
  //     + MSAA 4x -> 1x                           29.6   (+24%)
  //     + static shadow casters off               28.8   (+21%)
  //     + PCSS soft shadows off                   27.6   (+16%)
  //   this preset (all of the above)              35.9
  //   the same, but with those three effects on   27.6
  //
  // Supersampling is most of it - the shipped default renders 2560x1600 on a
  // part that cannot hold it - but the effects cost a further 23% on top, so
  // both halves of this preset earn their place.
  //
  // Three knobs are deliberately LEFT ON: SSAO (24.2), light shafts (24.7) and
  // halved draw distance (24.0) each measured inside the run-to-run spread of
  // the 23.8 base. Together they are worth about 5.5% (35.9 against 38.0 with
  // them off too), which is a fair price for ambient occlusion, god rays and a
  // world that does not pop in.
  //
  // This preset is GPU-bound, and the numbers above are a statement about a
  // WEAK GPU rather than about the game. The same build on a discrete RTX
  // 4050, with the full quality preset, runs at 169.6 fps - 5.9 ms a frame
  // against 27.8 here - so the guest CPU work is at most ~6 ms and everything
  // else is the integrated part being slow.
  //
  // Do not be misled by a CPU profile: the guest `render_thread` sits at 99.7%
  // of a core while GPU-bound, because it spins waiting on completion. That
  // reads exactly like a CPU wall and is not one. Screen resolution is a poor
  // probe too - a nine-fold pixel range moves this only 9% (640x400 -> 37.3,
  // 1920x1200 -> 34.1) because the bottleneck is per-draw and shadow work,
  // not fill rate. The honest test is a faster GPU, and it says the ceiling
  // here is the GPU.
  static constexpr Preset kDeck = {"1", "1", "2.0", "2.0", "true",
                                   "true", "true", "true", "false", "false", "1024"};

  // Frames first, picture last, for the player who wants every one they can
  // get. Measured on the Intel UHD at 1280x800 against the deck preset's 36.0:
  //
  //   36.0  deck
  //   37.5  + HDR intermediate and bloom off
  //   40.1  + shadows off entirely        <- the one real win, +11%
  //   40.7  + HDR/bloom and shadows off
  //   42.8  + SSAO and light shafts off   <- this preset
  //
  // Shadows are most of it. Draw distance stays at 2.0: measured on its own it
  // changed nothing (36.3 at half, 36.0 at a quarter), so cutting it would buy
  // pop-in for free.
  //
  // The shadow toggle is what a player actually notices - with it off the
  // skater stops reading against the ground - which is why this is a separate
  // preset and not the integrated-GPU default.
  static constexpr Preset kPerformance = {"1", "1", "2.0", "2.0", "false",
                                          "false", "false", "false", "false", "false", "1024"};

  const Preset& preset = profile == Profile::kPerformance ? kPerformance
                         : profile == Profile::kDeck       ? kDeck
                         : profile == Profile::kBalanced   ? kBalanced
                                                           : kQuality;

  // A cvar the player set EXPLICITLY wins over the preset.
  //
  // This was a plain SetFlagByName, which clobbered them - and it did real
  // damage before it was caught. A whole sweep measuring
  // `--skate3_performance_profile=deck --skate3_native_render_scene_msaa=2`
  // silently rendered at MSAA x1, because the preset overwrote the flag after
  // the command line was parsed. Four configurations "agreed" at 36 fps for
  // the excellent reason that they were all the same configuration, and the
  // conclusion drawn from them - that the effects were free - was backwards.
  // Only the renderer's own `pipelines created (MSAA xN)` line exposed it.
  //
  // The same trap is waiting for any player who pairs a preset with a tweak.
  const auto set = [](const char* name, const char* value) {
    if (NamedOnCommandLine(name)) {
      REXLOG_INFO("Skate 3 video preset: keeping explicit {} = {}", name,
                  rex::cvar::GetFlagByName(name));
      return;  // the player asked for this one by name; leave it alone
    }
    rex::cvar::SetFlagByName(name, value);
  };

  set("resolution_scale", preset.resolution_scale);
  set("draw_resolution_scale_x", preset.resolution_scale);
  set("draw_resolution_scale_y", preset.resolution_scale);
  set("skate3_native_render_scene_msaa", preset.msaa);
  set("skate3_draw_distance_scale", preset.draw_distance);
  set("skate3_lod_distance_scale", preset.lod_distance);
  set("skate3_native_render_scene_ssao", preset.ssao);
  set("skate3_native_render_scene_shafts", preset.shafts);
  set("skate3_native_render_scene_bloom", preset.bloom);
  set("skate3_native_render_scene_shadows", preset.shadows);
  set("skate3_native_render_scene_shadow_pcss", preset.shadow_pcss);
  set("skate3_native_render_scene_shadow_static_casters", preset.shadow_static_casters);
  set("skate3_native_render_scene_shadow_static_size", preset.shadow_static_size);

  REXLOG_INFO("Skate 3 video preset: {}", ProfileName(profile));
}

void InstallLiveProfileSwitch() {
  // The in-game Video menu changes this cvar; apply the preset there and then
  // rather than only at startup, so picking one takes effect immediately.
  //
  // Re-entrancy is why this only acts on a real preset name: ApplyProfile sets
  // a dozen cvars, and if any of them were this one the callback would recurse.
  // None is - and "auto" is deliberately ignored here, because the menu writes
  // it back after applying so the choice does not re-run at every launch and
  // overwrite whatever the player tunes by hand afterwards.
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  rex::cvar::RegisterChangeCallback(
      "skate3_performance_profile", [](std::string_view, std::string_view value) {
        const std::string requested = ToLower(std::string(value));
        if (requested == "quality") {
          ApplyProfile(Profile::kQuality);
        } else if (requested == "balanced") {
          ApplyProfile(Profile::kBalanced);
        } else if (requested == "deck") {
          ApplyProfile(Profile::kDeck);
        } else if (requested == "performance") {
          ApplyProfile(Profile::kPerformance);
        }
      });
}

bool ApplyRequestedProfile(bool first_run) {
  const std::string requested = ToLower(REXCVAR_GET(skate3_performance_profile));

  if (requested == "quality") {
    ApplyProfile(Profile::kQuality);
    return true;
  }
  if (requested == "balanced") {
    ApplyProfile(Profile::kBalanced);
    return true;
  }
  if (requested == "deck") {
    ApplyProfile(Profile::kDeck);
    return true;
  }
  if (requested == "performance") {
    ApplyProfile(Profile::kPerformance);
    return true;
  }

  // "auto" only ever acts on a FIRST run. On any later launch the player's
  // saved settings are the answer, and re-deriving a preset would silently
  // undo whatever they changed in the settings screen.
  if (!first_run) {
    return false;
  }
  const Profile detected = DetectProfile();
  REXLOG_INFO("Skate 3 video preset: no saved settings; detected '{}' for this machine",
              ProfileName(detected));
  ApplyProfile(detected);
  return true;
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

  ApplyProfile(Profile::kDeck);

  // The panel's own refresh rate, as the frame cap. A cap the panel can
  // actually hold beats a higher target it cannot: on a handheld a steady 60
  // reads better than an unsteady 75, and the OLED's 90 is real headroom the
  // LCD does not have.
  //
  // skate3_guest_fps_cap, not video_mode_refresh_rate. The guest cap is what
  // the frame pacer actually reads on this platform, and it is one of the
  // settings that gets written back to settings.toml - video_mode_refresh_rate
  // is neither, so setting it looked right and did nothing that survived the
  // session.
  const double refresh = model == DeckModel::kOled ? 90.0 : 60.0;
  if (!rex::cvar::HasNonDefaultValue("skate3_guest_fps_cap")) {
    rex::cvar::SetFlagByName("skate3_guest_fps_cap", std::to_string(refresh));
    rex::cvar::SetFlagByName("skate3_guest_fps_cap_auto", "false");
  }
  REXLOG_INFO("Steam Deck ({}) detected: handheld preset applied, frame cap {} Hz",
              model == DeckModel::kOled ? "OLED / Galileo" : "LCD / Jupiter", refresh);

  std::filesystem::create_directories(user_data_root, ec);
  std::ofstream(marker) << "The handheld preset and this panel's frame cap were applied once.\n"
                           "Delete this file to have them applied again.\n";
  return true;
}

}  // namespace skate3::perf
