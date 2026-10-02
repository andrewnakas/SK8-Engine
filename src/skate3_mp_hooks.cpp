#include "skate3_mp_hooks.h"

#include <mutex>

namespace skate3::mp {
namespace {

// Function-local so registration from another translation unit's static
// initialiser can't run before this is constructed.
Hooks& Table() {
  static Hooks hooks;
  return hooks;
}

void (*g_open_settings_page)() = nullptr;
void (*g_restart_on_pack)(const std::string&) = nullptr;
std::mutex g_pack_root_mutex;
std::string g_pack_root;

}  // namespace

void SetOpenSettingsPage(void (*open)()) { g_open_settings_page = open; }

void OpenSettingsPage() {
  if (g_open_settings_page) {
    g_open_settings_page();
  }
}

void SetRestartOnPack(void (*restart)(const std::string&)) { g_restart_on_pack = restart; }

void RestartOnPack(const std::string& pack) {
  if (g_restart_on_pack) {
    g_restart_on_pack(pack);
  }
}

void SetContentPackRoot(const std::string& root) {
  std::lock_guard lock(g_pack_root_mutex);
  g_pack_root = root;
}

std::string ContentPackRoot() {
  std::lock_guard lock(g_pack_root_mutex);
  return g_pack_root;
}

void Register(const Hooks& hooks) { Table() = hooks; }

const Hooks& Get() { return Table(); }

}  // namespace skate3::mp
