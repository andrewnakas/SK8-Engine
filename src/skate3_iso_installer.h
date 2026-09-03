#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rex/rex_app.h>

namespace skate3 {

// Heading and numbered instructions for getting a large file onto this
// machine, shown in the install wizards. On iOS - where the file cannot
// simply be browsed to and the disc is far too big to email to yourself -
// these are the Finder/iTunes file-sharing steps; on desktop they are the
// short version. `what` names the file in the sentences ("your Xbox 360
// ISO"), `action` names the wizard button that opens the picker.
const char* FileTransferStepsTitle();
std::vector<std::string> FileTransferSteps(const char* what, const char* action);

bool IsGameInstalled(const std::filesystem::path& game_root);

// game_root itself, or the single folder inside it that holds default.xex when
// game_root does not. Copying an extracted disc across as a folder is the
// usual way people end up one level too deep; this finds that case and nothing
// more ambiguous. Returns game_root unchanged when there is no clear answer.
std::filesystem::path ResolveNestedGameRoot(const std::filesystem::path& game_root);
void ShowRexglueIsoInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                 std::function<void(rex::PathConfig)> complete);
bool RunRexglueIsoInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                        rex::ui::Window* window,
                                        rex::ui::ImGuiDrawer* drawer,
                                        rex::PathConfig runtime_paths,
                                        rex::PathConfig& installed_paths);

}  // namespace skate3
