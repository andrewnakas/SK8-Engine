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
 *   1. snapshot registers and the watched memory windows
 *   2. run the LIFTED body -- this is authoritative, the game keeps its real result
 *   3. rewind to the snapshot, run the NATIVE body
 *   4. diff registers and memory
 *   5. restore the lifted result before returning
 *
 * So a wrong native implementation cannot corrupt the running game: it executes only
 * against a rewound copy, and its output is discarded after comparison. That makes it
 * safe to leave armed during ordinary play, which is the point -- the game generates far
 * better test inputs than anything hand-written, at 187.5 audio frames a second.
 *
 * What is compared, and why not everything:
 *
 *   registers  only what the PowerPC ABI obliges a callee to preserve -- r1, r2, r13-r31,
 *              f14-f31, v14-v31, v64-v127 and cr2-cr4 -- plus the return registers the
 *              caller names. The ranges follow the __savegprlr/__savefpr/__savevmx helpers
 *              the image actually uses. A whole-context compare reports a divergence on
 *              every call, because lifted bodies leave scratch values in volatile
 *              registers (EVENT_SUBMIT leaves r9-r11 and cr6) that native code has no
 *              reason to reproduce.
 *   memory     a list of windows. They must cover every byte the function writes: a write
 *              outside them keeps the lifted value while the native body runs, so the
 *              native body could read it back and agree for the wrong reason.
 *
 * Not compared: lr, ctr, xer, fpscr, msr and the reservation state.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <span>

#include <rex/ppc/context.h>

namespace skate3::audio {

/// A guest memory range the function writes.
struct ShadowWindow {
  uint32_t addr;
  uint32_t len;
};

/// Return registers to compare, since a void function leaves r3 as scratch.
enum ShadowReturn : uint32_t {
  kReturnNone = 0,
  kReturnR3 = 1u << 0,
  kReturnF1 = 1u << 1,
  kReturnV2 = 1u << 2,
};

/// Per-function counters. Keep one as a static beside the hook that owns it; constructing
/// it registers it with the reporter that logs running totals.
struct ShadowStats {
  explicit ShadowStats(const char* function_name);
  const char* name;
  std::atomic<uint64_t> runs{0};
  std::atomic<uint64_t> register_diffs{0};
  std::atomic<uint64_t> memory_diffs{0};
  std::atomic<uint64_t> reports{0};
  std::atomic<uint64_t> logged_runs{0};
};

/**
 * Run `native` against `lifted` on identical state and report any divergence.
 *
 * Returns true when the two agreed exactly. The context and watched memory are left
 * holding the lifted result either way. Logs a summary line per function at 1, 16, 256
 * and 4096 runs, and every 10 s from a reporter thread whenever the count has moved. The
 * reporter is what gets the final count of a function that stops being called into the
 * log, so a clean session leaves positive evidence rather than an absence of errors.
 */
bool ShadowCompare(PPCContext& ctx, uint8_t* base, PPCFunc* native, PPCFunc* lifted,
                   std::span<const ShadowWindow> windows,
                   std::span<const ShadowWindow> inputs, uint32_t returns,
                   ShadowStats& stats);

/// Why `inputs` exists, separately from `windows`.
///
/// `windows` is a specification of what a function **writes**: the harness snapshots them,
/// rewinds them and compares them. It is not a specification of what a function **reads**. A
/// consumer reads its command record to find the player; the producer reads the system pointer
/// and the ring base; `EVENT_PLAY` reads the source pointer. None of those are written, so none
/// appear in `windows`.
///
/// That distinction is invisible until someone replays a recorded vector outside the game, at
/// which point every vector fails on its first read rather than on a comparison. `inputs` is
/// the read set, recorded into the vector dump so the replay can reconstruct the memory the
/// function actually saw. Nothing else uses it: it is not snapshotted, rewound or compared.

/// Whether shadow comparison is armed (cvar-backed, read once).
bool ShadowEnabled();

}  // namespace skate3::audio
