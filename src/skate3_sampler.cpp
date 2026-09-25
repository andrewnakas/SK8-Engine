// A sampling profiler for guest threads on Horizon.
//
// The career world is starved by the guest render thread (60-90% of a core in
// [cpu]) and nothing on this platform could say what it runs: the sampling
// profiler in skate3_guest_trace.cpp needs signals or Mach, and the function
// wrappers ([audio-prof], [phys-prof]) only see what they were told to wrap.
//
// Two views per sample, for each thread named in skate3_sampler_threads:
//
//   host   the thread is paused (svcSetThreadActivity), its registers read
//          (svcGetThreadContext3) and it is resumed. The PC names the exact
//          recompiled function - or runtime function - that was executing.
//          Symbolise offline against skate3.debug.elf using the anchor line.
//   guest  the PowerPC back chain from the context's r1, the same walk the
//          hang report uses. Every return address on it is counted once per
//          sample, so a function's count is its INCLUSIVE share: which game
//          subsystem the time is spent under, not only the leaf.
//
// Nothing is taken while a thread is paused - no lock, no allocation, no log -
// so the sampler cannot deadlock against the thread it stopped. Each 30 s
// window is appended to sdmc:/switch/skate3/samples.txt and summarised at warn
// as [sampler]. Off unless skate3_sampler_threads is set.

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(skate3_sampler_threads, "", "Skate 3",
                      "Comma-separated guest thread name prefixes to sample, e.g. "
                      "'render_thread,Main XThread,Job Manager'. Empty = off. Results go to "
                      "samples.txt beside the log, one block per 30 s window.");

REXCVAR_DEFINE_INT32(skate3_sampler_hz, 500, "Skate 3",
                     "Samples per second per thread for skate3_sampler_threads.")
    .range(10, 4000);

#if defined(__SWITCH__)

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

namespace {

constexpr int kMaxDepth = 32;
constexpr const char* kOutPath = "sdmc:/switch/skate3/samples.txt";

struct Target {
  std::string name;
  Handle handle = INVALID_HANDLE;
  const ::PPCContext* ctx = nullptr;
  uint64_t samples = 0;
  uint64_t pause_fail = 0;
  uint64_t no_stack = 0;
  std::unordered_map<uint64_t, uint32_t> host;   // host pc -> samples
  std::unordered_map<uint32_t, uint32_t> guest;  // guest return address -> samples containing it
};

// Its address is logged so host PCs can be made relative to a symbol in the ELF.
__attribute__((noinline, used)) void SamplerAnchor() {
  asm volatile("");
}

std::vector<std::string> SplitPrefixes(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t comma = s.find(',', start);
    if (comma == std::string::npos) {
      comma = s.size();
    }
    std::string part = s.substr(start, comma - start);
    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (!part.empty()) {
      out.push_back(part);
    }
    start = comma + 1;
  }
  return out;
}

// Rebuilt every few seconds: guest threads come and go across loads.
void RefreshTargets(std::vector<Target>& targets, const std::vector<std::string>& prefixes) {
  auto* ks = rex::system::kernel_state();
  if (ks == nullptr) {
    return;
  }
  const auto threads = ks->object_table()->GetObjectsByType<rex::system::XThread>();
  for (const auto& thread : threads) {
    if (!thread || !thread->is_running()) {
      continue;
    }
    auto* host = thread->thread();
    auto* ts = thread->thread_state();
    if (host == nullptr || ts == nullptr || ts->context() == nullptr) {
      continue;
    }
    const std::string name = thread->thread_name();
    const bool wanted = std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) {
      return name.compare(0, p.size(), p) == 0;
    });
    if (!wanted) {
      continue;
    }
    const Handle h = Handle(uintptr_t(host->native_handle()));
    const bool known = std::any_of(targets.begin(), targets.end(),
                                   [&](const Target& t) { return t.handle == h; });
    if (known) {
      continue;
    }
    Target t;
    t.name = name + "/" + std::to_string(targets.size());
    t.handle = h;
    t.ctx = ts->context();
    t.host.reserve(1 << 14);
    t.guest.reserve(1 << 14);
    targets.push_back(std::move(t));
  }
}

void SampleOne(Target& t, rex::memory::Memory* memory) {
  // Host: stop it, read it, start it again. Only svcs in between.
  ThreadContext tc;
  bool have_pc = false;
  if (R_SUCCEEDED(svcSetThreadActivity(t.handle, ThreadActivity_Paused))) {
    have_pc = R_SUCCEEDED(svcGetThreadContext3(&tc, t.handle));
    svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
  }
  // Guest: the context is read live, as the hang report does; a torn frame
  // ends the walk early rather than looping, because the chain must ascend.
  uint32_t links[kMaxDepth + 1];
  int nlinks = 0;
  const uint32_t lr = uint32_t(t.ctx->lr);
  if (lr >= 0x82000000u && lr < 0x84000000u) {
    links[nlinks++] = lr;
  }
  uint32_t frame = uint32_t(t.ctx->r1.u64 & 0xFFFFFFFFull);
  for (int depth = 0; depth < kMaxDepth && memory != nullptr; ++depth) {
    if (frame == 0 || (frame & 3) != 0 || frame < 0x10000) {
      break;
    }
    auto* next_ptr = memory->TranslateVirtual<const uint32_t*>(frame);
    if (next_ptr == nullptr) {
      break;
    }
    const uint32_t next = __builtin_bswap32(*next_ptr);
    if (next <= frame || (next & 3) != 0 || next - frame > 0x100000) {
      break;
    }
    const uint32_t candidates[] = {next - 8, next + 4};
    for (uint32_t at : candidates) {
      auto* link_ptr = memory->TranslateVirtual<const uint32_t*>(at);
      if (link_ptr == nullptr) {
        continue;
      }
      const uint32_t link = __builtin_bswap32(*link_ptr);
      if (link >= 0x82000000u && link < 0x84000000u) {
        links[nlinks++] = link;
        break;
      }
    }
    frame = next;
  }

  t.samples++;
  if (have_pc) {
    t.host[tc.pc.x]++;
  } else {
    t.pause_fail++;
  }
  if (nlinks == 0) {
    t.no_stack++;
  }
  // Once per sample per address, so recursion does not count twice.
  for (int i = 0; i < nlinks; ++i) {
    bool seen = false;
    for (int j = 0; j < i; ++j) {
      if (links[j] == links[i]) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      t.guest[links[i]]++;
    }
  }
}

void WriteWindow(std::vector<Target>& targets, int window, double secs) {
  std::FILE* f = std::fopen(kOutPath, window == 1 ? "w" : "a");
  std::string summary;
  for (auto& t : targets) {
    summary += fmt::format(" {}: {} samples ({} no pc, {} no stack)", t.name, t.samples,
                           t.pause_fail, t.no_stack);
    if (f != nullptr) {
      std::fprintf(f, "W %d %.1f T %s samples=%llu pause_fail=%llu no_stack=%llu\n", window, secs,
                   t.name.c_str(), (unsigned long long)t.samples,
                   (unsigned long long)t.pause_fail, (unsigned long long)t.no_stack);
      for (const auto& [pc, n] : t.host) {
        std::fprintf(f, "H %llx %u\n", (unsigned long long)pc, n);
      }
      for (const auto& [addr, n] : t.guest) {
        std::fprintf(f, "G %08x %u\n", addr, n);
      }
    }
    t.samples = t.pause_fail = t.no_stack = 0;
    t.host.clear();
    t.guest.clear();
  }
  if (f != nullptr) {
    std::fclose(f);
  }
  REXLOG_WARN("[sampler] window {} ({:.0f}s):{}", window, secs, summary);
}

void SamplerMain(void*) {
  const auto prefixes = SplitPrefixes(REXCVAR_GET(skate3_sampler_threads));
  const int64_t period_ns = 1000000000ll / std::max(10, REXCVAR_GET(skate3_sampler_hz));
  REXLOG_WARN("[sampler] anchor SamplerAnchor=0x{:x} period={}us threads='{}'",
              uintptr_t(&SamplerAnchor), period_ns / 1000, REXCVAR_GET(skate3_sampler_threads));
  std::vector<Target> targets;
  const u64 freq = armGetSystemTickFreq();
  u64 window_start = armGetSystemTick();
  u64 last_refresh = 0;
  int window = 0;
  for (;;) {
    svcSleepThread(period_ns);
    const u64 now = armGetSystemTick();
    if (now - last_refresh > 3 * freq) {
      last_refresh = now;
      RefreshTargets(targets, prefixes);
    }
    auto* ks = rex::system::kernel_state();
    auto* memory = ks ? ks->memory() : nullptr;
    for (auto& t : targets) {
      SampleOne(t, memory);
    }
    if (now - window_start >= 30 * freq) {
      WriteWindow(targets, ++window, double(now - window_start) / double(freq));
      window_start = now;
    }
  }
}

}  // namespace

namespace skate3 {

void StartGuestSampler() {
  if (REXCVAR_GET(skate3_sampler_threads).empty()) {
    return;
  }
  // Above the guest threads (59) so a sample lands when it is due, and short
  // enough per wake that it costs them little. Any of the three game cores.
  static Thread thread;
  if (R_FAILED(threadCreate(&thread, SamplerMain, nullptr, nullptr, 0x10000, 0x2C, -2))) {
    REXLOG_WARN("[sampler] could not create the sampler thread");
    return;
  }
  svcSetThreadCoreMask(thread.handle, -3, 0x7);  // -3: keep the ideal core
  threadStart(&thread);
}

}  // namespace skate3

#else

namespace skate3 {
void StartGuestSampler() {}
}  // namespace skate3

#endif
