#pragma once

// The engine side of the multiplayer seam. The multiplayer module (built from
// SKATE3_MULTIPLAYER_DIR, see CMakeLists.txt) fills this table in at static
// initialisation; the engine calls through it at a few fixed points. With no
// module linked every pointer stays null and each call site is one branch.

namespace rex::ui {
class ImGuiDrawer;
struct SimpleSettingsExtraPage;
}

#include <vector>

namespace skate3::native_scene {
struct FrameScene;
struct DrawItem;
}

namespace skate3::mp {

struct Hooks {
  // Skate3BaseApp::OnCreateDialogs, after the engine's own overlays exist.
  void (*on_app_init)(rex::ui::ImGuiDrawer* drawer) = nullptr;
  // Skate3BaseApp::OnShutdown, before the engine's dialogs are torn down.
  void (*on_app_shutdown)() = nullptr;
  // Every frame scene the native renderer publishes, just before it becomes
  // visible to the render thread (guest submit thread).
  void (*on_scene_published)(const native_scene::FrameScene& scene) = nullptr;
  // Render thread, once per rendered frame after the scene's own draw lists
  // are built: extra items to draw this frame (remote players' skaters).
  void (*on_render_scene)(
      const native_scene::FrameScene& scene,
      std::vector<const native_scene::DrawItem*>& extra_opaque,
      std::vector<const native_scene::DrawItem*>& extra_transparent) = nullptr;
  // When the settings menu is built: the module's own page for its rail
  // (Multiplayer). Fill in name/desc/build; leave build empty for no page.
  void (*build_settings_page)(rex::ui::SimpleSettingsExtraPage& page) = nullptr;
};

void Register(const Hooks& hooks);
const Hooks& Get();

// The app's side of the seam, for the module to call: open the settings menu
// straight onto the module's page (toggles closed if it is already showing).
// UI thread. A no-op until the app has installed it.
void SetOpenSettingsPage(void (*open)());
void OpenSettingsPage();

}  // namespace skate3::mp
