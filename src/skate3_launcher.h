// The launcher home screen: what the player sees before the game starts.
//
// Android has had this since the beginning, as an Activity - Play, install the
// disc, fetch the title update, choose a map pack, save a diagnostic report,
// and a status block saying which of those are done. iOS had none of it. The
// app went straight into the guest, so the only way to see whether the disc was
// staged or the title update had landed was to start the game and watch what
// happened, and the only way to get a log off the phone was to ask the player
// to find it in the Files app.
//
// The pieces were all already here and simply had no front door: the ISO
// wizard, the title update wizard and the pack chooser are existing async
// dialogs that OnFinalizePaths already chains together when something is
// missing. This screen is that same mechanism, shown when nothing is missing,
// so the player can reach any of them on purpose instead of only by lacking a
// file.
//
// Deliberately NOT modelled on the Android launcher's process split: there is
// no separate launcher process here and cannot be one, because iOS forbids
// spawning. This is a dialog inside the same app, drawn before the guest is
// created.

#ifndef SKATE3_LAUNCHER_H_
#define SKATE3_LAUNCHER_H_

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace skate3 {

// What the launcher needs to describe the install, and what it does when a row
// is chosen. The app supplies these rather than the launcher deriving them: it
// does not know the runtime paths, and a path guessed wrong here is worse than
// none - on a phone nobody can check it by eye.
struct LauncherActions {
  std::filesystem::path game_root;
  std::filesystem::path user_root;

  // Map pack folder names found in Documents, for the status block.
  std::vector<std::string> packs;

  // Start the game. Exactly one of the callbacks below fires, once.
  std::function<void()> play;
  // Re-run the disc installer, then come back here.
  std::function<void()> install_disc;
  // Re-run the title update installer, then come back here.
  std::function<void()> install_title_update;
  // Open the pack chooser, then come back here.
  std::function<void()> choose_pack;
};

// Shows the launcher and returns immediately.
//
// Asynchronous for the same reason every other startup dialog here is: this
// runs from OnFinalizePaths, before OnInitialize has returned, so the event loop
// is not pumping yet and a blocking wait draws nothing at all - which is the
// black screen the pack chooser produced when it was written the other way.
void ShowLauncher(rex::ui::WindowedAppContext& app_context, rex::ui::ImGuiDrawer* drawer,
                  LauncherActions actions);

}  // namespace skate3

#endif  // SKATE3_LAUNCHER_H_
