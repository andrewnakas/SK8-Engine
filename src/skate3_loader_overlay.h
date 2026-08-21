#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace skate3 {

// The guest memory base, published once per frame from the D3D swap hook. The
// level picker reads the frontend's selection cursor through it so it can walk
// the map list with feedback instead of pressing 'down' a fixed number of times.
void SetLoaderGuestBase(uint8_t* base);

// Whether the launcher asked for the session to start at the map picker.
bool LoaderPickerOpensAtStart();

// True while the picker's feedback-driven menu walk is in progress.
bool LoaderNavigationActive();

// Full-screen "loading <map>" cover, drawn over the game's own output while an
// external launcher's boot macro drives the menus.
//
// This exists so map selection can be fully automated without the player
// watching the frontend get puppeted: the launcher stages one DLC pack, the
// demo-path macro walks the pause menu to the map, and this overlay covers the
// whole thing until the world is up. Doing it here rather than in a separate
// launcher window avoids fighting the compositor for stacking order -- on
// Wayland a client cannot raise itself above another app at all, and even on
// X11 a fullscreen game window outranks an ordinary always-on-top window.
//
// Enabled with skate3_loader_overlay; the title/subtitle/hue cvars carry what
// to draw. It hides itself once the macro has finished AND the guest has come
// back to the gameplay presence context, i.e. the map has actually loaded.
class LoaderOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit LoaderOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  // True while the overlay still wants the screen.
  bool covering() const { return phase_ != Phase::kDone; }

  // Re-arm for an in-game map switch: cover the screen again and follow the
  // new load through to gameplay.
  void Restart(const std::string& title, const std::string& subtitle, float hue);

 protected:
  // The overlay animates and polls guest presence, so it must repaint even
  // when the guest is not producing frames (it is covering exactly the periods
  // where the guest stalls on streaming).
  bool WantsContinuousRepaint() const override { return covering(); }
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Phase {
    kBooting,      // waiting for the guest to reach gameplay the first time
    kNavigating,   // macro is walking the menus
    kStreaming,    // macro done; world is loading (presence dropped to menus)
    kSettling,     // presence back in gameplay; brief hold before revealing
    kDone,
  };

  void Advance(float delta_seconds);
  float TargetFraction() const;

  Phase phase_ = Phase::kBooting;
  float shown_fraction_ = 0.0f;
  float settle_seconds_ = 0.0f;
  float phase_seconds_ = 0.0f;
  float fade_ = 1.0f;
  bool saw_streaming_ = false;
  bool saw_nav_ = false;
  // Set by Restart(); empty means "use the cvars the launcher passed".
  std::string title_override_;
  std::string subtitle_override_;
  float hue_override_ = -1.0f;
};

// In-game map picker. The launcher passes the staged pack's map list in
// skate3_loader_levels; choosing one replays the same pause-menu macro the boot
// path uses, so you can change maps without quitting to the launcher.
//
// Limited to the pack staged at boot, by construction: the set of installed DLC
// is fixed when the game starts, so reaching a different pack still needs a
// relaunch.
class LevelSelectDialog final : public rex::ui::ImGuiDialog {
 public:
  LevelSelectDialog(rex::ui::ImGuiDrawer* drawer, LoaderOverlay* overlay)
      : ImGuiDialog(drawer), overlay_(overlay) {}

  void Toggle() {
    visible_ = !visible_;
    LogToggle();
  }
  bool visible() const { return visible_; }
  // True only when a launcher passed a level list, i.e. this session is
  // launcher-driven and Escape should mean "change level".
  static bool available();
  // The picker also shows itself whenever this returns true -- used to pin it
  // beside the Escape settings screen.
  void SetCompanionPredicate(std::function<bool()> predicate) {
    companion_ = std::move(predicate);
  }
  void SetCloseMenusCallback(std::function<void()> callback) {
    close_menus_ = std::move(callback);
  }
  // Open the picker as soon as the game reaches gameplay, so a session can
  // start at "choose a map" instead of booting into one.
  void RequestOpenOnGameplay() { open_on_gameplay_ = true; }

 protected:
  bool WantsContinuousRepaint() const override { return visible_; }
  void OnDraw(ImGuiIO& io) override;

 private:
  void Choose(int index, const std::string& name);
  void LogToggle();

  // Called when a map is chosen, so the Escape settings screen closes with the
  // picker rather than being left open over the new load.
  std::function<void()> close_menus_;

  LoaderOverlay* overlay_ = nullptr;
  bool visible_ = false;
  int focus_ = 0;
  // Set when the controller moves the selection, so the list scrolls to keep
  // it visible; mouse hover deliberately does not set it.
  bool scroll_to_focus_ = false;
  std::function<bool()> companion_;
  bool open_on_gameplay_ = false;
  bool boot_navigation_done_ = false;
};

}  // namespace skate3
