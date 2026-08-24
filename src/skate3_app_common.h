#pragma once

#include "generated/skate3_init.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <rex/rex_app.h>
#include <rex/ui/overlay/simple_settings_overlay.h>

#include "skate3_loader_overlay.h"
#include "skate3_native_debug_dialog.h"
#include "skate3_touch_controls.h"

namespace rex::ui {
class ImGuiDrawer;
}

class Skate3BaseApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~Skate3BaseApp() override;

 protected:
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  void InstallRecipeOverlay();
  // Fills the level picker from the staged game folder when no launcher has.
  void PopulateMapList();
  void InstallBigDeviceAliases();
  void InstallDlcPackages();
  void ToggleSimpleSettings();
  void ToggleNativeDebug();
  void ApplySettingsCursorMode();
  void ApplyGameplayCursorMode();
  void RestartGame();
  void SaveDrawFingerprintLog();
  void LogUserMarker();
  void LogDebugMarker();
  void ApplySelectedProfileToRuntime();

  static bool IsRecipeNameChar(char c);
  static std::set<std::string> DiscoverRecipeAliases(
      const std::filesystem::path& content_root);
  static bool CreateOverlayDirectory(const std::filesystem::path& overlay_root,
                                     std::string_view guest_path);

  std::filesystem::path config_path_;
  std::filesystem::path user_settings_path_;
  std::filesystem::path profiles_path_;
  std::unique_ptr<rex::ui::SimpleSettingsDialog> simple_settings_dialog_;
  std::unique_ptr<skate3::NativeDebugDialog> native_debug_dialog_;
  std::unique_ptr<skate3::RenderModeIndicator> render_mode_indicator_;
  std::unique_ptr<skate3::TouchControlsOverlay> touch_controls_;
  bool map_list_populated_ = false;
  // Full-screen "loading <map>" cover for launcher-driven map selection.
  std::unique_ptr<skate3::LoaderOverlay> loader_overlay_;
  std::unique_ptr<skate3::LevelSelectDialog> level_select_dialog_;
  bool recipe_overlay_installed_ = false;
  bool big_device_aliases_installed_ = false;
  std::atomic<uint32_t> debug_marker_count_{0};
};
