"""CMake / ctest rules of plan F3 Task 1 (F2 carry-overs), checked through real cmake and ctest runs:

  * ENG_REQUIRE_RHI_BACKENDS (cmake/RequireBackends.cmake): configure fails when a listed backend is not
    built, and passes when all are;
  * the skip rule of the GPU ctest entries (cmake/TestSkip.cmake, ENG_BACKEND_ABSENT_SKIP_REGEX): a tool that
    exits 3 because the backend is absent ("backend not available: ...") is ctest Skipped; a tool that exits 3
    for another reason on a present backend (the T0 tier rejection "pipeline needs TIER 1, device is TIER 0")
    is ctest Failed. A tiny ctest project runs tests/golden/cli_exit_code.cmake (the script the headless
    entries use) with a fake tool and the shared regex.

cmake / ctest: $ENG_CMAKE, else the pinned C:/Program Files/CMake/bin/cmake.exe, else PATH.
Run: python -m unittest tests/unit/scripts/test_ctest_rules.py
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def cmake_exe() -> str:
    env = os.environ.get("ENG_CMAKE")
    if env:
        return env
    pinned = Path("C:/Program Files/CMake/bin/cmake.exe")
    if pinned.is_file():
        return str(pinned)
    found = shutil.which("cmake")
    if not found:
        raise unittest.SkipTest("cmake not found")
    return found


def ctest_exe() -> str:
    c = Path(cmake_exe())
    sibling = c.with_name("ctest" + c.suffix)
    return str(sibling) if sibling.is_file() else (shutil.which("ctest") or "ctest")


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")


class RequireBackends(unittest.TestCase):
    SCRIPT = REPO / "cmake" / "RequireBackends.cmake"

    def check(self, required: str, built: str) -> subprocess.CompletedProcess:
        return run([cmake_exe(), f"-DENG_REQUIRE_RHI_BACKENDS={required}", f"-DENG_BUILT_RHI_BACKENDS={built}",
                    "-P", str(self.SCRIPT)])

    def test_missing_backend_fails_configure(self):
        p = self.check("vulkan;d3d11;metal", "vulkan;d3d11")
        self.assertNotEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn("ENG_REQUIRE_RHI_BACKENDS: required RHI backend(s) not built: metal", p.stderr)

    def test_all_built_passes(self):
        p = self.check("vulkan;d3d11", "vulkan;d3d11")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_empty_requirement_passes(self):
        p = self.check("", "")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)


FAKE_TOOL = """# Prints MSG to stderr and exits with CODE (cmake_language(EXIT) needs CMake 3.29).
message("${MSG}")
cmake_language(EXIT ${CODE})
"""

PROJECT = """cmake_minimum_required(VERSION 3.29)
project(skip_rule NONE)
enable_testing()
include("@REPO@/cmake/TestSkip.cmake")
foreach(case absent tier)
    if(case STREQUAL "absent")
        set(msg "headless: backend not available: vulkan: Vulkan loader not found")
    else()
        set(msg "headless: pipeline needs TIER 1, device is TIER 0")
    endif()
    add_test(NAME gpu.${case}
             COMMAND "${CMAKE_COMMAND}" "-DTOOL=${CMAKE_COMMAND}" -DEXPECT=0 "-DWORK=${CMAKE_BINARY_DIR}/w_${case}"
                     "-DARGS=-DMSG=${msg}|-DCODE=3|-P|${CMAKE_SOURCE_DIR}/fake_tool.cmake"
                     -P "@REPO@/tests/golden/cli_exit_code.cmake")
    set_tests_properties(gpu.${case} PROPERTIES SKIP_REGULAR_EXPRESSION "${ENG_BACKEND_ABSENT_SKIP_REGEX}")
endforeach()
"""


class SkipRule(unittest.TestCase):
    def test_tier_rejection_fails_absent_backend_skips(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, build = Path(tmp) / "src", Path(tmp) / "build"
            src.mkdir()
            (src / "fake_tool.cmake").write_text(FAKE_TOOL, encoding="utf-8")
            (src / "CMakeLists.txt").write_text(PROJECT.replace("@REPO@", REPO.as_posix()), encoding="utf-8")
            cfg = run([cmake_exe(), "-S", str(src), "-B", str(build)])
            self.assertEqual(cfg.returncode, 0, cfg.stdout + cfg.stderr)
            t = run([ctest_exe(), "--test-dir", str(build), "-C", "Debug", "-V"])
            out = t.stdout + t.stderr
            status = dict(re.findall(r"Test +#\d+: (gpu\.\w+) \.+\**\s*(Passed|Failed|Skipped|Not Run)", out))
            self.assertEqual(status.get("gpu.absent"), "Skipped", out)
            self.assertEqual(status.get("gpu.tier"), "Failed", out)


class ViewerRunnerSkip(unittest.TestCase):
    """tests/viewer/run_viewer.py: exit 3 is a skip only with the backend-absent text."""

    def setUp(self):
        import importlib.util
        import sys
        spec = importlib.util.spec_from_file_location("run_viewer", REPO / "tests" / "viewer" / "run_viewer.py")
        self.rv = importlib.util.module_from_spec(spec)
        sys.modules["run_viewer"] = self.rv
        spec.loader.exec_module(self.rv)

    def test_classification(self):
        self.assertTrue(self.rv.backend_absent(3, "viewer: backend not available: vulkan: no physical device\n"))
        self.assertFalse(self.rv.backend_absent(3, "viewer: pipeline needs TIER 1, device is TIER 0\n"))
        self.assertFalse(self.rv.backend_absent(0, "backend not available: x"))
        self.assertFalse(self.rv.backend_absent(4, "backend not available: x"))


if __name__ == "__main__":
    unittest.main()
