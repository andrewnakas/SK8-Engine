#pragma once

// The engine side of the multiplayer seam. The multiplayer module (built from
// SKATE3_MULTIPLAYER_DIR, see CMakeLists.txt) fills this table in at static
// initialisation; the engine calls through it at a few fixed points. With no
// module linked every pointer stays null and each call site is one branch.

namespace rex::ui {
class ImGuiDrawer;
}

namespace skate3::native_scene {
struct FrameScene;
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
};

void Register(const Hooks& hooks);
const Hooks& Get();

}  // namespace skate3::mp
