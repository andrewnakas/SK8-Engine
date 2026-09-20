#include "skate3_launcher.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#else
#include <SDL3/SDL.h>
#endif
#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "skate3_diagnostics_report.h"
#include "skate3_iso_installer.h"
#include "skate3_title_update_installer.h"

namespace skate3 {
namespace {

class LauncherDialog final : public rex::ui::ImGuiDialog {
 public:
  LauncherDialog(rex::ui::WindowedAppContext& app_context, rex::ui::ImGuiDrawer* drawer,
                 LauncherActions actions)
      : ImGuiDialog(drawer), app_context_(app_context), actions_(std::move(actions)) {
    ready_ = IsGameInstalled(actions_.game_root) && IsTitleUpdateInstalled(actions_.game_root);
  }

  ~LauncherDialog() {
#if !defined(__SWITCH__)
    for (SDL_Gamepad* pad : opened_) {
      SDL_CloseGamepad(pad);
    }
#endif
    if (report_thread_.joinable()) {
      report_thread_.join();
    }
  }

 protected:
  // The guest does not exist yet, so nothing else is driving frames, and the
  // report lands on another thread - without this its status would not appear
  // until the player happened to touch the screen again.
  bool WantsContinuousRepaint() const override { return !done_; }

  // Controller navigation read straight from SDL, exactly as the pack chooser
  // does and for the same reason: the runtime's input system does not exist
  // this early, so nothing else is translating a pad. Touch needs no such help
  // - SDL synthesises mouse events from a finger, which is the path ImGui
  // already uses, and it is how the settings menu is driven on a phone.
#if defined(__SWITCH__)
  void PollPad(size_t count) {
    if (!pad_ready_) {
      padConfigureInput(1, HidNpadStyleSet_NpadStandard);
      padInitializeDefault(&pad_);
      pad_ready_ = true;
    }
    padUpdate(&pad_);
    const u64 buttons = padGetButtons(&pad_);
    const HidAnalogStickState stick = padGetStickPos(&pad_, 0);
    bool up = (buttons & HidNpadButton_Up) != 0;
    bool down = (buttons & HidNpadButton_Down) != 0;
    const bool accept = (buttons & (HidNpadButton_A | HidNpadButton_B)) != 0;
    up |= stick.y > 16000;
    down |= stick.y < -16000;
    ApplyPadEdges(count, up, down, accept);
  }
#else
  void PollPad(size_t count) {
    if (!pad_subsystem_ready_) {
      pad_subsystem_ready_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
      if (!pad_subsystem_ready_) {
        return;
      }
    }
    SDL_PumpEvents();
    int pad_count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&pad_count);
    if (pads == nullptr) {
      return;
    }
    bool up = false, down = false, accept = false;
    for (int i = 0; i < pad_count; ++i) {
      // Open it: SDL_GetGamepadFromID only hands back an already-open pad, so
      // on its own every attached controller reads as dead.
      SDL_Gamepad* pad = SDL_GetGamepadFromID(pads[i]);
      if (pad == nullptr) {
        pad = SDL_OpenGamepad(pads[i]);
        if (pad == nullptr) {
          continue;
        }
        opened_.push_back(pad);
      }
      up |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
      down |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      accept |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
      const Sint16 ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
      up |= ly < -16000;
      down |= ly > 16000;
    }
    SDL_free(pads);
    ApplyPadEdges(count, up, down, accept);
  }
#endif

  // Edge-triggered: held is one move, not one per frame.
  void ApplyPadEdges(size_t count, bool up, bool down, bool accept) {
    if (count == 0) {
      return;
    }
    if (up && !up_held_ && selected_ > 0) {
      --selected_;
    }
    if (down && !down_held_ && selected_ + 1 < count) {
      ++selected_;
    }
    accept_pressed_ = accept && !accept_held_;
    up_held_ = up;
    down_held_ = down;
    accept_held_ = accept;
  }

  // The report can take a second or two on a big log, and doing it inside
  // OnDraw would stall the frame it is drawn from. Run it on a thread and show
  // the result when it lands.
  void StartReport() {
    if (report_running_.load()) {
      return;
    }
    report_running_ = true;
    SetReportStatus("Writing the report...");
    if (report_thread_.joinable()) {
      report_thread_.join();
    }
    report_thread_ = std::thread([this] {
      std::string error;
      const auto path = WriteDiagnosticsReport(actions_.game_root, actions_.user_root, error);
      if (path.empty()) {
        SetReportStatus("Could not write the report: " + error);
      } else {
        // The filename, not the full path. The player reaches it through the
        // Files app under "On My iPhone > Skate 3", where the sandbox prefix
        // is not shown and would only confuse.
        SetReportStatus("Saved " + path.filename().string() +
                        " to the Skate 3 folder. Share it from the Files app.");
      }
      report_running_ = false;
    });
  }

  void SetReportStatus(std::string text) {
    std::lock_guard<std::mutex> lock(report_status_mutex_);
    report_status_ = std::move(text);
  }

  std::string ReportStatus() {
    std::lock_guard<std::mutex> lock(report_status_mutex_);
    return report_status_;
  }

  // One row. Returns true when it was activated this frame, by touch, mouse or
  // pad. Sized for a thumb rather than a cursor: this is the first screen a
  // phone player meets.
  bool Row(const char* label, size_t index, bool enabled) {
    const bool highlighted = selected_ == index;
    if (!enabled) {
      ImGui::BeginDisabled();
    }
    if (highlighted) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    }
    const ImVec2 size(-1.0f, ImGui::GetTextLineHeight() * 2.4f);
    bool activated = ImGui::Button(label, size);
    if (highlighted && accept_pressed_ && enabled) {
      activated = true;
    }
    if (highlighted) {
      ImGui::PopStyleColor();
    }
    if (!enabled) {
      ImGui::EndDisabled();
    }
    return activated;
  }

  void OnDraw(ImGuiIO& io) override {
    if (done_) {
      return;
    }

    // Rows are built into a list first so the pad and the layout always agree
    // on how many there are. A mismatch here is the bug where the legend and
    // the action disagree, which this project has already paid for once.
    enum class Action { kPlay, kInstallDisc, kTitleUpdate, kChoosePack, kReport };
    struct RowSpec {
      const char* label;
      Action action;
      bool enabled;
    };
    const bool tu = IsTitleUpdateInstalled(actions_.game_root);
    const bool disc = IsGameInstalled(actions_.game_root);
    std::vector<RowSpec> rows;
    rows.push_back({"Play", Action::kPlay, ready_});
    rows.push_back({"Install from a disc image...", Action::kInstallDisc, true});
    // Only worth offering while it is the missing piece - the same rule the
    // Android launcher applies to its three title-update rows.
    if (!tu) {
      rows.push_back({"Install the title update...", Action::kTitleUpdate, true});
    }
    if (actions_.choose_pack && !actions_.packs.empty()) {
      rows.push_back({"Map packs...", Action::kChoosePack, true});
    }
    rows.push_back({"Save a diagnostic report", Action::kReport, !report_running_.load()});

    PollPad(rows.size());

    // Full screen, not a centred modal. A window floating in the middle of the
    // screen is what the in-game level picker looks like, and this is not that
    // - it is the app's front door, and it should read as the app rather than
    // as something the game has put on top of itself.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    // #101418, the Android launcher's background.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.063f, 0.078f, 0.094f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const float pad = std::max(20.0f, io.DisplaySize.y * 0.035f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::Begin("##skate3_launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    // Two columns, text left and actions right, each scrolling on its own.
    // Android does this because the activity is locked to landscape and a
    // single stacked column runs the buttons off the bottom of a phone held
    // sideways, hiding the one button the player needs. A phone running this
    // is in landscape for the same reason.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float gap = pad;
    const float left_w = (avail_w - gap) * 0.5f;
    const float right_w = avail_w - gap - left_w;

    ImGui::BeginChild("##left", ImVec2(left_w, 0.0f), false);
    {
      // 30sp against a 14sp body, which is the Android proportion.
      ImGui::SetWindowFontScale(2.1f);
      ImFont* bold = imgui_drawer() ? imgui_drawer()->ui_font_bold() : nullptr;
      if (bold != nullptr) {
        ImGui::PushFont(bold);
      }
      ImGui::TextUnformatted("Skate 3");
      if (bold != nullptr) {
        ImGui::PopFont();
      }
      ImGui::SetWindowFontScale(1.0f);
      ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));

      // #C8CDD4, the Android status colour.
      const ImVec4 status_colour(0.784f, 0.804f, 0.831f, 1.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, status_colour);
      ImGui::TextWrapped("%s", ready_ ? "Ready to play."
                                      : "The game's own files are not here yet. Choose "
                                        "\"Install from a disc image\" and pick your own "
                                        "Skate 3 disc image.");
      ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
      ImGui::Text("Disc files: %s", disc ? "installed" : "missing");
      ImGui::Text("Title update 3: %s", tu ? "staged" : "missing");
      if (actions_.packs.empty()) {
        ImGui::TextUnformatted("Map packs: none installed");
      } else {
        ImGui::Text("Map packs:");
        for (const std::string& pack : actions_.packs) {
          ImGui::BulletText("%s", pack.c_str());
        }
      }
      ImGui::PopStyleColor();

      // #9ED8F5, the colour Android gives the driver line - here it carries
      // whatever the last action had to say, which is the same job.
      const std::string status = ReportStatus();
      if (!status.empty()) {
        ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.620f, 0.847f, 0.961f, 1.0f));
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);

    ImGui::BeginChild("##actions", ImVec2(right_w, 0.0f), false);
    Action chosen = Action::kPlay;
    bool any = false;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (Row(rows[i].label, i, rows[i].enabled)) {
        chosen = rows[i].action;
        any = true;
      }
      ImGui::Dummy(ImVec2(0.0f, pad * 0.3f));
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    if (!any) {
      return;
    }

    // Everything below hands control somewhere else, so finish with this
    // dialog first. Resuming startup - or opening another wizard - while the
    // drawer is still iterating its dialogs, having just deleted this one, is
    // what made boot hang after a pick in the pack chooser.
    if (chosen == Action::kReport) {
      StartReport();
      return;
    }

    std::function<void()> callback;
    switch (chosen) {
      case Action::kPlay: callback = std::move(actions_.play); break;
      case Action::kInstallDisc: callback = std::move(actions_.install_disc); break;
      case Action::kTitleUpdate: callback = std::move(actions_.install_title_update); break;
      case Action::kChoosePack: callback = std::move(actions_.choose_pack); break;
      case Action::kReport: break;
    }
    if (!callback) {
      return;
    }
    done_ = true;
    REXLOG_WARN("Skate 3 launcher: row {} chosen", int(chosen));
    Close();
    app_context_.CallInUIThreadDeferred(
        [callback = std::move(callback)]() mutable { callback(); });
  }

 private:
  rex::ui::WindowedAppContext& app_context_;
  LauncherActions actions_;
  bool ready_ = false;
  bool done_ = false;
  size_t selected_ = 0;
  bool up_held_ = false, down_held_ = false, accept_held_ = false;
  bool accept_pressed_ = false;
  std::thread report_thread_;
  std::atomic<bool> report_running_{false};
  // Written by the report thread, read by the UI thread every frame. Guarded
  // rather than atomic because it is a std::string; the lock is uncontended
  // and held for a copy.
  std::mutex report_status_mutex_;
  std::string report_status_;
#if defined(__SWITCH__)
  bool pad_ready_ = false;
  PadState pad_{};
#else
  bool pad_subsystem_ready_ = false;
  std::vector<SDL_Gamepad*> opened_;
#endif
};

}  // namespace

void ShowLauncher(rex::ui::WindowedAppContext& app_context, rex::ui::ImGuiDrawer* drawer,
                  LauncherActions actions) {
  REXLOG_WARN("Skate 3: showing the launcher");
  // Owns itself; ImGuiDialog deletes on Close().
  new LauncherDialog(app_context, drawer, std::move(actions));
}

}  // namespace skate3
