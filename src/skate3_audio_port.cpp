/**
 * @file        skate3_audio_port.cpp
 * @brief       Census counters and reporter shared by every port hook and the census TU
 *
 * Integer and I/O work only: these run on the audio worker thread inside a guest call, where
 * floating-point exceptions are unmasked (docs/shadow-harness.md, the SIGFPE).
 */
#include "skate3_audio_port.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

REXCVAR_DEFINE_BOOL(
    skate3_audio_port_census, false, "Skate 3",
    "Count calls to every hooked audio function and log the totals every 10 s.\n"
    "\n"
    "The guest tracer records breadth, not frequency, and REQUEUE passed every gate and was then "
    "never called once. This answers 'how often, and on which thread' for the whole audio-thread "
    "set in one session, before any port is written. The first four calls of each function also "
    "log their entry registers, which is what a window builder is derived from.");

REXCVAR_DECLARE(bool, skate3_audio_native);  // defined in skate3_audio_native.cpp

namespace skate3::audio {
namespace {

bool CensusOn() {
  static const bool on = REXCVAR_GET(skate3_audio_port_census);
  return on;
}

struct Registry {
  std::mutex mutex;
  std::vector<CensusTable*> tables;
  std::vector<PortCounters*> ports;
  std::map<long, std::string> thread_names;  // std::map: node addresses are stable
  bool reporter_started = false;
};

Registry& Reg() {
  static Registry r;
  return r;
}

const char* ThreadName() {
  const long tid = static_cast<long>(syscall(SYS_gettid));
  std::lock_guard<std::mutex> lock(Reg().mutex);
  auto it = Reg().thread_names.find(tid);
  if (it == Reg().thread_names.end()) {
    char path[64], buf[64] = {0};
    std::snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
    if (std::FILE* f = std::fopen(path, "r")) {
      if (std::fgets(buf, sizeof buf, f) == nullptr) buf[0] = 0;
      std::fclose(f);
    }
    std::string name(buf);
    while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
    for (char& c : name) if (c == ' ') c = '_';
    if (name.empty()) name = "t" + std::to_string(tid);
    it = Reg().thread_names.emplace(tid, name + "/" + std::to_string(tid)).first;
  }
  return it->second.c_str();
}

void DumpEntry(const char* name, uint64_t n, const PPCContext& ctx) {
  REXLOG_INFO("skate3-audio-census: {} call={} thread={} r1={:08X} r3={:08X} r4={:08X} r5={:08X} "
              "r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} r11={:08X} f1={:016X} "
              "lr={:08X}",
              name, n, ThreadName(), ctx.r1.u32, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
              ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.r11.u32, ctx.f1.u64,
              static_cast<uint32_t>(ctx.lr));
}

void ReporterMain() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::lock_guard<std::mutex> lock(Reg().mutex);
    uint64_t total = 0;
    size_t called = 0, of = 0;
    for (CensusTable* t : Reg().tables) {
      for (size_t i = 0; i < t->count; i++) {
        CensusSlot& s = t->slots[i];
        const uint64_t n = s.calls.load(std::memory_order_relaxed);
        of++;
        if (n == 0) continue;
        called++;
        total += n;
        if (n != s.logged.load(std::memory_order_relaxed)) {
          s.logged.store(n, std::memory_order_relaxed);
          const char* thr = s.thread.load(std::memory_order_relaxed);
          REXLOG_INFO("skate3-audio-census: {} calls={} thread={}", t->names[i], n,
                      thr ? thr : "?");
        }
      }
    }
    for (PortCounters* p : Reg().ports) {
      const uint64_t n = p->calls.load(std::memory_order_relaxed);
      of++;
      if (n == 0) continue;
      called++;
      total += n;
      if (n != p->logged.load(std::memory_order_relaxed)) {
        p->logged.store(n, std::memory_order_relaxed);
        const char* thr = p->thread.load(std::memory_order_relaxed);
        REXLOG_INFO("skate3-audio-census: {} calls={} thread={}", p->name, n, thr ? thr : "?");
        REXLOG_INFO("skate3-audio-port: {} status={} skipped={} oversize={} native={}", p->name,
                    static_cast<int>(p->status), p->skipped.load(std::memory_order_relaxed),
                    p->oversize.load(std::memory_order_relaxed),
                    p->native_runs.load(std::memory_order_relaxed));
      }
    }
    if (total != 0) {
      REXLOG_INFO("skate3-audio-census: {} functions of {} called, {} calls total", called, of,
                  total);
    }
  }
}

void EnsureReporterLocked() {
  if (!Reg().reporter_started) {
    Reg().reporter_started = true;
    std::thread(ReporterMain).detach();
  }
}

template <typename Slot>
uint64_t Hit(Slot& s, const char* name, const PPCContext& ctx) {
  const uint64_t n = s.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!CensusOn()) return n;
  if (n <= 4) {
    // First four calls: entry registers and the thread, once each. The dumped counter keeps two
    // racing early calls from both logging as call 1.
    uint64_t d = s.dumped.load(std::memory_order_relaxed);
    while (d < 4 && !s.dumped.compare_exchange_weak(d, d + 1, std::memory_order_relaxed)) {
    }
    if (d < 4) {
      const char* thr = ThreadName();
      const char* none = nullptr;
      s.thread.compare_exchange_strong(none, thr, std::memory_order_relaxed);
      DumpEntry(name, n, ctx);
      std::lock_guard<std::mutex> lock(Reg().mutex);
      EnsureReporterLocked();
    }
  }
  return n;
}

}  // namespace

PortCounters::PortCounters(const char* function_name, PortStatus s)
    : name(function_name), status(s) {
  std::lock_guard<std::mutex> lock(Reg().mutex);
  Reg().ports.push_back(this);
}

void RegisterCensusTable(CensusTable& table) {
  std::lock_guard<std::mutex> lock(Reg().mutex);
  Reg().tables.push_back(&table);
}

uint64_t CensusHit(CensusTable& table, size_t index, const PPCContext& ctx) {
  return Hit(table.slots[index], table.names[index], ctx);
}

uint64_t PortCensus(PortCounters& c, const PPCContext& ctx) {
  return Hit(c, c.name, ctx);
}

void PortSkip(PortCounters& c, ShadowStats& stats, const char* why) {
  c.skipped.fetch_add(1, std::memory_order_relaxed);
  ShadowSkip(stats, why);
}

void PortOversize(PortCounters& c, ShadowStats& stats, uint32_t total) {
  const uint64_t n = c.oversize.fetch_add(1, std::memory_order_relaxed) + 1;
  stats.skipped.fetch_add(1, std::memory_order_relaxed);
  if (n == 1 || n == 16 || (n % 1024) == 0) {
    REXLOG_INFO("skate3-audio-shadow: {} not comparable on {} calls so far (watched {} bytes over "
                "the {} cap)",
                c.name, n, total, kPortWatchCap);
  }
}

void PortNativeRun(PortCounters& c) {
  const uint64_t n = c.native_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n == 16 || n == 256 || n == 4096 || (n % 65536) == 0) {
    REXLOG_INFO("skate3-audio-native: {} native runs={}", c.name, n);
  }
}

bool PortPromoted(PortStatus s) {
  static const bool on = REXCVAR_GET(skate3_audio_native);
  return on && s == kPortVerified;
}

}  // namespace skate3::audio
