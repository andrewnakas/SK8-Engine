# Performance builds

Two things make a build fast, and they are independent:

1. **Video presets**, chosen from the hardware at runtime. Nothing to compile.
2. **`SKATE3_ENABLE_X86_64_V3`**, an AVX2 build of the recompiled PowerPC code.
   This one is a compile-time choice and it is per platform.

The presets ship in every build. The AVX2 flag is what a "performance build"
means here, and it has to be set per platform because each has its own toolchain.

---

## What AVX2 actually buys

The recompiled code emulates the Xbox 360's AltiVec vector unit, which is
exactly the work AVX2 accelerates. Measured on a discrete RTX 4050 - the CPU is
the limit there, so the difference is visible:

| build | fps |
|---|---|
| generic `x86-64` baseline (SSSE3) | 186.7 |
| `x86-64-v3` (AVX2) | **225.2** |

**+20.6%.** On an integrated GPU it changes nothing (36.0 -> 36.2), because
that case is GPU-bound - see `RELEASE_README.md` for those numbers. It matters
most where the CPU is weakest and the GPU is not, which is a Steam Deck.

**The cost:** AVX2 needs a 2013-or-newer CPU (Intel Haswell, AMD Zen). Every
Steam Deck, every modern handheld, any recent laptop. A genuinely old desktop
would no longer start. Ship the baseline build too if that matters to you.

---

## Building one, per platform

All three need the game data on the build machine: the build recompiles
`default.xex` and `EAWebkit.xex` and needs the TU3 package. None of it may end
up in the archive - `packaging/make_linux_release.sh` refuses to package any
`.xex`, `.xexp`, `.big`, `.header` or `.iso`, and the CI workflow has the same
guard.

### Linux — DONE, measured

```sh
cmake --preset linux-release -DSKATE3_ENABLE_X86_64_V3=ON \
  -DSKATE3_GAME_DATA_ROOT=<your game dir> \
  -DSKATE3_TITLE_UPDATE_PACKAGE=<your TU3 package>
cmake --build --preset linux-release --parallel
packaging/make_linux_release.sh out/release
```

Toolchain: clang-20 + lld-20 from Ubuntu's own repos. Verify the result carries
AVX2 rather than trusting the build log:

```sh
objdump -d out/build/linux-release/skate3 | grep -cE 'vfmadd|vpermd'   # ~27000, not 0
```

### Windows — DONE, measured

```bat
:: from an x64 Native Tools Command Prompt, so clang-cl finds the MSVC headers
cmake --preset windows-release -DSKATE3_ENABLE_X86_64_V3=ON ^
  -DSKATE3_GAME_DATA_ROOT=<your game dir> ^
  -DSKATE3_TITLE_UPDATE_PACKAGE=<your TU3 package>
cmake --build --preset windows-release --parallel
```

Uses **clang-cl**: the SDK requires a Clang compiler id, and Windows needs the
MSVC ABI for D3D12 and the Win32 headers. clang-cl is both. D3D12 and Vulkan
both build; `gpu_backend` picks at runtime.

The bug already fixed before the first build: clang-cl reports its compiler id
as `Clang` while taking MSVC-style arguments, and the arch-flag branch tested
Clang before MSVC - so an AVX2 Windows build would have been handed
`-march=x86-64-v3`, which the cl driver rejects. MSVC is tested first now.

Three more real bugs surfaced getting the first Windows build to link, none of
them Windows-only hacks - all fixed on the shared source:

- `xboxkrnl_ob.cpp` passed a `u8"..."` (`char8_t`) literal into a
  `std::string_view` parameter - a hard C++20/23 type mismatch GCC let through
  and clang-cl did not.
- `rexglue-sdk`'s global compile options passed `-ffp-model=strict` and
  `-fno-char8_t` to clang-cl in their GCC spelling, which it rejects outright;
  routed through `/clang:` instead. A redundant `-O3` alongside clang-cl's own
  `/O2` default tripped `-Wunused-command-line-argument` and failed any target
  also building with `/WX` (SPIRV-Tools-opt).
- `skate3_heap_check.cpp` (the guest heap-corruption checker, off by default)
  used glibc-only `<execinfo.h>`/`backtrace()`/`prctl()` unconditionally; added
  real Windows equivalents (`RtlCaptureStackBackTrace`, `GetCurrentThreadId`)
  rather than stubbing the diagnostic out on this platform.

Verify AVX2 landed - `/arch:AVX2` is the Windows spelling, so the Linux
objdump check does not apply:

```bat
dumpbin /disasm out\build\windows-release\skate3.exe | findstr /R "vfmadd vpermd"
```

17,390 hits on the first successful build - not a marginal signal.

Artifacts are `skate3.exe` and `rexruntime.dll` (the workflow's allowlist
expects exactly those two names).

#### Measured: discrete vs. integrated, and how far the presets go

First real Windows numbers, one laptop with both a discrete GPU and an Intel
iGPU as separate DXGI adapters (`d3d12_adapter=0` / `=1`), AVX2 build,
uncapped (`skate3_guest_fps_cap_auto=false skate3_guest_fps_cap=0`), measured
via the engine's own `skate3_native_render_scene_perf_log=true` in real
gameplay (Career mode) rather than an idle menu:

| GPU | config | avg fps | range |
|---|---|---|---|
| NVIDIA RTX 4050 (discrete) | `quality` preset (2x2 supersample, 4x MSAA, full effects) | 159 | 120-185 |
| NVIDIA RTX 4050 (discrete) | `performance` preset | 167 | 128-208 |
| NVIDIA RTX 4050 (discrete) | `performance` + HDR/haze off + draw/LOD distance 0.5x + AF off | **255** | 163-433 |
| Intel UHD (integrated) | `performance` preset | 177 | 148-200 |

(The discrete max-performance row is the reliable one: 394 gameplay samples over
~20 minutes of varied Career mode play, filtered to frames actually drawing
scene content so menu/idle frames - which read a meaningless 400-690 "fps" on
an almost-empty screen - don't skew it. The other three rows are smaller
samples and read directionally correct but noisier.)

Two things fall out of this:

- **On this discrete GPU, the game is CPU-bound, not GPU-bound.** Switching
  `quality` -> `performance` alone only bought ~5-8%; that matches the AVX2
  table above showing AVX2 doing the real work on a discrete GPU while
  changing nothing on an iGPU. The preset system's biggest wins are aimed at
  GPU-bound machines, i.e. integrated graphics.
- **HDR and haze are not part of any preset** - they default on regardless of
  `skate3_performance_profile`, and turning them off (haze is a minor extra
  pass; HDR gates the format/cost of several others even with bloom/shafts
  already off) plus halving draw/LOD distance pushed fps to **+53%** over the
  plain `performance` preset. Pushing draw/LOD distance to the engine's floor
  (0.25x) and disabling lightmaps too was tried and rejected: no further
  measured fps gain over 0.5x, and lightmaps off looks noticeably worse
  (flat, unlit geometry) for nothing in return.

Max-performance launch line (discrete or integrated, adjust `d3d12_adapter`):

```bat
skate3.exe --skate3_performance_profile=performance ^
  --skate3_native_render_scene_haze=false --skate3_native_render_scene_hdr=false ^
  --skate3_draw_distance_scale=0.5 --skate3_lod_distance_scale=0.5 ^
  --anisotropic_override=0
```

What to expect to go wrong, in likely order, for anyone building this fresh:

- `clang-cl` not on PATH, or run outside the Native Tools prompt (no MSVC
  headers). This is the usual first failure.
- Ninja not installed, or an MSVC/clang-cl version mismatch.
- Codegen: it is ~4 minutes and 113 generated `.cpp` files on Linux; the same
  step has to succeed here.
- **Smart App Control** (on by default on many fresh Windows 11 installs)
  blocks a freshly-compiled, unsigned `skate3.exe`/`rexruntime.dll` from
  launching at all - a "Bad Image" dialog whose error code decodes to the
  Code Integrity facility, not a corrupt build. Off is one-way without a
  Windows reinstall, so this is a call for whoever owns the machine, not
  something to flip automatically.

### macOS — IN PROGRESS

```sh
cmake --preset macos-release -DSKATE3_ENABLE_X86_64_V3=ON \
  -DSKATE3_GAME_DATA_ROOT=<your game dir> \
  -DSKATE3_TITLE_UPDATE_PACKAGE=<your TU3 package>
cmake --build --preset macos-release --parallel
```

**On Apple Silicon `SKATE3_ENABLE_X86_64_V3` does nothing** and should be left
off: the flag is gated on `CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64"`, and
the block is skipped on Apple entirely (`if(NOT APPLE AND ...)`). An arm64 mac
gets the video presets and no AVX2 build, because there is no AVX2 - the NEON
equivalent would be a separate piece of work in the recompiler. An Intel mac
would take the flag.

Toolchain is Homebrew LLVM (`/opt/homebrew/opt/llvm`), deployment target 12.0.
The archive needs `libMoltenVK.dylib` and `MoltenVK_icd.json` beside the binary,
and the workflow ad-hoc codesigns both the executable and the dylibs.

---

## Releasing them together

`engine-release.yml` builds all three on self-hosted runners and uploads into an
existing draft release, one asset per platform:

```
skate3-engine-<version>-linux-x86_64.tar.gz
skate3-engine-<version>-windows-x86_64.zip
skate3-engine-<version>-macos-arm64.zip
```

It is `workflow_dispatch`-only, and that trigger is the guard that actually
holds: the repository is public and the runners are personal machines holding
the game data, so a fork's pull request must never be able to run on them.

Say which platforms are ready in the dispatch input rather than uploading a
build nobody has run. A missing asset is easy to add later; a broken one that
people have already downloaded is not.
