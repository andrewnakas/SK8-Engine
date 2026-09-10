// Global allocation counter.
//
// Removing ~750 per-frame allocations from the world walk took the hook cost
// from ~10 ms a frame to ~5 and the frame from 47-50 ms to 39-42. That put a
// malloc/free pair on this arena at roughly 6.7 us, which is pathological and
// is explained by its shape: 2.7-2.9 GB in use, 6-22 MB free, and a free-chunk
// count that climbs from ~2,900 to ~26,000 over three minutes of play.
//
// The lever is proven, so the question is where the REST of the traffic is.
// Guessing from the source has already been tried and it is slow; this counts
// instead. Overriding the global operators catches every C++ allocation -
// std::vector, std::string, unordered_map nodes, shared_ptr control blocks -
// across our code and the SDK's, which is exactly the population that matters.
// Guest-side allocations are not included: those run on the emulated guest
// heap, not this one.
//
// Cost when nobody is reading: two relaxed atomic increments per allocation.
// That is far below what the allocations themselves cost here.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#include "skate3_alloc_counter.h"

namespace skate3::alloc_counter {

// Per-thread current phase. Thread-local so the command processor and the
// guest render thread attribute independently with no synchronisation.
thread_local uint8_t g_phase = 0;
std::atomic<uint64_t> g_phase_allocs[size_t(Phase::kCount)] = {};

ScopedPhase::ScopedPhase(Phase p) : prev_(g_phase) { g_phase = uint8_t(p); }
ScopedPhase::~ScopedPhase() { g_phase = prev_; }

uint64_t PhaseAllocs(Phase p) {
  return g_phase_allocs[size_t(p)].load(std::memory_order_relaxed);
}

std::atomic<uint64_t> g_allocs{0};
std::atomic<uint64_t> g_frees{0};
std::atomic<uint64_t> g_bytes{0};

AllocSnapshot Read() {
  return AllocSnapshot{g_allocs.load(std::memory_order_relaxed),
                       g_frees.load(std::memory_order_relaxed),
                       g_bytes.load(std::memory_order_relaxed)};
}

}  // namespace skate3::alloc_counter

namespace {
inline void* CountedAlloc(std::size_t n) {
  skate3::alloc_counter::g_allocs.fetch_add(1, std::memory_order_relaxed);
  skate3::alloc_counter::g_bytes.fetch_add(n, std::memory_order_relaxed);
  skate3::alloc_counter::g_phase_allocs[skate3::alloc_counter::g_phase].fetch_add(
      1, std::memory_order_relaxed);
  // A zero-size new must still return a distinct pointer.
  return std::malloc(n != 0 ? n : 1);
}
inline void CountedFree(void* p) {
  if (p == nullptr) return;
  skate3::alloc_counter::g_frees.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}
}  // namespace

// The throwing forms report failure the way the standard requires; this port
// runs with ~10-20 MB free, so a null return here is a real possibility and
// must not be silently handed back as a valid pointer.
void* operator new(std::size_t n) {
  void* p = CountedAlloc(n);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  void* p = CountedAlloc(n);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return CountedAlloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return CountedAlloc(n); }
void operator delete(void* p) noexcept { CountedFree(p); }
void operator delete[](void* p) noexcept { CountedFree(p); }
void operator delete(void* p, std::size_t) noexcept { CountedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { CountedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
