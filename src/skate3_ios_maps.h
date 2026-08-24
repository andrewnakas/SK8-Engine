// Boot-time map discovery for the touch build.
//
// The level picker in skate3_loader_overlay is driven by cvars that a desktop
// launcher fills in. There is no launcher on a phone, so nothing ever set them
// and the picker never appeared. This scans the game folder the player staged
// and fills the same cvars, so the existing picker works with no launcher.
//
// Two kinds of map are found:
//   * the stock districts, as data/content/world<NAME>.big on the disc;
//   * custom packs, as DIST_<NAME> directories dropped into the game folder.
//     The whole game folder is mounted as the guest's d: drive, so a pack
//     copied in over file sharing is already visible to the game - it only has
//     to be named in the list.

#ifndef SKATE3_IOS_MAPS_H_
#define SKATE3_IOS_MAPS_H_

#include <filesystem>

namespace skate3::ios_maps {

// Scans game_root and fills skate3_loader_levels and friends. Safe to call on
// any platform; does nothing unless the picker has no list yet, so a launcher
// that already populated one always wins.
void PopulateFromGameFolder(const std::filesystem::path& game_root);

// Remembers the world to boot into and asks the player to reopen the app.
// The picker's normal path is to have a launcher relaunch the game, which a
// phone has no way to do - see the comment in the implementation.
bool RequestBootWorld(const std::string& world);

}  // namespace skate3::ios_maps

#endif  // SKATE3_IOS_MAPS_H_
