#pragma once
/**
 * @file        skate3_switch_bridge.h
 * @brief       Console-specific glue: clocks and memory reporting.
 *
 * The Android and iOS bridges exist to reach a system the game cannot call
 * directly - a document picker, a relaunch. Horizon has neither of those, so
 * this one is smaller and does the two things that platform genuinely offers:
 * it can be asked to run its clocks faster, and it will say exactly how much
 * memory the process is using.
 */

#include <cstdint>

namespace skate3::switch_bridge {

// Reads switch_overclock and applies it. Safe to call when the cvar is off, in
// which case it does nothing at all. Call after the arguments are parsed.
void ApplyClocks();

// Puts the clocks back. Called on the way out; the system would restore them
// anyway when the process dies, but not until then.
void RestoreClocks();

// Writes a line naming the used and total memory of this process's pool, no
// more often than the interval asked for. Cheap enough to call every frame.
void LogMemoryPeriodically();

}  // namespace skate3::switch_bridge
