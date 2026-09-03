#include "skate3_app_common.h"

#if defined(__ANDROID__)
#include "skate3_android_bridge.h"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "skate3_demo_path.h"
#include "skate3_diagnostics.h"
#include "skate3_fov.h"
#include "skate3_guest_trace.h"
#include "skate3_iso_installer.h"
#include "skate3_native_render.h"
#include "skate3_pack_select.h"
#include "skate3_performance_profile.h"
#include <rex/ui/windowed_app_context_sdl.h>

REXCVAR_DEFINE_BOOL(skate3_content_pack_menu, false, "Skate 3",
                    "Ask which content pack to load when several are installed. Off: the first "
                    "by name is staged. The chooser runs from OnPostSetup, where the UI is not "
                    "yet painting - it blocks there and the screen stays black, so it needs a "
                    "different insertion point before it can be on by default.");

REXCVAR_DEFINE_STRING(skate3_content_pack, "", "Skate 3",
                      "Which custom content pack in Documents to stage this launch, by folder "
                      "name. Empty stages the first by name. Only one is staged per launch: the "
                      "game's boot content scan does not cope with several at once.");
#include "skate3_touch_controls.h"
#include "skate3_native_scene.h"
#include "skate3_crash_report.h"
#include "skate3_screenshot.h"
#include "skate3_shader_disasm.h"
#include "skate3_win_icon.h"
#include "skate3_title_update_installer.h"
#include "skate3_user_settings.h"
#include "skate3_warp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <spawn.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/native_guest_renderer.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/perf/counter.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include <imgui.h>
#include <toml++/toml.hpp>

#if defined(__linux__) || defined(__APPLE__)
extern char** environ;
#endif

extern const rex::PPCImageInfo eawebkit_PPCImageConfig;

// Register multi-entry-function alternate entries that rex's analyzer can't
// lift via config because they overlap with auto-discovered parent functions.
extern "C" REX_FUNC(__restgprlr_19);

// Defined in skate3_native_scene.cpp; toggled by the showcase hotkey below.
REXCVAR_DECLARE(bool, skate3_native_render_capture_hotkeys);
REXCVAR_DECLARE(bool, skate3_native_render_scene_showcase);
// Defined in skate3_native_scene.cpp; the freecam hotkey below toggles it,
// and while it captures input the guest input system is gated off.
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam);
REXCVAR_DECLARE(bool, skate3_native_render_scene_freecam_capture_input);

// Defined by the two installers; read here to decide whether the interactive
// wizard is needed at all.
REXCVAR_DECLARE(std::string, skate3_install_iso);
REXCVAR_DECLARE(std::string, skate3_install_tu);

REXCVAR_DEFINE_STRING(skate3_dlc_root, "", "Skate 3",
                      "Directory containing Skate 3 DLC package files");
REXCVAR_DEFINE_BOOL(skate3_auto_install_dlc, true, "Skate 3",
                    "Install DLC package files found in configured DLC folders");
REXCVAR_DEFINE_BOOL(skate3_ultrawide, false, "Skate 3",
                    "Ultrawide display support: the native renderer draws true widescreen "
                    "frames at the host display aspect (requires the native renderer; the "
                    "emulated fallback presents 16:9 pillarboxed)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(skate3_field_of_view, 60.0, "Skate 3",
                      "Gameplay camera field of view in degrees")
    .range(40.0, 120.0);
REXCVAR_DEFINE_BOOL(skate3_ultrawide_widen_game_frustum, true, "Skate 3",
                    "Widen the game-side main-world cull frustum to match the ultrawide view");
REXCVAR_DEFINE_DOUBLE(skate3_ultrawide_target_aspect, 0.0, "Skate 3",
                      "Ultrawide display aspect (0 = derive from the host display at boot)")
    .range(0.0, 8.0);

namespace {

void ApplyDemoPathProfileOverride() {
  if (!rex::cvar::Query<bool>("skate3_demo_path") &&
      !rex::cvar::Query<bool>("skate3_demo_path_probe")) {
    return;
  }
  if (rex::cvar::Query<bool>("skate3_demo_path_signed_in")) {
    // Closer-to-real-use runs: keep the user's profile and save so the boot
    // flow resumes their career instead of a signed-out session.
    return;
  }

  rex::cvar::SetFlagByName("user_profile_signed_in", "false");
  rex::cvar::SetFlagByName("user_live_signed_in", "false");
}

#if defined(__linux__) || defined(__APPLE__)
std::vector<std::string> CurrentProcessArgumentsForRestart(
    const std::filesystem::path& executable_path) {
  std::vector<std::string> args;
#if defined(__APPLE__)
  int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  args.reserve(static_cast<size_t>(argc > 0 ? argc : 1));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i] ? argv[i] : "");
  }
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  std::string arg;
  char ch = 0;
  while (cmdline.get(ch)) {
    if (ch == '\0') {
      args.push_back(std::move(arg));
      arg.clear();
    } else {
      arg.push_back(ch);
    }
  }
  if (!arg.empty()) {
    args.push_back(std::move(arg));
  }
#endif

  const std::string executable = rex::path_to_utf8(executable_path);
  if (args.empty()) {
    args.push_back(executable);
  } else {
    args[0] = executable;
  }
  return args;
}

void SetRestartArgument(std::vector<std::string>& args, std::string name, std::string value) {
  const std::string option = "--" + name;
  const std::string option_with_equals = option + "=";
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == option) {
      if (i + 1 < args.size()) {
        args[i + 1] = std::move(value);
      } else {
        args.push_back(std::move(value));
      }
      return;
    }
    if (args[i].starts_with(option_with_equals)) {
      args[i] = option_with_equals + value;
      return;
    }
  }

  args.push_back(std::move(option));
  args.push_back(std::move(value));
}
#endif

constexpr std::string_view kUserDirectoryName = "skate3";
constexpr std::string_view kSettingsFilename = "settings.toml";
constexpr std::string_view kDlcDirectoryName = "dlc";
constexpr std::string_view kSavesDirectoryName = "saves";
constexpr double kSixteenNineAspect = 16.0 / 9.0;
constexpr double kUltrawideAspectEpsilon = 0.01;

struct DisplaySize {
  int32_t width = 0;
  int32_t height = 0;
};

#if defined(_WIN32)
BOOL CALLBACK CollectMonitorCallback(HMONITOR monitor_handle, HDC, LPRECT, LPARAM data) {
  auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
  monitors->push_back(monitor_handle);
  return TRUE;
}

HMONITOR GetConfiguredMonitorHandle() {
  const int32_t monitor_index = REXCVAR_GET(monitor);
  if (monitor_index > 0) {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorCallback,
                        reinterpret_cast<LPARAM>(&monitors));
    if (monitor_index <= static_cast<int32_t>(monitors.size())) {
      return monitors[monitor_index - 1];
    }
  }

  POINT origin{0, 0};
  return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  HMONITOR monitor_handle = GetConfiguredMonitorHandle();
  if (!monitor_handle) {
    return std::nullopt;
  }

  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!GetMonitorInfo(monitor_handle, &monitor_info)) {
    return std::nullopt;
  }

  const int32_t width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
  const int32_t height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  return DisplaySize{width, height};
}
#else
std::optional<DisplaySize> QueryFullscreenMonitorSize() {
  return std::nullopt;
}
#endif

std::optional<DisplaySize> ResolveUltrawideTargetDisplaySize() {
  if (rex::cvar::HasNonDefaultValue("resolution")) {
    return std::nullopt;
  }

  if (REXCVAR_GET(fullscreen)) {
    return QueryFullscreenMonitorSize();
  }

  const int32_t configured_window_width = REXCVAR_GET(window_width);
  const int32_t configured_window_height = REXCVAR_GET(window_height);
  if (rex::cvar::HasNonDefaultValue("window_width") &&
      rex::cvar::HasNonDefaultValue("window_height") && configured_window_width > 0 &&
      configured_window_height > 0) {
    return DisplaySize{configured_window_width, configured_window_height};
  }

  return std::nullopt;
}

void ApplyUltrawideVideoDefaults() {
  if (!REXCVAR_GET(skate3_ultrawide)) {
    return;
  }

  // Explicitly configured aspect wins; otherwise derive it from the
  // fullscreen monitor (or a configured window size) once at boot.
  double target_aspect = rex::cvar::HasNonDefaultValue("skate3_ultrawide_target_aspect")
                             ? REXCVAR_GET(skate3_ultrawide_target_aspect)
                             : 0.0;
  if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
    const std::optional<DisplaySize> target_size = ResolveUltrawideTargetDisplaySize();
    if (!target_size || target_size->width <= 0 || target_size->height <= 0) {
      return;
    }
    target_aspect =
        static_cast<double>(target_size->width) / static_cast<double>(target_size->height);
    if (target_aspect <= kSixteenNineAspect + kUltrawideAspectEpsilon) {
      return;
    }
    rex::cvar::SetFlagByName("skate3_ultrawide_target_aspect", std::to_string(target_aspect));
  }

  // The native renderer draws true wide frames at this aspect (wide guest
  // output + Hor+ projection + centered 2D band). Emulated fallback frames
  // keep the 16:9 guest output and present pillarboxed via the letterbox
  // default below.
  rex::graphics::SetNativeGuestOutputWideAspect(
      std::clamp(target_aspect, kSixteenNineAspect, 8.0));

  if (!rex::cvar::HasNonDefaultValue("present_letterbox")) {
    rex::cvar::SetFlagByName("present_letterbox", "true");
  }
}

void DisableActiveDebugDiagnostics() {
  constexpr std::string_view kFalseFlags[] = {
      "perf_draw_fingerprints",
      "perf_keep_heavyweight_draw_diagnostics",
      "trace_gpu_stream",
  };
  for (std::string_view flag : kFalseFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "false");
  }

  constexpr std::string_view kZeroFlags[] = {
      "vulkan_debug_log_frame_summaries_remaining",
      "vulkan_debug_log_resolve_decisions_remaining",
      "vulkan_debug_log_team_profile_background_candidates_remaining",
      "vulkan_debug_log_team_profile_background_bindings_remaining",
      "filesystem_debug_log_fe_asset_ops_remaining",
      "filesystem_debug_log_team_profile_background_remaining",
  };
  for (std::string_view flag : kZeroFlags) {
    rex::cvar::SetFlagByName(std::string(flag), "0");
  }
}

std::filesystem::path DefaultDocumentsUserRoot() {
  return rex::filesystem::GetUserFolder() / std::string(kUserDirectoryName);
}

std::filesystem::path DefaultRoamingUserRoot() {
#if defined(_WIN32)
  char* appdata = nullptr;
  size_t appdata_size = 0;
  if (_dupenv_s(&appdata, &appdata_size, "APPDATA") == 0 && appdata && *appdata) {
    std::filesystem::path result = std::filesystem::path(appdata) / std::string(kUserDirectoryName);
    std::free(appdata);
    return result;
  }
  std::free(appdata);
#endif
  return DefaultDocumentsUserRoot();
}

std::filesystem::path ResolveSkate3UserRoot(const rex::PathConfig& paths) {
  const auto executable_root = rex::filesystem::GetAppRootFolder();
  if (std::filesystem::exists(executable_root / "portable.txt")) {
    return executable_root;
  }

  const auto old_default = DefaultDocumentsUserRoot();
  if (!paths.user_data_root.empty() && paths.user_data_root != old_default) {
    return paths.user_data_root;
  }

  return DefaultRoamingUserRoot();
}

void ConfigureSkate3UserPaths(rex::PathConfig& paths, std::filesystem::path& settings_path,
                              std::filesystem::path& profiles_path) {
  const auto old_user_root = DefaultDocumentsUserRoot();
  const auto old_cache_root = old_user_root / "cache";
  const auto original_cache_root = paths.cache_root;
  const auto resolved_user_root = ResolveSkate3UserRoot(paths);

  paths.user_data_root = resolved_user_root;
  if (original_cache_root.empty() || original_cache_root == old_cache_root) {
    paths.cache_root = resolved_user_root / "cache";
  }
  settings_path = resolved_user_root / std::string(kSettingsFilename);
  profiles_path = skate3::ProfilesFilePath(resolved_user_root);
}

bool ConfigContainsAnyKey(const toml::table& table, std::initializer_list<std::string_view> keys) {
  for (std::string_view key : keys) {
    if (table.contains(key)) {
      return true;
    }
  }
  return false;
}

bool ConfigFileContainsAnyKey(const std::filesystem::path& config_path,
                              std::initializer_list<std::string_view> keys) {
  if (config_path.empty() || !std::filesystem::exists(config_path)) {
    return false;
  }
  try {
    auto config = toml::parse_file(config_path.string());
    return ConfigContainsAnyKey(config, keys);
  } catch (const toml::parse_error&) {
    return false;
  }
}

bool DeveloperConfigHasResolutionScaleOverride(const std::filesystem::path& config_path) {
  return ConfigFileContainsAnyKey(config_path, {"resolution_scale", "draw_resolution_scale_x",
                                                "draw_resolution_scale_y"});
}

constexpr int kDefaultResolutionScale = 2;

void ApplyFirstRunVideoDefaults(const std::filesystem::path& settings_path,
                                const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path) ||
      DeveloperConfigHasResolutionScaleOverride(developer_config_path) ||
      rex::cvar::HasNonDefaultValue("resolution_scale") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_x") ||
      rex::cvar::HasNonDefaultValue("draw_resolution_scale_y")) {
    return;
  }

  const auto scale = std::to_string(kDefaultResolutionScale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
}

void LoadAndNormalizeSimpleSettings(const std::filesystem::path& settings_path,
                                    const std::filesystem::path& developer_config_path) {
  if (std::filesystem::exists(settings_path)) {
    rex::cvar::LoadConfig(settings_path);
  } else {
    ApplyFirstRunVideoDefaults(settings_path, developer_config_path);
  }
  rex::ui::EnsureSimpleSettingsConfig(settings_path);
}

// True if the path sits inside the application bundle. On iOS that is
// read-only and code signed, so installing there fails with "Operation not
// permitted"; on macOS it would corrupt the signature. Either way it is never
// a legitimate place to write game data.
bool IsInsideApplicationBundle(const std::filesystem::path& path) {
#if defined(__APPLE__)
  for (const auto& part : path) {
    const auto name = part.string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0) {
      return true;
    }
  }
#else
  (void)path;
#endif
  return false;
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
// Inside the iOS data container ONLY Documents/, Library/ and tmp/ are
// writable - the container root itself is not. A desktop XDG-style path like
// <container>/.local/share/game therefore fails with EPERM just as surely as
// the read-only app bundle does, which is exactly what the installer hit:
//
//   Unable to create the game directory at
//   .../Data/Application/<uuid>/.local/share/game: Operation not permitted
//
// So on iOS the answer is not "any path outside the bundle", it is "a path
// under one of the three writable roots".
bool IsWritableIosLocation(const std::filesystem::path& path) {
  const char* home = std::getenv("HOME");
  if (!home || !*home) {
    return false;
  }
  const std::filesystem::path home_path(home);
  for (const char* writable : {"Documents", "Library", "tmp"}) {
    const auto root = (home_path / writable).string();
    const auto candidate = path.string();
    if (candidate.compare(0, root.size(), root) == 0 &&
        (candidate.size() == root.size() || candidate[root.size()] == '/')) {
      return true;
    }
  }
  return false;
}
#endif

std::filesystem::path ResolveRuntimeGameDataRoot(const rex::PathConfig& paths) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // Honour a configured root only where it is somewhere we can actually write.
  if (!paths.game_data_root.empty() && IsWritableIosLocation(paths.game_data_root)) {
    return paths.game_data_root;
  }
  // Documents is both writable and the directory the user reaches over Finder
  // to drop an ISO into, so it is where the game belongs.
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / "Documents" / "game";
  }
#endif
  // A configured root is a BUILD machine's path, and a distributed build is
  // running somewhere else: /Users/<whoever>/skate3/game cannot even be
  // created on another Mac, so honouring it blindly makes a released app fail
  // to install the game with a permission error naming a stranger's home
  // directory. Accept it when it exists, or when its parent does and we could
  // create it there; otherwise fall through to somewhere this machine
  // actually has.
  if (!paths.game_data_root.empty() && !IsInsideApplicationBundle(paths.game_data_root)) {
    std::error_code ec;
    const bool exists = std::filesystem::is_directory(paths.game_data_root, ec) && !ec;
    const auto parent = paths.game_data_root.parent_path();
    // Parent exists = we could create it there. Deliberately not a real
    // writability probe: this runs on every start, and the install path
    // reports a permission failure clearly enough on its own.
    const bool creatable = !parent.empty() && std::filesystem::is_directory(parent, ec) && !ec;
    if (exists || creatable) {
      return paths.game_data_root;
    }
    REXLOG_INFO("Configured game root {} is not present on this machine; using a local path",
                paths.game_data_root.string());
  }

  // The working directory is only a sensible guess where the app is launched
  // from a shell. On iOS it IS the bundle, so this branch has to be able to
  // reject its own answer rather than hand back an unwritable path.
  const auto working_directory_game = std::filesystem::current_path() / "game";
  if (!IsInsideApplicationBundle(working_directory_game) &&
      skate3::IsGameInstalled(working_directory_game)) {
    return working_directory_game;
  }

  // Last resort. Both remaining candidates can land inside the bundle - which
  // is exactly how the installer came to report
  //   "Unable to create the game directory at .../skate3.app/game:
  //    Operation not permitted"
  // - so prefer the user data root, which is always somewhere writable,
  // and only fall back to the config directory when there is nothing better.
  if (!paths.user_data_root.empty() && !IsInsideApplicationBundle(paths.user_data_root)) {
    // Under the user data root, beside dlc/ and saves/, so everything this
    // app owns lives in one folder. Earlier builds put it one level up,
    // outside that folder entirely - keep using that when a game is actually
    // installed there, so an existing install is not orphaned by the move.
    const auto legacy = paths.user_data_root.parent_path() / "game";
    if (skate3::IsGameInstalled(legacy)) {
      return legacy;
    }
    return paths.user_data_root / "game";
  }

  const auto config_game = paths.config_path.parent_path() / "game";
  if (!IsInsideApplicationBundle(config_game)) {
    return config_game;
  }

#if defined(__APPLE__) && TARGET_OS_IPHONE
  // Nothing usable was configured and every candidate is inside the bundle.
  // $HOME is the sandbox root on iOS, so Documents is both writable and the
  // directory the user can reach over Finder to drop an ISO in.
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / "Documents" / "game";
  }
#endif
  return config_game;
}

const char* FirstExistingFontPath(std::initializer_list<const char*> paths) {
  for (const char* path : paths) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return nullptr;
}

std::string Hex8(uint32_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
  return stream.str();
}

std::filesystem::path InstalledMarketplaceContentPath(const std::filesystem::path& content_root,
                                                      uint32_t title_id,
                                                      const std::filesystem::path& package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "00000002" /
         package_path.filename();
}

std::filesystem::path InstalledMarketplaceHeaderPath(const std::filesystem::path& content_root,
                                                     uint32_t title_id,
                                                     const std::filesystem::path& package_path) {
  return content_root / "0000000000000000" / Hex8(title_id) / "Headers" / "00000002" /
         (package_path.filename().string() + ".header");
}

bool IsInstalledMarketplaceContent(const std::filesystem::path& content_root, uint32_t title_id,
                                   const std::filesystem::path& package_path) {
  return std::filesystem::is_directory(
             InstalledMarketplaceContentPath(content_root, title_id, package_path)) &&
         std::filesystem::is_regular_file(
             InstalledMarketplaceHeaderPath(content_root, title_id, package_path));
}

std::vector<std::filesystem::path> DiscoverDlcSourceDirectories(
    const std::filesystem::path& executable_root, const std::filesystem::path& game_data_root,
    const std::filesystem::path& user_data_root) {
  std::vector<std::filesystem::path> dirs;
  auto add_dir = [&](std::filesystem::path dir) {
    if (dir.empty()) {
      return;
    }
    std::error_code ec;
    dir = std::filesystem::absolute(dir, ec);
    if (ec) {
      return;
    }
    for (const auto& existing : dirs) {
      if (std::filesystem::equivalent(existing, dir, ec)) {
        return;
      }
      ec.clear();
    }
    dirs.push_back(std::move(dir));
  };

  const std::string configured_root = REXCVAR_GET(skate3_dlc_root);
  if (!configured_root.empty()) {
    add_dir(configured_root);
  }
  add_dir(executable_root / std::string(kDlcDirectoryName));
#if defined(__APPLE__) && !TARGET_OS_IPHONE
  // Inside a .app the executable sits in Contents/MacOS, so "beside the
  // executable" is three levels below where a person actually sees the app.
  // Look beside the BUNDLE too, so dropping a dlc/ folder next to Skate 3.app
  // works the way it does next to the Linux binary.
  if (executable_root.has_parent_path()) {
    const auto contents = executable_root.parent_path();          // Contents
    if (contents.filename() == "Contents" && contents.has_parent_path()) {
      const auto bundle = contents.parent_path();                 // Skate 3.app
      if (bundle.has_parent_path()) {
        add_dir(bundle.parent_path() / std::string(kDlcDirectoryName));
      }
    }
  }
#endif
  add_dir(game_data_root / std::string(kDlcDirectoryName));
  add_dir(user_data_root / std::string(kDlcDirectoryName));
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
  // On a phone the only place a pack can be put is the top of the app's
  // Documents folder - Finder and the Files app will not drop a folder into a
  // subdirectory of it. The scan below is recursive, so naming the root is
  // enough to find DMJUMPLINE/dmjumpline_00000000.big and its header.
  add_dir(user_data_root.parent_path());
#endif
  return dirs;
}

}  // namespace

#if !SKATE3_HAS_TITLE_UPDATE
// Retail sub_82EBAE4C: alternate entry into sub_82EBAE34 (37 vtable refs in .data).
// Body: lwz r3, 0x164(r31); addi r1, r31, 0x140; b __restgprlr_19
static void Sub82EBAE4CImpl(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0x164);
  ctx.r1.s64 = ctx.r31.s64 + 0x140;
  __restgprlr_19(ctx, base);
}
#endif

Skate3BaseApp::~Skate3BaseApp() = default;

void Skate3BaseApp::OnConfigurePaths(rex::PathConfig& paths) {
  ConfigureSkate3UserPaths(paths, user_settings_path_, profiles_path_);
  config_path_ = paths.config_path;
  LoadAndNormalizeSimpleSettings(user_settings_path_, config_path_);
  Skate3InitializeFieldOfViewOverride();
  ApplyUltrawideVideoDefaults();
  DisableActiveDebugDiagnostics();
}

void Skate3BaseApp::OnConfigureFonts(ImFontAtlas* atlas) {
  if (!atlas) {
    return;
  }

  atlas->Clear();
  ImFontConfig font_config;
  font_config.OversampleH = 2;
  font_config.OversampleV = 2;
  font_config.PixelSnapH = false;

  const char* base_font_path = FirstExistingFontPath({
#if defined(_WIN32)
      "C:\\Windows\\Fonts\\Helvetica.ttf",
      "C:\\Windows\\Fonts\\helvetica.ttf",
      "C:\\Windows\\Fonts\\HelveticaNeue.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/SFCompact.ttf",
      "/System/Library/Fonts/HelveticaNeue.ttc",
      "/System/Library/Fonts/LucidaGrande.ttc",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/local/share/fonts/NotoSans-Regular.ttf",
      "/usr/local/share/fonts/LiberationSans-Regular.ttf",
#endif
  });

  if (base_font_path) {
    atlas->AddFontFromFileTTF(base_font_path, 16.0f, &font_config,
                              atlas->GetGlyphRangesDefault());
  } else {
    atlas->AddFontDefault();
  }

#if defined(_WIN32)
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (std::filesystem::exists(jp_font_path)) {
    ImFontConfig jp_font_config;
    jp_font_config.MergeMode = true;
    jp_font_config.OversampleH = jp_font_config.OversampleV = 1;
    jp_font_config.PixelSnapH = true;
    jp_font_config.FontNo = 0;
    atlas->AddFontFromFileTTF(jp_font_path, 16.0f, &jp_font_config,
                              atlas->GetGlyphRangesJapanese());
  }
#endif
}

std::optional<rex::PathConfig> Skate3BaseApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  config_path_ = defaults.config_path;
  user_settings_path_ = defaults.user_data_root / std::string(kSettingsFilename);
  profiles_path_ = skate3::ProfilesFilePath(defaults.user_data_root);

  auto profiles = skate3::LoadProfiles(profiles_path_);
  const bool has_profiles_file = std::filesystem::exists(profiles_path_);
  skate3::EnsureUsableProfileStore(profiles, "Player");
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    ApplyDemoPathProfileOverride();
  }

  const bool has_config_file = std::filesystem::exists(defaults.config_path);
  const bool has_game_path = std::filesystem::is_directory(defaults.game_data_root);
  if (!has_profiles_file && has_config_file && has_game_path) {
    skate3::SaveProfiles(profiles_path_, profiles);
  }
  auto runtime_paths = defaults;
  runtime_paths.game_data_root = ResolveRuntimeGameDataRoot(runtime_paths);
  REXLOG_INFO("game data root: {} (configured: {}, user data: {})",
              runtime_paths.game_data_root.string(),
              defaults.game_data_root.empty() ? "<empty>" : defaults.game_data_root.string(),
              defaults.user_data_root.empty() ? "<empty>" : defaults.user_data_root.string());
  runtime_paths.config_path.clear();
  if (!skate3::IsGameInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Game files not found at {}; launching rexglue ISO installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__) || defined(__ANDROID__)
    // The blocking pump below never pumps SDL events on these platforms, so
    // the interactive wizard has to run through the asynchronous path; only
    // an automated install (SKATE3_INSTALL_ISO from the shell or the activity)
    // may block.
    if (REXCVAR_GET(skate3_install_iso).empty() &&
        [] {
          const char* v = std::getenv("SKATE3_INSTALL_ISO");
          return v == nullptr || *v == '\0';
        }()) {
#if SKATE3_HAS_TITLE_UPDATE
      // Chain the title update wizard after the ISO install completes.
      auto resume_after_title_update =
          [this, resume = std::move(resume)](rex::PathConfig paths) mutable {
            if (!skate3::IsTitleUpdateInstalled(paths.game_data_root)) {
              skate3::ShowTitleUpdateInstallWizard(imgui_drawer(), std::move(paths),
                                                   std::move(resume));
              return;
            }
            resume(std::move(paths));
          };
      skate3::ShowRexglueIsoInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                          std::move(resume_after_title_update));
#else
      skate3::ShowRexglueIsoInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                          std::move(resume));
#endif
      return std::nullopt;
    }
#endif
    rex::PathConfig installed_paths;
    const bool installed = skate3::RunRexglueIsoInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths, installed_paths);
    if (!installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    runtime_paths = std::move(installed_paths);
  }

#if SKATE3_HAS_TITLE_UPDATE
  // This build executes Title Update 3 code; the game cannot boot without the
  // TU payloads staged next to the installed game files. Existing installs
  // from releases that predate TU support land here with the game present but
  // the title update missing.
  if (!skate3::IsTitleUpdateInstalled(runtime_paths.game_data_root)) {
    REXLOG_INFO("Skate 3 Title Update 3 not staged at {}; launching title update installer",
                runtime_paths.game_data_root.string());
#if defined(__APPLE__) || defined(__ANDROID__)
    if (REXCVAR_GET(skate3_install_tu).empty() &&
        [] {
          const char* v = std::getenv("SKATE3_INSTALL_TU");
          return v == nullptr || *v == '\0';
        }()) {
      skate3::ShowTitleUpdateInstallWizard(imgui_drawer(), std::move(runtime_paths),
                                           std::move(resume));
      return std::nullopt;
    }
#endif
    rex::PathConfig tu_paths;
    const bool tu_installed = skate3::RunTitleUpdateInstallWizardBlocking(
        app_context(), window(), imgui_drawer(), runtime_paths, tu_paths);
    if (!tu_installed) {
      app_context().QuitFromUIThread();
      return std::nullopt;
    }
    runtime_paths = std::move(tu_paths);
  }
#endif
#if defined(_WIN32)
  // Window/taskbar + Explorer icon sourced from the user's OWN game art at
  // runtime (game/nxeart); the shipped exe and the repo carry no EA
  // artwork (see skate3_win_icon.h). Game files are guaranteed installed by
  // this point on Windows (the install wizards above run blocking).
  skate3::ApplyGameIconFromGameData(
      runtime_paths.game_data_root,
      window() ? window()->GetNativeWindowHandle() : nullptr);
#endif
  // Ask which pack to load, before anything of the game starts. Asynchronous:
  // this runs from OnFinalizePaths, before OnInitialize has returned, so the
  // event loop is not pumping yet and a blocking wait would draw nothing -
  // which is exactly the black screen it produced. Returning nullopt lets the
  // loop start and the dialog render; startup resumes from the callback, the
  // same shape the install wizards above use on Apple platforms.
  if (REXCVAR_GET(skate3_content_pack).empty() && !chose_content_pack_) {
    std::vector<std::string> packs;
    std::error_code pack_ec;
    const auto documents = runtime_paths.user_data_root.parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(documents, pack_ec)) {
      if (pack_ec || !entry.is_directory()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name == "user" || name == "game") {
        continue;
      }
      if (IsContentPackFolder(entry.path())) {
        packs.push_back(name);
        continue;
      }
      // Packs are often shipped inside a wrapper folder holding the package
      // beside a readme, and dragging that whole folder in is the obvious
      // thing to do - so look one level down as well.
      std::error_code nest_ec;
      for (const auto& child : std::filesystem::directory_iterator(entry.path(), nest_ec)) {
        if (nest_ec || !child.is_directory()) {
          continue;
        }
        if (IsContentPackFolder(child.path())) {
          // The LEAF name, which is what staging matches on - a wrapper-folder
          // path would never compare equal to the candidate it refers to, and
          // nothing would be staged at all.
          packs.push_back(child.path().filename().string());
          break;
        }
      }
    }
    std::sort(packs.begin(), packs.end());
    if (packs.size() > 1) {
      skate3::ShowPackSelect(app_context(), imgui_drawer(), packs,
                             [this, paths = runtime_paths, resume](std::string choice) mutable {
                               chosen_content_pack_ = std::move(choice);
                               chose_content_pack_ = true;
                               resume(std::move(paths));
                             });
      return std::nullopt;
    }
  }

  return runtime_paths;
}

void Skate3BaseApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // Native/emulated corner readout (top right; off by default, cvar
  // skate3_native_render_mode_indicator shows it live). Input-transparent,
  // so it never affects cursor or focus handling.
  // Give back what can be rebuilt when the system says it is short. On a
  // phone the usual cause is something else entirely - a call arriving - and
  // an app that does not answer gets its graphics resources reclaimed out from
  // under the driver instead. The stores refill from guest memory on demand,
  // so the cost is a few seconds of redecoding rather than a lost session.
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
  if (auto* sdl_context = dynamic_cast<rex::ui::SDLWindowedAppContext*>(&app_context())) {
    sdl_context->SetLowMemoryHandler([]() {
      skate3::native_scene::FlushTextureCache();
      skate3::native_scene::FlushMeshCache();
    });
  }
#endif
  render_mode_indicator_ = std::make_unique<skate3::RenderModeIndicator>(drawer);
  // On-screen pad. Constructed everywhere and inert off-device; it draws only
  // while the touch driver reports no physical controller attached.
  touch_controls_ = std::make_unique<skate3::TouchControlsOverlay>(drawer);
  // Loading cover for launcher-driven map selection. Constructed unconditionally
  // (it is inert unless skate3_loader_overlay is set) so the cvar stays live.
  loader_overlay_ = std::make_unique<skate3::LoaderOverlay>(drawer);
  // In-game map picker for launcher-driven sessions (F3). Inert unless the
  // launcher passed a level list.
  level_select_dialog_ =
      std::make_unique<skate3::LevelSelectDialog>(drawer, loader_overlay_.get());
  // Shown beside the normal Escape settings screen, so the system menu keeps
  // everything it had and simply gains a map list.
  level_select_dialog_->SetCompanionPredicate(
      [this] { return simple_settings_dialog_ && simple_settings_dialog_->visible(); });
  // Choosing a map closes the whole system menu, so the new world is not loaded
  // underneath a settings screen nobody asked to keep open.
  level_select_dialog_->SetCloseMenusCallback([this] {
    if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
      ToggleSimpleSettings();
    }
  });
  if (skate3::LoaderPickerOpensAtStart()) {
    level_select_dialog_->RequestOpenOnGameplay();
  }
  // Backtick is the level picker, the way a console key would be. F7 stays as a
  // second binding so existing muscle memory and any saved config still work.
  rex::ui::RegisterBind("bind_skate3_level_select", "Backtick", "Skate 3 level select", [this] {
    if (level_select_dialog_) {
      level_select_dialog_->Toggle();
    }
  });
  rex::ui::RegisterBind("bind_skate3_level_select_alt", "F7",
                        "Skate 3 level select (alternate)", [this] {
    if (level_select_dialog_) {
      level_select_dialog_->Toggle();
    }
  });
  rex::ui::RegisterBind("bind_skate3_menu", "Escape", "Skate 3 settings", [this] {
    // Escape backs out of the settings screen level by level (rows ->
    // category rail -> closed); when the screen is closed it opens it.
    if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
      simple_settings_dialog_->NavigateBack();
    } else {
      ToggleSimpleSettings();
    }
  });
  rex::ui::RegisterBind("bind_skate3_menu_alt", "F1", "Skate 3 settings alternate", [this] {
    ToggleSimpleSettings();
  });
  // Keyboard counterpart to the Start + Select chord. Sits next to the FPS
  // counter's F2 because it answers the same question in more detail.
  rex::ui::RegisterBind("bind_skate3_perf_menu", "Shift+F2", "Skate 3 performance menu",
                        [this] { TogglePerformanceMenu(); });
  // Remembered handle: the F11 paired A/B parity capture (native + emulated
  // screenshots + gsnap, sequenced from the guest frame loop in
  // skate3_native_render.cpp) needs the window without an app pointer.
  skate3::screenshot::RememberWindow(window() ? window()->GetNativeWindowHandle()
                                              : nullptr);
  rex::ui::RegisterBind("bind_skate3_screenshot", "F6",
                        "Save screenshot to screenshots/", [this] {
                          skate3::screenshot::CaptureWindow(
                              window() ? window()->GetNativeWindowHandle() : nullptr);
                        });
  rex::ui::RegisterBind("bind_skate3_native_render_toggle", "F5",
                        "Toggle native/emulated renderer", [] {
                          skate3::native_scene::ToggleSceneEnabled();
                        });
  rex::ui::RegisterBind("bind_skate3_save_draw_fingerprints", "F8",
                        "Save draw fingerprint log", [this] {
                          SaveDrawFingerprintLog();
                        });
  rex::ui::RegisterBind("bind_skate3_log_debug_marker", "F9",
                        "Write debug marker to log", [this] {
                          LogDebugMarker();
                        });
  rex::ui::RegisterBind("bind_skate3_log_user_marker", "F10",
                        "Write marker to log", [this] {
                          LogUserMarker();
                        });
  rex::ui::RegisterBind("bind_skate3_native_debug", "F12",
                        "Native render debug menu", [this] {
                          ToggleNativeDebug();
                        });
  rex::ui::RegisterBind("bind_skate3_showcase", "Ctrl+Shift+B",
                        "Graphics build-up showcase", [] {
                          // A capture/recording tool, not a player feature:
                          // like the capture hotkeys, the bind is inert
                          // unless the diagnostics cvar opts in (the F12
                          // Showcase Setup window still starts runs).
                          if (!REXCVAR_GET(skate3_native_render_capture_hotkeys)) {
                            return;
                          }
                          REXCVAR_SET(
                              skate3_native_render_scene_showcase,
                              !REXCVAR_GET(skate3_native_render_scene_showcase));
                        });
  rex::ui::RegisterBind("bind_skate3_freecam", "End",
                        "Drone camera (free fly)", [] {
                          REXCVAR_SET(
                              skate3_native_render_scene_freecam,
                              !REXCVAR_GET(skate3_native_render_scene_freecam));
                        });
}

void Skate3BaseApp::OnPostSetup() {
  // Arm the hang watchdog before the guest runs. The full reporter installs
  // from the guest's first D3D Swap, so a boot that never reaches one - the
  // freeze where the main thread is resumed and then simply never executes -
  // produced no thread dump at all, which is why that failure had no evidence.
  skate3::crash_report::StartWatchdogEarly();
  // Before anything that might want to be measured, and after the cvars are
  // parsed, so an ios_args.txt that asks for diagnostics gets them from frame
  // one rather than from whenever the settings screen is first opened.
  skate3::InstallDiagnosticsSwitch();
  // After the cvars and settings.toml are in, so naming a preset at launch
  // beats both - and before the renderer reads any of them.
  skate3::ApplyRequestedPerformanceProfile();
  skate3::shader_disasm::RunIfRequested();
  ApplySelectedProfileToRuntime();
  ApplyGameplayCursorMode();

  if (auto* input_system = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    input_system->SetActiveCallback([this]() {
      const bool settings_visible = simple_settings_dialog_ && simple_settings_dialog_->visible();
      const bool xam_ui_active = rex::kernel::xam::xeXamIsUIActive();
      // The drone cam owns the keyboard while flying: keep guest input off
      // so the fly keys don't also steer the skater.
      const bool freecam_captures =
          REXCVAR_GET(skate3_native_render_scene_freecam) &&
          REXCVAR_GET(skate3_native_render_scene_freecam_capture_input);
      return !settings_visible && !xam_ui_active && !freecam_captures;
    });
    input_system->SetMenuChordCallback([this]() {
      app_context().CallInUIThreadDeferred([this]() { ToggleSimpleSettings(); });
    });
    // Start + Select opens the performance menu (cvar perf_chord).
    input_system->SetPerfChordCallback([this]() {
      app_context().CallInUIThreadDeferred([this]() { TogglePerformanceMenu(); });
    });
    // The middle Xbox button opens the level picker (cvar picker_chord). It
    // only reaches the input system when guide_button is on, so a launcher that
    // wants it must set both.
    input_system->SetPickerChordCallback([this]() {
      app_context().CallInUIThreadDeferred([this]() {
        if (level_select_dialog_) {
          level_select_dialog_->Toggle();
        }
      });
    });
  }

  if (std::getenv("SKATE3_DISABLE_BIG_ALIASES") == nullptr) {
    InstallBigDeviceAliases();
  }
  // Portable saves: a "saves" folder next to the executable takes over
  // saved-game content when it exists (launcher-managed installs). Layout is
  // flattened to saves/<xuid>/<save>; nothing is migrated from the user
  // content tree, and without the folder the default location is unchanged.
  {
    const auto portable_saves =
        rex::filesystem::GetAppRootFolder() / std::string(kSavesDirectoryName);
    if (std::filesystem::is_directory(portable_saves) && runtime()->kernel_state() &&
        runtime()->kernel_state()->content_manager()) {
      runtime()->kernel_state()->content_manager()->SetContentTypeRoot(
          rex::system::XContentType::kSavedGame, portable_saves);
      REXLOG_INFO("Skate 3 saves: using portable folder {}", portable_saves.string());
    }
  }
  InstallDlcPackages();
  InstallRecipeOverlay();

  // Register retail multi-entry-function alternate entries.
#if !SKATE3_HAS_TITLE_UPDATE
  runtime()->function_dispatcher()->SetFunction(0x82EBAE4C, &Sub82EBAE4CImpl);
#endif

  auto* dispatcher = runtime()->function_dispatcher();
  skate3::native_render::Install();
  skate3::demo_path::InstallHooks(dispatcher);
  // Guest call trace: no-op unless --skate3_trace=true. Installed after the
  // demo path because its controller reads that macro's progress milestones.
  skate3::guest_trace::Install();
  skate3::guest_trace::InstallSampler();
  // Direct level load and its probe: no-op unless one of its cvars is set.
  // Last, so a probe chains to whatever hook the features above installed
  // rather than replacing it.
  skate3::warp::Install(dispatcher);
  // User-facing intro-movie skip (independent of the demo path): the movie
  // completion override polls the merged UI pad state through this provider.
  skate3::demo_path::SetUiInputProvider([this]() {
    return static_cast<rex::input::InputSystem*>(runtime()->input_system());
  });
  if (dispatcher->InitializeFunctionTable(eawebkit_PPCImageConfig.code_base,
                                          eawebkit_PPCImageConfig.code_size,
                                          eawebkit_PPCImageConfig.image_base,
                                          eawebkit_PPCImageConfig.image_size)) {
    for (int i = 0; eawebkit_PPCImageConfig.func_mappings[i].guest != 0; ++i) {
      auto* host = eawebkit_PPCImageConfig.func_mappings[i].host;
      if (host) {
        dispatcher->SetFunction(
            static_cast<uint32_t>(eawebkit_PPCImageConfig.func_mappings[i].guest),
            host);
      }
    }
  }
}

void Skate3BaseApp::OnShutdown() {
  rex::ui::UnregisterBind("bind_skate3_menu");
  rex::ui::UnregisterBind("bind_skate3_menu_alt");
  rex::ui::UnregisterBind("bind_skate3_save_draw_fingerprints");
  rex::ui::UnregisterBind("bind_skate3_log_debug_marker");
  rex::ui::UnregisterBind("bind_skate3_log_user_marker");
  rex::ui::UnregisterBind("bind_skate3_native_debug");
  ApplyGameplayCursorMode();
  skate3::native_scene::SetSettingsMenuBlur(false);
  simple_settings_dialog_.reset();
  native_debug_dialog_.reset();
  render_mode_indicator_.reset();
}

void Skate3BaseApp::ToggleSimpleSettings() {
  if (simple_settings_dialog_ && simple_settings_dialog_->visible()) {
    simple_settings_dialog_->Hide();
    return;
  }
  EnsureSimpleSettingsDialog();
  ApplySettingsCursorMode();
  skate3::native_scene::SetSettingsMenuBlur(true);
  simple_settings_dialog_->Show();
}

void Skate3BaseApp::TogglePerformanceMenu() {
  EnsureSimpleSettingsDialog();
  // Set up as if opening: TogglePerformance() closes instead when the
  // Performance page is already the one on screen, and the Hide() it runs
  // fires close_settings, which puts the blur and cursor back.
  ApplySettingsCursorMode();
  skate3::native_scene::SetSettingsMenuBlur(true);
  simple_settings_dialog_->TogglePerformance();
}

void Skate3BaseApp::EnsureSimpleSettingsDialog() {
  if (simple_settings_dialog_) {
    return;
  }

  auto load_profiles = [this]() {
    rex::ui::SimpleProfileState state;
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    state.selected_index = 0;
    for (int i = 0; i < static_cast<int>(store.profiles.size()); ++i) {
      const auto& profile = store.profiles[i];
      state.profiles.push_back({profile.id, profile.gamertag, profile.signed_in});
      if (profile.id == store.selected_profile) {
        state.selected_index = i;
      }
    }
    return state;
  };
  auto save_profile = [this](int selected_index, std::string gamertag, bool signed_in) {
    auto store = skate3::LoadProfiles(profiles_path_);
    skate3::EnsureUsableProfileStore(store, "Player");
    if (store.profiles.empty()) {
      return;
    }
    selected_index = std::clamp(selected_index, 0, static_cast<int>(store.profiles.size()) - 1);
    auto& profile = store.profiles[selected_index];
    if (!gamertag.empty()) {
      profile.gamertag = std::move(gamertag);
    }
    profile.signed_in = signed_in;
    if (!profile.signed_in) {
      profile.live_signed_in = false;
    }
    store.selected_profile = profile.id;
    skate3::SaveProfiles(profiles_path_, store);
    skate3::ApplyProfileCvars(profile);
    ApplyDemoPathProfileOverride();
    ApplySelectedProfileToRuntime();
  };
  // Fires on every Hide (B/Esc, Close Settings, Close Game), the one spot
  // that reliably sees the menu close regardless of who initiated it.
  auto close_settings = [this]() {
    skate3::native_scene::SetSettingsMenuBlur(false);
    ApplyGameplayCursorMode();
    // Release the Performance page's claim on frame-stat collection. The
    // app's own policy (FPS/debug overlays) is a separate input and is left
    // exactly as it was.
    if (auto* presenter = imgui_drawer() ? imgui_drawer()->presenter() : nullptr) {
      presenter->SetGuestFrameStatsHold(false);
    }
  };
  auto close_game = [this]() {
#if REX_PLATFORM_MAC || REX_PLATFORM_LINUX
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      REXLOG_WARN("Close Game watchdog exiting process after shutdown timeout");
      std::_Exit(EXIT_SUCCESS);
    }).detach();
#endif
    app_context().CallInUIThreadDeferred([this]() {
#if REX_PLATFORM_MAC
      app_context().QuitFromUIThread();
#else
      if (window()) {
        window()->RequestClose();
      } else {
        app_context().QuitFromUIThread();
      }
#endif
    });
  };
  auto restart_game = [this]() { RestartGame(); };
  // Raw pad poll for menu navigation: bypasses the is_active gate that
  // zeroes guest-facing input while the settings screen is open.
  auto poll_gamepad = [this]() {
    rex::ui::SimpleSettingsGamepad pad;
    // The dialog can be painted (and thus poll) during a frame that races
    // runtime teardown at close time, when runtime() is already gone.
    auto* rt = runtime();
    auto* input_system =
        rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    if (input_system) {
      rex::input::X_INPUT_GAMEPAD state;
      if (input_system->GetUiGamepadState(&state)) {
        pad.connected = true;
        pad.buttons = state.buttons;
        pad.thumb_lx = state.thumb_lx;
        pad.thumb_ly = state.thumb_ly;
      }
    }
    return pad;
  };
  // Live frame timings for the Performance page. Collecting them costs GPU
  // timestamp queries, so the page holds collection on only while it is the
  // page on screen - the hold is released in close_settings above.
  auto poll_perf_stats = [this]() {
    rex::ui::SimpleSettingsPerfStats out;
    auto* presenter = imgui_drawer() ? imgui_drawer()->presenter() : nullptr;
    if (!presenter) {
      return out;
    }
    presenter->SetGuestFrameStatsHold(true);
    const rex::ui::Presenter::GuestFrameStats stats = presenter->GetGuestFrameStats();
    // The first polls after the hold is taken land before any frame has been
    // timed; report nothing rather than a confident zero.
    if (stats.frame_count == 0 || stats.fps <= 0.0) {
      return out;
    }
    out.valid = true;
    out.fps = stats.fps;
    out.frame_time_ms = stats.frame_time_ms;
    out.wait_ms = stats.wait_ms;
    out.gpu_ms = stats.gpu_ms;
    out.gpu_draw_ms = stats.gpu_draw_ms;
    out.gpu_resolve_ms = stats.gpu_resolve_ms;
    out.gpu_dump_ms = stats.gpu_dump_ms;
    return out;
  };
  simple_settings_dialog_ =
      std::make_unique<rex::ui::SimpleSettingsDialog>(
          imgui_drawer(), user_settings_path_, std::move(load_profiles), std::move(save_profile),
          std::move(close_settings), std::move(close_game), std::move(restart_game),
          std::move(poll_gamepad), std::move(poll_perf_stats));
}

void Skate3BaseApp::ToggleNativeDebug() {
  if (!native_debug_dialog_) {
    native_debug_dialog_ = std::make_unique<skate3::NativeDebugDialog>(imgui_drawer());
  }
  if (native_debug_dialog_->visible()) {
    native_debug_dialog_->Hide();
    ApplyGameplayCursorMode();
  } else {
    ApplySettingsCursorMode();
    native_debug_dialog_->Show();
  }
}

void Skate3BaseApp::ApplySettingsCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
}

void Skate3BaseApp::ApplyGameplayCursorMode() {
  if (window()) {
    window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
  }
}

void Skate3BaseApp::RestartGame() {
  app_context().CallInUIThreadDeferred([this]() {
#if defined(_WIN32)
    wchar_t executable_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    std::wstring command_line = GetCommandLineW();
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(executable_path, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup_info, &process_info)) {
      REXLOG_WARN("Restart requested, but launching a new process failed");
      return;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#elif defined(__linux__) || defined(__APPLE__)
    const auto executable_path = rex::filesystem::GetExecutablePath();
    if (executable_path.empty()) {
      REXLOG_WARN("Restart requested, but the executable path could not be resolved");
      return;
    }

    auto args = CurrentProcessArgumentsForRestart(executable_path);
    std::filesystem::path restart_game_data_root;
    if (runtime()) {
      restart_game_data_root = runtime()->game_data_root();
    }
    if (restart_game_data_root.empty()) {
      restart_game_data_root = game_data_root();
    }
    if (!restart_game_data_root.empty()) {
      SetRestartArgument(args, "game_data_root", rex::path_to_utf8(restart_game_data_root));
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

#if REX_PLATFORM_IOS
    // iOS forbids a sandboxed app from spawning anything, so posix_spawn here
    // fails with EPERM, logs, and returns - which is why "Apply & Restart" did
    // nothing at all on a phone. The settings are already written to disk by
    // ApplyAndRestart's SaveVideo() before this runs, so quitting IS applying
    // them; the player just relaunches by hand. Falls through to the quit
    // below rather than trying and failing first.
    REXLOG_INFO("Restart requested: settings saved, quitting for a manual relaunch (iOS "
                "cannot restart itself)");
#elif REX_PLATFORM_ANDROID
    // An app process cannot exec itself either, but the activity can relaunch
    // the app: it starts a fresh task from a helper process and lets this one
    // exit. Settings are already on disk, so quitting below completes it.
    if (skate3::android::RequestRestart()) {
      REXLOG_INFO("Restart requested: relaunch scheduled through the activity");
    } else {
      REXLOG_WARN("Restart requested, but the activity declined; quitting for a manual relaunch");
    }
#else
    const std::string executable = rex::path_to_utf8(executable_path);
    pid_t child_pid = 0;
    const int spawn_result =
        posix_spawn(&child_pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_result != 0) {
      REXLOG_WARN("Restart requested, but launching a new process failed: {}",
                  std::strerror(spawn_result));
      return;
    }
#endif
#else
    REXLOG_WARN("Restart requested, but automatic restart is not implemented on this platform");
    return;
#endif

  #if REX_PLATFORM_MAC
    app_context().QuitFromUIThread();
  #else
    if (window()) {
      window()->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
  #endif
  });
}

void Skate3BaseApp::SaveDrawFingerprintLog() {
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  REXLOG_WARN("Perf capture is unavailable because perf counters are disabled in this build");
  return;
#else
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &now_time);
#else
  localtime_r(&now_time, &local_time);
#endif

  std::ostringstream counters_filename;
  counters_filename << "perf_capture_counters_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
                    << ".csv";
  std::ostringstream filename;
  filename << "perf_capture_draw_fingerprints_" << std::put_time(&local_time, "%Y%m%d_%H%M%S")
           << ".csv";

  std::error_code ec;
  std::filesystem::create_directories(cache_root(), ec);
  const auto counters_path = cache_root() / counters_filename.str();
  const auto path = cache_root() / filename.str();
  if (rex::perf::StartCapture(counters_path, path)) {
    REXLOG_INFO("Started perf capture; counter log will be saved to {}",
                counters_path.string());
    REXLOG_INFO("Started perf capture; draw fingerprint log will be saved to {}",
                path.string());
  } else {
    REXLOG_WARN("Perf capture is already running");
  }
#endif
}

void Skate3BaseApp::LogUserMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  REXLOG_WARN("USER LOG MARKER #{}: F10 pressed", marker);
}

void Skate3BaseApp::LogDebugMarker() {
  const uint32_t marker = debug_marker_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  DisableActiveDebugDiagnostics();
  REXLOG_WARN("USER DEBUG MARKER #{}: F9 pressed; active diagnostics disabled", marker);
}

void Skate3BaseApp::ApplySelectedProfileToRuntime() {
  if (!runtime() || !runtime()->kernel_state() || !runtime()->kernel_state()->user_profile()) {
    return;
  }
  auto profiles = skate3::LoadProfiles(profiles_path_);
  if (auto* profile = skate3::FindSelectedProfile(profiles)) {
    skate3::ApplyProfileCvars(*profile);
    ApplyDemoPathProfileOverride();
    runtime()->kernel_state()->user_profile()->SetIdentity(profile->xuid, profile->gamertag);
  }
}

bool Skate3BaseApp::IsRecipeNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

std::set<std::string> Skate3BaseApp::DiscoverRecipeAliases(
    const std::filesystem::path& content_root) {
  static constexpr size_t kScanBytes = 8 * 1024 * 1024;
  static constexpr std::string_view kPrefix = "data/content/recipe/";

  std::set<std::string> aliases;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(content_root, ec)) {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".big") {
      continue;
    }

    std::ifstream stream(entry.path(), std::ios::binary);
    if (!stream) {
      continue;
    }

    std::string buffer;
    buffer.resize(kScanBytes);
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<size_t>(stream.gcount()));

    size_t pos = 0;
    while ((pos = buffer.find(kPrefix, pos)) != std::string::npos) {
      size_t name_start = pos + kPrefix.size();
      size_t name_end = name_start;
      while (name_end < buffer.size() && IsRecipeNameChar(buffer[name_end])) {
        ++name_end;
      }
      if (name_end > name_start) {
        aliases.insert(buffer.substr(name_start, name_end - name_start));
      }
      pos = name_end;
    }
  }

  return aliases;
}

bool Skate3BaseApp::CreateOverlayDirectory(
    const std::filesystem::path& overlay_root, std::string_view guest_path) {
  std::filesystem::path path = overlay_root;
  size_t segment_start = 0;
  while (segment_start < guest_path.size()) {
    const size_t slash = guest_path.find('/', segment_start);
    const size_t segment_end = slash == std::string_view::npos ? guest_path.size() : slash;
    if (segment_end > segment_start) {
      path /= std::string(guest_path.substr(segment_start, segment_end - segment_start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    segment_start = slash + 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  return !ec;
}

void Skate3BaseApp::InstallRecipeOverlay() {
  if (recipe_overlay_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  const auto content_root = runtime()->game_data_root() / "data" / "content";
  if (!std::filesystem::exists(content_root)) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; content root not found: {}",
                content_root.string());
    return;
  }

  const auto aliases = DiscoverRecipeAliases(content_root);
  if (aliases.empty()) {
    REXLOG_WARN("Skipping Skate 3 recipe VFS overlay; no recipe aliases found in {}",
                content_root.string());
    return;
  }

  const auto overlay_root = cache_root() / "vfs_big_directory_aliases";
  std::error_code ec;
  std::filesystem::create_directories(overlay_root, ec);
  if (ec) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create {}: {}",
                overlay_root.string(), ec.message());
    return;
  }

  size_t created = 0;
  for (const auto& alias : aliases) {
    const std::string guest_path = std::string("data/content/recipe/") + alias;
    if (CreateOverlayDirectory(overlay_root, guest_path)) {
      ++created;
    }
  }

  static constexpr std::string_view kBigDirectoryAliases[] = {
      "data/scene",
      "data/scene/anim",
      "data/scene/trickguide",
      "data/livingworld/PluginDescriptor",
      "data/state/livingworldentities/pedestrian/plugin",
  };
  for (std::string_view alias : kBigDirectoryAliases) {
    if (CreateOverlayDirectory(overlay_root, alias)) {
      ++created;
    }
  }

  if (!created) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to create alias directories in {}",
                overlay_root.string());
    return;
  }

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(
      "skate3bigdirs:", overlay_root, true);
  if (!device->Initialize()) {
    REXLOG_WARN("Skipping Skate 3 BIG-directory VFS overlay; failed to initialize host device for {}",
                overlay_root.string());
    return;
  }

  runtime()->file_system()->RegisterDevice(std::move(device));
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\content\\recipe",
      "skate3bigdirs:\\data\\content\\recipe");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\scene",
      "skate3bigdirs:\\data\\scene");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\livingworld\\PluginDescriptor",
      "skate3bigdirs:\\data\\livingworld\\PluginDescriptor");
  runtime()->file_system()->RegisterSymbolicLink(
      "\\Device\\Harddisk0\\Partition1\\data\\state\\livingworldentities\\pedestrian\\plugin",
      "skate3bigdirs:\\data\\state\\livingworldentities\\pedestrian\\plugin");
  recipe_overlay_installed_ = true;
  REXLOG_INFO("Installed Skate 3 BIG-directory VFS overlay with {} aliases from {}",
              created, content_root.string());
}

void Skate3BaseApp::InstallBigDeviceAliases() {
  if (big_device_aliases_installed_ || !runtime() || !runtime()->file_system()) {
    return;
  }

  // Skate 3 uses these file-server style schemes for world content probes.
  // Route them to the normal title mount so existing disc files resolve.
  runtime()->file_system()->RegisterSymbolicLink(
      "big:", "\\Device\\Harddisk0\\Partition1");
  runtime()->file_system()->RegisterSymbolicLink(
      "dlcbig:", "\\Device\\Harddisk0\\Partition1");
  big_device_aliases_installed_ = true;
}

bool Skate3BaseApp::IsContentPackFolder(const std::filesystem::path& dir) {
  // Both halves, and a header of a plausible size. The descriptor is a fixed
  // 328-byte record whose trailing fields are anchored to the end of the file,
  // so a truncated or emptied one is not something to offer the player - it
  // would stage and then be dropped by the content manager without a word.
  bool has_big = false;
  std::error_code ec;
  for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
    if (ec || !file.is_regular_file()) {
      continue;
    }
    const auto ext = file.path().extension();
    if (ext == ".big") {
      has_big = true;
    } else if (ext == ".header") {
      std::error_code size_ec;
      if (std::filesystem::file_size(file.path(), size_ec) < 256 || size_ec) {
        return false;
      }
    }
  }
  if (!has_big) {
    return false;
  }
  for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
    if (!ec && file.is_regular_file() && file.path().extension() == ".header") {
      return true;
    }
  }
  return false;
}

void Skate3BaseApp::StageContentPacks() {
  // Custom map packs ship as already-extracted marketplace content - a .big
  // beside its .header - rather than an STFS package the DLC installer could
  // open. The layout the content manager expects splits the two apart, which
  // is what the desktop loader produces:
  //
  //   <user>/0000000000000000/<title>/00000002/<PKG>/<file>.big
  //   <user>/0000000000000000/<title>/Headers/00000002/<PKG>.header
  //
  // The header is NOT left beside the .big, and its name is the package name,
  // not the file's own. Putting it in the package folder leaves the content
  // manager with content it has no descriptor for.
  //
  // On a phone a pack can only be dropped at the top of Documents, so this
  // moves it into place at every start. The folder name is load-bearing: the
  // id inside the header has to match it or the package is dropped silently.
  if (!runtime() || !runtime()->kernel_state()) {
    return;
  }
  const uint32_t title_id = runtime()->kernel_state()->title_id();
  if (title_id == 0) {
    return;
  }
  const auto documents = runtime()->user_data_root().parent_path();
  const auto title_root = runtime()->user_data_root() / "0000000000000000" /
                          fmt::format("{:08X}", title_id);
  const auto content_dir = title_root / "00000002";
  const auto headers_dir = title_root / "Headers" / "00000002";

  // ONE pack per launch. Staging every pack found overloads the game's boot
  // content scan - the desktop loader hit the same wall with the shipped Danny
  // Way and Maloof content and settled on staging exactly one - and it is also
  // what makes "which map am I loading" answerable. Which one is
  // skate3_content_pack; empty means the first by name.
  std::string wanted = REXCVAR_GET(skate3_content_pack);
  std::vector<std::filesystem::path> candidates;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(documents, ec)) {
    if (ec || !entry.is_directory()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name == "user" || name == "game") {
      continue;
    }
    if (IsContentPackFolder(entry.path())) {
      candidates.push_back(entry.path());
      continue;
    }
    std::error_code nest_ec;
    for (const auto& child : std::filesystem::directory_iterator(entry.path(), nest_ec)) {
      if (!nest_ec && child.is_directory() && IsContentPackFolder(child.path())) {
        candidates.push_back(child.path());
        break;
      }
    }
  }
  std::sort(candidates.begin(), candidates.end());
  // More than one and no choice already made: ask, before anything is staged.
  // The guest has not started, so blocking here is free - and it has to happen
  // now, because the boot content scan reads what this stages.
  if (chose_content_pack_) {
    // Answered before the guest started; empty means "stock game, stage
    // nothing".
    if (chosen_content_pack_.empty()) {
      REXLOG_INFO("Skate 3: no content pack staged this launch");
      return;
    }
    wanted = chosen_content_pack_;
  }

  for (const auto& entry : candidates) {
    const std::string package = entry.filename().string();
    // A named pack wins; otherwise the first by name, so the choice is at
    // least stable between launches rather than filesystem order.
    if (!wanted.empty() && package != wanted) {
      continue;
    }

    std::filesystem::path big, header;
    std::error_code scan_ec;
    for (const auto& file : std::filesystem::directory_iterator(entry, scan_ec)) {
      if (scan_ec || !file.is_regular_file()) {
        continue;
      }
      const auto ext = file.path().extension();
      if (ext == ".big") {
        big = file.path();
      } else if (ext == ".header") {
        header = file.path();
      }
    }
    // Both halves or nothing: content without its descriptor is what the
    // content manager silently drops, and a descriptor alone names nothing.
    if (big.empty() || header.empty()) {
      continue;
    }

    // Clear every other pack we know about first. Staging is additive, and a
    // pack left behind from an earlier launch stays installed - so choosing a
    // different one simply added it beside the old, and the game went on
    // loading whichever it found first. Only the packs this scan found are
    // touched, so official content installed by other means is left alone.
    for (const auto& other : candidates) {
      const std::string other_name = other.filename().string();
      if (other_name == package) {
        continue;
      }
      std::error_code rm_ec;
      const auto stale_dir = content_dir / other_name;
      if (std::filesystem::exists(stale_dir, rm_ec)) {
        std::filesystem::remove_all(stale_dir, rm_ec);
        REXLOG_INFO("Skate 3: removed previously staged pack '{}'", other_name);
      }
      std::filesystem::remove(headers_dir / (other_name + ".header"), rm_ec);
    }

    const auto target_dir = content_dir / package;
    const auto target_big = target_dir / big.filename();
    const auto target_header = headers_dir / (package + ".header");
    if (std::filesystem::exists(target_big, ec) &&
        std::filesystem::exists(target_header, ec)) {
      REXLOG_INFO("Skate 3 content pack '{}' already staged", package);
      break;  // one per launch; others were cleared above
    }

    std::filesystem::create_directories(target_dir, ec);
    std::filesystem::create_directories(headers_dir, ec);
    std::error_code copy_ec;
    std::filesystem::copy_file(big, target_big,
                               std::filesystem::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
      REXLOG_WARN("Could not stage '{}' content: {}", package, copy_ec.message());
      continue;
    }
    std::filesystem::copy_file(header, target_header,
                               std::filesystem::copy_options::overwrite_existing, copy_ec);
    if (copy_ec) {
      REXLOG_WARN("Could not stage '{}' header: {}", package, copy_ec.message());
      continue;
    }
    REXLOG_INFO("Staged Skate 3 content pack '{}' ({} + {})", package,
                target_big.string(), target_header.string());
    break;  // one per launch
  }
}

void Skate3BaseApp::InstallDlcPackages() {
  StageContentPacks();

  if (!REXCVAR_GET(skate3_auto_install_dlc) || !runtime() || !runtime()->kernel_state() ||
      !runtime()->kernel_state()->content_manager()) {
    return;
  }

  const uint32_t title_id = runtime()->kernel_state()->title_id();
  if (title_id == 0) {
    REXLOG_WARN("Skipping Skate 3 DLC install; title ID is not available");
    return;
  }

  const auto user_dlc_root = runtime()->user_data_root() / std::string(kDlcDirectoryName);
  std::error_code create_ec;
  std::filesystem::create_directories(user_dlc_root, create_ec);
  if (create_ec) {
    REXLOG_WARN("Could not create Skate 3 DLC drop folder {}: {}", user_dlc_root.string(),
                create_ec.message());
  }

  const auto source_dirs =
      DiscoverDlcSourceDirectories(rex::filesystem::GetAppRootFolder(),
                                   runtime()->game_data_root(), runtime()->user_data_root());
  std::unordered_set<std::string> seen_packages;
  size_t installed_count = 0;
  size_t skipped_count = 0;

  for (const auto& source_dir : source_dirs) {
    if (!std::filesystem::is_directory(source_dir)) {
      continue;
    }

    std::error_code iter_ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source_dir, iter_ec)) {
      if (iter_ec) {
        REXLOG_WARN("Could not scan Skate 3 DLC folder {}: {}", source_dir.string(),
                    iter_ec.message());
        break;
      }
      if (!entry.is_regular_file()) {
        continue;
      }

      const auto package_path = entry.path();
      // The folder ships its own README, and people leave notes next to their
      // packs. A content package has no extension (LIVE/CON) or is a .big, so
      // skipping plain text costs nothing and stops the scan warning about
      // files it was never meant to read.
      {
        auto ext = package_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".txt" || ext == ".md" || ext == ".nfo" || ext == ".ds_store") {
          continue;
        }
        if (package_path.filename() == ".DS_Store") {
          continue;
        }
      }
      std::error_code canonical_ec;
      auto package_key = std::filesystem::weakly_canonical(package_path, canonical_ec).string();
      if (canonical_ec) {
        package_key = std::filesystem::absolute(package_path).string();
      }
      if (!seen_packages.insert(package_key).second) {
        continue;
      }

      // The next three skips are INFO, not WARN. Every one of them means "a
      // file in this folder is not DLC for this game", which is the normal
      // state of a folder containing anything else - and the scan hits every
      // save and content package on the device, so at warn they were around
      // fifty lines a boot burying whatever real warning followed. The summary
      // below reports the count; a skip that is genuinely a failure (install
      // returned an error) is still a warning.
      const auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
      if (!header) {
        REXLOG_INFO("Skipping DLC candidate with invalid STFS header: {}", package_path.string());
        ++skipped_count;
        continue;
      }

      const auto content_type =
          static_cast<rex::system::XContentType>(header->metadata.content_type);
      if (content_type != rex::system::XContentType::kMarketplaceContent) {
        REXLOG_INFO("Skipping non-DLC content package {} with type {:08X}",
                    package_path.filename().string(), static_cast<uint32_t>(content_type));
        ++skipped_count;
        continue;
      }

      const uint32_t package_title_id = header->metadata.execution_info.title_id;
      if (package_title_id != 0 && package_title_id != title_id) {
        REXLOG_INFO("Skipping DLC package {} for title {:08X}; running title is {:08X}",
                    package_path.filename().string(), package_title_id, title_id);
        ++skipped_count;
        continue;
      }

      if (IsInstalledMarketplaceContent(runtime()->user_data_root(), title_id, package_path)) {
        REXLOG_INFO("DLC package already installed: {}", package_path.filename().string());
        ++skipped_count;
        continue;
      }

      REXLOG_INFO("Installing Skate 3 DLC package: {}", package_path.string());
      const auto result = runtime()->kernel_state()->content_manager()->InstallContent(package_path);
      if (result == 0) {
        ++installed_count;
        REXLOG_INFO("Installed Skate 3 DLC package: {}", package_path.filename().string());
      } else {
        ++skipped_count;
        REXLOG_WARN("Failed to install Skate 3 DLC package {}: {:08X}",
                    package_path.filename().string(), static_cast<uint32_t>(result));
      }
    }
  }

  if (installed_count || skipped_count) {
    REXLOG_INFO("Skate 3 DLC scan complete: {} installed, {} skipped", installed_count,
                skipped_count);
  } else {
    REXLOG_INFO("No Skate 3 DLC packages found. Drop legally obtained DLC package files in {}",
                user_dlc_root.string());
  }
}
