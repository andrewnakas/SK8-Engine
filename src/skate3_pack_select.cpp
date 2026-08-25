#include "skate3_pack_select.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace skate3 {
namespace {

class PackSelectDialog final : public rex::ui::ImGuiDialog {
 public:
  PackSelectDialog(rex::ui::ImGuiDrawer* drawer, std::vector<std::string> packs,
                   std::function<void(std::string)> chosen)
      : ImGuiDialog(drawer), packs_(std::move(packs)), chosen_(std::move(chosen)) {}

 protected:
  // Controller navigation, read straight from SDL. The runtime's input system
  // does not exist yet this early in startup, and ImGui here has no gamepad
  // navigation configured - but a player on a phone with a pad attached still
  // has to be able to answer this, and it is the one screen between them and
  // the game.
  ~PackSelectDialog() {
    for (SDL_Gamepad* pad : opened_) {
      SDL_CloseGamepad(pad);
    }
  }

  void PollPad(size_t count) {
    // Bring the gamepad subsystem up ourselves. It is normally started by the
    // SDL input driver, which belongs to the runtime's input system - and that
    // does not exist yet this early, so without this SDL_GetGamepads finds
    // nothing and the pad appears dead. Refcounted, so the driver starting it
    // again later is harmless.
    if (!pad_subsystem_ready_) {
      pad_subsystem_ready_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
      if (!pad_subsystem_ready_) {
        return;
      }
      REXLOG_INFO("Skate 3 pack chooser: gamepad input ready");
    }
    // Pads are discovered through the event queue, which nothing is pumping
    // this early either.
    SDL_PumpEvents();
    int pad_count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&pad_count);
    if (pads == nullptr) {
      return;
    }
    bool up = false, down = false, accept = false;
    for (int i = 0; i < pad_count; ++i) {
      // OPEN it. SDL_GetGamepadFromID only hands back a gamepad that is
      // already open, so on its own it returns null for every attached pad and
      // the controller looks dead. Handles are kept and closed with the
      // dialog, rather than reopening each frame.
      SDL_Gamepad* pad = SDL_GetGamepadFromID(pads[i]);
      if (pad == nullptr) {
        pad = SDL_OpenGamepad(pads[i]);
        if (pad == nullptr) {
          continue;
        }
        opened_.push_back(pad);
        REXLOG_INFO("Skate 3 pack chooser: controller '{}' connected",
                    SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
      }
      up |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
      down |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
      accept |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
      // The stick too - a d-pad is not everyone's first reach.
      const Sint16 ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
      up |= ly < -16000;
      down |= ly > 16000;
    }
    SDL_free(pads);

    // Edge-triggered: held is one move, not one per frame.
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

  void OnDraw(ImGuiIO& io) override {
    if (done_) {
      return;
    }
    // One row per pack plus the skip row.
    PollPad(packs_.size() + 1);
    const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(centre, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.62f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("Choose a map pack", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextWrapped(
        "More than one custom pack is installed. One can be loaded at a time; pick the one "
        "for this session.");
    ImGui::Separator();
    // Sized for a thumb rather than a mouse - this is the one screen a phone
    // player has to hit before the game will start.
    const ImVec2 row(-1.0f, ImGui::GetTextLineHeight() * 2.4f);
    std::string picked;
    bool picked_any = false;
    for (size_t i = 0; i < packs_.size(); ++i) {
      const bool highlighted = selected_ == i;
      if (highlighted) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
      }
      if (ImGui::Button(packs_[i].c_str(), row) || (highlighted && accept_pressed_)) {
        picked = packs_[i];
        picked_any = true;
      }
      if (highlighted) {
        ImGui::PopStyleColor();
      }
    }
    ImGui::Separator();
    const bool skip_highlighted = selected_ == packs_.size();
    if (skip_highlighted) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    }
    if (ImGui::Button("Skip - load the stock game", ImVec2(-1.0f, row.y)) ||
        (skip_highlighted && accept_pressed_)) {
      picked_any = true;
    }
    if (skip_highlighted) {
      ImGui::PopStyleColor();
    }
    ImGui::End();
    if (!picked_any) {
      return;
    }
    // Finish before handing control back: the callback resumes startup, which
    // can reach code that tears this dialog down.
    done_ = true;
    auto chosen = std::move(chosen_);
    REXLOG_INFO("Skate 3: pack '{}' chosen", picked.empty() ? "(none)" : picked);
    Close();
    if (chosen) {
      chosen(std::move(picked));
    }
  }

 private:
  std::vector<std::string> packs_;
  std::function<void(std::string)> chosen_;
  bool done_ = false;
  size_t selected_ = 0;
  bool up_held_ = false, down_held_ = false, accept_held_ = false;
  bool accept_pressed_ = false;
  bool pad_subsystem_ready_ = false;
  std::vector<SDL_Gamepad*> opened_;
};

}  // namespace

void ShowPackSelect(rex::ui::ImGuiDrawer* drawer, const std::vector<std::string>& packs,
                    std::function<void(std::string)> chosen) {
  REXLOG_INFO("Skate 3: asking which of {} content packs to load", packs.size());
  // Owns itself; ImGuiDialog deletes on Close().
  new PackSelectDialog(drawer, packs, std::move(chosen));
}

}  // namespace skate3
