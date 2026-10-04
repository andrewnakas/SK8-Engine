#pragma once

// A scripted render camera for watching someone else: while set, the render
// camera looks from `eye` at `target` instead of following the local skater,
// and - like the drone camera - the game's own main camera is taken over
// (ViewCamera::SetViewMatrix), so the game culls and submits the world around
// the watched spot. The game keeps simulating the local skater as usual.
//
// Any thread; the next built frame picks it up. The drone camera, when
// engaged, wins.

namespace skate3::native_scene {

void SetSpectatorCamera(const float eye[3], const float target[3]);
void ClearSpectatorCamera();

}  // namespace skate3::native_scene
