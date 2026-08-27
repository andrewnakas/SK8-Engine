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

### Windows — PRESET ADDED, NEVER BUILT

**Status: unverified.** `windows-release` did not exist until now, so the
`engine-release.yml` Windows job has never been able to run - it has always
invoked a preset the repo did not define. The preset now exists and one real
bug in its path is fixed, but nobody has compiled it yet.

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

The bug already fixed: clang-cl reports its compiler id as `Clang` while taking
MSVC-style arguments, and the arch-flag branch tested Clang before MSVC - so an
AVX2 Windows build would have been handed `-march=x86-64-v3`, which the cl
driver rejects. MSVC is tested first now.

What to expect to go wrong, in likely order:

- `clang-cl` not on PATH, or run outside the Native Tools prompt (no MSVC
  headers). This is the usual first failure.
- Ninja not installed, or an MSVC/clang-cl version mismatch.
- Codegen: it is ~4 minutes and 113 generated `.cpp` files on Linux; the same
  step has to succeed here.
- Verify AVX2 landed - `/arch:AVX2` is the Windows spelling, so the Linux
  objdump check does not apply. `dumpbin /disasm` or a debugger will show
  `vfmadd`/`vpermd`.

Artifacts are `skate3.exe` and `rexruntime.dll` (the workflow's allowlist
expects exactly those two names).

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
