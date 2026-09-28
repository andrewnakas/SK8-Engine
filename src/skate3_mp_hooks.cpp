#include "skate3_mp_hooks.h"

namespace skate3::mp {
namespace {

// Function-local so registration from another translation unit's static
// initialiser can't run before this is constructed.
Hooks& Table() {
  static Hooks hooks;
  return hooks;
}

}  // namespace

void Register(const Hooks& hooks) { Table() = hooks; }

const Hooks& Get() { return Table(); }

}  // namespace skate3::mp
