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
// Must sit at global scope beside the one above: the macro names a storage
// accessor with external linkage, and declaring it inside skate3's anonymous
// namespace asks the linker for a symbol nothing defines.
REXCVAR_DECLARE(double, touch_opacity);

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

// Everything the controls draw is scaled by this; the editor chrome is not,
// because a player who has turned the controls all the way down still has to
// be able to see what they are dragging.
float Alpha(float a) {
  // Full strength while the editor is open: a control turned down to invisible
  // is still a control that has to be dragged.
  if (rex::input::touch::TouchLayoutEditing()) {
    return std::clamp(a * 2.0f, 0.0f, 1.0f);
  }
  return std::clamp(a * float(REXCVAR_GET(touch_opacity)), 0.0f, 1.0f);
}

ImU32 White(float a) { return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a)); }
ImU32 Black(float a) { return ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, a)); }
ImU32 Accent(float a) { return ImGui::GetColorU32(ImVec4(0.35f, 0.78f, 1.0f, a)); }

// What sits inside a button.
//
// The four directions, Start and Back used to be Unicode characters in the
// label. The ImGui default font has none of those code points, so all six came
// out as literal question marks - which is what the very first screenshot in
// the shadows bug report shows, six "?" circles around the edge of the screen.
// Drawing them costs a few lines and cannot be defeated by a font.
void DrawGlyph(ImDrawList* dl, const rex::input::touch::TouchControl& c,
               const ImVec2 centre, float r, bool held) {
  using rex::input::touch::TouchGlyph;
  const bool editing = rex::input::touch::TouchLayoutEditing();
  const auto a = [editing](float v) { return editing ? v : Alpha(v); };
  const ImU32 ink = White(a(held ? 0.95f : kLabelAlpha));
  const ImU32 shadow = Black(a(kLabelAlpha));
  const float g = r * 0.42f;  // glyph half-extent

  // Every shape is drawn twice, one pixel down and right in black first: the
  // game behind these is every colour at some point, white alone disappears
  // against concrete.
  const auto triangle = [&](float dx, float dy) {
    for (int pass = 0; pass < 2; ++pass) {
      const float o = pass == 0 ? 1.0f : 0.0f;
      const ImU32 col = pass == 0 ? shadow : ink;
      const ImVec2 tip(centre.x + dx * g + o, centre.y + dy * g + o);
      // The base is perpendicular to the tip direction.
      const ImVec2 a(centre.x - dx * g * 0.7f + dy * g + o,
                     centre.y - dy * g * 0.7f + dx * g + o);
      const ImVec2 b(centre.x - dx * g * 0.7f - dy * g + o,
                     centre.y - dy * g * 0.7f - dx * g + o);
      dl->AddTriangleFilled(tip, a, b, col);
    }
  };

  switch (c.glyph) {
    case TouchGlyph::kNone:
      return;
    case TouchGlyph::kUp:
      triangle(0.0f, -1.0f);
      return;
    case TouchGlyph::kDown:
      triangle(0.0f, 1.0f);
      return;
    case TouchGlyph::kLeft:
      triangle(-1.0f, 0.0f);
      return;
    case TouchGlyph::kRight:
      triangle(1.0f, 0.0f);
      return;
    case TouchGlyph::kStart: {
      // Two stacked bars, the shape every pad has used for Start since the
      // Xbox 360 controller stopped writing the word out.
      for (int pass = 0; pass < 2; ++pass) {
        const float o = pass == 0 ? 1.0f : 0.0f;
        const ImU32 col = pass == 0 ? shadow : ink;
        for (int bar = 0; bar < 2; ++bar) {
          const float y = centre.y + (bar == 0 ? -g * 0.45f : g * 0.25f) + o;
          dl->AddRectFilled(ImVec2(centre.x - g + o, y),
                            ImVec2(centre.x + g + o, y + g * 0.34f), col);
        }
      }
      return;
    }
    case TouchGlyph::kBack: {
      // A left chevron: "go back", and distinguishable from the d-pad's left
      // triangle at a glance because it is an outline rather than a solid.
      for (int pass = 0; pass < 2; ++pass) {
        const float o = pass == 0 ? 1.0f : 0.0f;
        const ImU32 col = pass == 0 ? shadow : ink;
        dl->AddLine(ImVec2(centre.x + g * 0.5f + o, centre.y - g + o),
                    ImVec2(centre.x - g * 0.5f + o, centre.y + o), col, 2.0f);
        dl->AddLine(ImVec2(centre.x - g * 0.5f + o, centre.y + o),
                    ImVec2(centre.x + g * 0.5f + o, centre.y + g + o), col, 2.0f);
      }
      return;
    }
    case TouchGlyph::kMenu: {
      // A gear: a ring with teeth. Reads as "settings" without a word on it,
      // which matters because this button sits beside Start and Back and the
      // three must not be confusable at a glance.
      for (int pass = 0; pass < 2; ++pass) {
        const float o = pass == 0 ? 1.0f : 0.0f;
        const ImU32 col = pass == 0 ? shadow : ink;
        const ImVec2 c(centre.x + o, centre.y + o);
        dl->AddCircle(c, g * 0.55f, col, 20, 2.0f);
        for (int tooth = 0; tooth < 8; ++tooth) {
          const float ang = float(tooth) * 3.14159265f / 4.0f;
          const float cx = std::cos(ang);
          const float sy = std::sin(ang);
          dl->AddLine(ImVec2(c.x + cx * g * 0.62f, c.y + sy * g * 0.62f),
                      ImVec2(c.x + cx * g, c.y + sy * g), col, 2.0f);
        }
      }
      return;
    }
    case TouchGlyph::kLabel:
    default:
      break;
  }
  if (c.label == nullptr || c.label[0] == '\0') {
    return;
  }
  const ImVec2 size = ImGui::CalcTextSize(c.label);
  const ImVec2 at(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f);
  dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), shadow, c.label);
  dl->AddText(at, ink, c.label);
}

// A band across the top saying what this mode is and how to leave it.
//
// Worth the space: the mode takes the pad away from the game, and a player who
// cannot work out how to get it back has lost their session. The controls
// themselves become the instructions - drag one, pinch one - so this only has
// to name the two things the controls cannot say.
void DrawEditorChrome(ImDrawList* dl, ImGuiIO& io, const TouchVisualState& state) {
  const float w = io.DisplaySize.x;
  const float h = io.DisplaySize.y;
  const float band = std::max(52.0f, h * 0.11f);
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(w, band), Black(0.72f));
  dl->AddLine(ImVec2(0.0f, band), ImVec2(w, band), Accent(0.8f), 2.0f);

  const char* title = "Arranging the on-screen controls";
  const char* help =
      "Drag a control to move it.  Two fingers on one control to resize it.  "
      "Close the settings menu when you are done.";
  const ImVec2 title_size = ImGui::CalcTextSize(title);
  const ImVec2 help_size = ImGui::CalcTextSize(help);
  dl->AddText(ImVec2((w - title_size.x) * 0.5f, band * 0.22f), Accent(0.95f), title);
  dl->AddText(ImVec2((w - help_size.x) * 0.5f, band * 0.58f), White(0.8f), help);

  // Dim the game so the controls read as the subject rather than as an
  // overlay on top of something that is still being played.
  dl->AddRectFilled(ImVec2(0.0f, band), ImVec2(w, h), Black(0.35f));
  (void)state;
}

}  // namespace

bool TouchControlsOverlay::WantsContinuousRepaint() const {
  // Only while the controls are up: a repaint every frame with no controller
  // attached is the price of following the thumb, but paying it when a pad is
  // plugged in would present frames the guest never produced.
  //
  // The editor is the exception: it has to follow the finger whether or not a
  // pad is attached, because arranging the touch controls with a controller
  // plugged in is a perfectly reasonable thing to do.
  return rex::input::touch::TouchControlsActive() ||
         rex::input::touch::TouchLayoutEditing();
}

void TouchControlsOverlay::OnDraw(ImGuiIO& io) {
  const TouchVisualState state = rex::input::touch::GetTouchVisualState();
  const bool editing = rex::input::touch::TouchLayoutEditing();
  // The editor draws even with a pad attached - see WantsContinuousRepaint.
  if (!state.active && !editing) {
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

  if (editing) {
    DrawEditorChrome(dl, io, state);
  }

  for (size_t i = 0; i < count; ++i) {
    const TouchControl& c = layout[i];
    // While editing, "held" means the finger is dragging this one. Nothing is
    // pressed in that mode, so the highlight is free to mean something else.
    const bool held = editing ? (state.held == c.id) : state.pressed[size_t(c.id)];
    const ImVec2 centre(c.centre_x * w, c.centre_y * h);
    const float r = c.radius * unit;

    if (c.is_stick) {
      // The well, then the thumb pad offset inside it - the same convention a
      // physical stick has, so where the thumb is reads immediately.
      dl->AddCircleFilled(centre, r, Black(Alpha(kIdleFill * 1.6f)), 48);
      dl->AddCircle(centre, r, White(Alpha(held ? kHeldOutline : kIdleOutline)), 48, 2.0f);
      const float ax = (c.id == TouchControlId::kLeftStick) ? state.left_x : state.right_x;
      const float ay = (c.id == TouchControlId::kLeftStick) ? state.left_y : state.right_y;
      // y flips back: the state is y-up, the screen is y-down.
      const ImVec2 knob(centre.x + ax * r * 0.62f, centre.y - ay * r * 0.62f);
      dl->AddCircleFilled(knob, r * 0.38f,
                          White(Alpha(held ? kHeldFill + 0.15f : kIdleFill + 0.06f)), 32);
      dl->AddCircle(knob, r * 0.38f, White(Alpha(held ? kHeldOutline : kIdleOutline)), 32,
                    1.5f);
      continue;
    }

    dl->AddCircleFilled(centre, r, Black(Alpha(kIdleFill * 1.6f)), 32);
    dl->AddCircleFilled(centre, r, White(Alpha(held ? kHeldFill : kIdleFill)), 32);
    dl->AddCircle(centre, r, White(Alpha(held ? kHeldOutline : kIdleOutline)), 32, 2.0f);

    DrawGlyph(dl, c, centre, r, held);
  }
}

#else  // !SKATE3_HAS_TOUCH_CONTROLS

bool TouchControlsOverlay::WantsContinuousRepaint() const { return false; }
void TouchControlsOverlay::OnDraw(ImGuiIO&) {}

#endif

}  // namespace skate3
