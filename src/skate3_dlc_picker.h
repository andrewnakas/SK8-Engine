#pragma once

// Startup add-on selection: drop map packs in dlc/, pick one at boot.
//
// The shipped build installed every package it could find in the DLC folders
// and let the content scan sort it out. That does not survive a real map
// collection: the boot content scan crashes once too many packages are
// installed, and even when it survives, every pack's locations pile into one
// menu. The working practice from the external launcher was always "stage
// exactly ONE pack per launch"; this module moves that into the game.
//
// What it does, before the guest module launches:
//
//   1. Scans the DLC search folders (dlc/ beside the executable, under the
//      game data root, under the user data root, or skate3_dlc_root) for
//      installable packs - both signed STFS containers and the raw
//      <PKG>/<name>_00000000.big + .header layout community maps ship in.
//   2. Zero packs: nothing to choose, boot the base game.
//      One pack: load it, no questions asked.
//      More than one: ask, with a controller/keyboard/mouse picker.
//   3. Stages the ONE chosen pack into a content root of its own and points
//      marketplace content at it, so the game sees exactly one add-on however
//      many are on disk. Nothing is copied twice: an STFS container is
//      extracted once and reused, a raw .big is linked.

#include <filesystem>
#include <string>
#include <vector>

namespace rex {
class Runtime;
namespace ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace ui
}  // namespace rex

namespace skate3::dlc {

// One installable add-on found on disk.
struct Candidate {
  std::filesystem::path path;    // the container, or the pack's .big
  std::filesystem::path header;  // sibling .header for a raw .big; empty for STFS
  bool stfs = false;
  std::string package;       // content directory name; must equal the header content id
  std::string display_name;  // what the picker shows
  uint64_t bytes = 0;
};

// Everything installable under `search_dirs`, sorted by display name.
std::vector<Candidate> Discover(const std::vector<std::filesystem::path>& search_dirs,
                                uint32_t title_id);

// Ask (or don't), stage the choice, and point marketplace content at it.
// Returns false only when the player quit out of the picker, in which case the
// caller should quit rather than boot.
bool RunStartupSelection(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                         rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime,
                         const std::vector<std::filesystem::path>& search_dirs);

}  // namespace skate3::dlc
