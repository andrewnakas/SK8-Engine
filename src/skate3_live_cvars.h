#pragma once

namespace skate3::live_cvars {

// Poll a file for cvar assignments and apply them to the running game.
//
// No-op unless skate3_live_cvars is true. Installed from the app's startup
// path alongside the guest sampler.
void Install();

}  // namespace skate3::live_cvars
