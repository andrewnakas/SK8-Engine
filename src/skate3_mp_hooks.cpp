#include "skate3_mp_hooks.h"

namespace skate3::mp {
namespace {

// Function-local so registration from another translation unit's static
// initialiser can't run before this is constructed.
Hooks& Table() {
  static Hooks hooks;
  return hooks;
}

void (*g_open_settings_page)() = nullptr;

}  // namespace

void SetOpenSettingsPage(void (*open)()) { g_open_settings_page = open; }

void OpenSettingsPage() {
  if (g_open_settings_page) {
    g_open_settings_page();
  }
}

void Register(const Hooks& hooks) { Table() = hooks; }

const Hooks& Get() { return Table(); }

}  // namespace skate3::mp
