#include "skate3_pack_select.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

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
  void OnDraw(ImGuiIO& io) override {
    if (done_) {
      return;
    }
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
    for (const std::string& pack : packs_) {
      if (ImGui::Button(pack.c_str(), row)) {
        picked = pack;
        picked_any = true;
      }
    }
    ImGui::Separator();
    if (ImGui::Button("Skip - load the stock game", ImVec2(-1.0f, row.y))) {
      picked_any = true;
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
};

}  // namespace

void ShowPackSelect(rex::ui::ImGuiDrawer* drawer, const std::vector<std::string>& packs,
                    std::function<void(std::string)> chosen) {
  REXLOG_INFO("Skate 3: asking which of {} content packs to load", packs.size());
  // Owns itself; ImGuiDialog deletes on Close().
  new PackSelectDialog(drawer, packs, std::move(chosen));
}

}  // namespace skate3
