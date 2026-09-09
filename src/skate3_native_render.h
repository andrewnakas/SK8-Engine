#pragma once

// Skate 3 native renderer (data-driven scene renderer).
//
// The guest-function hooks in skate3_native_render.cpp are link-time
// weak-symbol overrides and are always present; everything they do is gated
// behind the skate3_native_render cvar. Install() only announces state and
// validates configuration. The scene capture/build and rendering live in
// skate3_native_scene.cpp.

namespace skate3::native_render {

void Install();

// The title's screen manager, sampled from a host thread.
//
// sub_82965C90 is the loop the guest main thread never leaves: a small state
// machine over a screen object, switching on a state word and asking a loader
// "is the next screen ready yet". Its state is invisible from the outside - a
// thread dump shows the main thread parked in the ordinary per-frame wait
// whether the machine is advancing or stuck. Reading the state word directly
// is the only way to tell those apart. The pointer is captured when the guest
// enters the loop; before that this logs nothing.
void LogScreenManagerState();

// Where a frame's time actually goes, averaged: inside the guest's own swap,
// inside the hook layer, and everything else the guest does between swaps.
void LogFrameBudget();

// How long the guest job manager's candidate list is: the scan that dominates
// the frame is O(list length).
void LogJobScan();

}  // namespace skate3::native_render
