"""Tests for scripts/compile_shaders.py (stdlib unittest; the shaderc process is faked).

Run: python -m unittest discover tests   (or: python -m unittest tests/unit/scripts/test_compile_shaders.py)
The real compile of every permutation is the gate command `python scripts/compile_shaders.py --out <dir>`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load_module():
    spec = importlib.util.spec_from_file_location("compile_shaders", REPO / "scripts" / "compile_shaders.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["compile_shaders"] = mod
    spec.loader.exec_module(mod)
    return mod


cs = _load_module()
PERMS = REPO / "shaders" / "pbr" / "permutations.json"


class FakeShaderc:
    """Stands in for the shaderc process: writes the output file and prints what shaderc prints."""

    def __init__(self, fail: set[tuple[str, str, str]] | None = None) -> None:
        self.fail = fail or set()  # (target, entry, key)
        self.calls: list[list[str]] = []

    def __call__(self, cmd: list[str]) -> tuple[int, str, str]:
        self.calls.append(cmd)
        arg = {cmd[i]: cmd[i + 1] for i in range(len(cmd) - 1) if cmd[i].startswith("--")}
        target, entry, out = arg["--target"], arg["--entry"], Path(arg["--out"])
        key = out.name
        if (target, entry, key) in self.fail:
            return 1, "", f"shaderc: {target} {entry}: dxc failed (exit 1)"
        out.mkdir(parents=True, exist_ok=True)
        f = out / f"pbr.{entry}.{target}.bin"
        f.write_bytes(b"x" * 10)
        status = "PENDING-CI" if target == "msl" else "passed"
        return 0, f"shaderc: {target} {entry} -> {f} [{status}]\n", ""


class MatrixTest(unittest.TestCase):
    def test_matrix_is_8_per_target_per_stage_dxbc_16(self) -> None:
        # Plan F2 Task 7: dxbc carries T0 (SM 4.0, D3D11 FL10_x) and T1 (SM 5.0, FL11_x), 16 define sets.
        perms = cs.load_permutations(PERMS)
        jobs = cs.expand_jobs(perms)
        self.assertEqual(len(jobs), (6 * 8 + 16) * 2)
        per = {}
        for j in jobs:
            per[(j.target, j.stage)] = per.get((j.target, j.stage), 0) + 1
        self.assertEqual({k: v for k, v in per.items() if v != 8}, {("dxbc", "vs"): 16, ("dxbc", "fs"): 16})
        self.assertEqual(len(per), 14)
        self.assertEqual({t for j in jobs if j.target == "glsl-es300" for t in [j.defines["TIER"]]}, {"0"})
        self.assertEqual({t for j in jobs if j.target == "dxil" for t in [j.defines["TIER"]]}, {"2"})
        self.assertEqual({t for j in jobs if j.target == "wgsl" for t in [j.defines["TIER"]]}, {"1"})
        self.assertEqual({t for j in jobs if j.target == "dxbc" for t in [j.defines["TIER"]]}, {"0", "1"})
        self.assertIn("T0_SKIN0_MORPH0_UNLIT0", {j.key for j in jobs if j.target == "dxbc"})

    def test_budget_is_8_per_tier_per_target_per_stage(self) -> None:
        # The F1 budget (8 define sets per target per stage) holds per tier; dxbc has two tiers since F2 Task 7.
        perms = json.loads(PERMS.read_text(encoding="utf-8"))
        perms["axes"]["FG_EXTRA"] = [0, 1]  # 16 define sets per tier
        with self.assertRaisesRegex(ValueError, "dxil: 16 define sets per tier per stage > 8"):
            cs.check_permutations(perms)

    def test_defines_are_in_canonical_order(self) -> None:
        jobs = cs.expand_jobs(cs.load_permutations(PERMS))
        self.assertEqual(list(jobs[0].defines),
                         ["TIER", "FG_SKIN", "FG_MORPH", "FG_UNLIT", "FG_CLEARCOAT_SHEEN_SPEC", "FG_TRANSMISSION_VOLUME"])
        self.assertEqual(jobs[0].key, "T2_SKIN0_MORPH0_UNLIT0")

    def test_excluded_must_be_complement_of_tiers(self) -> None:
        perms = json.loads(PERMS.read_text(encoding="utf-8"))
        perms["excluded"] = [e for e in perms["excluded"] if e["target"] != "wgsl"]
        with self.assertRaisesRegex(ValueError, "wgsl"):
            cs.check_permutations(perms)


class RunTest(unittest.TestCase):
    def run_main(self, fake: FakeShaderc, out: Path, extra: list[str] | None = None) -> tuple[int, str]:
        shaderc = out.parent / "shaderc.exe"
        shaderc.write_bytes(b"")
        buf = io.StringIO()
        with redirect_stdout(buf), redirect_stderr(buf):
            rc = cs.main(["--out", str(out), "--shaderc", str(shaderc), "--jobs", "4"] + (extra or []), runner=fake)
        return rc, buf.getvalue()

    def test_all_ok_summary(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "ç ğ out"
            fake = FakeShaderc()
            rc, _ = self.run_main(fake, out)
            self.assertEqual(rc, 0)
            s = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(s["total"], 128)
            self.assertEqual(s["ok"], 112)
            self.assertEqual(s["failed"], [])
            self.assertEqual(len(s["pendingCi"]), 16)
            self.assertTrue(all("msl" in p for p in s["pendingCi"]))
            self.assertIsInstance(s["msTotal"], (int, float))
            self.assertEqual(len(s["items"]), 128)
            self.assertEqual(s["items"][0]["bytes"], 10)
            # One output directory per (target, permutation): nothing overwritten.
            dirs = {c[c.index("--out") + 1] for c in fake.calls}
            self.assertEqual(len(dirs), 64)
            self.assertTrue(all("--validate" in c for c in fake.calls))

    def test_failure_exits_1_and_is_listed(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "out"
            fake = FakeShaderc(fail={("wgsl", "PSMain", "T1_SKIN0_MORPH0_UNLIT1")})
            rc, _ = self.run_main(fake, out)
            self.assertEqual(rc, 1)
            s = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(len(s["failed"]), 1)
            self.assertEqual(s["failed"][0]["id"], "wgsl/T1_SKIN0_MORPH0_UNLIT1/PSMain")
            self.assertIn("dxc failed", s["failed"][0]["error"])
            self.assertEqual(s["ok"] + len(s["failed"]) + len(s["pendingCi"]), s["total"])

    def test_dxbc_path_is_passed_through(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            fake = FakeShaderc()
            rc, _ = self.run_main(fake, Path(d) / "out", ["--dxbc-path", "direct"])
            self.assertEqual(rc, 0)
            dxbc = [c for c in fake.calls if c[c.index("--target") + 1] == "dxbc"]
            self.assertTrue(dxbc)
            self.assertTrue(all(c[c.index("--dxbc-path") + 1] == "direct" for c in dxbc))

    def test_missing_shaderc_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            buf = io.StringIO()
            with redirect_stdout(buf), redirect_stderr(buf):
                rc = cs.main(["--out", str(Path(d) / "out"), "--shaderc", str(Path(d) / "no-shaderc.exe")],
                             runner=FakeShaderc())
            self.assertEqual(rc, 2)
            self.assertIn("tool not found: shaderc", buf.getvalue())


LIBRARY = REPO / "shaders" / "library.json"


class LibraryTest(unittest.TestCase):
    """Build step for the RHI (plan F2 Task 2): every shader of shaders/library.json, only the targets the
    RHI backends load, written to <out>/<shader>/<target>/<key>/ for ShaderLibrary."""

    def run_main(self, fake: FakeShaderc, out: Path, extra: list[str]) -> tuple[int, str]:
        shaderc = out.parent / "shaderc.exe"
        shaderc.write_bytes(b"")
        buf = io.StringIO()
        with redirect_stdout(buf), redirect_stderr(buf):
            rc = cs.main(["--out", str(out), "--shaderc", str(shaderc), "--jobs", "4"] + extra, runner=fake)
        return rc, buf.getvalue()

    def test_library_layout_and_target_filter(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "ç shaders"
            fake = FakeShaderc()
            rc, text = self.run_main(fake, out, ["--library", str(LIBRARY), "--targets", "spirv,dxbc"])
            self.assertEqual(rc, 0, text)
            s = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            # pbr: spirv T2 x 8 + dxbc (T0 + T1) x 8 define sets, 2 entries; triangle: 2 targets x 2 entries;
            # color_triangle: the same + its dxbc T0 variant (2 entries, plan F2 Task 7); textured: 2 x 2 + its
            # dxbc direct-path variant (2 entries); skybox (plan F2 Task 5): 2 x 2 + dxbc T0 variant (2);
            # ui/imgui (Task 6): 2 targets x 2 entries; plan F3 Task 5: ibl/{equirect_to_cube, prefilter_ggx,
            # prefilter_lambert, brdf_lut} 2 targets x 2 entries each (T1 / T2 only), ibl/background 2 x 2 + its dxbc
            # T0 variant (2). Plan F3 Task 7: test/depth_sample 2 targets x 4 entries + its dxbc T0 variant (4);
            # shadow: spirv T2 x 4 + dxbc (T0 + T1) x 4 define sets, 2 entries.
            self.assertEqual(s["total"], 48 + 4 + 6 + 6 + 6 + 4 + 4 * 4 + 6 + 12 + 24)
            self.assertEqual(s["failed"], [])
            self.assertEqual({i["target"] for i in s["items"]}, {"spirv", "dxbc"})
            dirs = {Path(c[c.index("--out") + 1]).relative_to(out).as_posix() for c in fake.calls}
            self.assertIn("pbr/spirv/T2_SKIN0_MORPH0_UNLIT0", dirs)
            self.assertIn("pbr/dxbc/T1_SKIN1_MORPH1_UNLIT1", dirs)
            self.assertIn("pbr/dxbc/T0_SKIN0_MORPH0_UNLIT0", dirs)
            self.assertIn("color_triangle/dxbc/default-t0", dirs)
            self.assertIn("skybox/dxbc/default-t0", dirs)
            self.assertIn("triangle/spirv/default", dirs)
            self.assertIn("triangle/dxbc/default", dirs)
            self.assertIn("color_triangle/spirv/default", dirs)
            self.assertIn("textured/dxbc/default", dirs)
            self.assertIn("skybox/spirv/default", dirs)
            self.assertIn("imgui/dxbc/default", dirs)
            self.assertIn("prefilter_ggx/spirv/default", dirs)
            self.assertIn("background/dxbc/default-t0", dirs)
            self.assertIn("depth_sample/dxbc/default-t0", dirs)
            self.assertIn("shadow/spirv/T2_SKIN1_MORPH1", dirs)
            self.assertIn("shadow/dxbc/T0_SKIN0_MORPH0", dirs)
            self.assertIn("pbr/spirv/T2_SKIN0_MORPH0_UNLIT0/PSMain", {i["id"] for i in s["items"]})

    def test_shader_without_tier_axis_has_key_default(self) -> None:
        perms = cs.load_permutations(REPO / "shaders" / "test" / "triangle.permutations.json")
        jobs = cs.expand_jobs(perms)
        self.assertEqual({j.key for j in jobs}, {"default"})
        self.assertEqual({j.target for j in jobs}, {"spirv", "dxbc"})
        self.assertEqual({j.shader for j in jobs}, {"triangle"})

    def test_dxbc_paths_build_a_direct_variant_with_its_own_key(self) -> None:
        # Plan F2 Task 4: textured.hlsl is also built on the direct DXBC path (key "default-direct"), so
        # the D3D11 RHI test can pair a cross VS with a direct PS and see the rejection.
        perms = cs.load_permutations(REPO / "shaders" / "test" / "textured.permutations.json")
        jobs = cs.expand_jobs(perms, library=True)
        dxbc = sorted((j.key, j.entry, j.dxbc_path) for j in jobs if j.target == "dxbc")
        self.assertEqual(dxbc, [("default", "PSMain", "cross"), ("default", "VSMain", "cross"),
                                ("default-direct", "PSMain", "direct"), ("default-direct", "VSMain", "direct")])
        self.assertEqual({j.key for j in jobs if j.target == "spirv"}, {"default"})
        direct = next(j for j in jobs if j.key == "default-direct")
        cmd = cs.command(Path("shaderc"), direct.source, direct, Path("out"), None)
        self.assertEqual(cmd[cmd.index("--dxbc-path") + 1], "direct")
        cross = next(j for j in jobs if j.target == "dxbc" and j.key == "default")
        # A per-target path wins over the run's --dxbc-path.
        cmd = cs.command(Path("shaderc"), cross.source, cross, Path("out"), "direct")
        self.assertEqual(cmd[cmd.index("--dxbc-path") + 1], "cross")

    def test_tier0_variant_builds_a_t0_key_with_tier_0(self) -> None:
        # Plan F2 Task 7: a shader without a TIER axis can add a dxbc T0 variant (key "<key>-t0", TIER=0, so
        # shaderc emits SM 4.0 for D3D11 FL10_x); the "default" outputs stay SM 5.0 and unchanged.
        perms = cs.load_permutations(REPO / "shaders" / "test" / "color_triangle.permutations.json")
        jobs = cs.expand_jobs(perms, library=True)
        dxbc = sorted((j.key, j.entry, tuple(j.defines.items())) for j in jobs if j.target == "dxbc")
        self.assertEqual(dxbc, [("default", "PSMain", ()), ("default", "VSMain", ()),
                                ("default-t0", "PSMain", (("TIER", "0"),)),
                                ("default-t0", "VSMain", (("TIER", "0"),))])
        self.assertEqual({j.key for j in jobs if j.target == "spirv"}, {"default"})
        t0 = next(j for j in jobs if j.key == "default-t0")
        cmd = cs.command(Path("shaderc"), t0.source, t0, Path("out"), None)
        self.assertEqual(cmd[cmd.index("--define") + 1], "TIER=0")
        self.assertEqual(t0.out_dir(Path("o")).as_posix(), "o/color_triangle/dxbc/default-t0")

    def test_tier0_variant_is_checked(self) -> None:
        base = json.loads((REPO / "shaders" / "test" / "color_triangle.permutations.json").read_text(encoding="utf-8"))
        bad = json.loads(json.dumps(base))
        bad["targets"]["spirv"]["tier0Variant"] = True
        with self.assertRaisesRegex(ValueError, "tier0Variant"):
            cs.check_permutations(bad)
        tiered = json.loads(PERMS.read_text(encoding="utf-8"))
        tiered["targets"]["dxbc"]["tier0Variant"] = True
        with self.assertRaisesRegex(ValueError, "tier0Variant"):
            cs.check_permutations(tiered)

    def test_dxbc_paths_are_checked(self) -> None:
        base = json.loads((REPO / "shaders" / "test" / "textured.permutations.json").read_text(encoding="utf-8"))
        bad = json.loads(json.dumps(base))
        bad["targets"]["dxbc"]["dxbcPaths"] = ["cross", "sideways"]
        with self.assertRaisesRegex(ValueError, "dxbcPaths"):
            cs.check_permutations(bad)
        bad = json.loads(json.dumps(base))
        bad["targets"]["spirv"]["dxbcPaths"] = ["cross"]
        with self.assertRaisesRegex(ValueError, "dxbcPaths"):
            cs.check_permutations(bad)

    def test_unknown_target_filter_is_usage_error(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            rc, text = self.run_main(FakeShaderc(), Path(d) / "out", ["--library", str(LIBRARY), "--targets", "hlsl"])
            self.assertEqual(rc, 2)
            self.assertIn("unknown target", text)


if __name__ == "__main__":
    unittest.main()
