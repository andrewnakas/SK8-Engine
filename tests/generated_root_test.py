"""Exercise real codegen CMake wiring with a synthetic, game-free recompiler.

Run with Python 3.11+ and CMake 3.25+: python3 tests/generated_root_test.py
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
MARKERS = "// Skate3UltrawideGameFrustumPatchScope\n// Skate3MaybeOverrideProjectionFovRadians\n// ShouldForceIntroMovieComplete\n"


class GeneratedRootTest(unittest.TestCase):
    def test_external_root_through_generate_all(self):
        with tempfile.TemporaryDirectory(prefix="skate3 codegen ") as directory:
            base = pathlib.Path(directory)
            source = base / "source"
            source.mkdir()
            external = base / "external directory/generated"
            build = base / "build"
            (source / "cmake").mkdir()
            shutil.copy2(ROOT / "cmake/ApplySkate3CodegenPatches.cmake", source / "cmake")
            # A wrong helper path must fail rather than appear to work because
            # an older generated tree happens to exist in the source directory.
            wrong = source / "generated/skate3_recomp.old.cpp"
            wrong.parent.mkdir()
            wrong.write_text("// Old output; no patch anchors.\n")
            (source / "codegen").write_text(textwrap.dedent(f'''\
                import pathlib, sys, tomllib
                config = tomllib.loads(pathlib.Path(sys.argv[1]).read_text())
                output = pathlib.Path(config["entrypoint"]["out_directory_path"])
                output.mkdir(parents=True, exist_ok=True)
                name = config["project"]["name"]
                if name == "skate3":
                    (output / "skate3_recomp.fixture.cpp").write_text({MARKERS!r})
                else:
                    (output / "eawebkit.fixture.cpp").write_text("// fixture\\n")
            '''))
            # Use the production normalization statements instead of defining
            # *_CONFIG ourselves, which would hide a missing top-level wiring.
            normalization = "\n".join(
                line for line in (ROOT / "CMakeLists.txt").read_text().splitlines()
                if line.startswith("file(TO_CMAKE_PATH ")
                and any(name in line for name in (
                    "SKATE3_SOURCE_DIR_CONFIG", "SKATE3_GAME_DATA_ROOT_CONFIG",
                    "SKATE3_GENERATED_ROOT_CONFIG"))
            )
            project = f'''\
cmake_minimum_required(VERSION 3.25)
project(GeneratedRootFixture LANGUAGES NONE)
add_executable(rex::rexglue IMPORTED GLOBAL)
set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION [==[{sys.executable}]==])
set(SKATE3_GENERATED_ROOT [==[{external.as_posix()}]==])
set(SKATE3_GAME_DATA_ROOT "${{CMAKE_CURRENT_SOURCE_DIR}}/unused-game")
{normalization}
file(MAKE_DIRECTORY "${{CMAKE_CURRENT_BINARY_DIR}}/manifests")
configure_file([==[{(ROOT / 'manifests/skate3.toml.in').as_posix()}]==]
               "${{CMAKE_CURRENT_BINARY_DIR}}/manifests/skate3.toml" @ONLY)
configure_file([==[{(ROOT / 'manifests/eawebkit.toml.in').as_posix()}]==]
               "${{CMAKE_CURRENT_BINARY_DIR}}/manifests/eawebkit.toml" @ONLY)
include([==[{(ROOT / 'cmake/CodegenTargets.cmake').as_posix()}]==])
'''
            (source / "CMakeLists.txt").write_text(project)
            subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=True)
            subprocess.run(["cmake", "--build", str(build), "--target", "generate-all"], check=True)
            self.assertEqual((external / "skate3_recomp.fixture.cpp").read_text(), MARKERS)
            self.assertTrue((external / "eawebkit/eawebkit.fixture.cpp").is_file())
            self.assertEqual(wrong.read_text(), "// Old output; no patch anchors.\n")
            # A repeated build exercises the helper's already-patched path.
            subprocess.run(["cmake", "--build", str(build), "--target", "generate-all"], check=True)
            self.assertEqual((external / "skate3_recomp.fixture.cpp").read_text(), MARKERS)

    def test_patch_helper_keeps_legacy_default(self):
        with tempfile.TemporaryDirectory(prefix="skate3 default ") as directory:
            source = pathlib.Path(directory)
            generated = source / "generated"
            generated.mkdir()
            fixture = generated / "skate3_recomp.fixture.cpp"
            fixture.write_text(MARKERS)
            subprocess.run(["cmake", f"-DSKATE3_SOURCE_DIR={source}", "-P",
                            str(ROOT / "cmake/ApplySkate3CodegenPatches.cmake")], check=True)
            self.assertEqual(fixture.read_text(), MARKERS)

    def test_empty_external_root_error_names_that_root(self):
        with tempfile.TemporaryDirectory(prefix="skate3 missing ") as directory:
            result = subprocess.run(
                ["cmake", f"-DSKATE3_SOURCE_DIR={ROOT}",
                 f"-DSKATE3_GENERATED_ROOT={directory}", "-P",
                 str(ROOT / "cmake/ApplySkate3CodegenPatches.cmake")],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("No generated Skate 3 recompilation files found", result.stderr)
            self.assertIn(directory, result.stderr)


if __name__ == "__main__":
    unittest.main()
