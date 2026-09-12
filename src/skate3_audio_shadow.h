/**
 * @file        skate3_audio_shadow.h
 * @brief       Differential verification for native audio replacements
 *
 * Replacing a guest audio function with native C++ is only worth doing if the native
 * version is provably equivalent. This runs both against the same state and diffs them.
 *
 * The lifted body stays reachable as `__imp__sub_XXXXXXXX` even when a strong
 * `sub_XXXXXXXX` overrides it, so both implementations can be executed on identical
 * inputs. Order matters for safety:
 *
 *   1. snapshot registers and the watched memory window
 *   2. run the LIFTED body -- this is authoritative, the game keeps its real result
 *   3. rewind to the snapshot, run the NATIVE body
 *   4. diff registers and memory
 *   5. restore the lifted result before returning
 *
 * So a wrong native implementation cannot corrupt the running game: it executes only
 * against a rewound copy, and its output is discarded after comparison. That makes it
 * safe to leave armed during ordinary play, which is the point -- the game generates far
 * better test inputs than anything hand-written, at 187.5 audio frames a second.
 */
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace skate3::audio {

/// Result of one comparison, accumulated per function.
struct ShadowStats {
  uint64_t runs = 0;
  uint64_t register_diffs = 0;
  uint64_t memory_diffs = 0;
};

/**
 * Run `native` against `lifted` on identical state and report any divergence.
 *
 * @param watch_addr  guest address of a memory window to compare, 0 for none
 * @param watch_len   bytes to compare; capped internally, this runs at audio rate
 * @param name        identifies the function in log output
 *
 * Returns true when the two agreed exactly. The context and watched memory are left
 * holding the lifted result either way.
 */
bool ShadowCompare(PPCContext& ctx, uint8_t* base, PPCFunc* native, PPCFunc* lifted,
                   uint32_t watch_addr, uint32_t watch_len, const char* name);

/// Whether shadow comparison is armed (cvar-backed, read once).
bool ShadowEnabled();

}  // namespace skate3::audio
