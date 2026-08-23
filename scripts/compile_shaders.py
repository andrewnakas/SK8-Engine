#!/usr/bin/env python3
"""Regenerate src/native/shaders/spirv/skate3_native_shaders_spirv.h with DXC.

The native renderer carries each shader twice: the HLSL text, embedded by
cmake/EmbedShaders.cmake and compiled at runtime by the D3D12 backend, and
offline SPIR-V, which is what the Vulkan backend consumes. Only the HLSL half
regenerates itself. Until now the SPIR-V half was produced by hand and the exact
invocation survived only as a comment in the generated file, so editing a shader
silently left every Vulkan build running the previous bytecode.

This reproduces that invocation. Run it after touching anything in
src/native/shaders, and commit the regenerated header:

    python3 scripts/compile_shaders.py                 # needs dxc on PATH
    python3 scripts/compile_shaders.py --dxc /path/to/dxc
    python3 scripts/compile_shaders.py --check         # report drift, write nothing

MATRIX below is the full set of (file, entry, macro variant) tuples the renderer
can request. It is the contract: MakeShaderDesc() in
src/skate3_native_scene_gpu.cpp looks blobs up by exactly these keys and logs a
named error on a miss under Vulkan. Adding a pipeline variant means adding it
here and regenerating.

The generated header also records a SHA-256 of every shader source, so
scripts/check_native_shader_spirv.py can tell that the committed bytecode no
longer matches the sources without needing a compiler.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SHADER_DIR = REPO / "src" / "native" / "shaders"
OUT_HEADER = SHADER_DIR / "spirv" / "skate3_native_shaders_spirv.h"

# The frozen invocation, from the header this script replaces. -O3 and the
# vulkan1.1 target env are load-bearing: the register bindings DXC emits under
# them are the set/binding plan the Vulkan RHI backend expects.
DXC_FLAGS = ["-spirv", "-fspv-target-env=vulkan1.1", "-O3"]

# Shader model for the offline SPIR-V. The D3D12 backend runtime-compiles the
# same sources at vs_5_0 / ps_5_0; DXC's SPIR-V path needs a 6.x profile.
PROFILES = {"vs": "vs_6_0", "ps": "ps_6_0"}

# (file, entry) -> the macro variants the renderer builds. "" is the plain
# build. Variant strings are the canonical macro signature and must match the
# strings composed at the call sites byte for byte.
MATRIX: dict[str, dict[str, list[str]]] = {
    "scene.hlsl": {
        "vs_main": [""],
        "ps_main": ["", "HDR=1", "SHOWCASE=1", "HDR=1;SHOWCASE=1"],
        "ps_shadow_caster": [""],
        "ps_shadow_caster_clip": [""],
        "ps_refl_gbuf": [""],
    },
    "shadow_blur.hlsl": {"vs_main": [""], "ps_main": [""]},
    # No unmacro'd variant: a resolve pipeline is only built when MSAA > 1.
    "resolve.hlsl": {
        "vs_main": ["SAMPLES=2", "SAMPLES=4", "SAMPLES=8"],
        "ps_main": ["SAMPLES=2", "SAMPLES=4", "SAMPLES=8"],
    },
    "blur.hlsl": {
        "vs_main": [""], "ps_main": [""], "ps_blit": [""],
        "ps_down": [""], "ps_menu_gauss": [""],
    },
    "outline.hlsl": {"vs_main": [""], "ps_main": [""]},
    "overlay2d.hlsl": {"vs_main": [""], "ps_main": [""], "ps_yuv2d": [""]},
    "spline.hlsl": {
        "vs_main": [""],
        "ps_default": ["", "HDR=1", "SHOWCASE=1", "HDR=1;SHOWCASE=1"],
        "ps_darken": ["", "HDR=1", "SHOWCASE=1", "HDR=1;SHOWCASE=1"],
    },
    "photo_fx.hlsl": {
        "vs_raw": ["", "PFX_MSAA=1"], "vs_offset": [""], "vs_scaled": [""],
        "ps_depthpack": ["", "PFX_MSAA=1"], "ps_visualfx": [""],
        "ps_dof_down": [""], "ps_dof_mb": [""], "ps_dof": [""],
        "ps_uber": [""], "ps_fisheye": [""], "ps_blit": [""],
        "ps_pfx_debug": [""],
    },
    "ssao.hlsl": {
        "vs_main": [""], "ps_linearize": ["", "AO_MSAA=1"],
        "ps_gtao": ["", "HDR=1"], "ps_blur": [""], "ps_luma": ["", "HDR=1"],
        "ps_composite": [""], "ps_debug": [""], "ps_depth_max": [""],
    },
    "hdr.hlsl": {
        "vs_main": [""], "ps_bloom_down": ["", "BLOOM_FIRST=1"],
        "ps_bloom_up": [""], "ps_tonemap": ["", "SHOWCASE=1"],
        "ps_vol_linearize": ["", "VOL_MSAA=1"], "ps_vol_shafts": [""],
    },
    "ssr.hlsl": {
        "vs_main": [""], "ps_linearize": ["", "SSR_MSAA=1"],
        "ps_march": [""], "ps_composite": ["", "SHOWCASE=1"],
    },
}


def expand_includes(name: str) -> str:
    """Inline #include "x.hlsli" the way cmake/EmbedShaders.cmake does.

    The D3D12 backend hands D3DCompile the fully expanded text with no include
    handler, so the offline SPIR-V has to be built from the same expansion or
    the two backends would not be compiling the same program.
    """
    src = (SHADER_DIR / name).read_text()
    for _ in range(33):
        match = re.search(r'#include "([^"]+)"', src)
        if not match:
            return src
        include = match.group(1)
        path = SHADER_DIR / include
        if not path.exists():
            sys.exit(f"{name}: include not found: {include}")
        src = src.replace(f'#include "{include}"', path.read_text())
    sys.exit(f"{name}: include expansion did not converge (cycle?)")


def symbol(file_name: str, entry: str, variant: str) -> str:
    stem = file_name[: -len(".hlsl")]
    suffix = re.sub(r"[^A-Za-z0-9]+", "_", variant).strip("_")
    return f"k_{stem}_{entry}" + (f"_{suffix}" if suffix else "")


def compile_one(dxc: str, source_path: pathlib.Path, entry: str,
                variant: str, label: str) -> bytes:
    prefix = entry.split("_", 1)[0]
    profile = PROFILES.get(prefix)
    if profile is None:
        sys.exit(f"{label}: cannot infer a shader profile from entry '{entry}'")
    with tempfile.NamedTemporaryFile(suffix=".spv", delete=False) as out:
        out_path = pathlib.Path(out.name)
    cmd = [dxc, *DXC_FLAGS, "-T", profile, "-E", entry]
    for macro in filter(None, variant.split(";")):
        cmd += ["-D", macro]
    cmd += ["-Fo", str(out_path), str(source_path)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{label}: dxc failed\n{result.stdout}\n{result.stderr}")
    data = out_path.read_bytes()
    out_path.unlink(missing_ok=True)
    if len(data) % 4 != 0 or len(data) == 0:
        sys.exit(f"{label}: dxc produced {len(data)} bytes, not a SPIR-V module")
    return data


def format_words(data: bytes) -> str:
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    lines = []
    for i in range(0, len(words), 8):
        chunk = ", ".join(f"0x{w:08x}" for w in words[i:i + 8])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def render_header(dxc: str, version: str) -> str:
    out = [
        "// Generated file -- DO NOT EDIT. Compiled offline with DXC from the HLSL",
        "// sources in src/native/shaders; regenerate whenever one of them changes.",
        f"// Compiler: {version}",
        f"// Flags: {' '.join(DXC_FLAGS)}, register bindings per",
        "// the frozen Vulkan set/binding plan the RHI backend expects.",
        "//",
        "// Regenerate with scripts/compile_shaders.py. The hashes below let",
        "// scripts/check_native_shader_spirv.py detect a source edit that was",
        "// never recompiled, without needing a shader compiler.",
    ]
    for path in sorted(SHADER_DIR.glob("*.hlsl")) + sorted(SHADER_DIR.glob("*.hlsli")):
        if path.name == "raytraced_mirror.hlsl":
            continue  # DXR 1.1 inline ray query, D3D12-only; compiled to DXIL by CMake.
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        out.append(f"// source-sha256: {path.name} = {digest}")
    out += [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace skate3::native_spirv {",
        "",
    ]

    rows = []
    for file_name, entries in MATRIX.items():
        source = expand_includes(file_name)
        with tempfile.TemporaryDirectory() as work:
            source_path = pathlib.Path(work) / file_name
            source_path.write_text(source)
            for entry, variants in entries.items():
                for variant in variants:
                    label = f'{file_name}:{entry} "{variant}"'
                    print(f"  {label}", file=sys.stderr)
                    data = compile_one(dxc, source_path, entry, variant, label)
                    name = symbol(file_name, entry, variant)
                    out.append(f"// {file_name} : {entry}"
                               + (f" [{variant}]" if variant else "")
                               + f" ({len(data)} bytes)")
                    out.append(f"static const uint32_t {name}[] = {{")
                    out.append(format_words(data))
                    out.append("};")
                    out.append("")
                    rows.append((file_name, entry, variant, name))

    out += [
        "struct NativeSpirvBlob {",
        "  const char* file;",
        "  const char* entry;",
        "  const char* variant;",
        "  const uint32_t* data;",
        "  size_t size_bytes;",
        "};",
        "",
        "static const NativeSpirvBlob kNativeSpirvBlobs[] = {",
    ]
    for file_name, entry, variant, name in rows:
        out.append(f'    {{"{file_name}", "{entry}", "{variant}", {name}, sizeof({name})}},')
    out += [
        "};",
        "",
        "static const size_t kNativeSpirvBlobCount =",
        "    sizeof(kNativeSpirvBlobs) / sizeof(kNativeSpirvBlobs[0]);",
        "",
        "}  // namespace skate3::native_spirv",
        "",
    ]
    return "\n".join(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", default=shutil.which("dxc"),
                        help="path to the DirectX Shader Compiler")
    parser.add_argument("--check", action="store_true",
                        help="compare against the committed header, write nothing")
    args = parser.parse_args()

    if not args.dxc:
        sys.exit("dxc not found on PATH; pass --dxc /path/to/dxc.\n"
                 "DXC ships with the Vulkan SDK (LunarG) and the Windows SDK.")

    version = subprocess.run([args.dxc, "--version"], capture_output=True,
                             text=True).stdout.strip().replace("\n", " ")
    header = render_header(args.dxc, version or "unknown dxc")

    if args.check:
        current = OUT_HEADER.read_text(errors="replace")
        # The compiler banner legitimately differs between machines.
        strip = lambda t: "\n".join(
            l for l in t.splitlines() if not l.startswith("// Compiler:"))
        if strip(current) == strip(header):
            print("SHADER_SPIRV_UP_TO_DATE")
            return
        sys.exit("SHADER_SPIRV_STALE: the committed header does not match the "
                 "sources; rerun without --check and commit the result")

    OUT_HEADER.write_text(header)
    print(f"wrote {OUT_HEADER.relative_to(REPO)}")


if __name__ == "__main__":
    main()
