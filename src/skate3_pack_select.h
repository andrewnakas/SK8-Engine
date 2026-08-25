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
#include <functional>
#include <vector>

namespace rex::ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace skate3 {

// Shows the list and returns immediately; `chosen` fires on the UI thread with
// the folder name, or empty for "stock game".
//
// Asynchronous rather than blocking, because this runs from OnFinalizePaths -
// before OnInitialize has returned, so the event loop is not pumping yet and a
// blocking wait draws nothing at all. The caller returns std::nullopt and
// resumes from the callback, which is the same shape the install wizards use
// on Apple platforms.
void ShowPackSelect(rex::ui::ImGuiDrawer* drawer, const std::vector<std::string>& packs,
                    std::function<void(std::string)> chosen);

}  // namespace skate3

#endif  // SKATE3_PACK_SELECT_H_
