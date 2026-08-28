# Building the Windows performance release

Hand this file to a Claude session on the Windows machine. It is written to be
the whole brief: what to install, what to copy over, what to run, and how to
tell whether it worked.

**Nobody has ever built this on Windows.** `windows-release` did not exist as a
CMake preset until recently, so the CI job invoked a preset the repo did not
define. The preset now exists and one bug in its path is fixed, but the first
successful Windows build will be yours. Expect to debug; the last section lists
what is most likely to break, in order.

---

## 1. What to install

| tool | why | check |
|---|---|---|
| Visual Studio 2022 Build Tools, **"Desktop development with C++"** | the MSVC headers, libs and the Windows SDK (D3D12) | — |
| **clang-cl** — the "C++ Clang tools for Windows" component in that same installer | the SDK hard-requires a Clang compiler id; clang-cl is Clang with an MSVC-style driver | `clang-cl --version` |
| **Ninja** | the generator every preset uses | `ninja --version` |
| **CMake** ≥ 3.25 | presets v6 | `cmake --version` |
| **Python 3** | the title-update extraction step runs a Python script | `python --version` |
| **Git** | submodules | `git --version` |

**Run every command from an "x64 Native Tools Command Prompt for VS 2022"**, or
from PowerShell after importing that environment. Outside it, clang-cl cannot
find the MSVC headers, and that is the single most common failure.

To use PowerShell:

```powershell
# adjust the edition (Community/Professional/BuildTools) to match your install
Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -DevCmdArguments "-arch=x64 -host_arch=x64" -SkipAutomaticLocation
```

---

## 2. What to copy from the Linux side

The build **recompiles the game**, so it needs the retail executables on the
build machine. It reads exactly two files out of the game root plus the title
update package — about **13 MB total, not the 6 GB game directory**:

| file | size | on the Linux partition |
|---|---|---|
| `default.xex` | 6.6 MB | `Documents/skate3/Skate3Recomp-Linux/game/default.xex` |
| `data/webkit/EAWebkit.xex` | 4.9 MB | `Documents/skate3/Skate3Recomp-Linux/game/data/webkit/EAWebkit.xex` |
| TU3 package `TU_12K2276_000000C000000.00000000000O3` | 1.7 MB | `.local/share/skate3/title_update/` |

Lay the two xex files out on Windows preserving that relative structure, e.g.

```
C:\skate3\game\default.xex
C:\skate3\game\data\webkit\EAWebkit.xex
C:\skate3\tu\TU_12K2276_000000C000000.00000000000O3
```

**To actually play** the result you need the full game data as well — the same
`game\` directory contents the Linux build uses, or let the engine's own ISO
installer extract them from your disc image on first run.

**None of this may end up in a release archive.** The packaging script and the
CI workflow both refuse to archive any `.xex`, `.xexp`, `.big`, `.header` or
`.iso`, and that guard is not optional — the repository is public.

---

## 3. Getting the source

Two repositories, both forks, both on a branch:

```powershell
git clone --recursive -b perf/video-presets-and-dlc-picker `
  https://github.com/andrewnakas/skate3recomp.git
cd skate3recomp
```

`--recursive` matters. `.gitmodules` on this branch points at
`andrewnakas/rexglue-skate3` branch `perf/video-presets-and-quiet-logs`, because
the pinned SDK commit (`f377fe6`) only exists on that fork. A non-recursive
clone builds nothing.

Verify the submodule actually resolved before going further:

```powershell
git -C third_party\rexglue-sdk rev-parse HEAD    # expect f377fe6...
```

The upstream clone has two known traps that a fresh clone may hit — a dead imgui
submodule pin and a `RasterizerGamma` field that exists in no stock imgui. If
`--recursive` fails partway, that is what you are looking at; see the project
notes rather than guessing.

---

## 4. Building

```powershell
cmake --preset windows-release -DSKATE3_ENABLE_X86_64_V3=ON `
  -DSKATE3_GAME_DATA_ROOT=C:/skate3/game `
  -DSKATE3_TITLE_UPDATE_PACKAGE=C:/skate3/tu/TU_12K2276_000000C000000.00000000000O3

cmake --build --preset windows-release --target generate-all --parallel
cmake --preset windows-release -DSKATE3_ENABLE_X86_64_V3=ON `
  -DSKATE3_GAME_DATA_ROOT=C:/skate3/game `
  -DSKATE3_TITLE_UPDATE_PACKAGE=C:/skate3/tu/TU_12K2276_000000C000000.00000000000O3
cmake --build --preset windows-release --parallel
```

Forward slashes in the CMake paths — backslashes are escape characters to CMake.

**Why configure twice.** `generate-all` runs the recompiler and writes ~113
`.cpp` files into `generated\`. That directory is globbed at configure time, so
the first configure does not see the files that do not exist yet; the second one
picks them up. On Linux codegen is about 4 minutes and the full build about 10.

`SKATE3_ENABLE_X86_64_V3=ON` is what makes it a *performance* build: it targets
AVX2, which the recompiled PowerPC vector code uses directly. Measured on Linux,
same machine, discrete GPU: **225.2 fps against 186.7** without it. The cost is
that it needs a 2013-or-newer CPU (Haswell / Zen). Build the baseline too, by
leaving the flag off, if you want to support older machines.

Artifacts land in `out\build\windows-release\`:

- `skate3.exe`
- `rexruntime.dll`

Those two names are exactly what the CI workflow's allowlist copies, so do not
rename them.

---

## 5. Checking it worked

**Verify the artifact, never the build log.** A wrapper that greps for "error"
prints success on a failed link, and the next test then silently runs the
previous binary.

```powershell
# 1. it is fresh
Get-Item out\build\windows-release\skate3.exe | Select-Object LastWriteTime, Length

# 2. AVX2 really landed (should print many hits, not zero)
dumpbin /disasm out\build\windows-release\skate3.exe | Select-String -Pattern "vfmadd|vpermd" | Measure-Object

# 3. the new code is in there
Select-String -Path out\build\windows-release\rexruntime.dll -Pattern "Handheld / iGPU" -Encoding Byte
```

Then run it. The things to confirm, in order:

1. It reaches gameplay at all.
2. **Escape → Video → Preset** — the row is first on the page. Stepping it
   changes Render Scale and the effect rows below it, live.
3. `--skate3_performance_profile=deck` on the command line applies at startup,
   and the console says `Skate 3 video preset: deck`.
4. An explicit cvar beats the preset:
   `--skate3_performance_profile=deck --skate3_native_render_scene_msaa=4`
   should render at MSAA x4. The log line
   `native-scene: pipelines created (MSAA x4, HDR)` is the ground truth here —
   the settings file is not, because a killed process never writes it back.
5. Drop a map pack in `dlc\` and confirm the add-on picker appears.

Windows also has a graphics API choice Linux does not: both D3D12 and Vulkan are
built, and `gpu_backend` picks at runtime. Worth measuring both — the Video page
exposes it as "Graphics API".

---

## 6. What will probably break first

In rough order of likelihood:

1. **`clang-cl` not found, or MSVC headers not found.** You are not in the
   x64 Native Tools environment. This is the usual first failure.
2. **CMake picks MSVC `cl.exe` instead of clang-cl**, and the SDK stops with
   "ReXGlue requires Clang compiler". The preset sets both
   `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` to `clang-cl`; if it is not on
   PATH, pass an absolute path to it.
3. **Codegen fails or produces nothing.** On Linux the rexglue tool segfaults on
   *teardown* after writing all its output correctly, which makes ninja abort
   with exit 139 even though the codegen succeeded. If that happens on Windows,
   check whether `generated\` filled up before assuming failure — and if it did,
   run the two codegen steps directly instead of through `generate-all`.
4. **The second configure still says `generated/sources.cmake not found`.**
   Codegen did not actually write. Do not proceed; the link will fail in a much
   more confusing way.
5. **D3D12 or Windows SDK headers missing** — the "Desktop development with C++"
   workload was installed without the Windows 10/11 SDK component.

If something needs a real fix rather than a setup correction, it belongs in a
commit on `perf/video-presets-and-dlc-picker`, not a local patch — the whole
point is that the Windows build is reproducible afterwards.

---

## 7. Publishing it

There is a **draft** release at
`andrewnakas/skate3-level-loader` tagged `v0.1.3` with the Linux asset already
attached. Add Windows to it as:

```
skate3-engine-0.1.3-windows-x86_64.zip
```

containing just `skate3.exe` and `rexruntime.dll` (plus the launcher docs if you
want parity with the Linux archive), and update `SHA256SUMS`. Then say so in the
release notes — the notes currently state plainly that Windows is unbuilt and
untested, and that line should only change when it is neither.

`engine-release.yml` can do this on a self-hosted Windows runner tagged
`[self-hosted, skate3-engine, Windows, X64]`; it is `workflow_dispatch`-only,
which is the guard that keeps a public repo's forks off your hardware.
