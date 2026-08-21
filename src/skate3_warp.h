#pragma once

#include <string>

#include <rex/ppc/context.h>

// Direct level load ("warp"), and the probe used to find it.
//
// The macro path reaches a custom map by driving the pause menu with synthetic
// pad input, which costs ~36 s and loads the stock world first. This takes over
// the game's own world-load call instead. See the .cpp for the method.

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::warp {

// Point the menu rows at a DIFFERENT world without relaunching. Safe to call
// from any thread (the UI thread picks maps): it only records the request, and
// the repoint itself happens on the guest's main thread, which is where the
// menu machinery already runs.
void RetargetWorld(const std::string& world);

// Replay the menu confirm on the calling guest thread, for the boot flow.
// No-op unless skate3_warp_boot_confirm is set.
void BootConfirm(PPCContext& ctx, uint8_t* base);

// Installs the probe hooks (--skate3_warp_probe) and, once a target is known,
// the warp itself. No-op unless one of those cvars is set.
void Install(rex::runtime::FunctionDispatcher* dispatcher);

}  // namespace skate3::warp
