// Direct level load, and the probe that finds it.
//
// Two things live here because they are the same mechanism pointed at
// different ends of the job:
//
//   the PROBE hooks an arbitrary guest address named by a cvar, logs what it
//     was called with, and chains to the original. `FunctionDispatcher` can
//     retarget any guest address at runtime, so confirming a candidate found
//     in a trace costs a relaunch rather than a rebuild - which matters when
//     the candidate list is a dozen `sub_XXXXXXXX` from a call graph.
//
//   the WARP hooks the world-load call and substitutes the world to load.
//     Taking over a call the game makes itself, at the moment it makes it, is
//     what keeps this out of the cross-thread corruption this project already
//     spent days on: nothing is called from a state the game did not choose.

#include "skate3_warp.h"

#include "skate3_native_scene.h"

#include "generated/skate3_init.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>

#include <rex/kernel/guest_presence.h>

#include "skate3_guest_trace.h"

REXCVAR_DEFINE_STRING(skate3_warp_probe, "", "Skate 3",
                      "Comma-separated guest addresses (hex, e.g. 82791CF8,827A52C8) to "
                      "hook and log on every call: arguments, any string they point at, "
                      "the guest caller, and the thread. The tool for confirming a "
                      "world-load candidate found in a trace without a rebuild.");
REXCVAR_DEFINE_INT32(skate3_warp_probe_limit, 24, "Skate 3",
                     "Log at most this many calls per probed address, so a function on a "
                     "per-frame path cannot flood the log.")
    .range(1, 100000);
REXCVAR_DEFINE_BOOL(skate3_warp_probe_backtrace, false, "Skate 3",
                    "Probe: also log a guest-named host backtrace on the first call of "
                    "each probed address - that backtrace IS the guest call chain.");

REXCVAR_DEFINE_STRING(skate3_warp_world, "", "Skate 3",
                      "Boot straight into this world instead of the stock one, with no "
                      "menu navigation: the bare world id (e.g. sk8itspillway) or the "
                      "full folder name (DIST_sk8itspillway). Empty leaves the boot path "
                      "completely alone.");
REXCVAR_DEFINE_STRING(skate3_warp_boot_world, "DIST_University", "Skate 3",
                      "The world the game boots into unaided; the warp substitutes only "
                      "for requests naming this one, so nothing else is disturbed.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_node, true, "Skate 3",
                    "Redirect a LOCATION CHANGE to skate3_warp_world by substituting the "
                    "spawn-node name (sub_828646E0's r5, 'Z_<world>_<variant>_Start'). "
                    "Measured to be the only thing the identity travels on: the folder "
                    "name, the stream slug and the registry query all follow from it. "
                    "Any menu row then lands on the requested world.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_lookup, true, "Skate 3",
                    "Redirect the frontend's per-row node lookup (sub_82911068's r4) to "
                    "skate3_warp_world, so every Locations row resolves to that world's "
                    "start node and confirming any row is a change to it. The row keeps "
                    "the OBJECT this returns, which is why substituting the name later "
                    "(at the confirm) changes nothing.");
REXCVAR_DEFINE_BOOL(skate3_warp_trace_locid, false, "Skate 3",
                    "Log every ID_LOCATION_* the menu resolves, with its call index, "
                    "without substituting anything. Isolates whether the sub_8264ACF8 "
                    "override is survivable at all, and shows WHICH call is the confirm's "
                    "- the function is hot and general-purpose, so substituting every "
                    "call is not an option.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_locid, false, "Skate 3",
                    "Redirect the menu row's own identity key (sub_8264ACF8's r3, "
                    "\"ID_LOCATION_<WORLD>_...\"). This is what the confirm carries, "
                    "ahead of the node name and the location-table entry - substituting "
                    "either of those left the world unchanged. OFF by default: with it on the game died ~20 s into the macro, before the Locations list was even built, so this hook needs isolating before it is trusted.");
REXCVAR_DEFINE_BOOL(skate3_warp_replay, false, "Skate 3",
                    "Once gameplay is reached, REPLAY the call sequence the menu confirm "
                    "runs, instead of substituting a name into it. Eight name "
                    "substitutions across six namespaces all failed because the identity "
                    "is an object the menu picks, not any string - so hand the game the "
                    "same calls with the wanted location instead.");
REXCVAR_DEFINE_STRING(skate3_warp_replay_mgr, "469F05D0", "Skate 3",
                      "Guest address of the location manager the confirm passes as r3. A "
                      "singleton, the same across runs, but a cvar because nothing at boot "
                      "hands it to us to capture.");
REXCVAR_DEFINE_INT32(skate3_warp_replay_delay_frames, 600, "Skate 3",
                     "Frames of gameplay to let settle before replaying. Calling a loader "
                     "the instant presence flips is the same class of bug as the "
                     "cross-thread free this project already paid for.")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_loader, false, "Skate 3",
                    "THE DIRECT WARP. sub_828A3270 is the world loader's front door: it "
                    "takes a 64-bit world identity in r4, compares it with the current world "
                    "at [session+2192], and loads when they differ. Hand it the wanted "
                    "world's identity - the hash pair the menu item carries at +0x18/+0x1C - "
                    "and the game loads that world with no menu involved.");
REXCVAR_DEFINE_INT32(skate3_warp_trace_loader_limit, 4, "Skate 3",
                     "How many world-load backtraces to log. The boot load eats the first "
                     "few, so raise this to also capture the menu confirm's load - the one "
                     "that actually works, and therefore the one worth copying.")
    .range(1, 200);
REXCVAR_DEFINE_INT32(skate3_warp_induce_delay_frames, 0, "Skate 3",
                     "Frames of gameplay to wait before asking for the world. Zero: the "
                     "change goes as soon as presence says gameplay, which is as early as "
                     "the worker and the session are both known. The wait was insurance "
                     "against a half-settled stock world and costs 3.3 s of a 22 s run.")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(skate3_warp_request_world, false, "Skate 3",
                    "ASK for the world instead of loading it. The game keeps a global pair "
                    "at 0x830B7AE8 - +40 the world it is IN, +48 the world it WANTS - and "
                    "the whole load is driven by them disagreeing; sub_82864E10 is that "
                    "comparison and nothing else. Writing the wanted identity into +48 "
                    "leaves every decision after it to the game, which is the half "
                    "skate3_warp_induce_load skips: calling sub_828A3270 by hand loads a "
                    "world nothing else in the title believes it is going to.");
REXCVAR_DEFINE_BOOL(skate3_warp_induce_switch, false, "Skate 3",
                    "Run the map change the way the game runs it. Logging every job the "
                    "title's worker thread was handed through a real menu confirm showed a "
                    "world change is three jobs on the world manager - sub_828A3700 "
                    "(prepare), sub_828A36D0 (load, taking the world from [mgr+2200]), "
                    "sub_828A39C8 (enter, which spawns the player from the transform at "
                    "[mgr+2224]) - each submitted as the previous one finishes. Without "
                    "this, skate3_warp_request_world writes a request nothing polls.");
REXCVAR_DEFINE_BOOL(skate3_warp_at_boot, false, "Skate 3",
                    "Load the requested world INSTEAD of the stock one, rather than after "
                    "it. Two thirds of a warp is the stock world booting first, and the "
                    "boot switch (sub_826D95E8) takes the world to load from "
                    "[0x830B7AE8+48] - so writing the wanted identity there before it reads "
                    "it skips the stock load entirely. MEASURED: the identity IS resolvable "
                    "that early, contrary to the long-standing note that the menu items "
                    "carrying it only exist once the Locations list has been opened. The "
                    "player still has to be put at the start node, which is the spawn kick "
                    "on its own once gameplay is up - no second load.");
REXCVAR_DEFINE_STRING(skate3_warp_chain, "boot", "Skate 3",
                      "Which route runs the load: 'confirm' submits the three jobs a real "
                      "menu confirm's driver submits (sub_828A3700 / sub_828A36D0 / "
                      "sub_828A39C8), 'boot' submits the two boot task-list steps that "
                      "reach the same loader (sub_826DAAE8 / sub_826DAB18). MEASURED: both "
                      "commit the world, and only 'boot' renders it - the confirm's three "
                      "leave a black screen with the UI still drawing, so they are not the "
                      "whole of what the driver does.");
REXCVAR_DEFINE_INT32(skate3_warp_spawn_offset, 0x30, "Skate 3",
                     "Byte offset of the spawn POSITION inside the start node's object. "
                     "The node carries a 4x4 transform and the position is its last row, "
                     "but the row is worth being able to move without a rebuild: the "
                     "logged dump of the node object next to the position a real confirm "
                     "passes (C2BEDAA0 43B0113C C30FC1CB 3F800000 for DMJumpline) settles "
                     "it in one run.")
    .range(0, 240);
REXCVAR_DEFINE_BOOL(skate3_warp_watch_request, true, "Skate 3",
                    "Log the world-request pair whenever either half changes. Cheap (it "
                    "only speaks on a change) and it is the one instrument that says "
                    "whether a warp was ever ASKED for, as opposed to whether data moved.");
REXCVAR_DEFINE_BOOL(skate3_warp_trace_loader, false, "Skate 3",
                    "Log a host backtrace the first few times a world stream folder is "
                    "requested. Host frames name guest functions one-for-one, so this is "
                    "the call chain that INITIATES a world load - which the confirm replay "
                    "demonstrably is not.");
REXCVAR_DEFINE_BOOL(skate3_warp_boot_confirm, false, "Skate 3",
                    "Replay the menu confirm during BOOT, while the frontend is still up, "
                    "instead of after gameplay is reached. The confirm ends by closing the "
                    "top frontend screen (sub_82D0AC88); from gameplay there is no screen to "
                    "close and the location change is inert, which is why every earlier "
                    "replay did nothing. Needs skate3_warp_replay=true.");
REXCVAR_DEFINE_INT32(skate3_warp_replay_depth, 2, "Skate 3",
                     "How many calls of the confirm sequence to replay: 2 = the two "
                     "lookups (verified to return exactly what the real confirm gets), "
                     "3 = plus sub_82864C40. Each step is judged separately because none "
                     "of their semantics are known.")
    .range(2, 8);
REXCVAR_DEFINE_STRING(skate3_warp_replay_machine, "40141760", "Skate 3",
                      "Guest address of the state machine the confirm pushes the loading "
                      "state onto (r3 of sub_826DFB30).");
REXCVAR_DEFINE_STRING(skate3_warp_replay_state, "82707508", "Skate 3",
                      "The loading state's handler, a CODE address, passed as r4 of "
                      "sub_826DFB30. This is the call that actually starts the load; "
                      "everything before it only decides what gets loaded.");
REXCVAR_DEFINE_INT32(skate3_warp_replay_state_id, 0xB, "Skate 3",
                     "r5 of sub_82707508 - the state the confirm enters (0xB in the "
                     "trace). Depth 5 makes that call, which is the most trigger-shaped "
                     "thing in the confirm's tail.")
    .range(0, 255);
REXCVAR_DEFINE_INT32(skate3_warp_replay_steps, 15, "Skate 3",
                     "Bitmask over the faithful reconstruction (depth 6+): 1 = resolve "
                     "the location, 2 = apply the spawn transform, 4 = set the handler's "
                     "flag. All three together blacked the screen; the mask says which "
                     "one did it.")
    .range(0, 15);
REXCVAR_DEFINE_BOOL(skate3_warp_replay_in_menu, false, "Skate 3",
                    "Fire the replay without waiting for gameplay presence, so it can run "
                    "while the Locations list is open. The handler only sets a flag the "
                    "frontend consumes, so replaying it from gameplay - where no frontend "
                    "is listening - may be why it resolves and nothing loads.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_item, false, "Skate 3",
                    "Rewrite the selected menu ITEM's stored location entry ([item+0x24]) "
                    "rather than only the resolve's argument. The item is what later "
                    "stages re-derive the world from, which is why substituting names and "
                    "arguments all failed.");
REXCVAR_DEFINE_BOOL(skate3_warp_trace_ids, false, "Skate 3",
                    "Log the identity pair that follows the menu confirm: the name hash "
                    "(sub_82911068) and the streamer request that consumes two hashes "
                    "(sub_8247C5A0). For comparing one world's confirm against another's.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_folder, true, "Skate 3",
                    "Substitute the world FOLDER name (DIST_<world>, r5 of the two "
                    "stream-open entry points). Separable from the slug below purely to "
                    "bisect which of the two the game cannot survive: a warp that "
                    "substituted only the slug rendered, one that substituted both left "
                    "the guest submitting no draw records at all.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_slug, true, "Skate 3",
                    "Substitute the lowercase variant SLUG (dist_<world>_<variant>, r4 "
                    "of sub_8247DBE0), which names the actual stream XMLs.");
REXCVAR_DEFINE_BOOL(skate3_warp_substitute_query, false, "Skate 3",
                    "Also redirect the per-frame world-by-name QUERY (sub_828A42F0). "
                    "MEASURED: on its own it does not change the boot world at all, and "
                    "it is a prime suspect for the load never completing - the game asks "
                    "'is <world> ready?' ~60x/s and gets an answer about a different "
                    "world. Off unless bisecting.");
REXCVAR_DEFINE_INT32(skate3_warp_max_substitutions, 0, "Skate 3",
                     "Stop substituting after this many calls (0 = no limit). The boot "
                     "world is looked up many times in a burst; a limit is for bisecting "
                     "which of those calls actually decides the world.")
    .range(0, 10000);

// Defined at file scope further down, next to the world-stream entry points.
uint64_t WantedWorldIdentity(uint8_t* base);

namespace skate3::warp {
namespace {

// Same regions scripts/memfind.py names, and the same reason as in the trace:
// never dereference an arbitrary guest address speculatively.
bool ReadableGuest(uint32_t address) {
  return (address >= 0x00010000 && address < 0x40000000) ||
         (address >= 0x40000000 && address < 0x80000000) ||
         (address >= 0x82000000 && address < 0x84000000);
}

// Text at `address`, or "" - a whole printable, NUL-terminated run only, so a
// struct that happens to start with a letter does not read as a string.
std::string GuestString(const uint8_t* base, uint32_t address, size_t cap = 48) {
  if (base == nullptr || !ReadableGuest(address)) {
    return {};
  }
  const char* src = reinterpret_cast<const char*>(base + address);
  size_t n = 0;
  while (n < cap) {
    const char c = src[n];
    if (c == 0) {
      break;
    }
    if (c < 0x20 || c > 0x7E) {
      return {};
    }
    ++n;
  }
  return n >= 4 ? std::string(src, n) : std::string();
}

// One probed address: where to chain, and how much noise it is still allowed.
struct Probe {
  uint32_t address = 0;
  PPCFunc* original = nullptr;
  std::atomic<int> logged{0};
  std::atomic<bool> traced{false};
};

// A fixed slot array, not a vector: the thunks read it from guest threads, and
// a vector that reallocated mid-install would move the storage under them.
// Sixteen is far more than a candidate list ever needs.
constexpr size_t kMaxProbes = 16;
Probe* g_probe_slots[kMaxProbes] = {};
size_t g_probe_count = 0;

Probe* FindProbe(uint32_t guest_address) {
  for (size_t i = 0; i < g_probe_count; ++i) {
    if (g_probe_slots[i] != nullptr && g_probe_slots[i]->address == guest_address) {
      return g_probe_slots[i];
    }
  }
  return nullptr;
}

void LogProbeCall(Probe* probe, PPCContext& ctx, uint8_t* base) {
  const int n = probe->logged.fetch_add(1);
  if (n >= REXCVAR_GET(skate3_warp_probe_limit)) {
    return;
  }
  char args[256];
  std::snprintf(args, sizeof(args), "r3=%08X r4=%08X r5=%08X r6=%08X", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32);
  std::string text;
  const uint32_t regs[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  for (int i = 0; i < 4; ++i) {
    const std::string s = GuestString(base, regs[i]);
    if (!s.empty()) {
      text += (text.empty() ? "" : " ") + std::string("r") + char('3' + i) + "=\"" + s + "\"";
    }
  }
  uint32_t caller_offset = 0;
  const uint32_t caller =
      skate3::guest_trace::GuestFunctionAt(uint32_t(ctx.lr), &caller_offset);
  REXLOG_INFO("skate3 warp probe: sub_{:08X} call #{} {} {} <- sub_{:08X}+0x{:X}", probe->address,
              n + 1, args, text, caller, caller_offset);
  if (REXCVAR_GET(skate3_warp_probe_backtrace) && !probe->traced.exchange(true)) {
    skate3::guest_trace::LogHostBacktrace("probe");
  }
}

// The probe hook. A hook must be a plain function pointer and the dispatcher
// passes no context, so each slot gets its own instantiation and finds its
// probe by that slot index.
template <size_t Index>
void ProbeThunk(PPCContext& __restrict ctx, uint8_t* base) {
  Probe* probe = g_probe_slots[Index];
  if (probe == nullptr) {
    return;
  }
  LogProbeCall(probe, ctx, base);
  if (probe->original != nullptr) {
    probe->original(ctx, base);
  }
}

template <size_t... I>
constexpr std::array<PPCFunc*, sizeof...(I)> MakeThunks(std::index_sequence<I...>) {
  return {&ProbeThunk<I>...};
}

std::vector<uint32_t> ParseAddresses(const std::string& spec) {
  std::vector<uint32_t> out;
  size_t start = 0;
  while (start < spec.size()) {
    size_t end = spec.find(',', start);
    if (end == std::string::npos) {
      end = spec.size();
    }
    std::string token = spec.substr(start, end - start);
    // Tolerate spaces and an 0x prefix; the addresses get copied out of trace
    // output by hand and both show up.
    size_t b = token.find_first_not_of(" \t");
    size_t e = token.find_last_not_of(" \t");
    if (b != std::string::npos) {
      token = token.substr(b, e - b + 1);
      if (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) {
        token = token.substr(2);
      }
      if (token.rfind("sub_", 0) == 0) {
        token = token.substr(4);
      }
      const uint32_t address = uint32_t(std::strtoul(token.c_str(), nullptr, 16));
      if (address >= 0x82000000 && address < 0x84000000) {
        out.push_back(address);
      } else if (!token.empty()) {
        REXLOG_WARN("skate3 warp probe: '{}' is not a guest code address - ignored", token);
      }
    }
    start = end + 1;
  }
  return out;
}

// ---- the warp --------------------------------------------------------------
//
// Found by tracing two macro teleports (Barcelona and Spillway) and diffing
// them - see skate3loader/scripts/tracediff.py. The world identity travels as
// a STRING, which is why comparing raw argument values found nothing until the
// trace started resolving pointers to text:
//
//   sub_826494C8(menu, 0x0C, ...)      identical in both runs - 0x0C is the
//                                      menu action, not the world
//   sub_82864628(mgr, out, descriptor)  descriptor differs per location
//   sub_828646E0(mgr, out, "Z_<world>_<variant>_Start")   the spawn node
//   sub_828A42F0(registry, "DIST_<world>", ...)           the world folder
//
// The last one is the hijack point, because it is the one the game calls
// **at boot**, with "DIST_University". Substituting the name there means the
// game loads the wanted world through its own boot path, in the state it
// expects - no second load, no call issued from outside, and therefore none of
// the cross-thread exposure that a hand-made call would carry.
//
// It is a strong-symbol override rather than a dispatcher hook on purpose:
// generated code calls known targets DIRECTLY (`sub_X(ctx, base)`), so a
// function-table entry only catches indirect call sites. The probe above is
// fine with that - it is for discovery - but the warp has to see every call.

std::atomic<bool> g_warp_ready{false};      // a target was requested and resolved
std::atomic<bool> g_warp_logged{false};
std::atomic<int> g_substitutions{0};
uint32_t g_warp_name_address = 0;           // guest address of the replacement name
std::string g_warp_boot_name;               // the name being replaced

// Normalizes "sk8itspillway" and "DIST_sk8itspillway" to the folder form the
// game actually asks for.
std::string WorldFolderName(const std::string& requested) {
  if (requested.empty()) {
    return {};
  }
  if (requested.size() > 5 &&
      (requested.compare(0, 5, "DIST_") == 0 || requested.compare(0, 5, "dist_") == 0)) {
    return requested;
  }
  return "DIST_" + requested;
}

// Find an existing occurrence of `needle` in the guest heap.
//
// Deliberately NOT synthesising the string into scratch memory: the name the
// game wants is already in the DLC's own string table (the trace found both
// worlds' names within a few KB of each other), and pointing at a string the
// title already owns avoids inventing a lifetime the guest does not know about.
uint32_t FindGuestString(const uint8_t* base, const std::string& needle) {
  if (base == nullptr || needle.empty()) {
    return 0;
  }
  // The world-name tables observed in the traces live in the low heap; scanning
  // the whole 4 GiB view would be slow and would wander into regions the
  // runtime guards.
  constexpr uint32_t kLo = 0x40000000;
  constexpr uint32_t kHi = 0x48000000;
  // Match the trailing NUL too, so "DIST_sk8itschool" cannot match inside
  // "DIST_sk8itschoolyard".
  const size_t n = needle.size() + 1;
  const char* needle_z = needle.c_str();
  const uint8_t* start = base + kLo;
  const uint8_t* end = base + kHi - n;
  for (const uint8_t* p = start; p < end; ++p) {
    // memchr for the first byte, then compare - a plain byte-at-a-time scan of
    // 128 MB is slow enough to be noticeable at boot.
    p = static_cast<const uint8_t*>(std::memchr(p, needle_z[0], size_t(end - p)));
    if (p == nullptr) {
      break;
    }
    if (std::memcmp(p, needle_z, n) == 0) {
      return uint32_t(p - base);
    }
  }
  return 0;
}

// Same, but matching a PREFIX: the stream layer wants the full variant slug
// (`dist_sk8itspillway_spillwaydlc`), which the loader does not know - only the
// world id. Scanning for `dist_<world>_` finds whatever the pack actually
// registered, so nothing has to be guessed or spelled out per pack.
uint32_t FindGuestStringPrefix(const uint8_t* base, const std::string& prefix,
                               std::string* full) {
  if (base == nullptr || prefix.empty()) {
    return 0;
  }
  constexpr uint32_t kLo = 0x40000000;
  constexpr uint32_t kHi = 0x48000000;
  const size_t n = prefix.size();
  const uint8_t* start = base + kLo;
  const uint8_t* end = base + kHi - n - 1;
  for (const uint8_t* p = start; p < end; ++p) {
    p = static_cast<const uint8_t*>(std::memchr(p, prefix[0], size_t(end - p)));
    if (p == nullptr) {
      break;
    }
    if (std::memcmp(p, prefix.c_str(), n) != 0) {
      continue;
    }
    const uint32_t address = uint32_t(p - base);
    const std::string text = GuestString(base, address, 64);
    // Require a plausible whole string, so a match inside binary data is not
    // handed to the streamer as a path.
    if (text.size() >= n) {
      if (full != nullptr) {
        *full = text;
      }
      return address;
    }
  }
  return 0;
}

std::string ToLower(std::string s) {
  for (char& c : s) {
    c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  return s;
}

// Resolved once, on the first stream call that names the boot world. Held as
// plain values because the streaming calls below run on the load thread.
std::atomic<uint32_t> g_folder_address{0};  // "DIST_<world>"
std::atomic<uint32_t> g_slug_address{0};    // "dist_<world>_<variant>"
std::atomic<bool> g_stream_resolved{false};
std::atomic<int> g_stream_logged{0};

// Shared by the three stream entry points: if `reg` names the boot world,
// point it at the requested world instead. Returns the substituted address.
bool SubstituteStreamName(uint8_t* base, uint32_t* reg, bool slug, const char* where) {
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || base == nullptr) {
    return false;
  }
  if (!(slug ? REXCVAR_GET(skate3_warp_substitute_slug)
             : REXCVAR_GET(skate3_warp_substitute_folder))) {
    return false;
  }
  const std::string asked = GuestString(base, *reg, 64);
  if (asked.empty()) {
    return false;
  }
  const std::string boot_folder = g_warp_boot_name;
  const std::string boot_slug = ToLower(boot_folder);
  const bool matches = slug ? (ToLower(asked) == boot_slug) : (asked == boot_folder);
  if (!matches) {
    return false;
  }

  if (!g_stream_resolved.exchange(true)) {
    const std::string world = WorldFolderName(requested);          // DIST_x
    const std::string bare = world.substr(5);                      // x
    std::string full_slug;
    g_folder_address.store(FindGuestString(base, world), std::memory_order_relaxed);
    uint32_t slug_addr = FindGuestStringPrefix(base, "dist_" + ToLower(bare) + "_", &full_slug);
    if (slug_addr == 0) {
      slug_addr = FindGuestString(base, "dist_" + ToLower(bare));
    }
    g_slug_address.store(slug_addr, std::memory_order_relaxed);
    REXLOG_INFO("skate3 warp: resolved '{}' -> folder 0x{:08X}, slug 0x{:08X} '{}'", world,
                g_folder_address.load(), slug_addr, full_slug);
  }

  const uint32_t replacement =
      slug ? g_slug_address.load(std::memory_order_relaxed)
           : g_folder_address.load(std::memory_order_relaxed);
  if (replacement == 0) {
    if (g_stream_logged.fetch_add(1) == 0) {
      REXLOG_WARN(
          "skate3 warp: no guest string for the requested world - the pack may not be "
          "staged. Loading '{}' as normal.",
          asked);
    }
    return false;
  }
  *reg = replacement;
  if (g_stream_logged.fetch_add(1) < 8) {
    REXLOG_INFO("skate3 warp: {} '{}' -> 0x{:08X}", where, asked, replacement);
  }
  return true;
}

// ---- the spawn-node substitution -------------------------------------------
//
// MEASURED (macro trace, 2026-08-13): the world identity a location change
// travels on is the SPAWN NODE NAME, and nothing else. At the menu confirm the
// main thread calls
//
//   sub_828646E0(mgr=469F05D0, out, "Z_sk8itspillway_spillway_Start")
//
// and ~3.3 s later the load thread asks for "DIST_sk8itspillway" and the
// registry query flips to the new world - all derived from that one string.
// The menu's selected row only chooses which entry of the DLC's pointer table
// (0x41C88xxx, one `const char*` per location) is passed in.
//
// So substituting r5 here redirects a location change to any world, whatever
// the menu had selected. That is what makes a menu row we did not navigate to
// land on the map the loader actually wants.
std::atomic<uint32_t> g_node_address{0};   // the "Z_<world>_..._Start" string
std::atomic<uint32_t> g_slot_address{0};   // the table entry that POINTS at it
std::atomic<bool> g_node_resolved{false};
std::atomic<int> g_node_logged{0};

uint32_t LoadGuestU32BE(const uint8_t* base, uint32_t address) {
  if (base == nullptr || !ReadableGuest(address)) {
    return 0;
  }
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
         uint32_t(p[3]);
}

uint64_t LoadGuestU64BE(const uint8_t* base, uint32_t address) {
  return (uint64_t(LoadGuestU32BE(base, address)) << 32) |
         uint64_t(LoadGuestU32BE(base, address + 4));
}

void StoreGuestU64BE(uint8_t* base, uint32_t address, uint64_t value) {
  uint8_t* p = base + address;
  for (int i = 0; i < 8; ++i) {
    p[i] = uint8_t(value >> (56 - 8 * i));
  }
}

// ---- the world-request block ----------------------------------------------
//
// READ OUT OF THE CODE (not a trace): sub_826D95E8, the world-switch step,
// opens with
//
//     lis  r11,-31989
//     addi r31,r11,31464        -> 0x830B7AE8
//     ld   r11,48(r31)          REQUESTED world identity
//     ld   r10,40(r31)          CURRENT  world identity
//     cmpld / beq <return>      equal - nothing to do
//
// and sub_82864E10 is that comparison on its own, as a predicate returning
// "requested == current". So this one global pair, not the session field at
// +2192, is the game's own statement of "which world do I want, and which one
// am I in".
//
// The menu confirm's carrier writes the REQUESTED half: sub_828646E0 copies
// its node-name argument into +64 and then does `std r11,48(r31)` with the
// 64-bit identity its `out` argument holds. Boot writes the pair too -
// sub_826DA6C8 sets current = -1 and requested = the default world - which is
// exactly the FFFFFFFFFFFFFFFF the loader was first seen with.
//
// This is the state the induced load never had. Calling sub_828A3270 directly
// hands the loader a world while the block still says the stock one is both
// current and requested, so nothing else in the game agrees a change is
// happening.
constexpr uint32_t kWorldRequest = 0x830B7AE8;
constexpr uint32_t kWorldRequestCurrent = kWorldRequest + 40;
constexpr uint32_t kWorldRequestWanted = kWorldRequest + 48;
constexpr uint32_t kWorldRequestNode = kWorldRequest + 64;

// Resolve, once, the pair the location change needs: the requested world's
// start-node string AND the table slot that points at it. The string alone is
// not enough - the caller (sub_82864628) is handed the SLOT, and substituting
// only the string it later dereferences leaves the location unchanged
// (measured: the macro still landed on the row it had selected).
//
// The anchor is the slot the game just passed in. That IS an entry of the very
// table we want, so the requested world's entry is a few 4-byte steps away and
// no memory scan is needed. Scanning was tried first and is worse in both
// directions: the frontend keeps heap copies of these names, so a global search
// finds a string nothing dispatches on (measured - substituting it changed
// nothing), and proving a candidate is in the real table costs another scan.
void ResolveNodeSlot(uint8_t* base, uint32_t anchor_slot, const std::string& bare) {
  if (g_node_resolved.exchange(true)) {
    return;
  }
  const std::string prefix = "Z_" + bare + "_";
  // 512 entries either way: SkateIT's 14 maps span 0x90 bytes around the anchor,
  // and a pack with more locations still lands well inside this.
  constexpr int kReach = 512 * 4;
  for (int delta = 0; delta <= kReach; delta += 4) {
    for (const int sign : {1, -1}) {
      const uint32_t slot = uint32_t(int(anchor_slot) + sign * delta);
      const uint32_t node = LoadGuestU32BE(base, slot);
      const std::string text = GuestString(base, node, 64);
      if (text.size() <= prefix.size() ||
          text.compare(0, prefix.size(), prefix) != 0 ||
          text.compare(text.size() - 6, 6, "_Start") != 0) {
        continue;
      }
      g_node_address.store(node, std::memory_order_relaxed);
      g_slot_address.store(slot, std::memory_order_relaxed);
      REXLOG_INFO("skate3 warp: location '{}' node 0x{:08X}, table slot 0x{:08X}", text,
                  node, slot);
      return;
    }
  }
  REXLOG_WARN(
      "skate3 warp: no 'Z_{}_..._Start' entry within {} bytes of the location table "
      "at 0x{:08X} - leaving the location change alone",
      bare, kReach, anchor_slot);
}

// True when `text` is a start node naming some world other than the requested
// one - i.e. a location change we should redirect.
bool IsForeignStartNode(const std::string& text, const std::string& bare) {
  return text.size() > 8 && text.compare(0, 2, "Z_") == 0 &&
         text.compare(text.size() - 6, 6, "_Start") == 0 &&
         text.compare(2, bare.size(), bare) != 0;
}

// The requested world's start node, found NEAR an anchor string the game just
// handed us. Every location name lives in the same pack blob, so a local window
// beats a global scan twice over: it is fast enough to run on a guest thread,
// and it cannot pick up the frontend's heap copies, which are the ones nothing
// dispatches on.
uint32_t FindStringNear(const uint8_t* base, uint32_t anchor, const std::string& prefix,
                        std::string* full, const char* suffix = nullptr) {
  constexpr uint32_t kWindow = 0x20000;
  const uint32_t lo = anchor > kWindow ? anchor - kWindow : 0x40000000;
  const uint32_t hi = anchor + kWindow;
  if (!ReadableGuest(lo) || !ReadableGuest(hi)) {
    return 0;
  }
  const size_t suffix_len = suffix != nullptr ? std::strlen(suffix) : 0;
  for (uint32_t a = lo; a < hi; ++a) {
    if (base[a] != prefix[0]) {
      continue;
    }
    const std::string text = GuestString(base, a, 64);
    if (text.size() > prefix.size() && text.compare(0, prefix.size(), prefix) == 0 &&
        (suffix_len == 0 ||
         (text.size() >= suffix_len &&
          text.compare(text.size() - suffix_len, suffix_len, suffix) == 0))) {
      if (full != nullptr) {
        *full = text;
      }
      return a;
    }
  }
  return 0;
}

// r4 of sub_82911068: the node NAME the frontend is resolving into the object a
// menu row will keep.
bool SubstituteNodeLookup(uint8_t* base, uint32_t* reg) {
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || base == nullptr ||
      !REXCVAR_GET(skate3_warp_substitute_lookup)) {
    return false;
  }
  const std::string asked = GuestString(base, *reg, 64);
  const std::string bare = WorldFolderName(requested).substr(5);
  if (!IsForeignStartNode(asked, bare)) {
    return false;
  }
  static std::atomic<uint32_t> s_node{0};
  static std::atomic<bool> s_resolved{false};
  if (!s_resolved.exchange(true)) {
    std::string full;
    const uint32_t node = FindStringNear(base, *reg, "Z_" + bare + "_", &full, "_Start");
    s_node.store(node, std::memory_order_relaxed);
    if (node != 0) {
      REXLOG_INFO("skate3 warp: row node '{}' at 0x{:08X} (near 0x{:08X})", full, node,
                  *reg);
    } else {
      REXLOG_WARN("skate3 warp: no 'Z_{}_..._Start' near 0x{:08X}", bare, *reg);
    }
  }
  const uint32_t node = s_node.load(std::memory_order_relaxed);
  if (node == 0) {
    return false;
  }
  *reg = node;
  static std::atomic<int> s_logged{0};
  if (s_logged.fetch_add(1) < 4) {
    REXLOG_INFO("skate3 warp: row lookup '{}' -> 0x{:08X}", asked, node);
  }
  return true;
}

std::string ToUpper(std::string s) {
  for (char& c : s) {
    c = char(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
  }
  return s;
}

// r3 of sub_8264ACF8: "ID_LOCATION_SK8ITBARECLONA_BARCELONA" - the menu row's
// own identity key, seen at the confirm ahead of every name substituted above.
//
// Two things this had to learn the hard way. r3 is a STACK buffer (0x7018xxxx),
// not a pointer into the pack's data, so repointing it at a string in the DLC
// blob had the guest writing through it into read-only pack data - the game
// died ~20 s later, nowhere near the hook. The content is therefore rewritten
// IN PLACE, and only when the replacement is no longer than what is there.
//
// And the function is hot and general-purpose: the Locations list build resolves
// all 14 ids through it before the confirm re-resolves the selected one. Which
// call is the confirm's is not a fixed index, but it IS the second sighting of
// a given id - the list build sees each exactly once. So substitute on repeats
// only, which needs no threshold and calibrates itself.
std::mutex g_locid_mutex;
std::unordered_set<std::string> g_locid_seen;
std::string g_locid_target;

bool SubstituteLocationId(uint8_t* base, uint32_t* reg) {
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || base == nullptr ||
      !REXCVAR_GET(skate3_warp_substitute_locid)) {
    return false;
  }
  const std::string asked = GuestString(base, *reg, 64);
  static const std::string kPrefix = "ID_LOCATION_";
  if (asked.size() <= kPrefix.size() || asked.compare(0, kPrefix.size(), kPrefix) != 0) {
    return false;
  }
  const std::string ours = kPrefix + ToUpper(WorldFolderName(requested).substr(5)) + "_";
  std::string target;
  {
    std::lock_guard<std::mutex> lock(g_locid_mutex);
    if (asked.compare(0, std::min(ours.size(), asked.size()), ours) == 0) {
      // The requested world's own id, straight from the list build - no scan,
      // no guess at how the variant half is spelled.
      if (g_locid_target.empty()) {
        g_locid_target = asked;
        REXLOG_INFO("skate3 warp: location id '{}' learned", asked);
      }
      return false;
    }
    if (g_locid_seen.insert(asked).second) {
      return false;  // first sighting: the list build. Leave the row alone.
    }
    target = g_locid_target;
  }
  if (target.empty() || target.size() > asked.size()) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXLOG_WARN("skate3 warp: location id '{}' cannot replace '{}' in place",
                  target.empty() ? "<unseen>" : target, asked);
    }
    return false;
  }
  std::memcpy(base + *reg, target.c_str(), target.size() + 1);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) {
    REXLOG_INFO("skate3 warp: location id '{}' -> '{}' (in place at 0x{:08X})", asked,
                target, *reg);
  }
  return true;
}

void StoreGuestU32BE(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// Repoint EVERY menu item at the requested world.
//
// Validated by reading guest memory with the Locations list open: the items
// live in two parallel arrays (14 at stride 0x30, 16 at stride 0xB0), and each
// holds a pointer to its pack-table record at +0x24. The records themselves are
// 0xC bytes apart (41C8803E, 41C8804A, ...), and the confirm passes
// [item+0x24]+4 to the resolve - Spillway's 41C880CE against the observed
// 41C880D2 confirms it.
//
// Rewriting all of them is what makes a row we never navigated to load the
// world we want. The scan only touches words that ALREADY hold a pack-table
// pointer, so nothing else in the heap can be hit by it.
int PatchItemEntries(uint8_t* base, uint32_t slot) {
  const uint32_t entry = slot - 4;
  const uint32_t lo = slot > 0x400 ? slot - 0x400 : slot;
  const uint32_t hi = slot + 0x400;

  // Dumping the items showed the record at +0x24 is only one of FOUR per-world
  // fields, and not the decisive one:
  //
  //   +00 per-world object pointer   40B65660 (Barcelona) / 40B65A20 (Spillway)
  //   +18 hash                       0A92DE3F              / DD9CEEAE
  //   +1C hash                       CA9FEBB7              / D67B3C21
  //   +24 pack record                41C8803E              / 41C880CE
  //
  // +18 is the value the resolve was handed as a "handle" (0x0A92DE3F), so the
  // identity travels as a HASH. Copy the whole set from the wanted world's item
  // onto every other item of the same shape.
  uint32_t tmpl = 0;
  for (uint32_t a = 0x40000000; a < 0x48000000 - 4; a += 4) {
    if (LoadGuestU32BE(base, a) == entry) {
      tmpl = a - 0x24;
      break;
    }
  }
  if (tmpl == 0) {
    REXLOG_WARN(
        "skate3 warp: no menu item holds record 0x{:08X} - the Locations list may not "
        "be built yet, or this world is not in the installed pack. Rows unchanged.",
        entry);
    return 0;
  }
  const uint32_t t00 = LoadGuestU32BE(base, tmpl + 0x00);
  const uint32_t t04 = LoadGuestU32BE(base, tmpl + 0x04);
  const uint32_t t10 = LoadGuestU32BE(base, tmpl + 0x10);
  const uint32_t t18 = LoadGuestU32BE(base, tmpl + 0x18);
  const uint32_t t1C = LoadGuestU32BE(base, tmpl + 0x1C);
  REXLOG_INFO("skate3 warp: item template 0x{:08X} +00={:08X} +18={:08X} +1C={:08X}",
              tmpl, t00, t18, t1C);

  int patched = 0;
  int shaped = 0;
  for (uint32_t a = 0x40000000; a < 0x48000000 - 4; a += 4) {
    const uint32_t v = LoadGuestU32BE(base, a);
    if (v < lo || v >= hi || v == entry) {
      continue;
    }
    const uint32_t item = a - 0x24;
    // Shape check: the two item arrays have DIFFERENT layouts (the 0xB0 one
    // holds a hash at +00, not a pointer), and writing a pointer into that
    // would be a corruption. Only items matching the template's shape.
    if (LoadGuestU32BE(base, item + 0x04) != t04 ||
        LoadGuestU32BE(base, item + 0x10) != t10) {
      StoreGuestU32BE(base, a, entry);  // record only, safe on any layout
      ++patched;
      continue;
    }
    StoreGuestU32BE(base, item + 0x00, t00);
    StoreGuestU32BE(base, item + 0x18, t18);
    StoreGuestU32BE(base, item + 0x1C, t1C);
    StoreGuestU32BE(base, a, entry);
    ++patched;
    ++shaped;
  }
  // Verify rather than assume: re-read one patched item and say so. A silent
  // "patched N" that did not take would look identical to success in the log.
  if (shaped > 0) {
    REXLOG_INFO("skate3 warp: {} of {} items took the full field set (+00/+18/+1C)",
                shaped, patched);
  }
  return patched;
}

// r5 of sub_82864628: the ADDRESS OF the table entry, not the string.
// ---- replaying the confirm ------------------------------------------------
//
// The menu confirm is a SEQUENCE, and substituting any single name inside it
// changes nothing (eight tried). So run the sequence ourselves, once, after
// gameplay has settled:
//
//   sub_82864628(mgr, out,  &table_entry)   resolve the location
//   sub_828646E0(mgr, out,  "Z_<world>_<variant>_Start")
//
// Called from inside the per-frame registry query, which runs on the MAIN
// thread - the same thread the menu confirm runs on. That matters more than it
// looks: a loader entered from the wrong thread is exactly the cross-thread
// use-after-free this project already spent days on.
//
// The callee gets a COPY of the context with its own stack slack, so whatever
// it writes through `out` cannot land in our caller's frame.
// The selection the handler fetched most recently, so the resolve's log line
// can carry it: correlating two separate log lines by timestamp failed - the FE
// calls the selection fetch constantly for other menus, and the one that
// mattered was buried.
std::atomic<uint32_t> g_last_item{0};
std::atomic<uint32_t> g_last_entry{0};

// A retarget requested from the UI thread, applied on the main thread.
std::mutex g_retarget_mutex;
std::string g_retarget_world;
std::atomic<bool> g_retarget_pending{false};

std::atomic<bool> g_replay_done{false};
std::atomic<uint64_t> g_replay_frames{0};

uint32_t FindPointerToNear(const uint8_t* base, uint32_t target, uint32_t anchor) {
  const uint8_t needle[4] = {uint8_t(target >> 24), uint8_t(target >> 16),
                             uint8_t(target >> 8), uint8_t(target)};
  constexpr uint32_t kWindow = 0x20000;
  const uint32_t lo = anchor > kWindow ? anchor - kWindow : anchor;
  for (uint32_t a = lo; a < anchor + kWindow; ++a) {
    if (std::memcmp(base + a, needle, 4) == 0) {
      return a;
    }
  }
  return 0;
}

// The pack blob's own copy of the node name, not the frontend's heap copies:
// only the blob's is in the location table, and only the table entry is what
// sub_82864628 takes.
bool EqualsNoCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

uint32_t FindPackNode(const uint8_t* base, const std::string& bare, uint32_t* slot_out,
                      std::string* full) {
  const std::string prefix = "Z_" + bare + "_";
  constexpr uint32_t kPackLo = 0x41000000;
  constexpr uint32_t kPackHi = 0x43000000;
  for (uint32_t a = kPackLo; a < kPackHi; ++a) {
    if (base[a] != 'Z' || base[a + 1] != '_') {
      continue;
    }
    const std::string text = GuestString(base, a, 64);
    // CASE-INSENSITIVE, and this is the whole reason the warp never fired on a
    // community pack. World ids come from the `DIST_` folder name and keep its
    // capitalisation - "DMJumpline" - while the spawn nodes inside the archive
    // are lower case: Z_dmjumpline_startline_Start. Built literally, the prefix
    // "Z_DMJumpline_" matches nothing, FindPackNode returns 0, the identity
    // lookup gives up before its heap scan, and the substitution is skipped
    // because it is gated on a non-zero identity.
    //
    // Every note about this warp was written against SkateIT, whose ids are
    // already lower case ("sk8itbareclona" -> Z_sk8itbareclona_barcelona_Start),
    // so the mismatch never showed.
    //
    // Insensitive but still LITERAL: no normalisation. Stripping separators
    // makes "San Vanelona" match "san_vanelona_artgallery", which is a
    // different location - the loader learned that one the same way.
    if (text.size() <= prefix.size() ||
        !EqualsNoCase(text.substr(0, prefix.size()), prefix) ||
        text.compare(text.size() - 6, 6, "_Start") != 0) {
      continue;
    }
    const uint32_t slot = FindPointerToNear(base, a, a);
    if (slot == 0) {
      continue;
    }
    if (slot_out != nullptr) *slot_out = slot;
    if (full != nullptr) *full = text;
    return a;
  }
  return 0;
}

// Fire the confirm replay from an arbitrary guest hook. The replay is gated on
// the FE being up for the boot case, so it needs a trigger that runs there -
// the gameplay-loop query it normally rides does not tick during the frontend.
void ForceReplayFrom(PPCContext& ctx, uint8_t* base);

void MaybeReplayLocationChange(PPCContext& ctx, uint8_t* base, bool force = false) {
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || base == nullptr || !REXCVAR_GET(skate3_warp_replay) ||
      g_replay_done.load(std::memory_order_relaxed)) {
    return;
  }
  // The confirm's mechanism only resolves and flags; the LOAD is driven by the
  // frontend reacting to that flag. So the replay may need the FE up (the
  // Locations list open) rather than plain gameplay - which is a different
  // moment entirely, and one where presence reads 0.
  if (!REXCVAR_GET(skate3_warp_replay_in_menu) &&
      rex::kernel::guest_presence::GameplayContextValue() != 1) {
    return;
  }
  // Two tick sources, because the two moments live on different loops: the
  // registry query paces the gameplay case, but it STOPS while the frontend is
  // up, so the in-menu case is fired directly (force) from the menu's own id
  // resolution instead of waiting for a frame count that never arrives.
  if (!force && g_replay_frames.fetch_add(1) <
                    uint64_t(REXCVAR_GET(skate3_warp_replay_delay_frames))) {
    return;
  }
  if (g_replay_done.exchange(true)) {
    return;
  }

  const std::string bare = WorldFolderName(requested).substr(5);
  uint32_t slot = 0;
  std::string full;
  const uint32_t node = FindPackNode(base, bare, &slot, &full);
  if (node == 0 || slot == 0) {
    REXLOG_WARN("skate3 warp replay: no pack node for '{}' - nothing to replay", bare);
    return;
  }
  const uint32_t mgr =
      uint32_t(std::strtoul(REXCVAR_GET(skate3_warp_replay_mgr).c_str(), nullptr, 16));
  if (!ReadableGuest(mgr)) {
    REXLOG_WARN("skate3 warp replay: manager 0x{:08X} is not readable", mgr);
    return;
  }
  REXLOG_INFO("skate3 warp replay: '{}' node 0x{:08X} slot 0x{:08X} mgr 0x{:08X}", full,
              node, slot, mgr);

  // Scratch well below the live frame, mirroring what the confirm's own locals
  // looked like in the trace (out, then out+0x10).
  PPCContext call = ctx;
  call.r1.u64 = uint64_t(ctx.r1.u32 - 0x1000);
  const uint32_t out = ctx.r1.u32 - 0x800;
  std::memset(base + out, 0, 0x80);

  call.r3.u64 = mgr;
  call.r4.u64 = out;
  call.r5.u64 = slot;
  __imp__sub_82864628(call, base);
  REXLOG_INFO("skate3 warp replay: sub_82864628 returned r3={:08X}", call.r3.u32);

  call.r3.u64 = mgr;
  call.r4.u64 = out;
  call.r5.u64 = node;
  __imp__sub_828646E0(call, base);
  REXLOG_INFO("skate3 warp replay: sub_828646E0 returned r3={:08X}", call.r3.u32);

  // Both lookups return exactly what the real confirm's do (43991CB0 for the
  // same arguments), so the resolve half is faithful and only what the caller
  // does NEXT is missing. In the trace that is sub_82864C40(out+0x10, 1,
  // out+0x10) - r3 and r5 the same address, one past the lookups' out. Depth
  // is a cvar so each added call can be judged on its own.
  const int32_t depth = REXCVAR_GET(skate3_warp_replay_depth);
  // 3/4/5 were built on misread traces; depth 6 is the faithful
  // reconstruction and must not drag them along.
  if (depth == 3 || depth == 4 || depth == 5) {
    call.r3.u64 = out + 0x10;
    call.r4.u64 = 1;
    call.r5.u64 = out + 0x10;
    __imp__sub_82864C40(call, base);
    REXLOG_INFO("skate3 warp replay: sub_82864C40 returned r3={:08X}", call.r3.u32);
  }

  // The resolve calls are faithful but INERT - nothing loads. In the trace the
  // load starts ~12 ms later with the state machine being pushed a state whose
  // handler is a CODE address: sub_826DFB30(40141760, 82707508, 1). That is the
  // trigger; everything above only decides what the trigger will load.
  if (depth == 4 || depth == 5) {
    const uint32_t machine =
        uint32_t(std::strtoul(REXCVAR_GET(skate3_warp_replay_machine).c_str(), nullptr, 16));
    const uint32_t state =
        uint32_t(std::strtoul(REXCVAR_GET(skate3_warp_replay_state).c_str(), nullptr, 16));
    if (!ReadableGuest(machine)) {
      REXLOG_WARN("skate3 warp replay: state machine 0x{:08X} not readable", machine);
      return;
    }
    call.r3.u64 = machine;
    call.r4.u64 = state;
    call.r5.u64 = 1;
    REXLOG_INFO("skate3 warp replay: pushing state 0x{:08X} on 0x{:08X}", state, machine);
    __imp__sub_826DFB30(call, base);
    REXLOG_INFO("skate3 warp replay: sub_826DFB30 returned r3={:08X}", call.r3.u32);
  }

  // Depth 4 was built on a misreading. Logging sub_826DFB30 during a real
  // confirm showed it is a REGISTRATION called constantly through boot
  // (r3=401786A0, a different handler in r4 each time) - not a trigger, which
  // is why it returned 1 and did nothing. In the same trace 82707508 is not
  // data at all: it is CALLED, as sub_82707508(40141760, 0, 0xB), one of a
  // cluster of calls on that object (82707320(...,1,0x100C0),
  // 82707BE0(...,0,1)). r5=0xB reads as the state being entered.
  if (depth == 5) {
    const uint32_t machine =
        uint32_t(std::strtoul(REXCVAR_GET(skate3_warp_replay_machine).c_str(), nullptr, 16));
    const uint32_t state_id = uint32_t(REXCVAR_GET(skate3_warp_replay_state_id));
    if (!ReadableGuest(machine)) {
      REXLOG_WARN("skate3 warp replay: state machine 0x{:08X} not readable", machine);
      return;
    }
    call.r3.u64 = machine;
    call.r4.u64 = 0;
    call.r5.u64 = state_id;
    REXLOG_INFO("skate3 warp replay: sub_82707508(0x{:08X}, 0, 0x{:X})", machine, state_id);
    __imp__sub_82707508(call, base);
    REXLOG_INFO("skate3 warp replay: sub_82707508 returned r3={:08X}", call.r3.u32);
  }
  // Depth 6 is the first version built from the handler's CODE rather than from
  // first-call traces. sub_826494C8 is only 182 lines, and reading it found two
  // outright defects above: sub_82864628 takes FIVE arguments (r6=0, r7=1 - the
  // earlier replay left them as whatever the hook's context held), and
  // sub_82864C40 is handed the node's 4x4 MATRIX, copied out of the node object,
  // not the zeroed scratch it was being given. There is also a flag byte the
  // handler stores unconditionally near the end.
  if (depth >= 6) {
    const int32_t steps = REXCVAR_GET(skate3_warp_replay_steps);
    const uint32_t matrix = out + 0x40;
    std::memset(base + matrix, 0, 0x40);

    // 1. resolve the location - the virtual call at sub_826494C8+0xA0.
    call.r3.u64 = mgr;
    call.r4.u64 = out;
    call.r5.u64 = slot;
    call.r6.u64 = 0;
    call.r7.u64 = 1;
    if (steps & 1) {
      __imp__sub_82864628(call, base);
    }

    // 2. the node object, from the registry the handler reads out of a global:
    //    r3 = [[r9+15308]+168] with r9 = -2096627712.
    const uint32_t registry_ptr = uint32_t(int32_t(-2096627712) + 15308);
    const uint32_t registry = LoadGuestU32BE(base, LoadGuestU32BE(base, registry_ptr) + 168);
    call.r3.u64 = registry;
    call.r4.u64 = node;
    __imp__sub_82911068(call, base);
    const uint32_t node_obj = call.r3.u32;
    REXLOG_INFO("skate3 warp replay: registry 0x{:08X} node object 0x{:08X}", registry,
                node_obj);
    if (!ReadableGuest(node_obj)) {
      REXLOG_WARN("skate3 warp replay: node object not readable - stopping here");
      return;
    }

    // 3. the spawn transform: four 16-byte rows, exactly the lvx block.
    std::memcpy(base + matrix, base + node_obj, 0x40);

    // 4. apply it: sub_82864C40(matrix, 1, matrix).
    call.r3.u64 = matrix;
    call.r4.u64 = 1;
    call.r5.u64 = matrix;
    if (steps & 2) {
      __imp__sub_82864C40(call, base);
    }
    REXLOG_INFO("skate3 warp replay: sub_82864C40(matrix) returned r3={:08X}",
                call.r3.u32);

    // 4b. THE TWO CALLS EVERY EARLIER REPLAY MISSED.
    //
    // Reading sub_826494C8 line by line (not the trace) shows the handler does
    // more between the matrix apply and the flag:
    //
    //     bl 0x824ad240      r3 = <singleton>      (accessor; allocates on first use)
    //     bl 0x82d0ac88      <singleton>->Commit()
    //
    // sub_82D0AC88 walks a begin/end pair at +16/+20 with a stride of 20 - the
    // frontend PUSH-STATE STACK - and stores 1 into [top entry + 52]. That is
    // the "close the menus" half of the confirm, i.e. the part that makes the
    // location change actually take effect rather than just being recorded.
    //
    // Every previous replay stopped at the flag and was inert. This is the most
    // likely reason.
    if (steps & 8) {
      __imp__sub_824AD240(call, base);
      const uint32_t singleton = call.r3.u32;
      REXLOG_INFO("skate3 warp replay: sub_824AD240 -> 0x{:08X}", singleton);
      if (ReadableGuest(singleton)) {
        __imp__sub_82D0AC88(call, base);
        REXLOG_INFO("skate3 warp replay: sub_82D0AC88(0x{:08X}) done", singleton);
      } else {
        REXLOG_WARN("skate3 warp replay: singleton 0x{:08X} not readable", singleton);
      }
    }

    // 5. the flag the handler always sets: [[-2096300032-540]+1665] = 1.
    const uint32_t flag_ptr = uint32_t(int32_t(-2096300032) + -540);
    const uint32_t flag_obj = LoadGuestU32BE(base, flag_ptr);
    if ((steps & 4) && ReadableGuest(flag_obj)) {
      base[flag_obj + 1665] = 1;
      REXLOG_INFO("skate3 warp replay: set flag [0x{:08X}+0x681] = 1", flag_obj);
    }
  }
}

// `out` is the handler's local: out[0] = the selected ITEM, out[1] = its entry.
// Read statically out of sub_82B6CCD8, which fills them as
// item = sub_82B69CE8(...); [out+4] = [item + 0x24]. That last word is the
// item->world link, and it is why substituting the resolve's ARGUMENT never
// moved the world: the load re-derives the world from the item afterwards, and
// the item still pointed at its own row. Rewrite the item's stored entry
// instead, so every later re-derivation follows.
bool SubstituteLocationSlot(uint8_t* base, uint32_t* reg, uint32_t out) {
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || base == nullptr ||
      !REXCVAR_GET(skate3_warp_substitute_node)) {
    return false;
  }
  const std::string asked = GuestString(base, LoadGuestU32BE(base, *reg), 64);
  const std::string bare = WorldFolderName(requested).substr(5);
  if (!IsForeignStartNode(asked, bare)) {
    return false;
  }
  ResolveNodeSlot(base, *reg, bare);
  const uint32_t slot = g_slot_address.load(std::memory_order_relaxed);
  if (slot == 0) {
    return false;
  }
  *reg = slot;
  // The handler passes r5 = entry + 4, so the item's entry field wants slot - 4.
  const uint32_t item = LoadGuestU32BE(base, out);
  // MEASURED: by the time the resolve runs, sub_8254E8F8 has replaced the
  // (item, entry) pair with a HANDLE - the word here read as 0x0A92DE3F, not a
  // pointer, and writing through it was a wild store into low guest memory.
  // Heap range only, and off by default until the real item can be reached.
  const bool item_ok = item >= 0x40000000 && item < 0x48000000;
  if (REXCVAR_GET(skate3_warp_substitute_item) && item_ok) {
    const uint32_t was = LoadGuestU32BE(base, item + 0x24);
    StoreGuestU32BE(base, item + 0x24, slot - 4);
    if (g_node_logged.load() < 4) {
      REXLOG_INFO("skate3 warp: item 0x{:08X} entry 0x{:08X} -> 0x{:08X}", item, was,
                  slot - 4);
    }
  }
  if (g_node_logged.fetch_add(1) < 4) {
    REXLOG_INFO("skate3 warp: location slot '{}' -> 0x{:08X}", asked, slot);
  }
  return true;
}

// r5 of sub_828646E0: the string itself. Substituted too, so both halves of the
// menu's confirm agree even if the caller kept its own copy of the name.
bool SubstituteSpawnNode(uint8_t* base, uint32_t* reg) {
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || base == nullptr ||
      !REXCVAR_GET(skate3_warp_substitute_node)) {
    return false;
  }
  const std::string asked = GuestString(base, *reg, 64);
  const std::string bare = WorldFolderName(requested).substr(5);
  if (!IsForeignStartNode(asked, bare)) {
    return false;
  }
  // No resolution from here: this runs AFTER sub_82864628, which already
  // resolved against the real table. Without that anchor there is nothing
  // trustworthy to resolve from.
  const uint32_t node = g_node_address.load(std::memory_order_relaxed);
  if (node == 0) {
    return false;
  }
  *reg = node;
  if (g_node_logged.fetch_add(1) < 8) {
    REXLOG_INFO("skate3 warp: spawn node '{}' -> 0x{:08X}", asked, node);
  }
  return true;
}


void ForceReplayFrom(PPCContext& ctx, uint8_t* base) {
  MaybeReplayLocationChange(ctx, base, /*force=*/true);
}

std::atomic<uint32_t> g_loader_session{0};
// The world-switch JOB, captured from the game's own submission at boot: the
// worker to hand it to, the function pointer, and its argument. Nothing else
// hands these to us, and all three are needed to submit the job again.
std::atomic<uint32_t> g_switch_worker{0};
std::atomic<uint32_t> g_switch_arg{0};
// The chain after the switch. Boot submits step 16 (sub_826DAB18 ->
// sub_826D9AC0) 28 ms after the switch step returns, every time; a switch
// submitted from gameplay gets no such follow-up, and the game then sits
// silent with the new world loaded and nothing running it. So run the rest of
// the list ourselves, one step at a time, each only once the worker is idle -
// the worker holds ONE job and clears the pair after calling it, so a step
// queued from inside the previous step would be wiped.
std::atomic<int> g_chain_step{-1};
// Set when the boot switch was handed the wanted world instead of the stock
// one. The world is then already right by the time gameplay starts, and all
// that is left is to put the player at the start node.
std::atomic<bool> g_boot_substituted{false};

// THE MAP CHANGE, as the game does it.
//
// Logging every job the title's worker thread was handed through a real menu
// confirm showed the change is three jobs on the world manager, each submitted
// as the previous one finishes:
//
//   sub_828A3700(mgr)  -> sub_828A3538(mgr)                     prepare
//   sub_828A36D0(mgr)  -> sub_828A3270(mgr, [mgr+2200])         load
//   sub_828A39C8(mgr)  -> reads the spawn transform at
//                         [mgr+2224] under [mgr+2336] and
//                         calls sub_828A3708                    enter/spawn
//
// Submitting those three by hand does work - it lands in the right world - but
// it is the tail of the change, and a world entered that way came up with no
// ramp and untextured ground. They are submitted BY something: sub_828A3928
// sets the spawn transform, sets the state word at [mgr+2208] to 3, and
// registers a callback pair on the object at [0x83083BCC+17140], which is the
// driver that then runs them. So the whole map change is that one call, plus
// the world to load in [mgr+2200].
//
// Boot takes a different route entirely: a six-step task list whose fourth
// step (sub_826D95E8, reached through the vtable thunk sub_826DAAE8) reaches
// the same loader. It is a good place to CAPTURE the worker and the manager,
// and a poor one to copy.
constexpr uint32_t kConfirmChain[] = {0x828A3700, 0x828A36D0, 0x828A39C8};
constexpr const char* kConfirmName[] = {"prepare", "load", "enter"};
constexpr uint32_t kBootChain[] = {0x826DAAE8, 0x826DAB18};
constexpr const char* kBootName[] = {"switch", "finish"};
constexpr uint32_t kSessionPendingWorld = 2200;

std::atomic<bool> g_requested{false};
std::atomic<bool> g_request_logged{false};
std::atomic<uint64_t> g_request_frames{0};

// ASK for the world, then run the map change the game runs.
//
// Two earlier shapes are worth keeping in mind because each got exactly half of
// this. Substituting the identity on the BOOT load reached the loader with the
// right world and produced 249 meshes of 3988 and a black screen - that load
// runs while the frontend still owns the session. Calling sub_828A3270 by hand
// from gameplay streamed the world (3671 meshes) and left the game in the old
// one, because the loader is the ENGINE of a map change and not the change:
// worse, it never returns, so it parked whichever guest thread called it and
// silently killed every instrument sequenced after it.
//
// What the game actually does is below: fill in the pending world and let its
// own worker run prepare / load / enter.
void MaybeRequestWorld(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(skate3_warp_request_world) || base == nullptr ||
      g_requested.load(std::memory_order_relaxed)) {
    return;
  }
  if (rex::kernel::guest_presence::GameplayContextValue() != 1) {
    return;
  }
  if (g_request_frames.fetch_add(1) <
      uint64_t(REXCVAR_GET(skate3_warp_induce_delay_frames))) {
    return;
  }
  const uint64_t wanted = ::WantedWorldIdentity(base);
  if (wanted == 0) {
    if (!g_request_logged.exchange(true)) {
      REXLOG_WARN("skate3 warp request: no identity for '{}' - see the warp identity lines",
                  REXCVAR_GET(skate3_warp_world));
    }
    return;
  }
  if (g_requested.exchange(true)) {
    return;
  }
  const uint64_t current = LoadGuestU64BE(base, kWorldRequestCurrent);
  const uint64_t was = LoadGuestU64BE(base, kWorldRequestWanted);

  // The name too: sub_828646E0 copies its node argument into +64 before it
  // writes the identity, so writing one without the other would leave the block
  // describing two different worlds.
  uint32_t slot = 0;
  std::string full;
  const std::string bare = WorldFolderName(REXCVAR_GET(skate3_warp_world)).substr(5);
  const uint32_t node = FindPackNode(base, bare, &slot, &full);
  if (!full.empty() && full.size() < 60) {
    std::memcpy(base + kWorldRequestNode, full.c_str(), full.size() + 1);
  }
  StoreGuestU64BE(base, kWorldRequestWanted, wanted);
  REXLOG_INFO("skate3 warp request: wanted {:016X} -> {:016X} (current {:016X}, node '{}')",
              was, wanted, current, full);

  if (!REXCVAR_GET(skate3_warp_induce_switch)) {
    return;
  }
  const uint32_t session = g_loader_session.load(std::memory_order_relaxed);
  if (session == 0 || g_switch_worker.load(std::memory_order_relaxed) == 0) {
    REXLOG_WARN("skate3 warp switch: no session (0x{:08X}) or no worker was seen at boot - "
                "the request stands unread",
                session);
    return;
  }
  // The world to load travels HERE, not in the global block: sub_828A36D0, the
  // load job, is `r4 = [mgr+2200]; sub_828A3270(mgr, r4)`. The block is the
  // game's own record of where it is and where it is going, kept up to date on
  // the way; this field is the argument. MEASURED at a real confirm: by the
  // time the map change is kicked off, both already hold the new world.
  const bool already_there = LoadGuestU64BE(base, session + 2192) == wanted;
  const uint64_t pending = LoadGuestU64BE(base, session + kSessionPendingWorld);
  StoreGuestU64BE(base, session + kSessionPendingWorld, wanted);
  REXLOG_INFO("skate3 warp switch: [session 0x{:08X} + 2200] {:016X} -> {:016X}{}", session,
              pending, wanted,
              already_there ? " (the boot load already brought this world - spawn only)"
                            : "");

  // THE KICK. A host backtrace through a real menu confirm ends at
  //
  //     sub_827080A0+0x55F:  sub_828A3928(mgr, r31+96, 0x821D2770, 1)
  //
  // and sub_828A3928 stores that vector at [mgr+2224], sets [mgr+2208] = 3 and
  // registers a callback pair on the object at [0x83083BCC+17140]. THAT driver
  // submits prepare / load / enter to the worker - the three jobs this file
  // used to submit by hand. Calling the kick instead of the jobs means the
  // whole change is the game's, in its order, on its threads.
  //
  // r31+96 is the frontend's own copy of the spawn position, which no route
  // without a frontend fills in, so take it from the start NODE - the same
  // node the identity came from.
  const uint32_t registry_ptr = uint32_t(int32_t(-2096627712) + 15308);
  const uint32_t registry = LoadGuestU32BE(base, LoadGuestU32BE(base, registry_ptr) + 168);
  PPCContext call = ctx;
  call.r1.u64 = uint64_t(ctx.r1.u32 - 0x1000);
  call.r3.u64 = registry;
  call.r4.u64 = node;
  __imp__sub_82911068(call, base);
  const uint32_t node_obj = call.r3.u32;
  if (!ReadableGuest(node_obj)) {
    REXLOG_WARN("skate3 warp switch: node object for '{}' not readable (registry 0x{:08X}) - "
                "no spawn position, so no kick",
                full, registry);
    return;
  }
  // The whole head of the node object, so the position's offset can be READ off
  // a run rather than guessed at: a real confirm passes C2BEDAA0 43B0113C
  // C30FC1CB 3F800000 for DMJumpline, and whichever row that is, is the row.
  for (uint32_t row = 0; row < 0x40; row += 16) {
    REXLOG_INFO("skate3 warp switch: node 0x{:08X}+0x{:02X}: {:08X} {:08X} {:08X} {:08X}",
                node_obj, row, LoadGuestU32BE(base, node_obj + row),
                LoadGuestU32BE(base, node_obj + row + 4),
                LoadGuestU32BE(base, node_obj + row + 8),
                LoadGuestU32BE(base, node_obj + row + 12));
  }
  // 16-byte aligned: the setter reads it with lvx, which masks the address.
  const uint32_t vec = (ctx.r1.u32 - 0x900) & ~uint32_t(0xF);
  std::memcpy(base + vec, base + node_obj + uint32_t(REXCVAR_GET(skate3_warp_spawn_offset)),
              16);
  // Nothing to drop and nothing to load when the boot load already brought this
  // world: the renderer's caches are the arriving world's own, and a second
  // load of a world the session is already in is the one case sub_828A3270
  // skips anyway. All that is missing is the player.
  if (!already_there) {
  // Drop the outgoing map's cached state FIRST, the way entering the loading
  // presence context does. This route never enters that context, and without
  // the drop the renderer keeps item cores and prewarm records keyed by guest
  // arena addresses the new world is about to reuse - which showed up as
  // texture decodes whose fetch words were file-path TEXT, and as a world that
  // arrived with untextured ground and no ramp.
    skate3::native_scene::BeginInducedMapChange();
  }
  REXLOG_INFO("skate3 warp switch: kicking sub_828A3928(mgr=0x{:08X}, vec=0x{:08X} "
              "[{:08X} {:08X} {:08X} {:08X}])",
              session, vec, LoadGuestU32BE(base, vec), LoadGuestU32BE(base, vec + 4),
              LoadGuestU32BE(base, vec + 8), LoadGuestU32BE(base, vec + 12));
  call.r3.u64 = session;
  call.r4.u64 = vec;
  call.r5.u64 = 0x821D2770;
  call.r6.u64 = 1;
  __imp__sub_828A3928(call, base);
  // MEASURED: the kick on its own registers the spawn and NOTHING ELSE happens
  // - not one job is submitted afterwards. Its driver is only run by the
  // frontend state machine closing (the backtrace above runs sub_826DFB30 ->
  // sub_827080A0 -> here), and there is no frontend on this route. So submit
  // the three jobs too, in the order the confirm's driver submits them.
  //
  // Order matters, and is why submitting them ALONE gave a black screen: the
  // third job SPAWNS the player from [mgr+2224], and only the kick fills that
  // in. Jobs without the kick spawn nowhere; the kick without jobs teleports
  // into the world that is still loaded - a fall through a blue void.
  if (already_there) {
    REXLOG_INFO("skate3 warp switch: kicked - the world is already loaded, so that is all");
    return;
  }
  g_chain_step.store(0, std::memory_order_relaxed);
  REXLOG_INFO("skate3 warp switch: kicked - now running prepare / load / enter");
}

// Hand one job to the captured worker, if it is idle. Returns false when it is
// busy, so the caller simply tries again next frame - the worker holds ONE job
// and clears the pair only after running it, so submitting over a pending job
// would drop it silently.
bool SubmitToSwitchWorker(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t arg,
                          const char* what) {
  const uint32_t worker = g_switch_worker.load(std::memory_order_relaxed);
  if (worker == 0 || LoadGuestU32BE(base, worker + 56) != 0) {
    return false;
  }
  REXLOG_INFO("skate3 warp switch: {} - job 0x{:08X}(0x{:08X}) to worker 0x{:08X}", what, fn,
              arg, worker);
  PPCContext call = ctx;
  call.r1.u64 = uint64_t(ctx.r1.u32 - 0x1000);
  call.r3.u64 = worker;
  call.r4.u64 = fn;
  call.r5.u64 = arg;
  __imp__sub_82966C88(call, base);
  return true;
}

// Walk the three jobs from the main thread, one per idle worker.
void AdvanceWorldTask(PPCContext& ctx, uint8_t* base) {
  const int step = g_chain_step.load(std::memory_order_relaxed);
  if (base == nullptr || step < 0) {
    return;
  }
  const uint32_t session = g_loader_session.load(std::memory_order_relaxed);
  const uint32_t worker = g_switch_worker.load(std::memory_order_relaxed);
  const bool boot = REXCVAR_GET(skate3_warp_chain) != "confirm";
  const uint32_t* chain = boot ? kBootChain : kConfirmChain;
  const char* const* names = boot ? kBootName : kConfirmName;
  const int count = boot ? 2 : 3;
  // The boot steps take the TASK, not the manager; the confirm's take the
  // manager. Handing either the other's argument is a wild pointer.
  const uint32_t arg = boot ? g_switch_arg.load(std::memory_order_relaxed) : session;
  if (step >= count) {
    // Arm the takeover HERE, at the far end. The gate re-arms on a LOADING
    // presence context, which the menu route passes through and this one never
    // does - measured: not one presence line for the whole change. Arming any
    // earlier is spent on the outgoing world, which is still fully resident:
    // both an arm at submit time and an arm at the kick took over within
    // milliseconds, on 569 and 787 of the old world's items.
    if (LoadGuestU32BE(base, worker + 56) == 0) {
      g_chain_step.store(-1, std::memory_order_relaxed);
      REXLOG_INFO("skate3 warp switch: the change is done - current world {:016X}",
                  LoadGuestU64BE(base, session + 2192));
      skate3::native_scene::ArmTakeoverForInducedLoad();
    }
    return;
  }
  if (SubmitToSwitchWorker(ctx, base, chain[step], arg, names[step])) {
    g_chain_step.store(step + 1, std::memory_order_relaxed);
  }
}

// The world-request pair, watched.
//
// Every metric this project has for a warp - mesh counts, takeovers, prewarm -
// measures whether DATA moved. None of them says whether the game was ever
// asked to change world. These two words do, and they are the loader's own
// condition, so a run where they never diverge cannot have loaded anything no
// matter what else the log says.
void WatchWorldRequest(uint8_t* base) {
  if (!REXCVAR_GET(skate3_warp_watch_request) || base == nullptr) {
    return;
  }
  static std::atomic<uint64_t> s_current{0};
  static std::atomic<uint64_t> s_wanted{0};
  const uint64_t current = LoadGuestU64BE(base, kWorldRequestCurrent);
  const uint64_t wanted = LoadGuestU64BE(base, kWorldRequestWanted);
  const uint64_t was_current = s_current.exchange(current, std::memory_order_relaxed);
  const uint64_t was_wanted = s_wanted.exchange(wanted, std::memory_order_relaxed);
  if (current != was_current || wanted != was_wanted) {
    REXLOG_INFO("skate3 warp block: current {:016X} -> {:016X}  wanted {:016X} -> {:016X}",
                was_current, current, was_wanted, wanted);
  }
}

}  // namespace

void Install(rex::runtime::FunctionDispatcher* dispatcher) {
  // Cached here, on the app thread, so the guest-thread hook never reads the
  // cvar's string storage.
  g_warp_boot_name = REXCVAR_GET(skate3_warp_boot_world);
  if (!REXCVAR_GET(skate3_warp_world).empty()) {
    REXLOG_INFO("skate3 warp: requested world '{}' (replacing '{}')",
                REXCVAR_GET(skate3_warp_world), g_warp_boot_name);
  }

  if (dispatcher == nullptr) {
    return;
  }
  const std::vector<uint32_t> addresses = ParseAddresses(REXCVAR_GET(skate3_warp_probe));
  if (addresses.empty()) {
    return;
  }
  static constexpr auto kThunks = MakeThunks(std::make_index_sequence<kMaxProbes>{});

  for (uint32_t address : addresses) {
    if (g_probe_count >= kMaxProbes) {
      REXLOG_WARN("skate3 warp probe: at most {} addresses; ignoring the rest", kMaxProbes);
      break;
    }
    if (FindProbe(address) != nullptr) {
      continue;
    }
    // Take the current target FIRST: that is what the hook has to chain to,
    // and it may itself be another feature's hook.
    PPCFunc* original = dispatcher->GetFunction(address);
    if (original == nullptr) {
      REXLOG_WARN("skate3 warp probe: sub_{:08X} is not in the function table - skipped",
                  address);
      continue;
    }
    auto* probe = new Probe();
    probe->address = address;
    probe->original = original;
    const size_t index = g_probe_count;
    // Published before the hook is installed, so a thunk can never observe an
    // empty slot.
    g_probe_slots[index] = probe;
    g_probe_count = index + 1;
    if (!dispatcher->SetFunction(address, kThunks[index])) {
      REXLOG_WARN("skate3 warp probe: could not hook sub_{:08X}", address);
      continue;
    }
    REXLOG_INFO("skate3 warp probe: watching sub_{:08X}", address);
  }
}


// Replay the menu confirm right now, on the calling guest thread.
//
// The point of firing it from the BOOT FLOW rather than from gameplay: the
// confirm's last act is sub_82D0AC88, which closes the top frontend screen.
// From gameplay there is no frontend to close, so the location change is
// recorded and nothing consumes it. During boot the frontend IS up, and the
// world load that follows it is the one we want pointed at our map.
void BootConfirm(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(skate3_warp_boot_confirm)) {
    return;
  }
  ForceReplayFrom(ctx, base);
}

}  // namespace skate3::warp

// ---- the world stream entry points ----------------------------------------
//
// These are the choke point BOTH paths go through - the boot load and the menu
// teleport call the same three functions with the same objects, differing only
// in the world they name:
//
//   boot       sub_82477C40(43AF2010, "data/content/world/stream", "DIST_University")
//   teleport   sub_82477C40(43AF2010, "data/content/world/stream", "DIST_sk8itbareclona")
//
// They are STRONG-SYMBOL overrides, not dispatcher hooks: probing them through
// the function table caught nothing at all, because generated code calls known
// targets directly and the table only serves indirect call sites.
//
// r5 carries the folder name (DIST_x); sub_8247DBE0's r4 carries the lowercase
// variant slug (dist_x_ydlc), which names the actual stream files.


// ---------------------------------------------------------------------------
// THE WORLD LOADER
//
// Found by taking a host backtrace at the stream-folder call on a run that
// really did load the requested map (skate3_warp_trace_loader):
//
//   load_thread -> ... -> sub_826D95E8 -> sub_828A3270 -> sub_82476AC8
//                                                      -> sub_82477C40 (open)
//
// sub_828A3270 opens with:
//
//     ld   r11,2192(r3)        current world
//     cmpld cr6,r4,r11         requested world
//     beq  <skip>              already there - do nothing
//
// So r4 is the world IDENTITY and it is SIXTY-FOUR BITS - which is exactly the
// pair of 32-bit hashes the menu item carries at +0x18/+0x1C (Spillway
// DD9CEEAE/D67B3C21). Everything else in this file has been chasing the two
// halves of this number separately.
//
// Substituting r4 here is the direct warp: the game asks to load a world, and
// it is handed ours instead. No menu, no macro, no confirm.
// ---------------------------------------------------------------------------

// The wanted world's 64-bit identity, read out of its own menu item. Zero until
// the Locations data is in memory.
uint64_t WantedWorldIdentity(uint8_t* base) {
  const std::string& want = REXCVAR_GET(skate3_warp_world);
  if (want.empty()) {
    return 0;
  }
  // Every failure here used to return a bare 0, which reaches the caller as
  // "wanted=0000000000000000" and says nothing about WHICH step failed. There
  // are three, and they fail for different reasons: the pack's location table
  // may not be found, the table may be found but no menu item points at it
  // (the Locations list is built when the menu is first opened, so before that
  // no item exists at all), or the item may be there with an empty pair.
  uint32_t slot = 0;
  std::string full;
  const std::string folder = skate3::warp::WorldFolderName(want).substr(5);
  const uint32_t node = skate3::warp::FindPackNode(base, folder, &slot, &full);
  if (node == 0 || slot == 0) {
    REXLOG_WARN("skate3 warp identity: no pack node for '{}' (folder '{}') - "
                "node=0x{:08X} slot=0x{:08X}",
                want, folder, node, slot);
    return 0;
  }
  REXLOG_INFO("skate3 warp identity: '{}' -> node 0x{:08X} slot 0x{:08X} full '{}'",
              want, node, slot, full);

  // The item whose +0x24 points at this world's pack record carries the pair.
  const uint32_t entry = slot - 4;
  uint32_t hits = 0;
  for (uint32_t a = 0x40000000; a < 0x48000000 - 4; a += 4) {
    if (skate3::warp::LoadGuestU32BE(base, a) == entry) {
      const uint32_t item = a - 0x24;
      const uint64_t hi = skate3::warp::LoadGuestU32BE(base, item + 0x18);
      const uint64_t lo = skate3::warp::LoadGuestU32BE(base, item + 0x1C);
      const uint64_t identity = (hi << 32) | lo;
      REXLOG_INFO("skate3 warp identity: entry 0x{:08X} referenced at 0x{:08X} "
                  "-> item 0x{:08X} pair {:08X}:{:08X}",
                  entry, a, item, static_cast<uint32_t>(hi), static_cast<uint32_t>(lo));
      if (identity != 0) {
        return identity;
      }
      ++hits;
    }
  }
  REXLOG_WARN("skate3 warp identity: entry 0x{:08X} for '{}' is referenced by {} "
              "heap words, none of which carried a non-zero pair at +0x18/+0x1C. "
              "If this is 0, the Locations list has not been built yet - the items "
              "that carry the identity only exist once the menu has been opened.",
              entry, want, hits);
  return 0;
}

std::atomic<bool> g_loader_substituted{false};

// THE MAP-CHANGE ENTRY POINTS, logged.
//
// The three worker jobs are the tail of the change, not its head: something
// copies the request into [mgr+2200], sets a state word at [mgr+2208], and
// registers a callback pair on the object at [0x83083BCC+17140], and THAT is
// what submits them. Four functions do that -
//
//   sub_8271C1C0(r3, r4)   state 1, no spawn transform
//   sub_8271C2D0(r3, r4)   state 3 via sub_828A3928, with a transform read
//                          out of [[r3+88]+40..48]
//   sub_8271C910 / sub_8271C9E8   the same pair again
//
// - and rather than guess which one the menu takes, log all four plus the
// spawn setter and drive the menu once.
extern "C" REX_FUNC(sub_8271C1C0) {
  REXLOG_INFO("skate3 warp entry: sub_8271C1C0(r3=0x{:08X}, r4={})", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8271C1C0(ctx, base);
}

extern "C" REX_FUNC(sub_8271C2D0) {
  REXLOG_INFO("skate3 warp entry: sub_8271C2D0(r3=0x{:08X}, r4={})", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8271C2D0(ctx, base);
}

extern "C" REX_FUNC(sub_8271C910) {
  REXLOG_INFO("skate3 warp entry: sub_8271C910(r3=0x{:08X}, r4={})", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8271C910(ctx, base);
}

extern "C" REX_FUNC(sub_8271C9E8) {
  REXLOG_INFO("skate3 warp entry: sub_8271C9E8(r3=0x{:08X}, r4={})", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8271C9E8(ctx, base);
}

// The spawn setter: [mgr+2224] = *r4 (a position), [mgr+2240] = r5,
// [mgr+2244] = r6, [mgr+2248] = [mgr+2252] = 255, then state 3.
extern "C" REX_FUNC(sub_828A3928) {
  const uint32_t v = ctx.r4.u32;
  REXLOG_INFO("skate3 warp entry: sub_828A3928(mgr=0x{:08X}, vec=0x{:08X} [{:08X} {:08X} "
              "{:08X} {:08X}], r5=0x{:08X}, r6={})",
              ctx.r3.u32, v, skate3::warp::LoadGuestU32BE(base, v),
              skate3::warp::LoadGuestU32BE(base, v + 4),
              skate3::warp::LoadGuestU32BE(base, v + 8),
              skate3::warp::LoadGuestU32BE(base, v + 12), ctx.r5.u32, ctx.r6.u32);
  REXLOG_INFO("skate3 warp entry: at that moment [mgr+2200] = {:016X}, block wanted {:016X}",
              skate3::warp::LoadGuestU64BE(base, ctx.r3.u32 + 2200),
              skate3::warp::LoadGuestU64BE(base, 0x830B7AE8 + 48));
  skate3::guest_trace::LogHostBacktrace("map-change sub_828A3928");
  __imp__sub_828A3928(ctx, base);
}

// THE JOB SUBMITTER.
//
//     sub_82966C88(worker, fn, arg):  [worker+56] = fn
//                                     [worker+60] = arg
//                                     SetEvent([worker+48])
//
// and the worker's loop (sub_82966BB8) waits on that event, calls fn(arg),
// clears the pair, and notifies. Boot submits the world switch through here, so
// this is where the three values a warp needs are handed over for free - there
// is no other way to learn which worker owns the switch.
// The six ordered steps of the world task, by thunk and by target.
bool IsWorldTaskStep(uint32_t fn) {
  switch (fn) {
    case 0x826DAA20: case 0x826D8A28:
    case 0x826DAA88: case 0x826D9040:
    case 0x826DAAB8: case 0x826D9470:
    case 0x826DAB18: case 0x826D9AC0:
    case 0x826DAB48: case 0x826DA0D8:
      return true;
    default:
      return false;
  }
}

extern "C" REX_FUNC(sub_82966C88) {
  const uint32_t worker = ctx.r3.u32, fn = ctx.r4.u32, arg = ctx.r5.u32;
  // The thunk is one instruction - `b 0x826d95e8` - so both addresses are the
  // world switch. Match either: which one boot submits is not worth assuming.
  if (fn == 0x826DAAE8 || fn == 0x826D95E8) {
    skate3::warp::g_switch_worker.store(worker, std::memory_order_relaxed);
    skate3::warp::g_switch_arg.store(arg, std::memory_order_relaxed);
    REXLOG_INFO("skate3 warp switch: captured the world-switch job - worker 0x{:08X} "
                "fn 0x{:08X} arg 0x{:08X}",
                worker, fn, arg);
  } else if (IsWorldTaskStep(fn)) {
    // The switch's SIBLINGS. sub_826DAA20/AA88/AAB8/AAE8/AB18/AB48 are six
    // one-instruction thunks in a row, each paired with a setter storing 3, 6,
    // 10, 13, 16, 22 into [task+68] - a progress code, so they are the ordered
    // steps of one job and the switch is the fourth. Running only the fourth
    // loads the world and leaves the game silent, which is what the two steps
    // AFTER it are presumably for. Logged unconditionally: the order boot
    // submits them in is the recipe.
    REXLOG_INFO("skate3 warp job: world-task step fn 0x{:08X} arg 0x{:08X} on worker 0x{:08X}",
                fn, arg, worker);
  } else if (REXCVAR_GET(skate3_warp_trace_loader)) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 40) {
      REXLOG_INFO("skate3 warp job: sub_82966C88(worker=0x{:08X}, fn=0x{:08X}, arg=0x{:08X})",
                  worker, fn, arg);
    }
  }
  __imp__sub_82966C88(ctx, base);
}

// THE WORLD-SWITCH STEP, one frame above the loader.
//
// Read out of the code: it opens by comparing the world-request block's wanted
// (+48) and current (+40) halves and RETURNS IMMEDIATELY when they agree. Only
// when they differ does it resolve the wanted identity to a world record, fall
// back to a default if that fails, and finally call sub_828A3270. It is reached
// through sub_826DAAE8, a one-instruction tail-call thunk that lives in a
// vtable - i.e. it is a STATE the game enters, not a function anything calls by
// name, which is why hunting for the load's caller kept ending at boot.
//
// Logged rather than driven: what is worth knowing is whether asking (see
// skate3_warp_request_world) makes the game come here on its own.
extern "C" REX_FUNC(sub_826D95E8) {
  const uint64_t wanted = skate3::warp::LoadGuestU64BE(base, 0x830B7AE8 + 48);
  const uint64_t current = skate3::warp::LoadGuestU64BE(base, 0x830B7AE8 + 40);
  REXLOG_INFO("skate3 warp switch: sub_826D95E8 entered on task 0x{:08X} - wanted {:016X} "
              "current {:016X}{}",
              ctx.r3.u32, wanted, current,
              wanted == current ? " (equal: it will do nothing)" : "");
  // MEASURED: writing the wanted half from gameplay changes nothing - this
  // function is never entered again, so nobody POLLS the pair. It is a step in
  // a task list (its neighbours sub_826D8A28 / sub_826D9040 / sub_826D9470 /
  // sub_826D9AC0 / sub_826DA0D8 each pair with a setter storing 3, 6, 10, 13,
  // 16, 22 into [task+68] - a progress code), reached through the vtable thunk
  // sub_826DAAE8. So the missing half is whatever RUNS that list, and a host
  // backtrace names it: generated code compiles one host function per guest
  // function, so the frames above are guest functions by name.
  if (REXCVAR_GET(skate3_warp_trace_loader)) {
    static std::atomic<int> remaining{4};
    if (remaining.fetch_sub(1, std::memory_order_relaxed) > 0) {
      skate3::guest_trace::LogHostBacktrace("world-switch sub_826D95E8");
    }
  }
  // Can the requested world be named THIS early? Two thirds of the 17.5 s a
  // warp takes is the stock world booting first, and the only way to skip it is
  // to hand this call the wanted world instead - which needs the identity and
  // the start node to already be resolvable before any world has loaded. The
  // identity comes out of a menu item, and the note this file has carried for
  // months is that those items only exist once the Locations list has been
  // opened. Ask, once, rather than assume.
  if (REXCVAR_GET(skate3_warp_at_boot) && !REXCVAR_GET(skate3_warp_world).empty() &&
      !skate3::warp::g_boot_substituted.load(std::memory_order_relaxed)) {
    const uint64_t early = ::WantedWorldIdentity(base);
    if (early == 0) {
      REXLOG_WARN("skate3 warp boot: the wanted identity does not resolve yet - booting the "
                  "stock world and switching after it");
    } else if (early != current) {
      skate3::warp::StoreGuestU64BE(base, 0x830B7AE8 + 48, early);
      skate3::warp::g_boot_substituted.store(true, std::memory_order_relaxed);
      REXLOG_INFO("skate3 warp boot: the stock world is skipped - loading {:016X} instead "
                  "of {:016X}",
                  early, wanted);
    }
  }
  __imp__sub_826D95E8(ctx, base);
  REXLOG_INFO("skate3 warp switch: sub_826D95E8 returned - wanted {:016X} current {:016X}",
              skate3::warp::LoadGuestU64BE(base, 0x830B7AE8 + 48),
              skate3::warp::LoadGuestU64BE(base, 0x830B7AE8 + 40));
  // The far end of the load, and the only moment the new world is certainly
  // there: this returns once sub_828A3270's completion loop has finished. The
  // menu route re-arms the takeover by passing through a LOADING presence
  // context; a switch submitted from gameplay never changes presence at all
  // (measured: not one presence line during the whole switch), so the arm has
  // to be made here or the new world is rendered by nobody.
}

extern "C" REX_FUNC(sub_828A3270) {
  // The session is the loader's own first argument, and there is no other way
  // to get it - so remember it here for the induced load below, which needs the
  // same session but runs from a completely different call site.
  skate3::warp::g_loader_session.store(ctx.r3.u32, std::memory_order_relaxed);
  if (REXCVAR_GET(skate3_warp_substitute_loader) && !g_loader_substituted.load()) {
    const uint64_t current = skate3::warp::LoadGuestU64BE(base, ctx.r3.u32 + 2192);
    const uint64_t requested = ctx.r4.u64;
    const uint64_t wanted = WantedWorldIdentity(base);
    REXLOG_INFO("skate3 warp loader: session 0x{:08X} current={:016X} requested={:016X} "
                "wanted={:016X}",
                ctx.r3.u32, current, requested, wanted);
    if (wanted != 0 && requested != wanted) {
      ctx.r4.u64 = wanted;
      g_loader_substituted.store(true);
      REXLOG_INFO("skate3 warp loader: SUBSTITUTED the world to load -> {:016X}", wanted);
    }
  }
  __imp__sub_828A3270(ctx, base);
}

// Who actually asks for a world?
//
// The replay is now a faithful copy of the menu confirm and still initiates no
// load, so the loader is somewhere else entirely. sub_82477C40 is the call that
// receives the stream FOLDER ("DIST_University"), i.e. it is downstream of
// whatever decided to load a world - so its caller chain IS the world-load path.
// The generated code compiles one host function per guest function, so a host
// backtrace here names guest functions directly.
void MaybeTraceWorldLoadCallers(const char* tag) {
  if (!REXCVAR_GET(skate3_warp_trace_loader)) {
    return;
  }
  // Four was enough to see the BOOT world load and nothing else - the boot
  // load consumes every slot before the menu is ever opened, so the confirm's
  // own chain (the one that matters, because it is the load that works) was
  // never captured. A cvar so both can be taken in one run and diffed.
  static std::atomic<int> remaining{REXCVAR_GET(skate3_warp_trace_loader_limit)};
  if (remaining.fetch_sub(1, std::memory_order_relaxed) <= 0) {
    return;
  }
  skate3::guest_trace::LogHostBacktrace(tag);
}

extern "C" REX_FUNC(sub_82477C40) {
  using namespace skate3::warp;
  MaybeTraceWorldLoadCallers("world-load sub_82477C40");
  SubstituteStreamName(base, &ctx.r5.u32, /*slug=*/false, "stream dir");
  __imp__sub_82477C40(ctx, base);
}

extern "C" REX_FUNC(sub_8247AD78) {
  using namespace skate3::warp;
  MaybeTraceWorldLoadCallers("world-load sub_8247AD78");
  SubstituteStreamName(base, &ctx.r5.u32, /*slug=*/false, "stream dir");
  __imp__sub_8247AD78(ctx, base);
}

// ---- identity tracing ------------------------------------------------------
//
// Substituting the node name and the table entry both failed to move the world,
// so the identity leaves the menu by some other path. These two log the pair
// that follows in the trace: a hash of a name, and the streamer request that
// takes two hashes. Behind a cvar, and they only log - the point is to compare
// a Barcelona confirm with a Spillway one.
// cNodeRegistry::Find(registry=432EC7D0, const char* node, 0x10) -> node object.
// Called once per location while the frontend BUILDS the Locations list, and the
// row keeps the object it returns. Substituting the NAME here means every row
// resolves to the requested world's start node, so confirming ANY row - even
// without navigating - is a location change to that world. This is the one
// point in the chain that hands the identity over as an object rather than a
// name; the two later ones (the table entry and the node name at the confirm)
// were both substituted and both left the world unchanged.
extern "C" REX_FUNC(sub_82911068) {
  using namespace skate3::warp;
  SubstituteNodeLookup(base, &ctx.r4.u32);
  if (REXCVAR_GET(skate3_warp_trace_ids)) {
    static std::atomic<int> logged{0};
    const std::string name = GuestString(base, ctx.r4.u32, 64);
    if (!name.empty() && logged.fetch_add(1) < 16) {
      const uint32_t r3 = ctx.r3.u32, r5 = ctx.r5.u32;
      __imp__sub_82911068(ctx, base);
      REXLOG_INFO("skate3 warp ids: hash('{}') r3={:08X} r5={:08X} -> {:08X}", name, r3,
                  r5, ctx.r3.u32);
      return;
    }
  }
  __imp__sub_82911068(ctx, base);
}

extern "C" REX_FUNC(sub_8247C5A0) {
  if (REXCVAR_GET(skate3_warp_trace_ids)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 16) {
      REXLOG_INFO("skate3 warp ids: streamer request r3={:08X} r4={:08X} r5={:08X}",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    }
  }
  __imp__sub_8247C5A0(ctx, base);
}

extern "C" REX_FUNC(sub_8264ACF8) {
  using namespace skate3::warp;
  // Logging-only mode exists because arming the substitution killed the game
  // ~20 s into the macro, before the Locations list was built - which does not
  // say whether the SUBSTITUTION or merely being in this function's path is
  // fatal. With the cvar on and the substitution off, a run that survives
  // acquits the override and indicts the substitution.
  if (REXCVAR_GET(skate3_warp_trace_locid)) {
    static std::atomic<int> calls{0};
    const int n = calls.fetch_add(1);
    const std::string text = GuestString(base, ctx.r3.u32, 64);
    if (!text.empty() && text.compare(0, 12, "ID_LOCATION_") == 0 && n < 4096) {
      REXLOG_INFO("skate3 warp locid: call #{} r3=0x{:08X} '{}'", n, ctx.r3.u32, text);
    }
  }
  // Fire the in-menu replay the moment the Locations list resolves the id of
  // the world we want: that is proof the screen is up AND populated, and needs
  // no threshold tuned per run.
  // Gate on the world only: the item repoint is independent of the replay.
  {
    const std::string& want = REXCVAR_GET(skate3_warp_world);
    if (!want.empty()) {
      const std::string text = GuestString(base, ctx.r3.u32, 64);
      const std::string ours =
          "ID_LOCATION_" + ToUpper(WorldFolderName(want).substr(5)) + "_";
      if (text.size() > ours.size() && text.compare(0, ours.size(), ours) == 0) {
        // The list is up and populated: repoint every row at the wanted world.
        if (REXCVAR_GET(skate3_warp_substitute_item)) {
          static std::atomic<bool> done{false};
          // No anchor exists yet - the resolve has not run - so find the pack's
          // record for this world directly. FindPackNode returns the address
          // holding the node pointer, which is the record start + 4.
          uint32_t slot = 0;
          std::string full;
          const std::string bare = WorldFolderName(want).substr(5);
          FindPackNode(base, bare, &slot, &full);
          if (slot != 0 && !done.exchange(true)) {
            const auto t0 = std::chrono::steady_clock::now();
            const int n = PatchItemEntries(base, slot);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            // Once per session is right: the loader relaunches per map, and the
            // scan is two passes over 128 MB. If the list is ever rebuilt within
            // a session this would need re-arming.
            REXLOG_INFO("skate3 warp: repointed {} menu items at 0x{:08X} ({} ms)", n,
                        slot - 4, ms);
          }
        }
        if (REXCVAR_GET(skate3_warp_replay_in_menu) && REXCVAR_GET(skate3_warp_replay)) {
          MaybeReplayLocationChange(ctx, base, /*force=*/true);
        }
      }
    }
  }
  SubstituteLocationId(base, &ctx.r3.u32);
  __imp__sub_8264ACF8(ctx, base);
}

// The handler's LAST call, on the selected item (r28) - the one piece the
// replay has never been able to supply, and now the only candidate left: every
// name in the chain is looked up from the item, which is exactly why
// substituting names changed nothing. Logged at a real confirm to learn the
// item pointer, and whether items for different rows sit at a fixed stride.
// BISECT: override removed to test whether hooking this hot frontend
// function is what stalled boot.
#if 0
extern "C" REX_FUNC(sub_82B69978) {
  using namespace skate3::warp;
  if (REXCVAR_GET(skate3_warp_trace_ids)) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8) {
      REXLOG_INFO("skate3 warp ids: sub_82B69978 item r3={:08X}", ctx.r3.u32);
    }
  }
  __imp__sub_82B69978(ctx, base);
}
#endif

// The selection fetch the handler starts with: out[0] = item, out[1] = entry.
// BISECT: override removed to test whether hooking this hot frontend
// function is what stalled boot.
#if 0
extern "C" REX_FUNC(sub_82B6CCD8) {
  using namespace skate3::warp;
  const uint32_t out = ctx.r3.u32;
  __imp__sub_82B6CCD8(ctx, base);
  if (!REXCVAR_GET(skate3_warp_trace_ids)) {
    return;  // hot FE path: no unconditional guest reads here
  }
  const uint32_t item = LoadGuestU32BE(base, out);
  const uint32_t entry = LoadGuestU32BE(base, out + 4);
  if (item != 0 || entry != 0) {
    g_last_item.store(item, std::memory_order_relaxed);
    g_last_entry.store(entry, std::memory_order_relaxed);
  }
}
#endif

// The state push the confirm uses to actually start a load. Logged rather than
// assumed: the address the replay pushes onto (40141760) was copied out of a
// MACRO trace, and a heap singleton that happens to be stable in one session is
// not evidence. r4 is a code address - the state's handler.
extern "C" REX_FUNC(sub_826DFB30) {
  using namespace skate3::warp;
  if (REXCVAR_GET(skate3_warp_trace_ids)) {
    static std::atomic<int> n{0};
    const int i = n.fetch_add(1);
    if (i < 24) {
      REXLOG_INFO("skate3 warp ids: sub_826DFB30 #{} r3={:08X} r4={:08X} r5={:08X}", i,
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    }
  }
  __imp__sub_826DFB30(ctx, base);
}

// The menu confirm's location lookup: r5 is the ADDRESS OF the DLC's
// `const char*` table entry for the selected row (0x41C880D2 = Spillway,
// 0x41C88042 = Barcelona - 0x90 apart, one 4-byte entry per location).
extern "C" REX_FUNC(sub_82864628) {
  using namespace skate3::warp;
  SubstituteLocationSlot(base, &ctx.r5.u32, ctx.r4.u32);
  const uint32_t r5 = ctx.r5.u32;
  __imp__sub_82864628(ctx, base);
  if (REXCVAR_GET(skate3_warp_trace_ids)) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 6) {
      // The replay's own call returns 43991CB0; if the real confirm returns
      // something else, the replay's lookup is failing rather than working.
      // The CALLER is what matters now: lr names the confirm handler and the
      // exact offset, which is a generated-code address I can go read instead
      // of guessing what the sequence does. Do NOT use skate3_warp_probe for
      // this - it hooks the function table on top of a strong-symbol override
      // and the two together killed boot in 0.2 s.
      uint32_t off = 0;
      const uint32_t caller = skate3::guest_trace::GuestFunctionAt(uint32_t(ctx.lr), &off);
      const uint32_t entry = g_last_entry.load(std::memory_order_relaxed);
      // entry+4 is what the handler passes as r5. If that equals r5, the item's
      // entry IS a pack-table address and the two are the same table; if not,
      // something maps one to the other and THAT is the item->world link.
      REXLOG_INFO(
          "skate3 warp ids: sub_82864628(r5={:08X}) -> r3={:08X} <- sub_{:08X}+0x{:X} | "
          "selection item={:08X} entry={:08X} entry+4={:08X} [entry+4]={:08X} '{}'",
          r5, ctx.r3.u32, caller, off, g_last_item.load(std::memory_order_relaxed), entry,
          entry + 4, LoadGuestU32BE(base, entry + 4),
          GuestString(base, LoadGuestU32BE(base, entry + 4), 48));
    }
  }
}

// cLocationManager::ResolveStartNode(mgr, out, const char* node) - or near
// enough: the menu confirm's carrier of "which location am I going to".
extern "C" REX_FUNC(sub_828646E0) {
  using namespace skate3::warp;
  SubstituteSpawnNode(base, &ctx.r5.u32);
  const uint32_t r5 = ctx.r5.u32;
  // This is the REQUEST. Reading the code: it copies r5 (the start-node name)
  // into the world-request block at +64, then does `std r11,48(r31)` with the
  // 64-bit word its r4 buffer holds - so r4 is not a scratch "out", it is the
  // wanted world identity, and this one store is what makes the game load.
  // Logged unconditionally for the first few calls: on a run that works it
  // names the identity the game chose for itself, which is the value any warp
  // has to reproduce.
  const uint64_t asking = LoadGuestU64BE(base, ctx.r4.u32);
  {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8) {
      REXLOG_INFO("skate3 warp request: sub_828646E0 asks for {:016X} (node '{}') while the "
                  "block holds wanted {:016X} current {:016X}",
                  asking, GuestString(base, r5, 64),
                  LoadGuestU64BE(base, kWorldRequestWanted),
                  LoadGuestU64BE(base, kWorldRequestCurrent));
    }
  }
  __imp__sub_828646E0(ctx, base);
  if (REXCVAR_GET(skate3_warp_trace_ids)) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 6) {
      REXLOG_INFO("skate3 warp ids: sub_828646E0(r5={:08X}) -> r3={:08X}", r5, ctx.r3.u32);
    }
  }
}

extern "C" REX_FUNC(sub_8247DBE0) {
  using namespace skate3::warp;
  SubstituteStreamName(base, &ctx.r4.u32, /*slug=*/true, "stream slug");
  __imp__sub_8247DBE0(ctx, base);
}

// cWorldRegistry::FindByName(registry, const char* name, ...) - or near enough.
// r4 is the world folder name; at boot it is the stock world, and this is
// where the boot world is chosen.
namespace skate3::warp {

void RetargetWorld(const std::string& world) {
  if (world.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_retarget_mutex);
    g_retarget_world = world;
  }
  g_retarget_pending.store(true, std::memory_order_release);
  REXLOG_INFO("skate3 warp: retarget requested -> '{}'", world);
}

}  // namespace skate3::warp

// Applied here because this runs every frame on the MAIN thread with a guest
// base in hand - the same thread the menu confirm runs on.
static void ApplyPendingRetarget(uint8_t* base) {
  using namespace skate3::warp;
  if (!g_retarget_pending.load(std::memory_order_acquire) || base == nullptr) {
    return;
  }
  std::string world;
  {
    std::lock_guard<std::mutex> lock(g_retarget_mutex);
    world = g_retarget_world;
  }
  g_retarget_pending.store(false, std::memory_order_relaxed);
  uint32_t slot = 0;
  std::string full;
  FindPackNode(base, WorldFolderName(world).substr(5), &slot, &full);
  if (slot == 0) {
    REXLOG_WARN("skate3 warp: retarget '{}' has no pack record - rows unchanged", world);
    return;
  }
  REXLOG_INFO("skate3 warp: retarget '{}' repointed {} menu items at 0x{:08X}", world,
              PatchItemEntries(base, slot), slot - 4);
}

extern "C" REX_FUNC(sub_828A42F0) {
  ApplyPendingRetarget(base);
  // Reaches the file-local state above; the anonymous namespace is nested in
  // this one, so its members come along.
  using namespace skate3::warp;
  MaybeReplayLocationChange(ctx, base);
  // BOTH WATCHES RUN FIRST, and that ordering is the point. sub_828A3270 never
  // returns, so an induced load parks this thread inside it - and every
  // instrument sequenced after the call therefore stops reporting at the exact
  // moment it has something to report. "[session+2192] reached the stock world
  // and never changed again" was that: the watch had died, not the field.
  WatchWorldRequest(base);
  // Watch the session's CURRENT world. sub_828A3270 compares its r4 against
  // [session+2192] and loads when they differ, so this field is the guest's own
  // answer to "which world am I in". After an induced load the data streams
  // (3671 meshes) and the renderer takes over, yet the stock world is still on
  // screen - so the question is whether the loader ever committed the switch
  // here, or only moved bytes.
  {
    const uint32_t session = skate3::warp::g_loader_session.load(std::memory_order_relaxed);
    if (session != 0) {
      static std::atomic<uint64_t> s_last{0};
      const uint64_t now = skate3::warp::LoadGuestU64BE(base, session + 2192);
      const uint64_t was = s_last.exchange(now, std::memory_order_relaxed);
      if (now != was) {
        REXLOG_INFO("skate3 warp: session current world {:016X} -> {:016X}", was, now);
      }
    }
  }
  MaybeRequestWorld(ctx, base);
  AdvanceWorldTask(ctx, base);
  const std::string& requested = REXCVAR_GET(skate3_warp_world);
  if (requested.empty() || !REXCVAR_GET(skate3_warp_substitute_query)) {
    __imp__sub_828A42F0(ctx, base);
    return;
  }
  // Resolve the replacement once, on the first call that names the boot world.
  const uint32_t name_ptr = ctx.r4.u32;
  const std::string asked = GuestString(base, name_ptr, 64);
  if (!asked.empty() && asked == g_warp_boot_name) {
    if (!g_warp_ready.load(std::memory_order_relaxed)) {
      const std::string want = WorldFolderName(requested);
      const uint32_t found = FindGuestString(base, want);
      if (found != 0) {
        g_warp_name_address = found;
        g_warp_ready.store(true, std::memory_order_release);
        REXLOG_INFO("skate3 warp: '{}' found at guest 0x{:08X}; substituting for '{}'", want,
                    found, g_warp_boot_name);
      } else if (!g_warp_logged.exchange(true)) {
        // Not an error worth aborting over: the DLC string table may simply not
        // be registered this early. Say so plainly - a silent no-op here would
        // look exactly like a warp that ran and did nothing.
        REXLOG_WARN(
            "skate3 warp: '{}' is not in guest memory yet - booting the stock world. The "
            "pack may not be staged, or the name may be wrong.",
            want);
      }
    }
    const int limit = REXCVAR_GET(skate3_warp_max_substitutions);
    if (g_warp_ready.load(std::memory_order_acquire) &&
        (limit == 0 || g_substitutions.load(std::memory_order_relaxed) < limit)) {
      const int n = g_substitutions.fetch_add(1) + 1;
      ctx.r4.u32 = g_warp_name_address;
      // MEASURED: this call is a per-frame query (~60/s indefinitely), not a
      // one-shot load request, so any "every Nth" rule still floods the log -
      // one run produced 10,500 lines. Log the first few and then a single
      // line saying it is continuous, which is the fact worth knowing.
      if (n <= 4) {
        REXLOG_INFO("skate3 warp: substitution #{} - 0x{:08X} '{}' -> 0x{:08X}", n, name_ptr,
                    asked, g_warp_name_address);
      } else if (n == 5) {
        REXLOG_INFO(
            "skate3 warp: sub_828A42F0 is called continuously - substituting silently from "
            "here on. It is a world-by-name QUERY, not the load request.");
      }
    }
  }
  __imp__sub_828A42F0(ctx, base);
}
