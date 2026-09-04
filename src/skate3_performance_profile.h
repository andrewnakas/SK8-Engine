#pragma once

// Video defaults that suit the machine the game is actually running on.
//
// The shipped defaults are a desktop discrete GPU's: the world is drawn at
// twice the window's resolution with 4x MSAA, soft shadows, ambient occlusion
// and doubled draw distance. That is the right picture on a machine that can
// hold it and unplayable on one that cannot, and a handheld cannot - a Steam
// Deck at 1280x800 with resolution_scale 2 is rendering 2560x1600 with MSAA
// before a single effect is switched on.
//
// So: name the bundles, pick one from the hardware on first run, and let
// anybody set one by hand afterwards.

#include <filesystem>
#include <string_view>

namespace skate3::perf {

enum class Profile {
  kQuality,      // the shipped desktop settings
  kBalanced,     // native resolution, effects kept
  kDeck,         // handhelds and integrated GPUs
  kPerformance,  // every frame that can be had, picture last
};

// What this machine looks like, from the things that can be read before the
// graphics device exists: the Steam Deck's own environment variable and its
// DMI board names. Anything not recognised is treated as a desktop, because
// guessing "slow" for a machine that is not would quietly halve its picture.
Profile DetectProfile();

// Which Steam Deck this is, by DMI board name. The two panels differ in
// refresh rate, which is the only reason the engine cares.
enum class DeckModel {
  kNotDeck,
  kLcd,   // "Jupiter" - 800p 60Hz
  kOled,  // "Galileo" - 800p 90Hz
};
DeckModel DetectSteamDeckModel();

// On Steam Deck hardware, apply the handheld preset and the panel's refresh
// cap ONCE, leaving a marker beside the settings so it never runs again.
//
// This exists because first-run detection alone does not reach a Deck that has
// run the game before: saves live in ~/.local/share/skate3 and survive
// reinstalling the engine, so settings.toml is already there on the "first"
// run of a new build and the auto preset never fires. Returns whether it ran.
bool ApplyDeckDefaultsOnce(const std::filesystem::path& user_data_root);

std::string_view ProfileName(Profile profile);

// Apply a bundle by setting the cvars it covers. Only ever called for a
// profile that was chosen deliberately - by DetectProfile on a first run, or
// by the player through skate3_performance_profile.
void ApplyProfile(Profile profile);

// Resolve skate3_performance_profile, applying it when the player asked for
// one and falling back to `on_first_run` (which is DetectProfile's answer)
// when they did not. Returns whether anything was applied.
bool ApplyRequestedProfile(bool first_run);

// Make the in-game Video menu's preset row take effect immediately, by
// applying whatever skate3_performance_profile is set to whenever it changes.
void InstallLiveProfileSwitch();

}  // namespace skate3::perf
