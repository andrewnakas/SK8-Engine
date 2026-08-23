#!/usr/bin/env python3
"""Static sanity checks on the committed native-renderer SPIR-V table.

The D3D12 backend compiles the embedded HLSL at runtime, so on Windows a shader
edit is picked up automatically. The Vulkan backend consumes the offline blobs
in src/native/shaders/spirv/skate3_native_shaders_spirv.h instead, looked up by
(file, entry, variant). A miss leaves ShaderDesc::spirv null, which fails Vulkan
pipeline creation and does nothing at all on D3D12 - so drift is easy to ship.

The full (file, entry, variant) matrix cannot be recovered by reading the C++:
several passes build their entry names from arrays and their variants from
runtime state. MakeShaderDesc() logs the exact missing tuple at runtime when the
Vulkan backend hits a miss; that is the authoritative check. This script covers
only what can be established without a GPU or a shader compiler:

  1. every .hlsl embedded for D3D12 has at least one blob;
  2. the table has no duplicate (file, entry, variant) rows;
  3. every blob names a shader file that still exists;
  4. recorded HLSL source hashes still match, once the header carries them.

Exits non-zero on any mismatch.
    python3 scripts/check_native_shader_spirv.py
"""

from __future__ import annotations

import collections
import hashlib
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SHADER_DIR = REPO / "src" / "native" / "shaders"
SPIRV_HEADER = SHADER_DIR / "spirv" / "skate3_native_shaders_spirv.h"
CMAKELISTS = REPO / "CMakeLists.txt"

TABLE_ROW = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*[A-Za-z_0-9]+\s*,\s*sizeof\(')


def fail(problems: list[str]) -> None:
    print("SHADER_SPIRV_FAIL")
    for problem in problems:
        print(f"  - {problem}")
    sys.exit(1)


def parse_table() -> list[tuple[str, str, str]]:
    text = SPIRV_HEADER.read_text(errors="replace")
    parts = text.split("kNativeSpirvBlobs[]", 1)
    if len(parts) != 2:
        fail([f"{SPIRV_HEADER.name}: kNativeSpirvBlobs table not found"])
    return [m.groups() for m in TABLE_ROW.finditer(parts[1])]


def embedded_shader_files() -> list[str]:
    text = CMAKELISTS.read_text()
    block = re.search(r"set\(SKATE3_NATIVE_SHADER_SOURCES(.*?)\n\)", text, re.S)
    if not block:
        fail(["CMakeLists.txt: SKATE3_NATIVE_SHADER_SOURCES not found"])
    return re.findall(r"shaders/([A-Za-z_0-9]+\.hlsl)", block.group(1))


def check_source_hashes(problems: list[str]) -> str:
    """Verify the per-source hashes, if scripts/compile_shaders.py recorded them."""
    head = SPIRV_HEADER.read_text(errors="replace")[:16384]
    recorded = dict(
        re.findall(r"//\s*source-sha256:\s*(\S+)\s*=\s*([0-9a-f]{64})", head))
    if not recorded:
        return ("source hashes not recorded; regenerate with "
                "scripts/compile_shaders.py to enable content checking")
    for name, want in sorted(recorded.items()):
        path = SHADER_DIR / name
        if not path.exists():
            problems.append(f"source hash recorded for missing shader {name}")
            continue
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        if got != want:
            problems.append(
                f"{name} changed since the SPIR-V was generated (recorded "
                f"{want[:12]}, on disk {got[:12]}); rerun "
                f"scripts/compile_shaders.py")
    return f"{len(recorded)} source hashes verified"


def main() -> None:
    problems: list[str] = []
    table = parse_table()
    if not table:
        fail(["the SPIR-V blob table parsed as empty"])

    duplicates = [key for key, count
                  in collections.Counter(table).items() if count > 1]
    for key in sorted(duplicates):
        problems.append(
            f'duplicate table row {key[0]}:{key[1]} variant "{key[2]}"; the '
            f"lookup takes the first match, so the later blob is dead")

    covered = {f for f, _, _ in table}
    for name in sorted(covered):
        if not (SHADER_DIR / name).exists():
            problems.append(f"table names {name}, which no longer exists")

    embedded = embedded_shader_files()
    if not embedded:
        fail(["SKATE3_NATIVE_SHADER_SOURCES parsed as empty"])
    for name in embedded:
        if name not in covered:
            problems.append(
                f"{name} is embedded for the D3D12 runtime compiler but has no "
                f"SPIR-V at all; every pipeline using it will fail on Vulkan")

    hash_note = check_source_hashes(problems)

    if problems:
        fail(problems)

    print(f"SHADER_SPIRV_PASS {len(table)} blobs over {len(covered)} shaders; "
          f"{len(embedded)} embedded sources all covered")
    print(f"  {hash_note}")


if __name__ == "__main__":
    main()
