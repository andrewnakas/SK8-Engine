#include "skate3_debug_input.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace skate3::debug_input {
namespace {
// Anchored on the first frame of a drag so the camera does not jump by the
// distance between the cursor and wherever it was last seen.
bool g_anchored = false;
POINT g_last = {};
}  // namespace

void Install(rex::ui::Window*) {}

bool IsKeyDown(rex::ui::VirtualKey key) {
  return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
}

bool PollLookDrag(double& dx_pixels, double& dy_pixels) {
  dx_pixels = 0.0;
  dy_pixels = 0.0;
  if (!IsKeyDown(rex::ui::VirtualKey::kRButton)) {
    g_anchored = false;
    return false;
  }
  POINT p;
  if (!GetCursorPos(&p)) {
    return false;
  }
  if (g_anchored) {
    dx_pixels = double(p.x - g_last.x);
    dy_pixels = double(p.y - g_last.y);
  }
  g_last = p;
  g_anchored = true;
  return true;
}

}  // namespace skate3::debug_input

#else  // !_WIN32

#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <cstdint>

namespace skate3::debug_input {
namespace {

// Key and mouse events are delivered on the UI thread while the freecam polls
// from the guest render thread, so the shared state is atomic. Relaxed is
// enough: each entry is independent and a one-frame-late key is invisible.
class DebugInputListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override { SetKey(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { SetKey(e.virtual_key(), false); }

  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      right_down_.store(true, std::memory_order_relaxed);
    }
    SetPosition(e);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      right_down_.store(false, std::memory_order_relaxed);
    }
    SetPosition(e);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override { SetPosition(e); }

  bool key_down(rex::ui::VirtualKey key) const {
    const auto index = static_cast<std::uint16_t>(key);
    if (index >= kKeyCount) {
      return false;
    }
    return keys_[index].load(std::memory_order_relaxed);
  }

  bool right_down() const { return right_down_.load(std::memory_order_relaxed); }

  void position(std::int32_t& x, std::int32_t& y) const {
    x = x_.load(std::memory_order_relaxed);
    y = y_.load(std::memory_order_relaxed);
  }

  // The window keeps no key state of its own, so a focus loss would otherwise
  // strand every held key down forever.
  void ReleaseAll() {
    for (auto& key : keys_) {
      key.store(false, std::memory_order_relaxed);
    }
    right_down_.store(false, std::memory_order_relaxed);
  }

 private:
  static constexpr std::uint16_t kKeyCount = 256;

  void SetKey(rex::ui::VirtualKey key, bool down) {
    const auto index = static_cast<std::uint16_t>(key);
    if (index < kKeyCount) {
      keys_[index].store(down, std::memory_order_relaxed);
    }
  }
  void SetPosition(const rex::ui::MouseEvent& e) {
    x_.store(e.x(), std::memory_order_relaxed);
    y_.store(e.y(), std::memory_order_relaxed);
  }

  std::atomic<bool> keys_[kKeyCount] = {};
  std::atomic<bool> right_down_{false};
  std::atomic<std::int32_t> x_{0};
  std::atomic<std::int32_t> y_{0};
};

// Focus changes arrive on WindowListener, not WindowInputListener.
class DebugFocusListener final : public rex::ui::WindowListener {
 public:
  explicit DebugFocusListener(DebugInputListener& input) : input_(input) {}
  void OnLostFocus(rex::ui::UISetupEvent&) override { input_.ReleaseAll(); }
  void OnClosing(rex::ui::UIEvent&) override { input_.ReleaseAll(); }

 private:
  DebugInputListener& input_;
};

DebugInputListener g_input;
DebugFocusListener g_focus(g_input);
rex::ui::Window* g_window = nullptr;

bool g_anchored = false;
std::int32_t g_last_x = 0;
std::int32_t g_last_y = 0;

}  // namespace

void Install(rex::ui::Window* window) {
  if (window == nullptr || window == g_window) {
    return;
  }
  if (g_window != nullptr) {
    g_window->RemoveInputListener(&g_input);
    g_window->RemoveListener(&g_focus);
  }
  g_window = window;
  // Behind the overlays and the mouse-and-keyboard driver: the debug camera
  // must never take a key away from the settings menu or from gameplay input.
  g_window->AddInputListener(&g_input, 0);
  g_window->AddListener(&g_focus);
}

bool IsKeyDown(rex::ui::VirtualKey key) { return g_input.key_down(key); }

bool PollLookDrag(double& dx_pixels, double& dy_pixels) {
  dx_pixels = 0.0;
  dy_pixels = 0.0;
  if (!g_input.right_down()) {
    g_anchored = false;
    return false;
  }
  std::int32_t x = 0;
  std::int32_t y = 0;
  g_input.position(x, y);
  if (g_anchored) {
    dx_pixels = double(x - g_last_x);
    dy_pixels = double(y - g_last_y);
  }
  g_last_x = x;
  g_last_y = y;
  g_anchored = true;
  return true;
}

}  // namespace skate3::debug_input

#endif  // _WIN32
