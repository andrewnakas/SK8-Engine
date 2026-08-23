// Held-key and look-drag state for the developer camera and debug hotkeys.
//
// The debug tooling was written against GetAsyncKeyState/GetCursorPos, which
// left the freecam and the capture hotkeys inert on the Vulkan-only platforms
// - the camera engaged and then simply never moved. This is the portable
// replacement: Windows keeps reading the async key state exactly as before,
// and every other platform is served by a window input listener attached to
// the game window.
//
// Only continuous state belongs here. One-shot hotkeys should prefer
// rex::ui::RegisterBind, which is rebindable and shows up in the settings
// overlay; see skate3_app_common.cpp.
#pragma once

#include <rex/ui/virtual_key.h>

namespace rex::ui {
class Window;
}

namespace skate3::debug_input {

// Attaches the input listener to the game window. Safe to call more than once
// and safe to call with nullptr; a no-op on Windows.
void Install(rex::ui::Window* window);

// True while the key is held. On Windows this is global async key state, as
// it has always been; elsewhere it is window-scoped, so held keys clear when
// the game loses focus.
bool IsKeyDown(rex::ui::VirtualKey key);

// Right-drag look. Returns false while the right button is up. While it is
// held, reports the pixel delta since the previous call and returns true; the
// first call of a drag reports (0, 0) so the camera does not jump.
bool PollLookDrag(double& dx_pixels, double& dy_pixels);

}  // namespace skate3::debug_input
