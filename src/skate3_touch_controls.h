// Draws the on-screen controls that stand in for a physical pad.
//
// The pad state itself comes from rex::input::touch, which owns the layout and
// the finger tracking; this only renders what that reports. Keeping the two
// apart means the thing the player sees is derived from the thing the guest
// receives, rather than the two being written twice and drifting.

#ifndef SKATE3_TOUCH_CONTROLS_H_
#define SKATE3_TOUCH_CONTROLS_H_

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace skate3 {

class TouchControlsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TouchControlsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  // The controls have to follow the finger, so this one does want a repaint
  // every frame - but only while it is actually being shown, which OnDraw
  // decides.
  bool WantsContinuousRepaint() const override;
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace skate3

#endif  // SKATE3_TOUCH_CONTROLS_H_
