#pragma once

// Name a video preset at launch, for scripting and for A/B measurement.
//
// The in-game Preset row on the Video and Performance pages is the way a player
// picks one. This is the way a measurement run picks one: a cvar, so it can be
// set from Documents/user/ios_args.txt or a desktop command line without
// touching a dozen individual settings and getting one of them wrong.
//
// Both go through rex::ui::ApplyGraphicsPresetByName and the same preset table,
// so they cannot come to mean different things.
//
// Deliberately NOT persisted and NOT auto-detected. A preset that re-applies
// itself every launch quietly undoes whatever the player tuned by hand
// afterwards, and a preset derived from the hardware does that without ever
// having been asked for. Naming one here is a request, made once.

namespace skate3 {

// Apply skate3_performance_profile if it names a preset. Does nothing when it
// is empty, which is the default.
void ApplyRequestedPerformanceProfile();

}  // namespace skate3
