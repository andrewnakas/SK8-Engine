// Startup pack chooser.
//
// Only one content pack can be staged per launch - the game's boot content
// scan does not cope with several at once - so when more than one is sitting
// in Documents, something has to pick. This asks, before the guest starts,
// because the choice has to be made before the content is staged and the boot
// scan reads it.
//
// Nothing is shown when there is one pack or none.

#ifndef SKATE3_PACK_SELECT_H_
#define SKATE3_PACK_SELECT_H_

#include <filesystem>
#include <string>
#include <vector>

namespace rex::ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace skate3 {

// Blocks until one is chosen, pumping the UI the way the title-update wizard
// does. Returns the chosen folder name, or empty if the choice was skipped or
// the app is quitting - in which case the caller stages the first by name.
std::string ChoosePackBlocking(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                               rex::ui::ImGuiDrawer* drawer,
                               const std::vector<std::string>& packs);

}  // namespace skate3

#endif  // SKATE3_PACK_SELECT_H_
