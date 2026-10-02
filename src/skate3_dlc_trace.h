#pragma once

// Tracing for the guest's DLC content path.
//
// Ported from the SK8-Engine fork of this project (MIT, 2026), which built it
// to chase a content-manager problem of its own. Kept here because it points
// straight at where THIS project's stalling map packs die: a pack that never
// mounts leaves the guest parked in a file wait holding a handle, with nothing
// in the log to say which archive it was waiting on.
//
// Off unless `skate3_dlc_trace` is set - the observers log at WARN and a boot
// produces a lot of them.

namespace rex::runtime {
class FunctionDispatcher;
}

namespace skate3::dlc_trace {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);

}  // namespace skate3::dlc_trace
