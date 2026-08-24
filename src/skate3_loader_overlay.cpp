#include "skate3_loader_overlay.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#include "skate3_ios_maps.h"
#endif

#include "skate3_warp.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/logging.h>

#include "skate3_demo_path.h"

REXCVAR_DEFINE_BOOL(skate3_loader_overlay, false, "Skate 3",
                    "Cover the screen with a 'loading <map>' card while the boot macro drives "
                    "the menus, so automated map selection is not visible. Hides itself once "
                    "the map has loaded.");
REXCVAR_DEFINE_STRING(skate3_loader_overlay_title, "", "Skate 3",
                      "Loading overlay: the map name to show.");
REXCVAR_DEFINE_STRING(skate3_loader_overlay_subtitle, "", "Skate 3",
                      "Loading overlay: the smaller line under the map name, e.g. the pack.");
REXCVAR_DEFINE_DOUBLE(skate3_loader_overlay_hue, 0.58, "Skate 3",
                      "Loading overlay: background hue, 0..1.");
REXCVAR_DEFINE_STRING(skate3_loader_levels, "", "Skate 3",
                      "In-game level picker: every known map name, '|'-separated. Empty "
                      "disables the picker.");
REXCVAR_DEFINE_STRING(skate3_loader_level_packs, "", "Skate 3",
                      "In-game level picker: the owning pack id for each entry in "
                      "skate3_loader_levels, same order, '|'-separated.");
REXCVAR_DEFINE_STRING(skate3_loader_level_worlds, "", "Skate 3",
                      "Pipe-separated world id per entry of skate3_loader_levels. An "
                      "in-session switch repoints the menu rows at the chosen world, so "
                      "the pick does not depend on landing its row.");
REXCVAR_DEFINE_STRING(skate3_loader_level_indices, "", "Skate 3",
                      "In-game level picker: each entry's position in its own pack's map "
                      "list, same order, '|'-separated.");
REXCVAR_DEFINE_STRING(skate3_loader_current_pack, "", "Skate 3",
                      "In-game level picker: the pack staged at boot. Maps in other packs "
                      "need a relaunch, which is requested through the launcher.");
REXCVAR_DEFINE_INT32(skate3_loader_boot_index, -1, "Skate 3",
                     "Navigate to this map index in the staged pack as soon as the game "
                     "reaches gameplay. -1 leaves the boot path alone. Uses the same "
                     "feedback-driven navigation as the in-game picker, so it walks the "
                     "menu only as far as it actually needs to.");
REXCVAR_DEFINE_BOOL(skate3_loader_open_picker, false, "Skate 3",
                    "Open the level picker as soon as the game reaches gameplay, so a "
                    "session starts at 'choose a map' rather than booting into one.");
REXCVAR_DEFINE_BOOL(skate3_loader_in_session_switch, false, "Skate 3",
                    "Switch maps inside the staged pack WITHOUT relaunching (~9 s against "
                    "~27 s). OFF because it does not currently land the right map: "
                    "PatchItemEntries cannot find the Locations rows from gameplay so the "
                    "retarget repoints 0 items and every row still points at the boot "
                    "world, and ClampToListEnd stops on a stock district because the "
                    "cursor field it trusts is not the Locations row index. Rio -> "
                    "MegaPark landed in PCU park. Left in for whoever fixes those two.");
REXCVAR_DEFINE_STRING(skate3_loader_request_path, "", "Skate 3",
                      "In-game level picker: file to write a relaunch request to. The "
                      "launcher watches it and restarts the game on the chosen pack.");

namespace skate3 {
namespace {

// Seconds to hold the cover after the world is up, so the first frame revealed
// is gameplay rather than the tail of a load. Trimmed 0.9 -> 0.5 with the rest
// of the boot work (2026-08-13): this sits entirely after the renderer has
// taken over, so it is time the player waits having already been given a world.
constexpr float kSettleSeconds = 0.5f;
// Fade-out length once we decide to reveal.
constexpr float kFadeSeconds = 0.3f;
// If presence never dips into a load after the macro finishes, give up
// covering rather than sitting on a black screen forever.
constexpr float kStreamingTimeout = 25.0f;
// Likewise, never cover for longer than this in total.
constexpr float kHardTimeout = 300.0f;

bool InGameplay() { return rex::kernel::guest_presence::GameplayContextValue() == 1; }

// --------------------------------------------------------------------------
// Menu navigation with feedback
//
// The old approach pressed 'down' forty times to clamp onto the end of a list.
// That is ~10 s of scrolling and forty menu clicks. The frontend's selection
// index is readable, so instead we step until it stops moving: a handful of
// presses, no overshoot, and no noise past the end of the list.
// --------------------------------------------------------------------------

std::atomic<uint8_t*> g_guest_base{nullptr};
std::thread g_nav_thread;
std::atomic<bool> g_nav_running{false};

// frontend manager -> +0x208 -> +0x218 (measured; see skate3-menu-navigation).
constexpr uint32_t kFrontEndManagerPtr = 0x830CFE14;
constexpr uint32_t kCursorObjectOffset = 0x208;
constexpr uint32_t kCursorOffset = 0x218;

uint32_t LoadBE32(const uint8_t* host) {
  return (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) | (uint32_t(host[2]) << 8) |
         uint32_t(host[3]);
}

// The list cursor, or -1 when it cannot be read (booting, or the frontend is
// mid-teardown). Callers fall back to blind stepping.
int ReadListCursor() {
  uint8_t* base = g_guest_base.load(std::memory_order_relaxed);
  if (!base) {
    return -1;
  }
  const uint32_t manager = LoadBE32(base + kFrontEndManagerPtr);
  if (manager < 0x10000 || manager >= 0xFFFFFFF0) {
    return -1;
  }
  const uint32_t object = LoadBE32(base + manager + kCursorObjectOffset);
  if (object < 0x10000 || object >= 0xFFFFFFF0) {
    return -1;
  }
  const uint32_t cursor = LoadBE32(base + object + kCursorOffset);
  return cursor > 4096 ? -1 : static_cast<int>(cursor);
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void Press(uint16_t buttons, int polls = 6) {
  rex::kernel::xam::QueueSyntheticInput(buttons, 0, 0, polls);
}

void PressTrigger(uint8_t right_trigger) {
  rex::kernel::xam::QueueSyntheticInput(0, 0, right_trigger, 12);
}

// Xbox pad bit masks.
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadStart = 0x0010;
constexpr uint16_t kPadDown = 0x0002;

// Tab presses to reach the Locations tab. Three is the exact count; the strip
// clamps there, so pressing more is free and covers a swallowed input.
constexpr int kTabPresses = 10;

// Step `down` until the selection stops changing, i.e. the list has clamped.
// Returns the number of presses actually used.
int ClampToListEnd(int max_steps, int step_ms) {
  int last = ReadListCursor();
  if (last < 0) {
    // No feedback available - fall back to the old blind clamp.
    for (int i = 0; i < max_steps; ++i) {
      Press(kPadDown);
      SleepMs(step_ms);
    }
    return max_steps;
  }
  int unchanged = 0;
  for (int i = 0; i < max_steps; ++i) {
    Press(kPadDown);
    SleepMs(step_ms);
    const int now = ReadListCursor();
    if (now < 0) {
      continue;
    }
    if (now == last) {
      // Two stationary presses in a row means the end, not a dropped input.
      if (++unchanged >= 2) {
        return i + 1;
      }
    } else {
      unchanged = 0;
      last = now;
    }
  }
  return max_steps;
}

std::vector<std::string> SplitLevels(const std::string& packed) {
  std::vector<std::string> out;
  std::string current;
  for (char c : packed) {
    if (c == '|') {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// The pause-menu route to a map, expanded: the game's own parser has no `*N`
// repeat (freeskate expands that before launch), so the downs are written out.
//
//   start,a  -> pause root then challenge map
//   rt xN    -> tab right to Locations (clamps there, so N is generous)
//   down x40 -> clamp onto the staged DLC pack (the list clamps, never wraps)
//   a        -> open the pack's map list
//   down x K -> the map's position in that list
//   a,a      -> confirm, then teleport
void NavigateToMap(int sub_index) {
  if (g_nav_running.exchange(true)) {
    return;  // a switch is already in flight
  }
  if (g_nav_thread.joinable()) {
    g_nav_thread.join();
  }
  g_nav_thread = std::thread([sub_index] {
    // Pause -> challenge map -> tab right to Locations.
    Press(kPadStart);
    SleepMs(420);
    Press(kPadA);
    SleepMs(520);
    // Ten presses, not the three it takes to get there. The tab strip CLAMPS
    // at Locations, so an extra press does nothing - but a SWALLOWED press
    // with only three leaves this on a challenges tab, and the confirm below
    // then starts a challenge and teleports the player somewhere unrelated.
    // Measured on the launcher's macro path (screenshot-verified, and
    // confirmed by eye): 3, 6 and 10 presses all reach the same map, which
    // could not happen if the strip wrapped. See loader/navigate.py.
    for (int i = 0; i < kTabPresses; ++i) {
      PressTrigger(255);
      SleepMs(760);
    }
    // Clamp onto the staged pack (the list holds one entry per pack).
    // Six is the whole Locations list with one pack staged, so this never
    // scrolls past the end; the cursor feedback just lets it stop even sooner.
    const int used = ClampToListEnd(6, 130);
    SleepMs(220);
    Press(kPadA);  // open the pack's map list
    SleepMs(900);
    for (int i = 0; i < sub_index; ++i) {
      Press(kPadDown);
      SleepMs(130);
    }
    SleepMs(180);
    Press(kPadA);  // confirm the map
    SleepMs(900);
    Press(kPadA);  // teleport
    REXLOG_INFO("Skate 3 level select: navigated with {} clamp presses to index {}", used,
                sub_index);
    g_nav_running.store(false);
  });
}

std::string BuildMapMacro(int index) {
  std::string macro = "start,a";
  for (int i = 0; i < kTabPresses; ++i) macro += ",rt:1500";
  for (int i = 0; i < 40; ++i) macro += ",down:260";
  macro += ",a:1200";
  for (int i = 0; i < index; ++i) macro += ",down:320";
  macro += ",a:1200,a:1500";
  return macro;
}

ImU32 HsvColor(float hue, float saturation, float value, float alpha) {
  float r = 0.0f, g = 0.0f, b = 0.0f;
  ImGui::ColorConvertHSVtoRGB(hue, saturation, value, r, g, b);
  return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

}  // namespace

void LoaderOverlay::Restart(const std::string& title, const std::string& subtitle, float hue) {
  title_override_ = title;
  subtitle_override_ = subtitle;
  hue_override_ = hue;
  // An in-game switch is already in gameplay, so skip the boot phase and follow
  // the macro straight through.
  phase_ = Phase::kNavigating;
  phase_seconds_ = 0.0f;
  settle_seconds_ = 0.0f;
  shown_fraction_ = 0.0f;
  fade_ = 1.0f;
  saw_streaming_ = false;
}

float LoaderOverlay::TargetFraction() const {
  switch (phase_) {
    case Phase::kBooting: {
      // No macro progress yet; creep toward the hand-off point on time alone.
      const float share = 1.0f - std::exp(-phase_seconds_ / 14.0f);
      return 0.05f + 0.40f * share;
    }
    case Phase::kNavigating: {
      const int32_t total = demo_path::GameplayInputsTotal();
      const int32_t done = demo_path::GameplayInputsInjected();
      if (total > 0) {
        return 0.45f + 0.35f * (static_cast<float>(done) / static_cast<float>(total));
      }
      return 0.45f;
    }
    case Phase::kStreaming: {
      const float share = 1.0f - std::exp(-phase_seconds_ / 18.0f);
      return 0.80f + 0.18f * share;
    }
    case Phase::kSettling:
    case Phase::kDone:
      return 1.0f;
  }
  return 0.0f;
}

void LoaderOverlay::Advance(float delta_seconds) {
  phase_seconds_ += delta_seconds;
  settle_seconds_ += delta_seconds;

  switch (phase_) {
    case Phase::kBooting:
      if (InGameplay()) {
        phase_ = Phase::kNavigating;
        phase_seconds_ = 0.0f;
      }
      break;

    case Phase::kNavigating:
      // No macro at all: this session starts at the picker, so there is nothing
      // to cover -- reveal as soon as the world is up.
      if (demo_path::GameplayInputsTotal() == 0 && !LoaderNavigationActive() &&
          phase_seconds_ > 2.0f) {
        phase_ = Phase::kSettling;
        phase_seconds_ = 0.0f;
        break;
      }
      // Our own navigator drives the menus without the demo-path worker; treat
      // its completion the same way.
      if (LoaderNavigationActive()) {
        saw_nav_ = true;
      } else if (saw_nav_) {
        phase_ = Phase::kStreaming;
        phase_seconds_ = 0.0f;
        saw_streaming_ = false;
        break;
      }
      // The macro ends with the teleport press; the world load follows.
      if (demo_path::GameplayInputSequenceComplete()) {
        phase_ = Phase::kStreaming;
        phase_seconds_ = 0.0f;
        saw_streaming_ = false;
      }
      break;

    case Phase::kStreaming:
      // Presence drops out of gameplay while the new world streams, then comes
      // back. Waiting for that dip is what distinguishes "the map is loading"
      // from "the macro just finished and nothing happened".
      if (!InGameplay()) {
        saw_streaming_ = true;
      } else if (saw_streaming_ || phase_seconds_ > kStreamingTimeout) {
        phase_ = Phase::kSettling;
        phase_seconds_ = 0.0f;
      }
      break;

    case Phase::kSettling:
      if (phase_seconds_ >= kSettleSeconds) {
        fade_ = std::max(0.0f, fade_ - delta_seconds / kFadeSeconds);
        if (fade_ <= 0.0f) {
          phase_ = Phase::kDone;
        }
      }
      break;

    case Phase::kDone:
      break;
  }

  if (settle_seconds_ > kHardTimeout) {
    phase_ = Phase::kDone;
  }
}

void LoaderOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(skate3_loader_overlay) || phase_ == Phase::kDone) {
    return;
  }

  Advance(io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f);
  if (phase_ == Phase::kDone) {
    return;
  }

  const float target = TargetFraction();
  // Ease toward the target so the bar never jumps or goes backwards.
  shown_fraction_ = std::max(shown_fraction_, 0.0f);
  if (target > shown_fraction_) {
    shown_fraction_ += (target - shown_fraction_) * std::min(1.0f, io.DeltaTime * 3.0f);
  }

  const float width = io.DisplaySize.x;
  const float height = io.DisplaySize.y;
  const float hue = hue_override_ >= 0.0f
                        ? hue_override_
                        : static_cast<float>(REXCVAR_GET(skate3_loader_overlay_hue));
  const float alpha = fade_;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::Begin("##skate3_loader_overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImDrawList* draw = ImGui::GetWindowDrawList();

  // Diagonal two-tone wash, the same idea as the launcher's generated cards so
  // a map looks the same in the library and on the loading screen.
  const ImU32 top_left = HsvColor(hue, 0.55f, 0.42f, alpha);
  const ImU32 top_right = HsvColor(hue + 0.04f, 0.60f, 0.24f, alpha);
  const ImU32 bottom_right = HsvColor(hue + 0.08f, 0.62f, 0.09f, alpha);
  const ImU32 bottom_left = HsvColor(hue + 0.02f, 0.58f, 0.16f, alpha);
  draw->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(width, height), top_left, top_right,
                                bottom_right, bottom_left);

  const float margin = std::max(48.0f, width * 0.05f);
  const float base_y = height - margin;

  const std::string title =
      title_override_.empty() ? REXCVAR_GET(skate3_loader_overlay_title) : title_override_;
  const std::string subtitle = subtitle_override_.empty()
                                   ? REXCVAR_GET(skate3_loader_overlay_subtitle)
                                   : subtitle_override_;

  const ImU32 white = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
  const ImU32 dim = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.72f * alpha));
  const ImU32 track = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f * alpha));

  // Progress bar.
  const float bar_height = std::max(6.0f, height * 0.008f);
  const float bar_y = base_y - bar_height;
  draw->AddRectFilled(ImVec2(margin, bar_y), ImVec2(width - margin, bar_y + bar_height), track);
  draw->AddRectFilled(
      ImVec2(margin, bar_y),
      ImVec2(margin + (width - margin * 2.0f) * std::min(1.0f, shown_fraction_),
             bar_y + bar_height),
      white);

  // Map name, scaled up from the default font.
  ImFont* font = ImGui::GetFont();
  const float title_size = std::max(34.0f, height * 0.062f);
  const float subtitle_size = std::max(14.0f, height * 0.021f);

  float cursor_y = bar_y - subtitle_size - 26.0f;
  if (!subtitle.empty()) {
    draw->AddText(font, subtitle_size, ImVec2(margin, cursor_y), dim, subtitle.c_str());
  }
  cursor_y -= title_size + 6.0f;
  if (!title.empty()) {
    // A soft drop shadow keeps the name readable over the lighter corner.
    draw->AddText(font, title_size, ImVec2(margin + 2.0f, cursor_y + 2.0f),
                  ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f * alpha)), title.c_str());
    draw->AddText(font, title_size, ImVec2(margin, cursor_y), white, title.c_str());
  }

  ImGui::End();
}

void LevelSelectDialog::LogToggle() {
  REXLOG_INFO("Skate 3 level select: toggle -> visible={} levels='{}'", visible_,
              REXCVAR_GET(skate3_loader_levels));
}

bool LevelSelectDialog::available() {
  return !REXCVAR_GET(skate3_loader_levels).empty();
}

void LevelSelectDialog::Choose(int entry, const std::string& name) {
  visible_ = false;

  const std::vector<std::string> packs = SplitLevels(REXCVAR_GET(skate3_loader_level_packs));
  const std::vector<std::string> indices = SplitLevels(REXCVAR_GET(skate3_loader_level_indices));
  const std::string current = REXCVAR_GET(skate3_loader_current_pack);

  const std::string pack =
      entry < static_cast<int>(packs.size()) ? packs[entry] : current;
  const int sub_index =
      entry < static_cast<int>(indices.size()) ? std::atoi(indices[entry].c_str()) : entry;

  // RELAUNCH BY DEFAULT, even within the staged pack.
  //
  // The in-session switch below is ~9 s against ~27 s for a relaunch, and it
  // does not work. Two independent reasons, both measured:
  //
  //   * `PatchItemEntries` cannot find the Locations rows from gameplay, so
  //     the retarget repoints ZERO items ("retarget 'sk8itmegapark' repointed
  //     0 menu items"). Every row is still aimed at whatever world the BOOT
  //     patch pointed them at - 32 items, all Rio - so confirming any row
  //     reloads the boot map.
  //   * `ClampToListEnd` trusts a cursor field that is not the Locations row
  //     index (it clamps at 0 and does not move on `up`), so the walk stops on
  //     a STOCK district instead of the pack. Rio -> MegaPark landed in PCU
  //     park.
  //
  // Relaunching is slower and correct: the launcher re-stages and the normal
  // boot path - which three 111-run sweeps verify - puts you in the right map.
  const bool in_session = REXCVAR_GET(skate3_loader_in_session_switch) &&
                          !pack.empty() && !current.empty() && pack == current;
  if (!in_session) {
    // A different pack can never be reached in-session anyway: the installed
    // DLC set is fixed at boot. Ask the launcher to restart us -- it stays
    // alive across the switch and re-stages before relaunching.
    const std::string request_path = REXCVAR_GET(skate3_loader_request_path);
    if (request_path.empty()) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
      // No launcher exists on a phone and an app cannot relaunch itself, so
      // the choice is written where the next launch reads it and the player
      // reopens the app. Crude, but it lands the right map - which the
      // in-session switch above still does not.
      const std::vector<std::string> ids =
          SplitLevels(REXCVAR_GET(skate3_loader_level_worlds));
      if (entry < static_cast<int>(ids.size()) && !ids[entry].empty()) {
        if (skate3::ios_maps::RequestBootWorld(ids[entry]) && overlay_) {
          overlay_->Restart(name + " - reopen the app to load", "", 0.08f);
        }
        return;
      }
#endif
      REXLOG_WARN("Skate 3 level select: '{}' is in pack '{}' but no launcher is listening",
                  name, pack);
      return;
    }
    if (FILE* file = std::fopen(request_path.c_str(), "w")) {
      std::fprintf(file, "%s\n%d\n%s\n", pack.c_str(), sub_index, name.c_str());
      std::fclose(file);
      REXLOG_INFO("Skate 3 level select: requested relaunch on pack '{}' for '{}'", pack, name);
    } else {
      REXLOG_WARN("Skate 3 level select: could not write relaunch request to {}", request_path);
    }
    return;
  }

  // Repoint every Locations row at the chosen world BEFORE navigating. The
  // walk below then only has to reach the pack's map list, not a specific row -
  // and a switch measured 9 s against ~27 s for a relaunch.
  const std::vector<std::string> worlds =
      SplitLevels(REXCVAR_GET(skate3_loader_level_worlds));
  if (entry < static_cast<int>(worlds.size()) && !worlds[entry].empty()) {
    skate3::warp::RetargetWorld(worlds[entry]);
  }

  if (overlay_) {
    // Give the cover a colour of its own per map, the same way the launcher
    // does, so a switch looks like a deliberate load rather than a glitch.
    const float hue = static_cast<float>((sub_index * 47 + 13) % 100) / 100.0f;
    overlay_->Restart(name, REXCVAR_GET(skate3_loader_overlay_subtitle), hue);
  }
  if (close_menus_) {
    close_menus_();
  }
  // Feedback-driven navigation: far quicker than the blind macro, and it stops
  // at the end of the list instead of hammering past it.
  NavigateToMap(sub_index);
  REXLOG_INFO("Skate 3 level select: switching to '{}' (pack index {})", name, sub_index);
}

namespace {

// Controller navigation for the picker.
//
// Deliberately NOT ImGui's gamepad nav: that needs NavInputs fed every frame
// and would also start steering the settings screen. This reads the merged UI
// pad directly and moves one row per press, with a hold-to-repeat that matches
// how the game's own menus feel.
struct PadNav {
  int move = 0;       // -1 up, +1 down
  bool accept = false;
  bool cancel = false;
};

PadNav ReadPadNav(float delta_seconds) {
  static bool s_up = false, s_down = false, s_a = false, s_b = false;
  static float s_repeat = 0.0f;
  PadNav out;

  rex::input::InputSystem* input = skate3::demo_path::GetUiInputSystem();
  if (input == nullptr) {
    return out;
  }
  rex::input::X_INPUT_GAMEPAD pad;
  if (!input->GetUiGamepadState(&pad)) {
    return out;
  }
  constexpr int16_t kStick = 16000;
  const bool up = (pad.buttons & rex::input::X_INPUT_GAMEPAD_DPAD_UP) != 0 ||
                  pad.thumb_ly > kStick;
  const bool down = (pad.buttons & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) != 0 ||
                    pad.thumb_ly < -kStick;
  const bool a = (pad.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0;
  const bool b = (pad.buttons & rex::input::X_INPUT_GAMEPAD_B) != 0;

  // Rising edge moves immediately; holding repeats after a short delay.
  if (up && !s_up) { out.move = -1; s_repeat = 0.45f; }
  else if (down && !s_down) { out.move = 1; s_repeat = 0.45f; }
  else if (up || down) {
    s_repeat -= delta_seconds;
    if (s_repeat <= 0.0f) { out.move = up ? -1 : 1; s_repeat = 0.12f; }
  }
  out.accept = a && !s_a;
  out.cancel = b && !s_b;
  s_up = up; s_down = down; s_a = a; s_b = b;
  return out;
}

}  // namespace

void LevelSelectDialog::OnDraw(ImGuiIO& io) {
  // Boot navigation: drive to the requested map once the world is up, using the
  // same feedback walk as a manual pick rather than a fixed-length macro.
  if (!boot_navigation_done_ && InGameplay()) {
    const int boot_index = REXCVAR_GET(skate3_loader_boot_index);
    if (boot_index >= 0) {
      boot_navigation_done_ = true;
      NavigateToMap(boot_index);
    } else if (boot_index < 0) {
      boot_navigation_done_ = true;
    }
  }
  // Start-at-the-menu sessions: pop open the moment the world is up.
  if (open_on_gameplay_ && available() && InGameplay()) {
    open_on_gameplay_ = false;
    visible_ = true;
  }
  const bool companion = companion_ && companion_();
  if (!visible_ && !companion) {
    return;
  }
  const std::vector<std::string> levels = SplitLevels(REXCVAR_GET(skate3_loader_levels));
  if (levels.empty()) {
    visible_ = false;
    return;
  }

  // ---- controller / keyboard navigation ------------------------------------
  const int count = static_cast<int>(levels.size());
  focus_ = std::max(0, std::min(focus_, count - 1));
  const PadNav nav = ReadPadNav(io.DeltaTime);
  if (nav.move != 0) {
    focus_ = (focus_ + nav.move + count) % count;   // wraps: it is a short list
    scroll_to_focus_ = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
    focus_ = (focus_ + 1) % count;
    scroll_to_focus_ = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
    focus_ = (focus_ - 1 + count) % count;
    scroll_to_focus_ = true;
  }
  if (nav.cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    visible_ = false;
    return;
  }
  const bool activate = nav.accept || ImGui::IsKeyPressed(ImGuiKey_Enter, false);

  // ---- look ----------------------------------------------------------------
  // Grip-tape black, one hot accent, hard edges, shouty type. The point is that
  // it reads as part of the game rather than a debug window.
  const float hue = static_cast<float>(REXCVAR_GET(skate3_loader_overlay_hue));
  const ImU32 accent = HsvColor(hue, 0.85f, 1.00f, 1.0f);
  const ImU32 accent_dim = HsvColor(hue, 0.85f, 1.00f, 0.22f);

  const float anchor_x = companion ? io.DisplaySize.x * 0.97f : io.DisplaySize.x * 0.5f;
  const float pivot_x = companion ? 1.0f : 0.5f;
  ImGui::SetNextWindowPos(ImVec2(anchor_x, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(pivot_x, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.36f, io.DisplaySize.y * 0.68f),
                           ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, 0.96f));
  ImGui::Begin("Choose a map", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoMove);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 win_pos = ImGui::GetWindowPos();
  const ImVec2 win_size = ImGui::GetWindowSize();
  // Accent spine down the left edge.
  draw->AddRectFilled(win_pos, ImVec2(win_pos.x + 6.0f, win_pos.y + win_size.y), accent);

  ImGui::Dummy(ImVec2(0.0f, 22.0f));
  ImGui::Indent(28.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  ImGui::SetWindowFontScale(1.6f);
  ImGui::TextUnformatted("SPOTS");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.45f));
  ImGui::TextUnformatted(REXCVAR_GET(skate3_loader_overlay_subtitle).c_str());
  ImGui::PopStyleColor();
  ImGui::Unindent(28.0f);
  ImGui::Dummy(ImVec2(0.0f, 14.0f));

  // ---- the list ------------------------------------------------------------
  const std::vector<std::string> packs = SplitLevels(REXCVAR_GET(skate3_loader_level_packs));
  const std::string current = REXCVAR_GET(skate3_loader_current_pack);
  const float footer = 74.0f;
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1, 1, 1, 0.14f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1, 1, 1, 0.22f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(1, 1, 1, 0.30f));
  ImGui::BeginChild("##spots", ImVec2(0.0f, win_size.y - ImGui::GetCursorPosY() - footer),
                    false, ImGuiWindowFlags_NoBackground);
  // The CHILD's draw list, not the parent's. Rows are drawn by hand, and a
  // parent draw list is not clipped by the child - the bottom rows painted
  // straight over the footer.
  ImDrawList* rows = ImGui::GetWindowDrawList();
  int chosen = -1;
  for (int i = 0; i < count; ++i) {
    // Must match what Choose() actually DOES, not just the cross-pack case.
    // Every pick relaunches now (the in-session path is off by default), and
    // leaving the tag on cross-pack rows only meant picking an untagged level
    // closed the game with no warning - which reads exactly like a crash.
    const bool in_session_ok =
        REXCVAR_GET(skate3_loader_in_session_switch) &&
        i < static_cast<int>(packs.size()) && !current.empty() &&
        packs[i] == current;
    const bool needs_relaunch = !in_session_ok;
    const bool selected = (i == focus_);
    ImGui::PushID(i);

    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    const float row_h = 44.0f;
    const ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_h);
    if (selected) {
      rows->AddRectFilled(row_min, row_max, accent_dim);
      rows->AddRectFilled(row_min, ImVec2(row_min.x + 4.0f, row_max.y), accent);
    }

    // An invisible button keeps the whole row clickable and hoverable while the
    // drawing above stays ours.
    if (ImGui::InvisibleButton("##row", ImVec2(ImGui::GetContentRegionAvail().x, row_h))) {
      chosen = i;
    }
    if (ImGui::IsItemHovered()) {
      // Only a MOVING mouse takes the selection. This used to be an
      // unconditional `focus_ = i`, which runs every frame the cursor rests
      // over a row - so the pad would move the selection and the hover would
      // slam it straight back on the next frame. With the mouse anywhere over
      // the list the controller looked completely dead.
      //
      // Gating on MouseDelta gives both devices what you expect: move the
      // mouse and it takes over, touch the stick and the mouse stops arguing
      // until it moves again.
      const ImVec2 moved = ImGui::GetIO().MouseDelta;
      if (moved.x != 0.0f || moved.y != 0.0f) {
        focus_ = i;
      }
      if (i != focus_) {
        rows->AddRectFilled(row_min, row_max, HsvColor(hue, 0.85f, 1.0f, 0.10f));
      }
    }

    const float text_y = row_min.y + row_h * 0.5f - ImGui::GetFontSize() * 0.62f;
    rows->AddText(ImVec2(row_min.x + 28.0f, text_y),
                  selected ? ImGui::GetColorU32(ImVec4(1, 1, 1, 1))
                           : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.72f)),
                  levels[i].c_str());
    if (needs_relaunch) {
      const char* tag = "RESTARTS";
      const ImVec2 size = ImGui::CalcTextSize(tag);
      rows->AddText(ImVec2(row_max.x - size.x - 22.0f, text_y + 2.0f),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 0.35f)), tag);
    }
    ImGui::PopID();
  }
  if (scroll_to_focus_) {
    scroll_to_focus_ = false;
    const float row_h = 44.0f;
    const float target = focus_ * row_h;
    if (target < ImGui::GetScrollY()) {
      ImGui::SetScrollY(target);
    } else if (target + row_h > ImGui::GetScrollY() + ImGui::GetWindowHeight()) {
      ImGui::SetScrollY(target + row_h - ImGui::GetWindowHeight());
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(4);

  if (activate && chosen < 0) {
    chosen = focus_;
  }

  // ---- footer --------------------------------------------------------------
  draw->AddLine(ImVec2(win_pos.x + 28.0f, win_pos.y + win_size.y - footer + 8.0f),
                ImVec2(win_pos.x + win_size.x - 28.0f, win_pos.y + win_size.y - footer + 8.0f),
                ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)));
  ImGui::Indent(28.0f);
  ImGui::Dummy(ImVec2(0.0f, 16.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.50f));
  ImGui::TextUnformatted("A / click  drop in        B / Esc / `  close");
  ImGui::PopStyleColor();
  ImGui::Unindent(28.0f);

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);

  if (chosen >= 0 && chosen < count) {
    Choose(chosen, levels[chosen]);
  }
}

bool LoaderPickerOpensAtStart() { return REXCVAR_GET(skate3_loader_open_picker); }

bool LoaderNavigationActive() { return g_nav_running.load(std::memory_order_relaxed); }

void SetLoaderGuestBase(uint8_t* base) {
  g_guest_base.store(base, std::memory_order_relaxed);
}

}  // namespace skate3
