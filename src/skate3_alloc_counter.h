#pragma once

// Global allocation counter; see skate3_alloc_counter.cpp for why this exists.
#include <cstdint>

namespace skate3::alloc_counter {

struct AllocSnapshot {
  uint64_t allocs;
  uint64_t frees;
  uint64_t bytes;
};

// Cumulative since process start. Difference two snapshots for a window.
AllocSnapshot Read();

}  // namespace skate3::alloc_counter
