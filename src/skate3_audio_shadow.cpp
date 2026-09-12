/**
 * @file        skate3_audio_shadow.cpp
 * @brief       Differential verification for native audio replacements
 *
 * See the header for why the lifted body runs first and stays authoritative, and for
 * exactly what is and is not compared.
 */
#include "skate3_audio_shadow.h"

#include <chrono>
#include <cstdio>
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

REXCVAR_DEFINE_BOOL(
    skate3_audio_vectors_diverged_only, false, "Skate 3",
    "Record a shadow vector only for comparisons that diverged.\n"
    "\n"
    "A session with two hundred hooks armed would otherwise write every call. The diverging "
    "vector is what a port fix needs: the entry registers, the read set, and the bytes the "
    "original produced where the native body disagreed.");

namespace skate3::audio {
namespace {

// Total bytes compared per call, across all windows: kShadowMaxWatch in the header. Audio
// functions run at audio rate, so this stays small; a kernel writing more is a hard failure.
constexpr uint32_t kMaxWatch = kShadowMaxWatch;

thread_local bool t_nested_replay = false;

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

template <typename T>
void HexOf(const T& v, char* out, size_t cap) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  size_t n = 0;
  for (size_t i = 0; i < sizeof(T) && n + 3 <= cap; i++) {
    std::snprintf(out + n, cap - n, "%02X", p[i]);
    n += 2;
  }
}

struct GprRef { const char* name; PPCRegister PPCContext::* mp; };
struct VrRef { const char* name; PPCVRegister PPCContext::* mp; };
struct CrRef { const char* name; PPCCRRegister PPCContext::* mp; };
constexpr GprRef kGprs[32] = {{"r0", &PPCContext::r0}, {"r1", &PPCContext::r1}, {"r2", &PPCContext::r2}, {"r3", &PPCContext::r3}, {"r4", &PPCContext::r4}, {"r5", &PPCContext::r5}, {"r6", &PPCContext::r6}, {"r7", &PPCContext::r7}, {"r8", &PPCContext::r8}, {"r9", &PPCContext::r9}, {"r10", &PPCContext::r10}, {"r11", &PPCContext::r11}, {"r12", &PPCContext::r12}, {"r13", &PPCContext::r13}, {"r14", &PPCContext::r14}, {"r15", &PPCContext::r15}, {"r16", &PPCContext::r16}, {"r17", &PPCContext::r17}, {"r18", &PPCContext::r18}, {"r19", &PPCContext::r19}, {"r20", &PPCContext::r20}, {"r21", &PPCContext::r21}, {"r22", &PPCContext::r22}, {"r23", &PPCContext::r23}, {"r24", &PPCContext::r24}, {"r25", &PPCContext::r25}, {"r26", &PPCContext::r26}, {"r27", &PPCContext::r27}, {"r28", &PPCContext::r28}, {"r29", &PPCContext::r29}, {"r30", &PPCContext::r30}, {"r31", &PPCContext::r31}};
constexpr GprRef kFprs[32] = {{"f0", &PPCContext::f0}, {"f1", &PPCContext::f1}, {"f2", &PPCContext::f2}, {"f3", &PPCContext::f3}, {"f4", &PPCContext::f4}, {"f5", &PPCContext::f5}, {"f6", &PPCContext::f6}, {"f7", &PPCContext::f7}, {"f8", &PPCContext::f8}, {"f9", &PPCContext::f9}, {"f10", &PPCContext::f10}, {"f11", &PPCContext::f11}, {"f12", &PPCContext::f12}, {"f13", &PPCContext::f13}, {"f14", &PPCContext::f14}, {"f15", &PPCContext::f15}, {"f16", &PPCContext::f16}, {"f17", &PPCContext::f17}, {"f18", &PPCContext::f18}, {"f19", &PPCContext::f19}, {"f20", &PPCContext::f20}, {"f21", &PPCContext::f21}, {"f22", &PPCContext::f22}, {"f23", &PPCContext::f23}, {"f24", &PPCContext::f24}, {"f25", &PPCContext::f25}, {"f26", &PPCContext::f26}, {"f27", &PPCContext::f27}, {"f28", &PPCContext::f28}, {"f29", &PPCContext::f29}, {"f30", &PPCContext::f30}, {"f31", &PPCContext::f31}};
constexpr VrRef kVrs[128] = {
    {"v0", &PPCContext::v0}, {"v1", &PPCContext::v1}, {"v2", &PPCContext::v2}, {"v3", &PPCContext::v3}, {"v4", &PPCContext::v4}, {"v5", &PPCContext::v5}, {"v6", &PPCContext::v6}, {"v7", &PPCContext::v7},
    {"v8", &PPCContext::v8}, {"v9", &PPCContext::v9}, {"v10", &PPCContext::v10}, {"v11", &PPCContext::v11}, {"v12", &PPCContext::v12}, {"v13", &PPCContext::v13}, {"v14", &PPCContext::v14}, {"v15", &PPCContext::v15},
    {"v16", &PPCContext::v16}, {"v17", &PPCContext::v17}, {"v18", &PPCContext::v18}, {"v19", &PPCContext::v19}, {"v20", &PPCContext::v20}, {"v21", &PPCContext::v21}, {"v22", &PPCContext::v22}, {"v23", &PPCContext::v23},
    {"v24", &PPCContext::v24}, {"v25", &PPCContext::v25}, {"v26", &PPCContext::v26}, {"v27", &PPCContext::v27}, {"v28", &PPCContext::v28}, {"v29", &PPCContext::v29}, {"v30", &PPCContext::v30}, {"v31", &PPCContext::v31},
    {"v32", &PPCContext::v32}, {"v33", &PPCContext::v33}, {"v34", &PPCContext::v34}, {"v35", &PPCContext::v35}, {"v36", &PPCContext::v36}, {"v37", &PPCContext::v37}, {"v38", &PPCContext::v38}, {"v39", &PPCContext::v39},
    {"v40", &PPCContext::v40}, {"v41", &PPCContext::v41}, {"v42", &PPCContext::v42}, {"v43", &PPCContext::v43}, {"v44", &PPCContext::v44}, {"v45", &PPCContext::v45}, {"v46", &PPCContext::v46}, {"v47", &PPCContext::v47},
    {"v48", &PPCContext::v48}, {"v49", &PPCContext::v49}, {"v50", &PPCContext::v50}, {"v51", &PPCContext::v51}, {"v52", &PPCContext::v52}, {"v53", &PPCContext::v53}, {"v54", &PPCContext::v54}, {"v55", &PPCContext::v55},
    {"v56", &PPCContext::v56}, {"v57", &PPCContext::v57}, {"v58", &PPCContext::v58}, {"v59", &PPCContext::v59}, {"v60", &PPCContext::v60}, {"v61", &PPCContext::v61}, {"v62", &PPCContext::v62}, {"v63", &PPCContext::v63},
    {"v64", &PPCContext::v64}, {"v65", &PPCContext::v65}, {"v66", &PPCContext::v66}, {"v67", &PPCContext::v67}, {"v68", &PPCContext::v68}, {"v69", &PPCContext::v69}, {"v70", &PPCContext::v70}, {"v71", &PPCContext::v71},
    {"v72", &PPCContext::v72}, {"v73", &PPCContext::v73}, {"v74", &PPCContext::v74}, {"v75", &PPCContext::v75}, {"v76", &PPCContext::v76}, {"v77", &PPCContext::v77}, {"v78", &PPCContext::v78}, {"v79", &PPCContext::v79},
    {"v80", &PPCContext::v80}, {"v81", &PPCContext::v81}, {"v82", &PPCContext::v82}, {"v83", &PPCContext::v83}, {"v84", &PPCContext::v84}, {"v85", &PPCContext::v85}, {"v86", &PPCContext::v86}, {"v87", &PPCContext::v87},
    {"v88", &PPCContext::v88}, {"v89", &PPCContext::v89}, {"v90", &PPCContext::v90}, {"v91", &PPCContext::v91}, {"v92", &PPCContext::v92}, {"v93", &PPCContext::v93}, {"v94", &PPCContext::v94}, {"v95", &PPCContext::v95},
    {"v96", &PPCContext::v96}, {"v97", &PPCContext::v97}, {"v98", &PPCContext::v98}, {"v99", &PPCContext::v99}, {"v100", &PPCContext::v100}, {"v101", &PPCContext::v101}, {"v102", &PPCContext::v102}, {"v103", &PPCContext::v103},
    {"v104", &PPCContext::v104}, {"v105", &PPCContext::v105}, {"v106", &PPCContext::v106}, {"v107", &PPCContext::v107}, {"v108", &PPCContext::v108}, {"v109", &PPCContext::v109}, {"v110", &PPCContext::v110}, {"v111", &PPCContext::v111},
    {"v112", &PPCContext::v112}, {"v113", &PPCContext::v113}, {"v114", &PPCContext::v114}, {"v115", &PPCContext::v115}, {"v116", &PPCContext::v116}, {"v117", &PPCContext::v117}, {"v118", &PPCContext::v118}, {"v119", &PPCContext::v119},
    {"v120", &PPCContext::v120}, {"v121", &PPCContext::v121}, {"v122", &PPCContext::v122}, {"v123", &PPCContext::v123}, {"v124", &PPCContext::v124}, {"v125", &PPCContext::v125}, {"v126", &PPCContext::v126}, {"v127", &PPCContext::v127}};
constexpr CrRef kCrs[8] = {{"cr0", &PPCContext::cr0}, {"cr1", &PPCContext::cr1}, {"cr2", &PPCContext::cr2}, {"cr3", &PPCContext::cr3}, {"cr4", &PPCContext::cr4}, {"cr5", &PPCContext::cr5}, {"cr6", &PPCContext::cr6}, {"cr7", &PPCContext::cr7}};

struct RegisterMismatch {
  const char* name = nullptr;
  char lifted[40] = {0};
  char native[40] = {0};
};

// The first named-result or preserved register that differs. `a` is the native result, `b` the
// lifted one. Float registers are compared as bits, so -0.0 against +0.0 and NaN payloads count
// as differences.
RegisterMismatch RegisterDiff(const PPCContext& a, const PPCContext& b, ShadowResults r) {
  RegisterMismatch m;
  auto gpr = [&](const GprRef& ref) {
    if (SameBits(a.*ref.mp, b.*ref.mp)) return false;
    m.name = ref.name;
    HexOf((b.*ref.mp).u64, m.lifted, sizeof m.lifted);
    HexOf((a.*ref.mp).u64, m.native, sizeof m.native);
    return true;
  };
  auto vr = [&](const VrRef& ref) {
    if (SameBytes(a.*ref.mp, b.*ref.mp)) return false;
    m.name = ref.name;
    HexOf(b.*ref.mp, m.lifted, sizeof m.lifted);
    HexOf(a.*ref.mp, m.native, sizeof m.native);
    return true;
  };
  auto cr = [&](const CrRef& ref) {
    if (SameBytes(a.*ref.mp, b.*ref.mp)) return false;
    m.name = ref.name;
    HexOf(b.*ref.mp, m.lifted, sizeof m.lifted);
    HexOf(a.*ref.mp, m.native, sizeof m.native);
    return true;
  };
  for (int i = 0; i < 32; i++) {
    if ((r.gprs >> i) & 1u) { if (gpr(kGprs[i])) return m; }
  }
  for (int i = 0; i < 32; i++) {
    if ((r.fprs >> i) & 1u) { if (gpr(kFprs[i])) return m; }
  }
  for (int i = 0; i < 64; i++) {
    if ((r.vrs_lo >> i) & 1ull) { if (vr(kVrs[i])) return m; }
  }
  for (int i = 0; i < 64; i++) {
    if ((r.vrs_hi >> i) & 1ull) { if (vr(kVrs[64 + i])) return m; }
  }
  for (int i = 0; i < 8; i++) {
    if ((r.crs >> i) & 1u) { if (cr(kCrs[i])) return m; }
  }
#define X(reg) if (gpr({#reg, &PPCContext::reg})) return m;
  SHADOW_PRESERVED_GPRS(X)
  SHADOW_PRESERVED_FPRS(X)
#undef X
#define X(reg) if (vr({#reg, &PPCContext::reg})) return m;
  SHADOW_PRESERVED_VRS(X)
#undef X
#define X(reg) if (cr({#reg, &PPCContext::reg})) return m;
  SHADOW_PRESERVED_CRS(X)
#undef X
  return m;
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
  REXLOG_INFO("skate3-audio-shadow: {} runs={} diverged: registers={} memory={} skipped={} "
              "overflow={}",
              stats.name, runs, stats.register_diffs.load(std::memory_order_relaxed),
              stats.memory_diffs.load(std::memory_order_relaxed),
              stats.skipped.load(std::memory_order_relaxed),
              stats.overflow.load(std::memory_order_relaxed));
}

// Started on the first comparison rather than at static init. Logs every function whose
// count moved since it was last logged.
void ReporterMain() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::lock_guard<std::mutex> lock(Registry().mutex);
    for (ShadowStats* stats : Registry().all) {
      const uint64_t runs = stats->runs.load(std::memory_order_relaxed) +
                            stats->skipped.load(std::memory_order_relaxed) +
                            stats->overflow.load(std::memory_order_relaxed);
      if (runs != stats->logged_runs.load(std::memory_order_relaxed)) {
        LogStats(*stats, stats->runs.load(std::memory_order_relaxed));
        stats->logged_runs.store(runs, std::memory_order_relaxed);
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

bool ShadowNestedReplay() {
  return t_nested_replay;
}

void ShadowSkip(ShadowStats& stats, const char* why) {
  EnsureReporter();
  const uint64_t skipped = stats.skipped.fetch_add(1, std::memory_order_relaxed) + 1;
  if (skipped == 1 || skipped == 16 || skipped == 256 || (skipped % 4096) == 0) {
    REXLOG_INFO("skate3-audio-shadow: {} not comparable on {} calls so far ({})", stats.name,
                skipped, why);
  }
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
                   std::span<const ShadowWindow> inputs, ShadowResults returns,
                   ShadowStats& stats) {
  static const uint64_t report_cap = REXCVAR_GET(skate3_audio_shadow_reports);
  static const bool diverged_only = REXCVAR_GET(skate3_audio_vectors_diverged_only);
  EnsureReporter();

  // Reached from an outer native replay: the outer comparison already brackets this call, and
  // comparing again would double every count and vector. Run the lifted body and nothing else.
  if (t_nested_replay) {
    lifted(ctx, base);
    return true;
  }

  // A window set over the byte budget is a hard failure. Dropping windows would let a native
  // write land outside them, which is never rewound and so reaches the live game.
  uint32_t total = 0;
  for (const ShadowWindow& w : windows) {
    total += w.len;
  }
  if (total > kMaxWatch) {
    const uint64_t n = stats.overflow.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n == 16 || n == 256 || (n % 4096) == 0) {
      REXLOG_ERROR("skate3-audio-shadow: {} budget overflow {} > {} bytes; NOT compared ({} so far)",
                   stats.name, total, kMaxWatch, n);
    }
    lifted(ctx, base);
    return false;
  }

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

  const uint64_t run_number = stats.runs.load(std::memory_order_relaxed) + 1;
  if (!diverged_only) {
    RecordVector(stats.name, entry, after_lifted, windows, inputs, base, mem_entry, mem_lifted,
                 run_number);
  }

  // 2. Rewind memory, last window first so overlapping windows end at their entry bytes.
  off = total;
  for (size_t i = windows.size(); i-- > 0;) {
    off -= windows[i].len;
    std::memcpy(base + windows[i].addr, mem_entry.data() + off, windows[i].len);
  }

  // 3. The native body, against the rewound copy.
  ctx = entry;
  t_nested_replay = true;
  native(ctx, base);
  t_nested_replay = false;
  const RegisterMismatch bad_register = RegisterDiff(ctx, after_lifted, returns);
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

  const bool diverged = bad_register.name != nullptr || bad_window != nullptr;
  if (bad_register.name) stats.register_diffs.fetch_add(1, std::memory_order_relaxed);
  if (bad_window) stats.memory_diffs.fetch_add(1, std::memory_order_relaxed);
  const uint64_t runs = stats.runs.fetch_add(1, std::memory_order_relaxed) + 1;

  if (diverged && diverged_only) {
    RecordVector(stats.name, entry, after_lifted, windows, inputs, base, mem_entry, mem_lifted,
                 runs);
  }
  if (diverged && stats.reports.fetch_add(1, std::memory_order_relaxed) < report_cap) {
    if (bad_register.name) {
      REXLOG_ERROR("skate3-audio-shadow: {} run {} diverges in register {} (lifted {}, native {})",
                   stats.name, runs, bad_register.name, bad_register.lifted, bad_register.native);
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
  return !diverged;
}

}  // namespace skate3::audio
