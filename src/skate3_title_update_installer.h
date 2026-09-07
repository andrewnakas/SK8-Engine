#pragma once

#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace skate3 {

// True when both title update payloads (default.xexp and
// data/webkit/EAWebkit.xexp) are staged in game_root and match the pinned
// SHA-256 hashes the recompilation was generated from.
bool IsTitleUpdateInstalled(const std::filesystem::path& game_root);

// Check the player's default.xex against the one the recompilation was
// generated from, and log the result.
//
// The recompiled code in this binary was produced from ONE specific
// default.xex. At run time the engine executes that code but reads its data -
// every static table, string and constant - out of the player's own copy. The
// two must be the same file. If they are not, the code is correct and every
// address it was compiled with points at the wrong bytes.
//
// That failure is silent and arrives late. A Retroid Pocket 6 and an AYN Thor
// both stopped at a frontend screen, still drawing 28,000 times per thirty
// seconds, because an audio codec tag read from a static table came back
// 'rwar' where this build expects 'PFN0' - the same address, the same code, a
// different image. Nothing before that point complained.
//
// Returns true when it matches. Never refuses to run: a mismatch may still get
// a long way, and a tester who cannot start cannot report. But it says so, at
// WARN, so it survives the shipped log level and lands in a diagnostic report.
bool VerifyBaseExecutable(const std::filesystem::path& game_root);

// Stages the title update payloads into game_root from a local source file:
// either the TU STFS package (CON/LIVE/PIRS container) or a raw .xexp payload.
bool StageTitleUpdateFromFile(const std::filesystem::path& source,
                              const std::filesystem::path& game_root, std::string& error);

void ShowTitleUpdateInstallWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig runtime_paths,
                                  std::function<void(rex::PathConfig)> complete);
bool RunTitleUpdateInstallWizardBlocking(rex::ui::WindowedAppContext& app_context,
                                         rex::ui::Window* window,
                                         rex::ui::ImGuiDrawer* drawer,
                                         rex::PathConfig runtime_paths,
                                         rex::PathConfig& installed_paths);

}  // namespace skate3
