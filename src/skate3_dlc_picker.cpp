#include "skate3_dlc_picker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#include <gtk/gtk.h>
#endif

REXCVAR_DEFINE_STRING(skate3_dlc_prompt, "auto", "Skate 3",
                      "When to ask which add-on to load at startup: 'auto' asks only when "
                      "more than one is installed, 'always' asks even for a single one, "
                      "'never' loads the remembered or only pack without asking.")
    .allowed({"auto", "always", "never"});
REXCVAR_DEFINE_STRING(skate3_dlc_select, "", "Skate 3",
                      "Load this add-on and skip the picker. Matches a package name, a "
                      "file name or a display name, case-insensitively. 'none' boots the "
                      "base game with no add-on staged.");
REXCVAR_DEFINE_BOOL(skate3_dlc_input_trace, false, "Skate 3",
                    "Log every non-empty pad report and the pointer position the add-on "
                    "picker sees. For working out why a picker chose something nobody "
                    "pressed.");
REXCVAR_DEFINE_BOOL(skate3_dlc_remember, true, "Skate 3",
                    "Remember the last add-on chosen and pre-select it next time.");

namespace skate3::dlc {
namespace {

namespace fs = std::filesystem;

using rex::system::XContentType;
using rex::system::xam::DummyDeviceId;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

constexpr uint64_t kMinPackBytes = 1ull << 20;
// How long the picker refuses to act on input after appearing. Long enough to
// swallow a click or a keypress meant for whatever had the screen a moment ago,
// short enough that nobody notices.
constexpr float kInputSettleSeconds = 0.6f;
// The content header IS this struct on disk: device_id, content_type, a
// UTF-16BE display name at 0x008, the 42-byte content id at 0x108 and the
// title id twelve bytes from the end. Community packs ship exactly this, which
// is why one can be read and written with no format of our own.
constexpr size_t kHeaderSize = sizeof(XCONTENT_AGGREGATE_DATA);
static_assert(kHeaderSize == 0x148, "content header size changed");

// Never a map pack, and cheap to rule out before touching the file.
bool IsIgnorableFile(const fs::path& path) {
  static const std::array<std::string_view, 12> kSkip = {
      ".header", ".txt", ".rtf", ".log", ".png", ".jpg",
      ".md",     ".xcp", ".iso", ".zip", ".rar", ".7z"};
  auto ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return std::find(kSkip.begin(), kSkip.end(), ext) != kSkip.end();
}

std::string ToUpperAlnum(std::string_view text, size_t limit) {
  std::string out;
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (out.size() >= limit) {
      break;
    }
  }
  return out;
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// "joyride_00000000" -> "Joyride"; "meebs_and_brassy_ports" -> "Meebs And Brassy Ports".
std::string PrettifyStem(std::string_view stem) {
  std::string out;
  bool start_of_word = true;
  for (char c : stem) {
    if (c == '_' || c == '-') {
      if (!out.empty() && out.back() != ' ') {
        out.push_back(' ');
      }
      start_of_word = true;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) && start_of_word) {
      // The trailing _00000000 chunk every community pack carries.
      break;
    }
    out.push_back(start_of_word ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                                : c);
    start_of_word = false;
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string Utf16ToUtf8(const std::u16string& text) {
  std::string out;
  out.reserve(text.size());
  for (char16_t c : text) {
    if (c == 0) {
      break;
    }
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

std::u16string Utf8ToUtf16(std::string_view text) {
  std::u16string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    char32_t code = c;
    size_t extra = 0;
    if (c >= 0xF0) {
      code = c & 0x07;
      extra = 3;
    } else if (c >= 0xE0) {
      code = c & 0x0F;
      extra = 2;
    } else if (c >= 0xC0) {
      code = c & 0x1F;
      extra = 1;
    }
    if (i + extra >= text.size()) {
      break;
    }
    for (size_t k = 1; k <= extra; ++k) {
      code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    }
    i += extra + 1;
    // The content header stores UCS-2; anything outside the BMP has no place
    // in a pack name, so fold it to a replacement rather than write a pair.
    out.push_back(code <= 0xFFFF ? static_cast<char16_t>(code) : u'?');
  }
  return out;
}

bool ReadContentHeader(const fs::path& path, XCONTENT_AGGREGATE_DATA& out) {
  std::error_code ec;
  if (fs::file_size(path, ec) < kHeaderSize || ec) {
    return false;
  }
  FILE* file = rex::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  const size_t read = std::fread(&out, 1, kHeaderSize, file);
  std::fclose(file);
  return read == kHeaderSize;
}

// A pack's shipped header, if it has one. Community packs put it beside the
// .big; the canonical DLC tree puts it in a sibling Headers/<type>/ folder.
fs::path FindHeaderFor(const fs::path& big, XCONTENT_AGGREGATE_DATA& out) {
  std::vector<fs::path> candidates;
  std::error_code ec;
  const fs::path preferred = big.parent_path() / (big.stem().string() + ".header");
  if (fs::is_regular_file(preferred, ec)) {
    candidates.push_back(preferred);
  }
  for (const auto& entry : fs::directory_iterator(big.parent_path(), ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".header") {
      candidates.push_back(entry.path());
    }
  }
  // .../00000002/<PKG>/pack.big  ->  .../Headers/00000002/<PKG>.header
  fs::path dir = big.parent_path();
  for (int level = 0; level < 4 && !dir.empty() && dir != dir.parent_path(); ++level) {
    const fs::path headers = dir / "Headers";
    if (fs::is_directory(headers, ec)) {
      for (const auto& entry : fs::recursive_directory_iterator(headers, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".header") {
          candidates.push_back(entry.path());
        }
      }
    }
    dir = dir.parent_path();
  }

  const std::string wanted = ToUpperAlnum(big.parent_path().filename().string(), 42);
  fs::path fallback;
  for (const auto& candidate : candidates) {
    XCONTENT_AGGREGATE_DATA data = {};
    if (!ReadContentHeader(candidate, data)) {
      continue;
    }
    if (static_cast<XContentType>(data.content_type) != XContentType::kMarketplaceContent) {
      continue;
    }
    // The content id must equal the package directory name, so when the pack
    // sits in one, that is the header that belongs to it.
    if (!wanted.empty() && ToUpperAlnum(data.file_name(), 42) == wanted) {
      out = data;
      return candidate;
    }
    if (fallback.empty()) {
      fallback = candidate;
      out = data;
    }
  }
  return fallback;
}


// ---------------------------------------------------------------------------
// Staging: one chosen pack, in a content root of its own
// ---------------------------------------------------------------------------


// Where the game itself looks for installed add-ons, and where this module
// parks the ones that are not being played.
//
// An earlier version pointed SetContentTypeRoot at a directory per add-on,
// which looked much tidier - no moving, no deleting, switch by pointing
// somewhere else. It does not work: with the flattened override layout the
// title exits by itself about eight seconds after the start screen, every
// time, while the identical pack in the normal layout plays for as long as you
// leave it (measured 8.6 s against 121 s, same header, same archive, same
// macro). So the add-on the player picked goes in the ordinary place, and the
// others are moved aside.
//
// Moving, not copying or deleting: a rename inside one filesystem is instant
// and lossless, so switching between two unpacked add-ons costs nothing and an
// add-on unpacked once is never unpacked again.
std::string TitleIdHex(uint32_t title_id) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "%08X", title_id);
  return buffer;
}

fs::path AddonsRoot(rex::Runtime* runtime) { return runtime->user_data_root() / "addons"; }

fs::path ParkRoot(rex::Runtime* runtime) { return AddonsRoot(runtime) / "parked"; }

fs::path LiveContentDir(rex::Runtime* runtime, uint32_t title_id) {
  return runtime->user_data_root() / "0000000000000000" / TitleIdHex(title_id) / "00000002";
}

fs::path LiveHeaderDir(rex::Runtime* runtime, uint32_t title_id) {
  return runtime->user_data_root() / "0000000000000000" / TitleIdHex(title_id) / "Headers" /
         "00000002";
}

fs::path LastChoicePath(rex::Runtime* runtime) { return AddonsRoot(runtime) / "last_choice.txt"; }

std::string ReadLastChoice(rex::Runtime* runtime) {
  FILE* file = rex::filesystem::OpenFile(LastChoicePath(runtime), "rb");
  if (!file) {
    return {};
  }
  char buffer[1024] = {};
  const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
  std::fclose(file);
  std::string value(buffer, read);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

void WriteLastChoice(rex::Runtime* runtime, const std::string& value) {
  if (!REXCVAR_GET(skate3_dlc_remember)) {
    return;
  }
  std::error_code ec;
  fs::create_directories(AddonsRoot(runtime), ec);
  if (FILE* file = rex::filesystem::OpenFile(LastChoicePath(runtime), "wb")) {
    std::fwrite(value.data(), 1, value.size(), file);
    std::fputc('\n', file);
    std::fclose(file);
  }
}

bool DirectoryHasFiles(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return false;
  }
  for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) {
      return true;
    }
  }
  return false;
}

uint64_t DirectorySize(const fs::path& dir) {
  std::error_code ec;
  uint64_t total = 0;
  for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file(ec)) {
      total += static_cast<uint64_t>(entry.file_size(ec));
    }
  }
  return total;
}

// Rename, falling back to a copy when the two paths straddle filesystems.
bool MovePath(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (!fs::exists(fs::symlink_status(from, ec))) {
    return false;
  }
  fs::create_directories(to.parent_path(), ec);
  ec.clear();
  fs::rename(from, to, ec);
  if (!ec) {
    return true;
  }
  ec.clear();
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) {
    REXLOG_WARN("Skate 3 add-ons: could not move {} to {}: {}", from.string(), to.string(),
                ec.message());
    return false;
  }
  fs::remove_all(from, ec);
  return true;
}

// Move every installed add-on out of the way, so the one being played is the
// only one the game's content scan can see. Nothing is deleted; everything
// parked here comes back the moment it is picked again.
void ParkInstalledAddons(rex::Runtime* runtime, uint32_t title_id,
                         const std::string& keep_package) {
  const fs::path live = LiveContentDir(runtime, title_id);
  const fs::path headers = LiveHeaderDir(runtime, title_id);
  const fs::path park = ParkRoot(runtime);
  std::error_code ec;
  if (!fs::is_directory(live, ec)) {
    return;
  }
  std::vector<fs::path> to_park;
  for (const auto& entry : fs::directory_iterator(live, ec)) {
    if (entry.is_directory(ec) && entry.path().filename().string() != keep_package) {
      to_park.push_back(entry.path());
    }
  }
  for (const auto& package : to_park) {
    const std::string name = package.filename().string();
    if (MovePath(package, park / name)) {
      MovePath(headers / (name + ".header"), park / (name + ".header"));
      REXLOG_INFO("Skate 3 add-on: parked '{}' (only one add-on loads at a time)", name);
    }
  }
}

// Bring a previously parked add-on back into the live content folder.
bool UnparkAddon(rex::Runtime* runtime, uint32_t title_id, const std::string& package) {
  const fs::path park = ParkRoot(runtime);
  if (!DirectoryHasFiles(park / package)) {
    return false;
  }
  if (!MovePath(park / package, LiveContentDir(runtime, title_id) / package)) {
    return false;
  }
  MovePath(park / (package + ".header"),
           LiveHeaderDir(runtime, title_id) / (package + ".header"));
  REXLOG_INFO("Skate 3 add-on: restored '{}' from the parking folder", package);
  return true;
}

// Put the archive where the content device expects it, without a second copy of
// a quarter-gigabyte map on disk. A hard link is free and behaves exactly like
// the file; a symlink is the fallback across filesystems; a copy is the last
// resort. The existing entry is reused whenever it still points at the same
// bytes, so a relaunch on the same pack does no work at all.
bool PlaceArchive(const fs::path& source, const fs::path& target, std::string& error) {
  std::error_code ec;
  if (fs::exists(fs::symlink_status(target, ec))) {
    std::error_code size_ec;
    if (fs::exists(target, size_ec) &&
        fs::file_size(target, size_ec) == fs::file_size(source, size_ec) && !size_ec) {
      return true;
    }
    fs::remove(target, ec);
  }
  fs::create_directories(target.parent_path(), ec);

  ec.clear();
  fs::create_hard_link(source, target, ec);
  if (!ec) {
    return true;
  }
  ec.clear();
  fs::create_symlink(fs::absolute(source), target, ec);
  if (!ec) {
    return true;
  }
  ec.clear();
  fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
  if (!ec) {
    return true;
  }
  error = "could not place " + source.filename().string() + ": " + ec.message();
  return false;
}

// Everything the picker needs to show and the pump needs to drive.
struct Selection {
  std::vector<Candidate> candidates;
  int chosen = -1;  // -1 = base game only
  bool decided = false;
  bool quit = false;
};

// ---------------------------------------------------------------------------
// Staging work, off the UI thread
// ---------------------------------------------------------------------------

struct StageProgress {
  std::atomic<uint64_t> copied{0};
  std::atomic<uint64_t> total{0};
  std::atomic<bool> done{false};
  std::atomic<bool> ok{false};
  std::string error;  // written before `done`, read after it
};

// The package directory an add-on installs itself as. A signed container names
// it after the container file, which is what ContentManager::InstallContent
// does; a raw archive uses the content id from its header, which is the one
// rule the content scan actually enforces.
std::string PackageDirName(const Candidate& candidate) {
  return candidate.stfs ? candidate.path.filename().string() : candidate.package;
}

fs::path InstalledPathFor(rex::Runtime* runtime, uint32_t title_id, const Candidate& candidate) {
  return LiveContentDir(runtime, title_id) / PackageDirName(candidate);
}

// Whether picking this add-on means unpacking a container, which is the only
// slow part of the whole flow and the only part that needs a progress screen.
bool NeedsExtraction(rex::Runtime* runtime, uint32_t title_id, const Candidate& candidate) {
  if (!candidate.stfs) {
    return false;
  }
  const std::string package = PackageDirName(candidate);
  return !DirectoryHasFiles(InstalledPathFor(runtime, title_id, candidate)) &&
         !DirectoryHasFiles(ParkRoot(runtime) / package);
}

// Make the chosen pack the one installed add-on. Safe to call on a worker
// thread: the guest has not launched, so nothing else is touching content.
bool Stage(rex::Runtime* runtime, const Candidate& candidate, StageProgress* progress,
           std::string& error) {
  auto* content = runtime->kernel_state()->content_manager();
  const uint32_t title_id = runtime->kernel_state()->title_id();
  const std::string package = PackageDirName(candidate);
  const fs::path installed = InstalledPathFor(runtime, title_id, candidate);

  ParkInstalledAddons(runtime, title_id, package);
  UnparkAddon(runtime, title_id, package);

  if (candidate.stfs) {
    if (DirectoryHasFiles(installed)) {
      REXLOG_INFO("Skate 3 add-on: '{}' is already unpacked", candidate.display_name);
      return true;
    }
    if (progress) {
      progress->total.store(candidate.bytes, std::memory_order_release);
    }
    const auto result = content->InstallContent(candidate.path);
    if (result != 0) {
      error = "the game could not unpack this add-on (error " + std::to_string(result) + ")";
      return false;
    }
    REXLOG_INFO("Skate 3 add-on: unpacked '{}' into {}", candidate.display_name,
                installed.string());
    return true;
  }

  // A raw BIG archive: place it under a package directory of its own and write
  // the enumeration header beside it. The header on disk is exactly
  // XCONTENT_AGGREGATE_DATA, so this is the same 328-byte file community packs
  // ship rather than a format of our own.
  if (!PlaceArchive(candidate.path, installed / candidate.path.filename(), error)) {
    return false;
  }

  XCONTENT_AGGREGATE_DATA data = {};
  data.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
  data.content_type = XContentType::kMarketplaceContent;
  data.set_display_name(Utf8ToUtf16(candidate.display_name));
  data.set_file_name(package);
  data.xuid = 0;
  data.title_id = title_id;
  if (content->WriteContentHeaderFile(0, data) != 0) {
    error = "could not write the content header for " + package;
    return false;
  }
  REXLOG_INFO("Skate 3 add-on: staged '{}' as package {} at {}", candidate.display_name, package,
              installed.string());
  return true;
}

// Nothing chosen: park everything, so a session with no add-on really is the
// base game rather than whatever was installed last time.
void StageBaseGameOnly(rex::Runtime* runtime) {
  ParkInstalledAddons(runtime, runtime->kernel_state()->title_id(), std::string());
}

// ---------------------------------------------------------------------------
// The picker
// ---------------------------------------------------------------------------

// Deliberately not the SDK's shared wizard screen: that one is keyboard and
// mouse only, and this dialog is the first thing a controller-only machine
// (a Steam Deck in game mode) shows. It reads the merged UI pad directly, the
// same way the in-game level picker does, and borrows that picker's look so
// the two read as one product.
class PickerDialog final : public rex::ui::ImGuiDialog {
 public:
  PickerDialog(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input, rex::Runtime* runtime,
               std::shared_ptr<Selection> selection)
      : ImGuiDialog(drawer),
        input_(input),
        runtime_(runtime),
        selection_(std::move(selection)) {
    focus_ = selection_->chosen + 1;  // row 0 is "base game only"
  }

  // Skip the question and go straight to unpacking, for a pack that was chosen
  // by cvar or is the only one installed but has never been unpacked.
  void StageWithoutAsking() {
    state_ = State::kStaging;
    StartStaging();
  }

 protected:
  bool WantsContinuousRepaint() const override { return true; }
  void OnDraw(ImGuiIO& io) override;
  // The drawer deletes dialogs through an ImGuiDialog* whose destructor is not
  // virtual, so this override is the only place our members can be wound down.
  void OnClose() override {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  enum class State { kChoosing, kStaging, kFailed };

  struct PadNav {
    int move = 0;
    bool accept = false;
    bool cancel = false;
  };

  PadNav ReadPad(float delta_seconds);
  void Commit(int row);
  void StartStaging();
  void PollStaging();
  void DrawBackdrop(ImGuiIO& io);
  void DrawChoosing(ImGuiIO& io);
  void DrawProgress(ImGuiIO& io);

  rex::input::InputSystem* input_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::shared_ptr<Selection> selection_;
  State state_ = State::kChoosing;
  int focus_ = 0;
  bool scroll_to_focus_ = true;
  std::shared_ptr<StageProgress> progress_;
  std::thread worker_;
  std::string error_;
  bool pad_up_ = false, pad_down_ = false, pad_a_ = false, pad_b_ = false;
  bool pad_primed_ = false;
  float repeat_ = 0.0f;
  float elapsed_ = 0.0f;
  float poll_countdown_ = 0.0f;
  const char* chosen_by_ = "?";
  float trace_countdown_ = 0.0f;
};

PickerDialog::PadNav PickerDialog::ReadPad(float delta_seconds) {
  PadNav out;
  if (!input_) {
    return out;
  }
  rex::input::X_INPUT_GAMEPAD pad = {};
  if (!input_->GetUiGamepadState(&pad)) {
    return out;
  }
  constexpr int16_t kStick = 16000;
  const bool up = (pad.buttons & rex::input::X_INPUT_GAMEPAD_DPAD_UP) != 0 || pad.thumb_ly > kStick;
  const bool down =
      (pad.buttons & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) != 0 || pad.thumb_ly < -kStick;
  const bool a = (pad.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0;
  const bool b = (pad.buttons & rex::input::X_INPUT_GAMEPAD_B) != 0;
  if (REXCVAR_GET(skate3_dlc_input_trace) && (up || down || a || b || pad.buttons)) {
    REXLOG_INFO("Skate 3 add-on picker: pad buttons={:04X} lx={} ly={} -> up={} down={} a={} b={}",
                static_cast<uint16_t>(pad.buttons), static_cast<int16_t>(pad.thumb_lx),
                static_cast<int16_t>(pad.thumb_ly), up, down, a, b);
  }
  // The first sample is a BASELINE, never an event. Without this a button that
  // happens to be down when the picker opens - or a device that reports a
  // garbage first frame as it is hot-plugged - reads as a rising edge and
  // chooses for the player.
  if (!pad_primed_) {
    pad_primed_ = true;
    pad_up_ = up;
    pad_down_ = down;
    pad_a_ = a;
    pad_b_ = b;
    return out;
  }

  // Rising edge moves at once; holding repeats, the way the game's own menus
  // feel to a thumb.
  if (up && !pad_up_) {
    out.move = -1;
    repeat_ = 0.45f;
  } else if (down && !pad_down_) {
    out.move = 1;
    repeat_ = 0.45f;
  } else if (up || down) {
    repeat_ -= delta_seconds;
    if (repeat_ <= 0.0f) {
      out.move = up ? -1 : 1;
      repeat_ = 0.12f;
    }
  }
  out.accept = a && !pad_a_;
  out.cancel = b && !pad_b_;
  pad_up_ = up;
  pad_down_ = down;
  pad_a_ = a;
  pad_b_ = b;
  return out;
}

void PickerDialog::Commit(int row) {
  selection_->chosen = row - 1;  // row 0 is the base game
  if (selection_->chosen < 0) {
    selection_->decided = true;
    Close();
    return;
  }
  state_ = State::kStaging;
  StartStaging();
}

void PickerDialog::StartStaging() {
  progress_ = std::make_shared<StageProgress>();
  const Candidate candidate = selection_->candidates[selection_->chosen];
  auto progress = progress_;
  auto* runtime = runtime_;
  worker_ = std::thread([runtime, candidate, progress]() {
    std::string error;
    const bool ok = Stage(runtime, candidate, progress.get(), error);
    progress->error = error;
    progress->ok.store(ok, std::memory_order_release);
    progress->done.store(true, std::memory_order_release);
  });
}

void PickerDialog::PollStaging() {
  // InstallContent has no progress callback, so measure the only thing that
  // actually moves: bytes landing in the destination. Twice a second, not every
  // frame - walking a half-gigabyte tree at 60 Hz costs more IO than the
  // extraction it is reporting on.
  poll_countdown_ -= 1.0f / 60.0f;
  if (progress_ && poll_countdown_ <= 0.0f &&
      progress_->total.load(std::memory_order_acquire) != 0) {
    poll_countdown_ = 0.5f;
    progress_->copied.store(
        DirectorySize(InstalledPathFor(runtime_, runtime_->kernel_state()->title_id(),
                                     selection_->candidates[selection_->chosen])),
        std::memory_order_release);
  }
  if (!progress_ || !progress_->done.load(std::memory_order_acquire)) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  if (progress_->ok.load(std::memory_order_acquire)) {
    selection_->decided = true;
    Close();
    return;
  }
  error_ = progress_->error;
  REXLOG_ERROR("Skate 3 add-on: {}", error_);
  state_ = State::kFailed;
}

void PickerDialog::DrawBackdrop(ImGuiIO& io) {
  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  draw->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(8, 8, 10, 255));
}

void PickerDialog::DrawChoosing(ImGuiIO& io) {
  elapsed_ += io.DeltaTime;
  // Nothing commits in the first moments the picker is on screen. A click or a
  // keypress aimed at whatever was in front of this window a frame ago must not
  // land on a row, and a controller plugged in as the game starts must not
  // choose with its first report.
  const bool settled = elapsed_ > kInputSettleSeconds;
  trace_countdown_ -= io.DeltaTime;
  const int count = static_cast<int>(selection_->candidates.size()) + 1;
  focus_ = std::clamp(focus_, 0, count - 1);

  const PadNav nav = ReadPad(io.DeltaTime);
  int move = nav.move;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
    move = 1;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
    move = -1;
  }
  if (move != 0) {
    focus_ = (focus_ + move + count) % count;
    scroll_to_focus_ = true;
  }
  const bool activate = settled && (nav.accept || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                    ImGui::IsKeyPressed(ImGuiKey_Space, false));

  const ImU32 accent = IM_COL32(255, 122, 0, 255);
  const ImU32 accent_dim = IM_COL32(255, 122, 0, 56);

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(
      ImVec2(std::min(io.DisplaySize.x * 0.62f, 760.0f), io.DisplaySize.y * 0.78f),
      ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
  ImGui::Begin("Choose an add-on", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoMove);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 win_pos = ImGui::GetWindowPos();
  const ImVec2 win_size = ImGui::GetWindowSize();
  draw->AddRectFilled(win_pos, ImVec2(win_pos.x + 6.0f, win_pos.y + win_size.y), accent);

  ImGui::Dummy(ImVec2(0.0f, 24.0f));
  ImGui::Indent(30.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  ImGui::SetWindowFontScale(1.7f);
  ImGui::TextUnformatted("CHOOSE AN ADD-ON");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.45f));
  ImGui::TextUnformatted("One loads per session. Quit to the desktop to pick another.");
  ImGui::PopStyleColor();
  ImGui::Unindent(30.0f);
  ImGui::Dummy(ImVec2(0.0f, 16.0f));

  const float footer = 74.0f;
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1, 1, 1, 0.14f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1, 1, 1, 0.22f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(1, 1, 1, 0.30f));
  ImGui::BeginChild("##addons", ImVec2(0.0f, win_size.y - ImGui::GetCursorPosY() - footer), false,
                    ImGuiWindowFlags_NoBackground);
  ImDrawList* rows = ImGui::GetWindowDrawList();
  constexpr float kRowHeight = 52.0f;
  int chosen = -1;
  for (int i = 0; i < count; ++i) {
    ImGui::PushID(i);
    const bool selected = (i == focus_);
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    const ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + kRowHeight);
    if (selected) {
      rows->AddRectFilled(row_min, row_max, accent_dim);
      rows->AddRectFilled(row_min, ImVec2(row_min.x + 4.0f, row_max.y), accent);
    }
    if (ImGui::InvisibleButton("##row", ImVec2(ImGui::GetContentRegionAvail().x, kRowHeight)) &&
        settled) {
      chosen = i;
      chosen_by_ = "click";
    }
    if (ImGui::IsItemHovered()) {
      if (REXCVAR_GET(skate3_dlc_input_trace) && trace_countdown_ <= 0.0f) {
        REXLOG_INFO("Skate 3 add-on picker: pointer {},{} hovering row {} (delta {},{}, down={})",
                    io.MousePos.x, io.MousePos.y, i, io.MouseDelta.x, io.MouseDelta.y,
                    ImGui::IsMouseDown(ImGuiMouseButton_Left));
        trace_countdown_ = 1.0f;
      }
      // Only a MOVING mouse takes the selection, so resting the cursor over a
      // row does not fight the stick for it.
      const ImVec2 moved = ImGui::GetIO().MouseDelta;
      if (moved.x != 0.0f || moved.y != 0.0f) {
        focus_ = i;
      }
      if (i != focus_) {
        rows->AddRectFilled(row_min, row_max, IM_COL32(255, 122, 0, 24));
      }
    }

    const char* title = i == 0 ? "Base game only" : selection_->candidates[i - 1].display_name.c_str();
    const float text_y = row_min.y + kRowHeight * 0.5f - ImGui::GetFontSize() * 1.05f;
    rows->AddText(ImVec2(row_min.x + 30.0f, text_y),
                  ImGui::GetColorU32(ImVec4(1, 1, 1, selected ? 1.0f : 0.78f)), title);

    std::string detail;
    if (i == 0) {
      detail = "Skate 3 as it shipped, with no add-on installed";
    } else {
      const Candidate& candidate = selection_->candidates[i - 1];
      const double mb = static_cast<double>(candidate.bytes) / (1024.0 * 1024.0);
      char buffer[256] = {};
      std::snprintf(buffer, sizeof(buffer), "%s  \xc2\xb7  %.0f MB", candidate.package.c_str(), mb);
      detail = buffer;
    }
    rows->AddText(ImVec2(row_min.x + 30.0f, text_y + ImGui::GetFontSize() * 1.35f),
                  ImGui::GetColorU32(ImVec4(1, 1, 1, 0.38f)), detail.c_str());
    ImGui::PopID();
  }
  if (scroll_to_focus_) {
    scroll_to_focus_ = false;
    const float target = focus_ * kRowHeight;
    if (target < ImGui::GetScrollY()) {
      ImGui::SetScrollY(target);
    } else if (target + kRowHeight > ImGui::GetScrollY() + ImGui::GetWindowHeight()) {
      ImGui::SetScrollY(target + kRowHeight - ImGui::GetWindowHeight());
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(4);

  if (activate && chosen < 0) {
    chosen = focus_;
    chosen_by_ = "activate";
  }

  draw->AddLine(ImVec2(win_pos.x + 30.0f, win_pos.y + win_size.y - footer + 8.0f),
                ImVec2(win_pos.x + win_size.x - 30.0f, win_pos.y + win_size.y - footer + 8.0f),
                ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)));
  ImGui::Indent(30.0f);
  ImGui::Dummy(ImVec2(0.0f, 16.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.50f));
  ImGui::TextUnformatted("A / Enter  play        B / Esc  base game");
  ImGui::PopStyleColor();
  ImGui::Unindent(30.0f);

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);

  if (settled && (nav.cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
    REXLOG_INFO("Skate 3 add-on picker: cancelled, playing the base game");
    Commit(0);
    return;
  }
  if (chosen >= 0) {
    REXLOG_INFO("Skate 3 add-on picker: row {} chosen by {} after {:.2f}s", chosen, chosen_by_,
                elapsed_);
    Commit(chosen);
  }
}

void PickerDialog::DrawProgress(ImGuiIO& io) {
  elapsed_ += io.DeltaTime;
  const bool failed = state_ == State::kFailed;
  const Candidate& candidate = selection_->candidates[std::max(0, selection_->chosen)];

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(io.DisplaySize.x * 0.62f, 760.0f), 260.0f),
                           ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 26.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, 1.0f));
  ImGui::Begin("Preparing add-on", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoMove);

  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  ImGui::SetWindowFontScale(1.5f);
  ImGui::TextUnformatted(failed ? "COULD NOT LOAD THAT ADD-ON" : "PREPARING");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.72f));
  ImGui::TextWrapped("%s", candidate.display_name.c_str());
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0.0f, 14.0f));

  if (failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", error_.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.55f));
    ImGui::TextUnformatted("A / Enter  continue without it");
    ImGui::PopStyleColor();
    const PadNav nav = ReadPad(io.DeltaTime);
    if (nav.accept || nav.cancel || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      selection_->chosen = -1;
      selection_->decided = true;
      Close();
    }
  } else {
    const uint64_t total = progress_ ? progress_->total.load(std::memory_order_acquire) : 0;
    const uint64_t copied = progress_ ? progress_->copied.load(std::memory_order_acquire) : 0;
    if (total > 0) {
      const float fraction = std::clamp(static_cast<float>(copied) / static_cast<float>(total),
                                        0.0f, 1.0f);
      char label[64] = {};
      std::snprintf(label, sizeof(label), "%.0f / %.0f MB",
                    static_cast<double>(copied) / (1024.0 * 1024.0),
                    static_cast<double>(total) / (1024.0 * 1024.0));
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.48f, 0.0f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.08f));
      ImGui::ProgressBar(fraction, ImVec2(-1.0f, 14.0f), label);
      ImGui::PopStyleColor(2);
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.45f));
    ImGui::TextUnformatted(
        "Unpacking this add-on. It only happens the first time you play it.");
    ImGui::PopStyleColor();
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

void PickerDialog::OnDraw(ImGuiIO& io) {
  if (state_ == State::kStaging) {
    PollStaging();
  }
  DrawBackdrop(io);
  if (state_ == State::kChoosing) {
    DrawChoosing(io);
  } else {
    DrawProgress(io);
  }
}

}  // namespace

std::vector<Candidate> Discover(const std::vector<fs::path>& search_dirs, uint32_t title_id) {
  std::vector<Candidate> found;
  std::vector<std::string> seen;

  for (const auto& dir : search_dirs) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      continue;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
      if (ec) {
        REXLOG_WARN("Skate 3 add-ons: could not scan {}: {}", dir.string(), ec.message());
        break;
      }
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      const fs::path path = entry.path();
      if (IsIgnorableFile(path)) {
        continue;
      }
      const uint64_t bytes = static_cast<uint64_t>(fs::file_size(path, ec));
      if (ec || bytes < kMinPackBytes) {
        continue;
      }

      std::error_code canonical_ec;
      std::string key = fs::weakly_canonical(path, canonical_ec).string();
      if (canonical_ec) {
        key = fs::absolute(path).string();
      }
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
        continue;
      }

      Candidate candidate;
      candidate.path = path;
      candidate.bytes = bytes;

      if (auto header = rex::filesystem::StfsContainerDevice::ReadPackageHeader(path)) {
        const auto content_type = static_cast<XContentType>(header->metadata.content_type);
        if (content_type != XContentType::kMarketplaceContent) {
          continue;
        }
        const uint32_t package_title = header->metadata.execution_info.title_id;
        if (package_title != 0 && package_title != title_id) {
          continue;
        }
        candidate.stfs = true;
        candidate.package = path.filename().string();
        candidate.display_name =
            Utf16ToUtf8(header->metadata.display_name(rex::system::XLanguage::kEnglish));
        if (candidate.display_name.empty()) {
          candidate.display_name = path.filename().string();
        }
      } else {
        // Not a signed container. The other shape a map arrives in is a raw
        // BIG archive with the 328-byte content header beside it.
        if (ToLower(path.extension().string()) != ".big") {
          continue;
        }
        XCONTENT_AGGREGATE_DATA data = {};
        candidate.header = FindHeaderFor(path, data);
        if (!candidate.header.empty()) {
          candidate.package = data.file_name();
          candidate.display_name = Utf16ToUtf8(data.display_name());
        }
        if (candidate.package.empty()) {
          // No header: the package name has to come from the layout. A pack in
          // its own <PKG>/ directory names itself; otherwise use the archive.
          const std::string parent = path.parent_path().filename().string();
          candidate.package = ToUpperAlnum(parent, 42);
          if (candidate.package.empty() || candidate.package.size() < 3) {
            candidate.package = ToUpperAlnum(path.stem().string(), 42);
          }
        }
        if (candidate.display_name.empty()) {
          candidate.display_name = PrettifyStem(path.stem().string());
          if (candidate.display_name.empty()) {
            candidate.display_name = candidate.package;
          }
        }
        if (candidate.package.empty()) {
          continue;
        }
      }

      seen.push_back(key);
      found.push_back(std::move(candidate));
    }
  }

  std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
    return ToLower(a.display_name) < ToLower(b.display_name);
  });
  return found;
}

namespace {

// Match what the player typed into skate3_dlc_select against the things they
// could reasonably have meant.
int FindByName(const std::vector<Candidate>& candidates, const std::string& wanted) {
  const std::string needle = ToLower(wanted);
  for (size_t i = 0; i < candidates.size(); ++i) {
    const Candidate& candidate = candidates[i];
    if (ToLower(candidate.package) == needle || ToLower(candidate.display_name) == needle ||
        ToLower(candidate.path.filename().string()) == needle ||
        ToLower(candidate.path.stem().string()) == needle) {
      return static_cast<int>(i);
    }
  }
  // Second pass: a substring of the display name, so "hastings" finds
  // "Hastings Bowl" without the player quoting anything.
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (ToLower(candidates[i].display_name).find(needle) != std::string::npos) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FindByPath(const std::vector<Candidate>& candidates, const std::string& path) {
  if (path.empty()) {
    return -1;
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    std::error_code ec;
    if (fs::weakly_canonical(candidates[i].path, ec).string() == path) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string CanonicalKey(const fs::path& path) {
  std::error_code ec;
  const std::string key = fs::weakly_canonical(path, ec).string();
  return ec ? fs::absolute(path).string() : key;
}

// Pump the UI while a pre-runtime dialog owns the screen. The guest has not
// launched yet, so nothing else drives the window: without this the picker
// would never paint. Mirrors the ISO installer's pump.
void PumpUntilDone(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                   const std::shared_ptr<Selection>& selection) {
  while (!selection->decided && !app_context.HasQuitFromUIThread()) {
    app_context.ExecutePendingFunctionsFromUIThread();
    if (window) {
      window->RequestPaint();
    }
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
    while (gtk_events_pending()) {
      gtk_main_iteration_do(FALSE);
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
}

}  // namespace

bool RunStartupSelection(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window,
                         rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime,
                         const std::vector<fs::path>& search_dirs) {
  if (!runtime || !runtime->kernel_state() || !runtime->kernel_state()->content_manager()) {
    return true;
  }
  const uint32_t title_id = runtime->kernel_state()->title_id();
  if (title_id == 0) {
    REXLOG_WARN("Skate 3 add-ons: no title ID yet, skipping add-on selection");
    return true;
  }

  auto selection = std::make_shared<Selection>();
  selection->candidates = Discover(search_dirs, title_id);
  for (const auto& candidate : selection->candidates) {
    REXLOG_INFO("Skate 3 add-on found: '{}' [{}] {}", candidate.display_name, candidate.package,
                candidate.path.string());
  }

  const std::string requested = REXCVAR_GET(skate3_dlc_select);
  const std::string prompt = REXCVAR_GET(skate3_dlc_prompt);
  const int remembered = FindByPath(selection->candidates, ReadLastChoice(runtime));

  bool ask = false;
  if (!requested.empty()) {
    // An explicit request is never questioned, including "none".
    selection->chosen = ToLower(requested) == "none"
                            ? -1
                            : FindByName(selection->candidates, requested);
    if (selection->chosen < 0 && ToLower(requested) != "none") {
      REXLOG_WARN("Skate 3 add-ons: nothing installed matches skate3_dlc_select='{}'; "
                  "booting the base game",
                  requested);
    }
  } else if (selection->candidates.empty()) {
    selection->chosen = -1;
  } else if (prompt == "never") {
    selection->chosen = remembered >= 0 ? remembered : 0;
  } else if (selection->candidates.size() == 1 && prompt != "always") {
    // Exactly one add-on on disk: there is no question to ask.
    selection->chosen = 0;
  } else {
    selection->chosen = remembered >= 0 ? remembered : 0;
    ask = true;
  }

  const bool needs_unpacking =
      selection->chosen >= 0 &&
      NeedsExtraction(runtime, title_id, selection->candidates[selection->chosen]);

  if (ask || needs_unpacking) {
    if (!drawer) {
      REXLOG_WARN("Skate 3 add-ons: no UI available for the picker; booting the base game");
      selection->chosen = -1;
    } else {
      auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system());
      auto* dialog = new PickerDialog(drawer, input, runtime, selection);
      if (!ask) {
        dialog->StageWithoutAsking();
      }
      PumpUntilDone(app_context, window, selection);
      if (!selection->decided) {
        // The window was closed out from under the picker.
        return false;
      }
      // The dialog staged the pack itself, and remembered nothing yet.
      if (selection->chosen >= 0) {
        WriteLastChoice(runtime, CanonicalKey(selection->candidates[selection->chosen].path));
        REXLOG_INFO("Skate 3 add-on: playing '{}'",
                    selection->candidates[selection->chosen].display_name);
      } else {
        REXLOG_INFO("Skate 3 add-on: none; playing the base game");
        StageBaseGameOnly(runtime);
      }
      return true;
    }
  }

  if (selection->chosen >= 0) {
    const Candidate& candidate = selection->candidates[selection->chosen];
    std::string error;
    if (Stage(runtime, candidate, nullptr, error)) {
      WriteLastChoice(runtime, CanonicalKey(candidate.path));
      REXLOG_INFO("Skate 3 add-on: playing '{}'", candidate.display_name);
      return true;
    }
    REXLOG_ERROR("Skate 3 add-on: {}; playing the base game instead", error);
  }

  // Base game: an empty content root, so nothing installed by a previous
  // session leaks into this one.
  REXLOG_INFO("Skate 3 add-on: none; playing the base game");
  StageBaseGameOnly(runtime);
  return true;
}

}  // namespace skate3::dlc
