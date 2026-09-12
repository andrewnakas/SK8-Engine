/**
 * @file        skate3_audio_shadow.cpp
 * @brief       Differential verification for native audio replacements
 *
 * See the header for why the lifted body runs first and stays authoritative, and for
 * exactly what is and is not compared.
 */
#include "skate3_audio_shadow.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(
    skate3_audio_shadow, false, "Skate 3",
    "Run native audio replacements alongside the original recompiled code and report "
    "any divergence.\n"
    "\n"
    "The original stays authoritative, so this cannot change what the game does; it "
    "doubles the work on the functions it covers. Leave it on through a play session to "
    "check a native implementation against real inputs rather than hand-written ones.");

REXCVAR_DEFINE_UINT32(
    skate3_audio_shadow_reports, 16, "Skate 3",
    "How many divergences to report per function before going quiet.");

REXCVAR_DEFINE_STRING(
    skate3_audio_vectors_path, "", "Skate 3",
    "Record every shadow comparison as a replayable vector to this path.\n"
    "\n"
    "Phase 4 tier 1 needs identical inputs fed to the verified C++ and to the Rust port. The "
    "harness already brackets every call, so rather than generating synthetic vectors this "
    "dumps the real ones: the entry registers, the watched windows, their bytes on entry, and "
    "the bytes the ORIGINAL lifted body produced. The Rust port is then replayed against them "
    "offline. Real inputs, because synthetic tests have never caught a format bug in this "
    "project and real data has caught every one.");

REXCVAR_DEFINE_INT32(
    skate3_audio_vectors_max, 4096, "Skate 3",
    "Stop recording shadow vectors after this many, so a session cannot fill the disk.");

namespace skate3::audio {
namespace {

// Total bytes compared per call, across all windows. Audio functions run at audio rate,
// so this stays small; a kernel writing more should be compared on its output buffer.
constexpr uint32_t kMaxWatch = 64 * 1024;

// Registers the ABI obliges a callee to preserve. The ranges match the save/restore
// helpers present in the image: __savegprlr_14..31, __savefpr_14..31, and __savevmx_14..31
// plus __savevmx_64..127.
#define SHADOW_PRESERVED_GPRS(X) \
  X(r1) X(r2) X(r13) X(r14) X(r15) X(r16) X(r17) X(r18) X(r19) X(r20) X(r21) X(r22) X(r23) \
  X(r24) X(r25) X(r26) X(r27) X(r28) X(r29) X(r30) X(r31)
#define SHADOW_PRESERVED_FPRS(X) \
  X(f14) X(f15) X(f16) X(f17) X(f18) X(f19) X(f20) X(f21) X(f22) X(f23) X(f24) X(f25) X(f26) \
  X(f27) X(f28) X(f29) X(f30) X(f31)
#define SHADOW_PRESERVED_VRS(X) \
  X(v14) X(v15) X(v16) X(v17) X(v18) X(v19) X(v20) X(v21) X(v22) X(v23) X(v24) X(v25) X(v26) \
  X(v27) X(v28) X(v29) X(v30) X(v31) X(v64) X(v65) X(v66) X(v67) X(v68) X(v69) X(v70) X(v71) \
  X(v72) X(v73) X(v74) X(v75) X(v76) X(v77) X(v78) X(v79) X(v80) X(v81) X(v82) X(v83) X(v84) \
  X(v85) X(v86) X(v87) X(v88) X(v89) X(v90) X(v91) X(v92) X(v93) X(v94) X(v95) X(v96) X(v97) \
  X(v98) X(v99) X(v100) X(v101) X(v102) X(v103) X(v104) X(v105) X(v106) X(v107) X(v108) \
  X(v109) X(v110) X(v111) X(v112) X(v113) X(v114) X(v115) X(v116) X(v117) X(v118) X(v119) \
  X(v120) X(v121) X(v122) X(v123) X(v124) X(v125) X(v126) X(v127)
#define SHADOW_PRESERVED_CRS(X) X(cr2) X(cr3) X(cr4)

bool SameBits(const PPCRegister& a, const PPCRegister& b) {
  return a.u64 == b.u64;
}

template <typename T>
bool SameBytes(const T& a, const T& b) {
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// The first preserved or return register that differs, or nullptr. Float registers are
// compared as bits, so -0.0 against +0.0 and NaN payloads count as differences.
const char* RegisterDiff(const PPCContext& a, const PPCContext& b, uint32_t returns) {
  if ((returns & kReturnR3) && !SameBits(a.r3, b.r3)) return "r3";
  if ((returns & kReturnF1) && !SameBits(a.f1, b.f1)) return "f1";
  if ((returns & kReturnV2) && !SameBytes(a.v2, b.v2)) return "v2";
#define X(r) \
  if (!SameBits(a.r, b.r)) return #r;
  SHADOW_PRESERVED_GPRS(X)
  SHADOW_PRESERVED_FPRS(X)
#undef X
#define X(r) \
  if (!SameBytes(a.r, b.r)) return #r;
  SHADOW_PRESERVED_VRS(X)
  SHADOW_PRESERVED_CRS(X)
#undef X
  return nullptr;
}

size_t FirstDiff(const uint8_t* a, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i]) return i;
  }
  return n;
}

struct StatsRegistry {
  std::mutex mutex;
  std::vector<ShadowStats*> all;
};

StatsRegistry& Registry() {
  static StatsRegistry registry;
  return registry;
}

void LogStats(ShadowStats& stats, uint64_t runs) {
  stats.logged_runs.store(runs, std::memory_order_relaxed);
  REXLOG_INFO("skate3-audio-shadow: {} runs={} diverged: registers={} memory={}", stats.name,
              runs, stats.register_diffs.load(std::memory_order_relaxed),
              stats.memory_diffs.load(std::memory_order_relaxed));
}

// Started on the first comparison rather than at static init. Logs every function whose
// count moved since it was last logged.
void ReporterMain() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::lock_guard<std::mutex> lock(Registry().mutex);
    for (ShadowStats* stats : Registry().all) {
      const uint64_t runs = stats->runs.load(std::memory_order_relaxed);
      if (runs != stats->logged_runs.load(std::memory_order_relaxed)) {
        LogStats(*stats, runs);
      }
    }
  }
}

void EnsureReporter() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(ReporterMain).detach(); });
}

}  // namespace

ShadowStats::ShadowStats(const char* function_name) : name(function_name) {
  std::lock_guard<std::mutex> lock(Registry().mutex);
  Registry().all.push_back(this);
}

bool ShadowEnabled() {
  static const bool armed = REXCVAR_GET(skate3_audio_shadow);
  return armed;
}

namespace {

// Vector recording for Phase 4. One line per comparison, hex, self-describing:
//
//   name  run  r3 r4 r5 r6 r7  ret_r3  nwin  addr:len:entry:lifted ...
//
// Only r3-r7 and the returned r3 are recorded, not the whole PPCContext: it carries 128 vector
// registers, and the functions these vectors cover take their arguments in r3-r7. A port that
// needs more than that is a port this file should not be feeding.
std::mutex g_vector_mutex;
std::FILE* g_vector_file = nullptr;
bool g_vector_tried = false;
uint64_t g_vector_count = 0;

void HexBytes(std::FILE* out, const uint8_t* data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    std::fprintf(out, "%02X", data[i]);
  }
}

void RecordVector(const char* name, const PPCContext& entry, const PPCContext& after,
                  std::span<const ShadowWindow> windows, std::span<const ShadowWindow> inputs,
                  const uint8_t* base, const std::vector<uint8_t>& mem_entry,
                  const std::vector<uint8_t>& mem_lifted, uint64_t run) {
  std::lock_guard<std::mutex> lock(g_vector_mutex);
  if (!g_vector_tried) {
    g_vector_tried = true;
    const std::string path = REXCVAR_GET(skate3_audio_vectors_path);
    if (!path.empty()) {
      g_vector_file = std::fopen(path.c_str(), "w");
      if (g_vector_file == nullptr) {
        REXLOG_ERROR("skate3-audio-shadow: cannot write vectors to '{}'", path);
      } else {
        std::fprintf(g_vector_file,
                     "# skate3 shadow vectors: name run r3 r4 r5 r6 r7 ret_r3 then spans.\n"
                     "# I:addr:len:bytes      = read set, the memory the function saw\n"
                     "# W:addr:len:entry:exp  = write set, entry bytes and what the ORIGINAL "
                     "produced\n");
        REXLOG_INFO("skate3-audio-shadow: recording vectors to '{}' (cap {})", path,
                    REXCVAR_GET(skate3_audio_vectors_max));
      }
    }
  }
  if (g_vector_file == nullptr) {
    return;
  }
  if (g_vector_count >= static_cast<uint64_t>(REXCVAR_GET(skate3_audio_vectors_max))) {
    if (g_vector_count == static_cast<uint64_t>(REXCVAR_GET(skate3_audio_vectors_max))) {
      g_vector_count++;
      REXLOG_INFO("skate3-audio-shadow: vector cap reached, stopping at {}", g_vector_count - 1);
      std::fflush(g_vector_file);
    }
    return;
  }
  g_vector_count++;

  std::fprintf(g_vector_file, "%s\t%llu\t%08X\t%08X\t%08X\t%08X\t%08X\t%08X", name,
               static_cast<unsigned long long>(run), entry.r3.u32, entry.r4.u32, entry.r5.u32,
               entry.r6.u32, entry.r7.u32, after.r3.u32);
  // The read set first, taken from memory as it stands now: these spans are not written by the
  // function, so their entry bytes are still intact after the lifted body ran.
  for (const ShadowWindow& w : inputs) {
    std::fprintf(g_vector_file, "\tI:%08X:%u:", w.addr, w.len);
    HexBytes(g_vector_file, base + w.addr, w.len);
  }
  size_t off = 0;
  for (const ShadowWindow& w : windows) {
    std::fprintf(g_vector_file, "\tW:%08X:%u:", w.addr, w.len);
    HexBytes(g_vector_file, mem_entry.data() + off, w.len);
    std::fputc(':', g_vector_file);
    HexBytes(g_vector_file, mem_lifted.data() + off, w.len);
    off += w.len;
  }
  std::fputc('\n', g_vector_file);
  std::fflush(g_vector_file);  // per line: a killed session keeps whole records
}

}  // namespace

bool ShadowCompare(PPCContext& ctx, uint8_t* base, PPCFunc* native, PPCFunc* lifted,
                   std::span<const ShadowWindow> windows,
                   std::span<const ShadowWindow> inputs, uint32_t returns,
                   ShadowStats& stats) {
  static const uint64_t report_cap = REXCVAR_GET(skate3_audio_shadow_reports);
  EnsureReporter();

  // Windows past the byte budget are dropped, loudly, rather than silently truncated.
  size_t count = 0;
  uint32_t total = 0;
  for (const ShadowWindow& w : windows) {
    if (total + w.len > kMaxWatch) {
      REXLOG_WARN("skate3-audio-shadow: {} watches more than {} bytes; windows past {:08X} "
                  "are not compared",
                  stats.name, kMaxWatch, w.addr);
      break;
    }
    total += w.len;
    count++;
  }
  windows = windows.first(count);

  // Per call rather than thread_local: a hooked function can call another hooked
  // function, and nested comparisons must not share buffers.
  std::vector<uint8_t> mem_entry(total), mem_lifted(total);
  size_t off = 0;
  for (const ShadowWindow& w : windows) {
    std::memcpy(mem_entry.data() + off, base + w.addr, w.len);
    off += w.len;
  }

  // 1. The lifted body, for real.
  const PPCContext entry = ctx;
  lifted(ctx, base);
  const PPCContext after_lifted = ctx;
  off = 0;
  for (const ShadowWindow& w : windows) {
    std::memcpy(mem_lifted.data() + off, base + w.addr, w.len);
    off += w.len;
  }

  RecordVector(stats.name, entry, after_lifted, windows, inputs, base, mem_entry, mem_lifted,
               stats.runs.load(std::memory_order_relaxed) + 1);

  // 2. Rewind memory, last window first so overlapping windows end at their entry bytes.
  off = total;
  for (size_t i = windows.size(); i-- > 0;) {
    off -= windows[i].len;
    std::memcpy(base + windows[i].addr, mem_entry.data() + off, windows[i].len);
  }

  // 3. The native body, against the rewound copy.
  ctx = entry;
  native(ctx, base);
  const char* bad_register = RegisterDiff(ctx, after_lifted, returns);
  const ShadowWindow* bad_window = nullptr;
  size_t bad_at = 0;
  uint8_t lifted_byte = 0, native_byte = 0;
  off = 0;
  for (const ShadowWindow& w : windows) {
    if (!bad_window) {
      const size_t at = FirstDiff(base + w.addr, mem_lifted.data() + off, w.len);
      if (at != w.len) {
        bad_window = &w;
        bad_at = at;
        lifted_byte = mem_lifted[off + at];
        native_byte = base[w.addr + at];
      }
    }
    off += w.len;
  }

  // 4. Restore the authoritative result, whatever the native code did.
  off = 0;
  for (const ShadowWindow& w : windows) {
    std::memcpy(base + w.addr, mem_lifted.data() + off, w.len);
    off += w.len;
  }
  ctx = after_lifted;

  if (bad_register) stats.register_diffs.fetch_add(1, std::memory_order_relaxed);
  if (bad_window) stats.memory_diffs.fetch_add(1, std::memory_order_relaxed);
  const uint64_t runs = stats.runs.fetch_add(1, std::memory_order_relaxed) + 1;

  if ((bad_register || bad_window) &&
      stats.reports.fetch_add(1, std::memory_order_relaxed) < report_cap) {
    if (bad_register) {
      REXLOG_ERROR("skate3-audio-shadow: {} run {} diverges in register {}", stats.name, runs,
                   bad_register);
    }
    if (bad_window) {
      REXLOG_ERROR("skate3-audio-shadow: {} run {} diverges in memory at {:08X}+{} "
                   "(lifted {:02X}, native {:02X})",
                   stats.name, runs, bad_window->addr, bad_at, lifted_byte, native_byte);
    }
  }
  if (runs == 1 || runs == 16 || runs == 256 || runs == 4096) {
    LogStats(stats, runs);
  }
  return !bad_register && !bad_window;
}

}  // namespace skate3::audio
