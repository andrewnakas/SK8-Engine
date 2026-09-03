#include "skate3_touch_controls.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include <rex/cvar.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
#define SKATE3_HAS_TOUCH_CONTROLS 1
#include <rex/input/touch_input_driver.h>
#else
#define SKATE3_HAS_TOUCH_CONTROLS 0
#endif

REXCVAR_DECLARE(bool, touch_controls);

namespace skate3 {

#if SKATE3_HAS_TOUCH_CONTROLS

namespace {

using rex::input::touch::TouchControl;
using rex::input::touch::TouchControlId;
using rex::input::touch::TouchVisualState;

// Deliberately faint. These sit on top of the game for the whole session, so
// they have to be readable at a glance and ignorable the rest of the time -
// the alpha rises only for the control under a thumb.
constexpr float kIdleFill = 0.10f;
constexpr float kIdleOutline = 0.28f;
constexpr float kHeldFill = 0.30f;
constexpr float kHeldOutline = 0.75f;
constexpr float kLabelAlpha = 0.55f;

ImU32 White(float a) { return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a)); }
ImU32 Black(float a) { return ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, a)); }

}  // namespace

bool TouchControlsOverlay::WantsContinuousRepaint() const {
  // Only while the controls are up: a repaint every frame with no controller
  // attached is the price of following the thumb, but paying it when a pad is
  // plugged in would present frames the guest never produced.
  return rex::input::touch::TouchControlsActive();
}

void TouchControlsOverlay::OnDraw(ImGuiIO& io) {
  const TouchVisualState state = rex::input::touch::GetTouchVisualState();
  if (!state.active) {
    return;
  }

  size_t count = 0;
  const TouchControl* layout = rex::input::touch::TouchLayout(&count);
  if (layout == nullptr || count == 0) {
    return;
  }

  // Drawn straight onto the background list rather than into a window: these
  // are not interactive ImGui widgets - the driver reads the touchscreen
  // itself - and a window would eat the very touches the driver needs.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float w = io.DisplaySize.x;
  const float h = io.DisplaySize.y;
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }
  // Radii are relative to the shorter side, matching the driver's hit test.
  const float unit = std::min(w, h);

  for (size_t i = 0; i < count; ++i) {
    const TouchControl& c = layout[i];
    const bool held = state.pressed[size_t(c.id)];
    const ImVec2 centre(c.centre_x * w, c.centre_y * h);
    const float r = c.radius * unit;

    if (c.is_stick) {
      // The well, then the thumb pad offset inside it - the same convention a
      // physical stick has, so where the thumb is reads immediately.
      dl->AddCircleFilled(centre, r, Black(kIdleFill * 1.6f), 48);
      dl->AddCircle(centre, r, White(held ? kHeldOutline : kIdleOutline), 48, 2.0f);
      const float ax = (c.id == TouchControlId::kLeftStick) ? state.left_x : state.right_x;
      const float ay = (c.id == TouchControlId::kLeftStick) ? state.left_y : state.right_y;
      // y flips back: the state is y-up, the screen is y-down.
      const ImVec2 knob(centre.x + ax * r * 0.62f, centre.y - ay * r * 0.62f);
      dl->AddCircleFilled(knob, r * 0.38f, White(held ? kHeldFill + 0.15f : kIdleFill + 0.06f), 32);
      dl->AddCircle(knob, r * 0.38f, White(held ? kHeldOutline : kIdleOutline), 32, 1.5f);
      continue;
    }

    dl->AddCircleFilled(centre, r, Black(kIdleFill * 1.6f), 32);
    dl->AddCircleFilled(centre, r, White(held ? kHeldFill : kIdleFill), 32);
    dl->AddCircle(centre, r, White(held ? kHeldOutline : kIdleOutline), 32, 2.0f);

    if (c.label != nullptr && c.label[0] != '\0') {
      const ImVec2 size = ImGui::CalcTextSize(c.label);
      const ImVec2 at(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f);
      // Shadow first: the game behind these is every colour at some point.
      dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), Black(kLabelAlpha), c.label);
      dl->AddText(at, White(held ? 0.95f : kLabelAlpha), c.label);
    }
  }
}

#else  // !SKATE3_HAS_TOUCH_CONTROLS

bool TouchControlsOverlay::WantsContinuousRepaint() const { return false; }
void TouchControlsOverlay::OnDraw(ImGuiIO&) {}

#endif

}  // namespace skate3
