#pragma once

// Global allocation counter; see skate3_alloc_counter.cpp for why this exists.
#include <cstdint>

namespace skate3::alloc_counter {

struct AllocSnapshot {
  uint64_t allocs;
  uint64_t frees;
  uint64_t bytes;
};

// Where an allocation happened. The counter said 3000 allocations a frame
// during every frame-rate collapse against 588 when the frame is healthy, and
// at ~6.7 us a malloc/free pair that is 20 ms of allocator - but a total
// cannot say WHERE. This can. Phases are set by a scope on the thread doing
// the work, so a thread with no scope lands in kOther, which is itself an
// answer.
enum class Phase : uint8_t {
  kOther = 0,
  kSceneBuild,   // BuildFrameScene, on the guest render thread
  kRenderScene,  // RenderScene, on the command processor
  kDecode,       // mesh/texture decode
  kTwoD,         // 2D capture and replay
  kCount,
};

// Sets the calling thread's phase for its lifetime. Restores the previous one,
// so scopes nest.
class ScopedPhase {
 public:
  explicit ScopedPhase(Phase p);
  ~ScopedPhase();
  ScopedPhase(const ScopedPhase&) = delete;
  ScopedPhase& operator=(const ScopedPhase&) = delete;

 private:
  uint8_t prev_;
};

// Cumulative allocation count for one phase.
uint64_t PhaseAllocs(Phase p);

// Cumulative since process start. Difference two snapshots for a window.
AllocSnapshot Read();

}  // namespace skate3::alloc_counter
