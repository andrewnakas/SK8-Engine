#include "skate3_ios_maps.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, skate3_loader_levels);
REXCVAR_DECLARE(std::string, skate3_loader_level_worlds);
REXCVAR_DECLARE(std::string, skate3_loader_level_packs);
REXCVAR_DECLARE(std::string, skate3_loader_level_indices);
REXCVAR_DECLARE(std::string, skate3_warp_boot_world);

namespace skate3::ios_maps {
namespace {

// "DIST_DownTownSkatePark" -> "Down Town Skate Park". The picker shows these
// to a player, and the raw identifiers read like filenames.
std::string Prettify(const std::string& world) {
  std::string name = world;
  if (name.rfind("DIST_", 0) == 0) {
    name = name.substr(5);
  }
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (i > 0 && std::isupper(static_cast<unsigned char>(name[i])) &&
        !std::isupper(static_cast<unsigned char>(name[i - 1]))) {
      out.push_back(' ');
    }
    out.push_back(name[i]);
  }
  return out;
}

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out.push_back('|');
    out += parts[i];
  }
  return out;
}

}  // namespace

void PopulateFromGameFolder(const std::filesystem::path& game_root) {
  // A launcher that already provided a list knows more than this scan does -
  // which pack is staged, which entries belong to it - so never overwrite one.
  if (!REXCVAR_GET(skate3_loader_levels).empty()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::exists(game_root, ec)) {
    return;
  }

  std::set<std::string> worlds;  // sorted, and deduped across both sources

  // Stock districts ship as data/content/world<NAME>.big.
  const auto content = game_root / "data" / "content";
  for (const auto& entry : std::filesystem::directory_iterator(content, ec)) {
    if (ec || !entry.is_regular_file() || entry.path().extension() != ".big") {
      continue;
    }
    const std::string stem = entry.path().stem().string();
    if (stem.rfind("world", 0) != 0) {
      continue;
    }
    const std::string world = stem.substr(5);
    // "dmo" and "misc" are content archives, not places you can skate.
    if (world.rfind("DIST_", 0) == 0) {
      worlds.insert(world);
    }
  }

  // Custom packs are DIST_<NAME> directories in the game folder itself. The
  // folder is mounted as the guest's d: drive, so they need no staging beyond
  // being copied in.
  for (const auto& entry : std::filesystem::directory_iterator(game_root, ec)) {
    if (ec || !entry.is_directory()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind("DIST_", 0) == 0) {
      worlds.insert(name);
    }
  }

  if (worlds.empty()) {
    return;
  }

  std::vector<std::string> names, ids, packs, indices;
  int index = 0;
  for (const std::string& world : worlds) {
    names.push_back(Prettify(world));
    ids.push_back(world);
    // One synthetic pack: every entry is reachable from this boot, because the
    // whole game folder is mounted rather than a pack being installed.
    packs.push_back("ios");
    indices.push_back(std::to_string(index++));
  }

  REXCVAR_SET(skate3_loader_levels, Join(names));
  REXCVAR_SET(skate3_loader_level_worlds, Join(ids));
  REXCVAR_SET(skate3_loader_level_packs, Join(packs));
  REXCVAR_SET(skate3_loader_level_indices, Join(indices));
  REXLOG_INFO("skate3 maps: {} found in {} ({})", names.size(), game_root.string(), Join(ids));
}

bool RequestBootWorld(const std::string& world) {
  // The picker's usual path is to ask a launcher to restart the game on the
  // chosen map, because switching in-session does not currently land the right
  // world. A phone has no launcher and an app cannot relaunch itself, so the
  // choice is written where the next launch will read it and the player
  // reopens the app.
  //
  // Written into the same ios_args.txt the player already edits, so the
  // mechanism is one they can see and correct by hand.
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    return false;
  }
  const std::filesystem::path args =
      std::filesystem::path(home) / "Documents" / "user" / "ios_args.txt";

  std::vector<std::string> lines;
  {
    std::ifstream in(args);
    std::string line;
    while (std::getline(in, line)) {
      // Drop any previous choice; everything else the player wrote survives.
      if (line.rfind("skate3_warp_boot_world", 0) == 0 ||
          line.rfind("--skate3_warp_boot_world", 0) == 0) {
        continue;
      }
      lines.push_back(line);
    }
  }
  lines.push_back("skate3_warp_boot_world=" + world);

  std::error_code ec;
  std::filesystem::create_directories(args.parent_path(), ec);
  std::ofstream out(args, std::ios::trunc);
  if (!out) {
    REXLOG_WARN("skate3 maps: could not write {}", args.string());
    return false;
  }
  for (const std::string& line : lines) {
    out << line << '\n';
  }
  REXLOG_INFO("skate3 maps: next launch boots into '{}'", world);
  return true;
}

}  // namespace skate3::ios_maps
