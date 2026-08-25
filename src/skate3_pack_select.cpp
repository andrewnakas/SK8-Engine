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
  PackSelectDialog(rex::ui::ImGuiDrawer* drawer, const std::vector<std::string>& packs,
                   std::shared_ptr<std::string> choice, std::shared_ptr<std::atomic<bool>> done)
      : ImGuiDialog(drawer), packs_(packs), choice_(std::move(choice)), done_(std::move(done)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
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
    for (const std::string& pack : packs_) {
      if (ImGui::Button(pack.c_str(), row)) {
        *choice_ = pack;
        done_->store(true, std::memory_order_release);
      }
    }
    ImGui::Separator();
    if (ImGui::Button("Skip - load the stock game", ImVec2(-1.0f, row.y))) {
      choice_->clear();
      done_->store(true, std::memory_order_release);
    }
    ImGui::End();
  }

 private:
  std::vector<std::string> packs_;
  std::shared_ptr<std::string> choice_;
  std::shared_ptr<std::atomic<bool>> done_;
};

}  // namespace

std::string ChoosePackBlocking(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                               rex::ui::ImGuiDrawer* drawer,
                               const std::vector<std::string>& packs) {
  if (drawer == nullptr || packs.size() < 2) {
    return packs.size() == 1 ? packs.front() : std::string();
  }

  auto choice = std::make_shared<std::string>();
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto dialog = std::make_unique<PackSelectDialog>(drawer, packs, choice, done);

  REXLOG_INFO("Skate 3: waiting for a pack choice ({} available)", packs.size());
  // Same pump as the title-update wizard: the guest has not started yet, so
  // this thread is free to sit here driving the UI until a button is hit.
  while (!done->load(std::memory_order_acquire) && !app_context.HasQuitFromUIThread()) {
    app_context.ExecutePendingFunctionsFromUIThread();
    if (window) {
      window->RequestPaint();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  if (!done->load(std::memory_order_acquire)) {
    return std::string();  // quitting
  }
  REXLOG_INFO("Skate 3: pack '{}' chosen", choice->empty() ? "(none)" : *choice);
  return *choice;
}

}  // namespace skate3
