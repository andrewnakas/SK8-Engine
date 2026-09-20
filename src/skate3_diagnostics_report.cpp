#include "skate3_diagnostics_report.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include <skate3_version.h>

#include "skate3_iso_installer.h"
#include "skate3_title_update_installer.h"

namespace skate3 {
namespace {

// How much of each log to carry. The tail is what matters - a crash is at the
// end - and a whole session's log is megabytes, which nobody can paste.
constexpr std::uintmax_t kLogTailBytes = 128 * 1024;

std::string Section(const std::string& title) {
  const size_t pad = title.size() < 56 ? 56 - title.size() : 4;
  return "\n===== " + title + " " + std::string(pad, '=') + "\n";
}

// Every section goes through this. A section that throws contributes a line
// saying which one failed and why, and the report continues.
//
// This is the whole point of the file. The Android original wrapped nothing,
// read one field that did not exist below API 31, and produced no report at all
// on the devices it was written for.
void Gather(std::ostringstream& out, const std::string& title,
            const std::function<void(std::ostringstream&)>& body) {
  out << Section(title);
  try {
    body(out);
  } catch (const std::exception& e) {
    out << "(this section failed to gather: " << e.what() << ")\n";
  } catch (...) {
    out << "(this section failed to gather: unknown error)\n";
  }
}

std::string NowStamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::array<char, 64> buf{};
  std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S %z", &tm);
  return buf.data();
}

// The last `bytes` of a file, or a line explaining why not. Seeking rather than
// reading the whole thing: these can be megabytes and the point is the end.
std::string Tail(const std::filesystem::path& path, std::uintmax_t bytes) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return "(not present: " + path.string() + ")\n";
  }
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return "(cannot size " + path.string() + ": " + ec.message() + ")\n";
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "(cannot open " + path.string() + ")\n";
  }
  std::string note;
  if (size > bytes) {
    in.seekg(static_cast<std::streamoff>(size - bytes));
    note = "(showing the last " + std::to_string(bytes / 1024) + " KB of " +
           std::to_string(size / 1024) + " KB)\n";
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return note + buf.str() + "\n";
}

// Whole file, for the small ones whose entire content is the evidence.
std::string WholeFile(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return "(not present)\n";
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "(cannot open)\n";
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string text = buf.str();
  if (text.empty()) {
    return "(empty)\n";
  }
  if (text.back() != '\n') {
    text.push_back('\n');
  }
  return text;
}

void DescribeFile(std::ostringstream& out, const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    out << "  missing   " << path.filename().string() << "\n";
    return;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  out << "  " << (ec ? std::string("?") : std::to_string(size)) << " bytes  "
      << path.filename().string() << "\n";
}

// One level of directory listing. Deliberately not recursive: the game tree is
// tens of thousands of files and the question this answers is "is the shape
// right", which the top two levels settle.
void ListDirectory(std::ostringstream& out, const std::filesystem::path& dir, int depth,
                   const std::string& indent) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    out << indent << "(not a directory: " << dir.string() << ")\n";
    return;
  }
  std::vector<std::filesystem::directory_entry> entries;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    entries.push_back(entry);
  }
  if (ec) {
    out << indent << "(cannot list: " << ec.message() << ")\n";
    return;
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
  size_t shown = 0;
  for (const auto& entry : entries) {
    if (shown++ >= 40) {
      out << indent << "... and " << (entries.size() - 40) << " more\n";
      break;
    }
    std::error_code sub_ec;
    if (entry.is_directory(sub_ec)) {
      out << indent << entry.path().filename().string() << "/\n";
      if (depth > 0) {
        ListDirectory(out, entry.path(), depth - 1, indent + "  ");
      }
    } else {
      const std::uintmax_t size = entry.file_size(sub_ec);
      out << indent << entry.path().filename().string() << "  "
          << (sub_ec ? std::string("?") : std::to_string(size)) << "\n";
    }
  }
}

}  // namespace

#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
// Non-Apple platforms have no ObjC half. Report what is portable rather than
// nothing: the report is still worth having on a desktop, and the section
// header stays in the same place so two reports line up when compared.
std::string DiagnosticsDeviceSection() {
  std::ostringstream o;
  o << "Platform " << SKATE3_BUILD_PLATFORM << "\n";
  o << "Hardware threads " << std::thread::hardware_concurrency() << "\n";
  o << "(no per-device detail on this platform)\n";
  return o.str();
}
#endif

std::string CollectDiagnosticsReport(const std::filesystem::path& game_root,
                                     const std::filesystem::path& user_root) {
  std::ostringstream out;
  out << "Skate 3 diagnostic report\n" << NowStamp() << "\n";

  Gather(out, "Build", [](std::ostringstream& o) {
    o << "Engine " << SKATE3_VERSION_STRING << " (" << SKATE3_BUILD_PLATFORM << ", "
      << SKATE3_BUILD_CONFIG << ")\n";
    o << "Built " << SKATE3_BUILD_TIMESTAMP << "\n";
  });

  // Device, OS and GPU. Per-platform, and the one section most likely to use an
  // API the running OS does not have - hence its own isolation.
  Gather(out, "Device", [](std::ostringstream& o) { o << DiagnosticsDeviceSection(); });

  Gather(out, "Game files", [&](std::ostringstream& o) {
    o << "Game root " << game_root.string() << "\n";
    o << "User root " << user_root.string() << "\n";
    std::error_code ec;
    o << "Game root exists " << (std::filesystem::is_directory(game_root, ec) ? "yes" : "NO")
      << "\n";
    o << "Disc files installed " << (IsGameInstalled(game_root) ? "yes" : "NO") << "\n";
    o << "Title update staged " << (IsTitleUpdateInstalled(game_root) ? "yes" : "NO") << "\n";
    // The two files every static address in the recompiled code refers into.
    // A wrong or truncated one of these explains a whole class of "it crashes
    // immediately" reports, and nothing else in the report would show it.
    DescribeFile(o, game_root / "default.xex");
    DescribeFile(o, game_root / "default.xexp");
    const auto space = std::filesystem::space(user_root, ec);
    if (!ec) {
      o << "Free space " << (space.available / (1024 * 1024)) << " MB of "
        << (space.capacity / (1024 * 1024)) << " MB\n";
    }
    o << "\nGame root listing:\n";
    ListDirectory(o, game_root, 1, "  ");
    o << "\nUser root listing:\n";
    ListDirectory(o, user_root, 1, "  ");
  });

  // Both files that can silently override what the build was compiled to do.
  //
  // These are first on the list of things to check and they have historically
  // been the last: a forgotten --log_level=debug buried a whole debugging
  // session, and a stale xma_old_ring_full_is_not_empty=true in ios_args.txt
  // made a fixed audio bug look unfixed on one platform and fixed on the other
  // for an entire afternoon. Whole contents, not a summary - the point is to
  // see the line nobody remembered leaving there.
  Gather(out, "Launch argument overrides (user/ios_args.txt)",
         [&](std::ostringstream& o) { o << WholeFile(user_root / "ios_args.txt"); });
  Gather(out, "Saved settings (user/settings.toml)",
         [&](std::ostringstream& o) { o << WholeFile(user_root / "settings.toml"); });

  Gather(out, "Content packs", [&](std::ostringstream& o) {
    // By name rather than by REXCVAR_GET: these are defined in three
    // different translation units, and a report is not worth a linkage
    // dependency on each of them. GetFlagByName answers for any registered
    // cvar and returns empty for one that is not, which is the right
    // degradation here.
    for (const char* name : {"skate3_content_pack", "skate3_content_pack_menu", "vfs_path_alias",
                             "license_mask", "user_language"}) {
      o << "  " << name << " = '" << rex::cvar::GetFlagByName(name) << "'\n";
    }
    o << "  (user_language 1 = English; DLC maps have been reported not to load "
         "on other languages)\n";
    o << "\nDocuments listing (where packs are dropped):\n";
    ListDirectory(o, user_root.parent_path(), 1, "  ");
  });

  Gather(out, "Engine crash report", [&](std::ostringstream& o) {
    o << Tail(user_root.parent_path() / "skate3_crash.txt", kLogTailBytes);
  });

  Gather(out, "Engine startup (stderr)", [&](std::ostringstream& o) {
    o << Tail(user_root.parent_path() / "stderr.log", 32 * 1024);
  });

  Gather(out, "Engine log", [&](std::ostringstream& o) {
    o << Tail(user_root.parent_path() / "skate3.log", kLogTailBytes);
  });

  out << "\n===== end of report " << std::string(40, '=') << "\n";
  return out.str();
}

std::filesystem::path WriteDiagnosticsReport(const std::filesystem::path& game_root,
                                             const std::filesystem::path& user_root,
                                             std::string& error) {
  error.clear();
  const std::string text = CollectDiagnosticsReport(game_root, user_root);
  // Beside the log, in Documents, because that is the directory file sharing
  // exposes. Under user/ it would be just as unreachable as the log was.
  const auto path = user_root.parent_path() / "skate3_diagnostic_report.txt";
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    error = "could not open " + path.string() + " for writing";
    return {};
  }
  f << text;
  f.flush();
  if (!f) {
    error = "could not write " + path.string();
    return {};
  }
  REXLOG_WARN("Skate 3: wrote diagnostic report to {} ({} bytes)", path.string(), text.size());
  return path;
}

}  // namespace skate3
