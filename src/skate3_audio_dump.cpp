/**
 * @file        skate3_audio_dump.cpp
 * @brief       audio_dump_path: a bit-exact capture of every guest mixer submit
 *
 * This is the oracle the exactness work is measured against. The guest mixer hands the
 * host one frame at a time through XAudioSubmitRenderDriverFrame: 256 samples x 6
 * channels of big-endian float32, planar, 6144 bytes. `sub_82F10980` builds that buffer
 * by copying the mixer's interleaved output into a planar stack buffer and submits it.
 * Nothing between the guest mixer and this call touches the samples; the 5.1 fold, if
 * any, happens later inside the host audio driver.
 *
 * The tap overrides the kernel import in the executable. The SDK implementation lives in
 * librexruntime.so, so the executable's definition wins for every guest call site and
 * the real one is still reachable through dlsym(RTLD_NEXT). The frame is copied before
 * forwarding, so what is written is exactly what the guest handed over.
 *
 * Output is raw 6144-byte records, the layout tools/mixdiff.py reads. Each record is
 * flushed as it is written, because the game ignores SIGTERM and a killed session should
 * still leave whole records on disk.
 *
 * The tap runs on the audio worker thread, inside a guest call, with that thread's
 * floating-point control state -- and there FP exceptions are NOT masked. The first
 * version of this file divided a double on close and took SIGFPE at the divsd, measured
 * under gdb. So all host work here runs inside FloatExceptionsMasked, which masks every FP
 * exception and puts the guest's control and sticky flags back on the way out. The MXCSR
 * found on entry is logged once, on the first submit.
 */
#include <dlfcn.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform/fpscr.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_STRING(
    audio_dump_path, "", "Audio",
    "Write every guest mixer submit to this file: 256 frames x 6 channels of big-endian "
    "float32, planar, 6144 bytes per submit, before any host downmix. This is the "
    "reference tools/mixdiff.py compares against. Empty disables the tap.");

REXCVAR_DEFINE_UINT32(
    audio_dump_max_frames, 4000, "Audio",
    "Stop audio_dump_path after this many submits (4000 is 21.3 s). 0 records until exit.");

namespace {

constexpr uint32_t kFrames = 256;
constexpr uint32_t kChannels = 6;
constexpr uint32_t kSubmitBytes = kFrames * kChannels * sizeof(float);
constexpr const char* kImport = "__imp__XAudioSubmitRenderDriverFrame";

struct DumpState {
  std::mutex mutex;
  std::FILE* file = nullptr;
  bool done = false;
  uint64_t submits = 0;
  uint64_t silent = 0;
  uint64_t non_finite = 0;
  float peak = 0.0f;
};

DumpState g_dump;

// Masks every host FP exception for the lifetime of the scope and restores the exact
// control word on exit, sticky flags included, so the tap is invisible to guest FP state.
class FloatExceptionsMasked {
 public:
  FloatExceptionsMasked() : saved_(rex::platform::FPSCRPlatform::getcsr()) {
    uint32_t masked = saved_;
    rex::platform::FPSCRPlatform::InitHostExceptions(masked);
    rex::platform::FPSCRPlatform::setcsr(masked);
  }
  ~FloatExceptionsMasked() { rex::platform::FPSCRPlatform::setcsr(saved_); }
  FloatExceptionsMasked(const FloatExceptionsMasked&) = delete;
  FloatExceptionsMasked& operator=(const FloatExceptionsMasked&) = delete;
  uint32_t entry_csr() const { return saved_; }

 private:
  uint32_t saved_;
};

PPCFunc* ResolveOriginal() {
  auto* original = reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, kImport));
  if (original == nullptr || original == &__imp__XAudioSubmitRenderDriverFrame) {
    const char* why = dlerror();
    REXLOG_ERROR("audio_dump: cannot resolve the runtime's {} ({}); audio cannot be "
                 "forwarded",
                 kImport, why ? why : "resolved to this override");
    std::abort();
  }
  return original;
}

void CloseLocked(const char* reason) {
  if (g_dump.file) {
    std::fclose(g_dump.file);
    g_dump.file = nullptr;
  }
  g_dump.done = true;
  REXLOG_INFO("audio_dump: {} - {} submits ({} bytes, {:.2f} s), {} all-silent, "
              "{} non-finite samples, peak {}",
              reason, g_dump.submits, g_dump.submits * kSubmitBytes,
              double(g_dump.submits) * kFrames / 48000.0, g_dump.silent, g_dump.non_finite,
              g_dump.peak);
}

void Tap(uint8_t* base, uint32_t samples_ptr) {
  static const std::string path = REXCVAR_GET(audio_dump_path);
  if (path.empty() || samples_ptr == 0) {
    return;
  }
  static const uint64_t cap = REXCVAR_GET(audio_dump_max_frames);

  const FloatExceptionsMasked fp_guard;
  std::lock_guard<std::mutex> lock(g_dump.mutex);
  if (g_dump.done) {
    return;
  }
  if (!g_dump.file) {
    g_dump.file = std::fopen(path.c_str(), "wb");
    if (!g_dump.file) {
      REXLOG_ERROR("audio_dump: cannot open '{}'", path);
      g_dump.done = true;
      return;
    }
    REXLOG_INFO("audio_dump: recording to '{}' (first submit at {:08X}, cap {}, host FP "
                "control word on entry {:#06x})",
                path, samples_ptr, cap, fp_guard.entry_csr());
  }

  const uint8_t* frame = base + samples_ptr;
  if (std::fwrite(frame, 1, kSubmitBytes, g_dump.file) != kSubmitBytes ||
      std::fflush(g_dump.file) != 0) {
    REXLOG_ERROR("audio_dump: write to '{}' failed", path);
    CloseLocked("stopped on write error");
    return;
  }
  g_dump.submits++;

  bool silent = true;
  for (uint32_t i = 0; i < kFrames * kChannels; i++) {
    uint32_t bits;
    std::memcpy(&bits, frame + i * 4, 4);
    const float v = std::bit_cast<float>(__builtin_bswap32(bits));
    if (!std::isfinite(v)) {
      g_dump.non_finite++;
      silent = false;
      continue;
    }
    if (v != 0.0f) silent = false;
    g_dump.peak = std::max(g_dump.peak, std::fabs(v));
  }
  if (silent) g_dump.silent++;

  if (cap != 0 && g_dump.submits >= cap) {
    CloseLocked("reached audio_dump_max_frames");
  }
}

}  // namespace

extern "C" REX_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
  static PPCFunc* const original = ResolveOriginal();
  Tap(base, ctx.r4.u32);
  original(ctx, base);
}
