"""Tests for scripts/run_golden.py, phase f02 additions (plan F2 Task 5): cases.json with `backend`,
`compareWith` (another case's output), `probes` ([{x, y, rgba, tol}] and the relational `darkerThan`
probe), a per-case output path (Review Focus 5) and the merge of probe results into results.json.

Run: python -m unittest tests/unit/scripts/test_run_golden.py
"""
from __future__ import annotations

import importlib.util
import json
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


rg = _load("run_golden", REPO / "scripts" / "run_golden.py")
model = _load("f02_pbr_model", REPO / "scripts" / "f02_pbr_model.py")


def png_rgba(w: int, h: int, rgba: bytes, filters=(0,)) -> bytes:
    """Small PNG writer: rows cycle through `filters` (only filter 0 actually encodes; the others are
    written with an all-zero predictor-compatible payload produced by the inverse filter)."""
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    stride = w * 4
    raw = bytearray()
    prev = bytes(stride)
    for y in range(h):
        line = rgba[y * stride:(y + 1) * stride]
        f = filters[y % len(filters)]
        out = bytearray(stride)
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = a
            elif f == 2:
                pred = b
            elif f == 3:
                pred = (a + b) // 2
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            out[i] = (line[i] - pred) & 0xFF
        raw += bytes([f]) + out
        prev = line
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


class DecodeTest(unittest.TestCase):
    def test_all_row_filters(self):
        w, h = 7, 5
        px = bytes((x * 31 + y * 17 + c * 5) & 0xFF for y in range(h) for x in range(w) for c in range(4))
        data = png_rgba(w, h, px, filters=(0, 1, 2, 3, 4))
        self.assertEqual(rg.decode_png(data), (w, h, px))


def write_cases(tmp: Path, cases: list[dict]) -> Path:
    p = tmp / "cases.json"
    p.write_text(json.dumps({"phase": "fx", "cases": cases}), encoding="utf-8")
    return p


def render_case(name="tri_vk", **kw) -> dict:
    c = {"name": name, "backend": "vulkan", "scene": "builtin:triangle", "width": 4, "height": 4,
         "ref": "ref.png", "thresholdFlip": 0.01, "thresholdReason": "test"}
    c.update(kw)
    return c


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="eng-rg-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_compare_case_needs_known_render_cases(self):
        good = [render_case("a"), render_case("b", backend="d3d11"),
                {"name": "a_vs_b", "test": "a", "compareWith": "b", "thresholdFlip": 0.01, "thresholdReason": "x"}]
        rg.load_manifest(write_cases(self.tmp, good))
        bad = [render_case("a"), {"name": "a_vs_z", "test": "a", "compareWith": "z", "thresholdFlip": 0.01,
                                  "thresholdReason": "x"}]
        with self.assertRaisesRegex(rg.DriverError, "compareWith 'z' is not a render case"):
            rg.load_manifest(write_cases(self.tmp, bad))

    def test_probe_schema(self):
        with self.assertRaisesRegex(rg.DriverError, "probe"):
            rg.load_manifest(write_cases(self.tmp, [render_case(probes=[{"x": 1, "y": 1, "rgba": [1, 2, 3]}])]))
        with self.assertRaisesRegex(rg.DriverError, "probe"):
            rg.load_manifest(write_cases(self.tmp, [render_case(probes=[{"x": 9, "y": 1, "rgba": [1, 2, 3, 4],
                                                                          "tol": 1}])]))
        rg.load_manifest(write_cases(self.tmp, [render_case(probes=[
            {"x": 1, "y": 1, "rgba": [1, 2, 3, 4], "tol": 1},
            {"x": 1, "y": 2, "darkerThan": {"x": 3, "y": 3}, "margin": 5},
            {"x": 1, "y": 2, "channelGreater": [2, 0], "margin": 5}])]))
        with self.assertRaisesRegex(rg.DriverError, "channelGreater"):
            rg.load_manifest(write_cases(self.tmp, [render_case(probes=[
                {"x": 1, "y": 2, "channelGreater": [2, 4], "margin": 5}])]))

    def test_threshold_needs_a_reason(self):
        c = render_case()
        del c["thresholdReason"]
        with self.assertRaisesRegex(rg.DriverError, "thresholdReason"):
            rg.load_manifest(write_cases(self.tmp, [c]))

    def test_reads_cases_json_or_manifest_json(self):
        d = self.tmp / "ph"
        d.mkdir()
        (d / "manifest.json").write_text(json.dumps({"phase": "ph", "cases": []}), encoding="utf-8")
        self.assertEqual(rg.manifest_path(self.tmp, "ph").name, "manifest.json")
        (d / "cases.json").write_text(json.dumps({"phase": "ph", "cases": []}), encoding="utf-8")
        self.assertEqual(rg.manifest_path(self.tmp, "ph").name, "cases.json")


class ProbeTest(unittest.TestCase):
    def setUp(self):
        self.w, self.h = 4, 4
        px = bytearray()
        for y in range(4):
            for x in range(4):
                px += bytes((10 * x, 10 * y, 200, 255))
        self.px = bytes(px)

    def test_absolute_probe_pass_and_fail(self):
        res = rg.check_probes(self.w, self.h, self.px, [
            {"x": 2, "y": 1, "rgba": [21, 9, 200, 255], "tol": 1},
            {"x": 3, "y": 3, "rgba": [30, 30, 190, 255], "tol": 3}])
        self.assertTrue(res[0]["pass"], res[0])
        self.assertEqual(res[0]["got"], [20, 10, 200, 255])
        self.assertFalse(res[1]["pass"], res[1])

    def test_darker_than_probe(self):
        res = rg.check_probes(self.w, self.h, self.px, [
            {"x": 0, "y": 0, "darkerThan": {"x": 3, "y": 3}, "margin": 50},
            {"x": 0, "y": 0, "darkerThan": {"x": 3, "y": 3}, "margin": 61},
            {"x": 3, "y": 3, "darkerThan": {"x": 0, "y": 0}, "margin": 0}])
        self.assertEqual([r["pass"] for r in res], [True, False, False])
        self.assertEqual(res[0]["got"], [0, 0, 200, 255])
        self.assertEqual(res[0]["gotOther"], [30, 30, 200, 255])

    def test_channel_greater_probe(self):
        # pixel (3, 0) = (30, 0, 200, 255): blue - red = 170
        res = rg.check_probes(self.w, self.h, self.px, [
            {"x": 3, "y": 0, "channelGreater": [2, 0], "margin": 170},
            {"x": 3, "y": 0, "channelGreater": [2, 0], "margin": 171},
            {"x": 3, "y": 0, "channelGreater": [0, 2], "margin": 0}])
        self.assertEqual([r["pass"] for r in res], [True, False, False])

    def test_failed_probe_fails_a_passing_case(self):
        results = {"phase": "fx", "cases": [
            {"name": "a", "status": "PASS", "reason": "ok", "detail": ""},
            {"name": "b", "status": "PASS", "reason": "ok", "detail": ""}]}
        extra = {"a": {"backend": "vulkan", "probes": [{"x": 0, "y": 0, "pass": False, "got": [1, 2, 3, 4]}]},
                 "b": {"backendPair": "vulkan vs d3d11"}}
        failed = rg.merge_case_info(results, extra)
        self.assertEqual(failed, 1)
        a, b = results["cases"]
        self.assertEqual((a["status"], a["reason"]), ("FAIL", "probe-failed"))
        self.assertIn("probe (0, 0)", a["detail"])
        self.assertEqual(a["backend"], "vulkan")
        self.assertEqual((b["status"], b["backendPair"]), ("PASS", "vulkan vs d3d11"))

    def test_missing_image_fails_every_probe(self):
        res = rg.check_probes(0, 0, b"", [{"x": 0, "y": 0, "rgba": [0, 0, 0, 255], "tol": 0}])
        self.assertFalse(res[0]["pass"])


class OutPathTest(unittest.TestCase):
    def test_out_override_is_repo_relative(self):
        out_dir = Path("C:/o") if sys.platform == "win32" else Path("/o")
        p = rg.case_output(render_case(out="build/f2/çıktı klasörü/tri.png"), out_dir)
        self.assertEqual(p, (REPO / "build" / "f2" / "çıktı klasörü" / "tri.png").resolve())
        self.assertEqual(rg.case_output(render_case(), out_dir), out_dir / "render" / "tri_vk.png")


class F02CasesTest(unittest.TestCase):
    """tests/golden/f02/cases.json itself (plan F2 Task 5 Step 1)."""

    @classmethod
    def setUpClass(cls):
        cls.path = REPO / "tests" / "golden" / "f02" / "cases.json"
        cls.manifest = rg.load_manifest(cls.path)
        cls.cases = {c["name"]: c for c in cls.manifest["cases"]}

    def test_three_scenes_two_backends_three_cross_compares(self):
        renders = [c for c in self.cases.values() if "compareWith" not in c and "out" not in c]
        pairs = sorted((c["scene"], c["backend"]) for c in renders)
        scenes = ["builtin:pbr-spheres", "builtin:textured-cube", "builtin:triangle"]
        self.assertEqual(pairs, sorted((s, b) for s in scenes for b in ("d3d11", "vulkan")))
        cross = [c for c in self.cases.values() if "compareWith" in c]
        self.assertEqual(len(cross), 3)
        for c in cross:
            self.assertEqual({self.cases[c["test"]]["backend"], self.cases[c["compareWith"]]["backend"]},
                             {"vulkan", "d3d11"})
            self.assertEqual(self.cases[c["test"]]["scene"], self.cases[c["compareWith"]]["scene"])
        # every render case is compared against the committed D3D11 reference of its scene
        for c in renders:
            self.assertEqual(c["ref"], f"../ref/f02/{c['scene'].split(':')[1]}.png", c["name"])

    def test_thresholds(self):
        for c in self.cases.values():
            scene = c.get("scene") or self.cases[c["test"]]["scene"]
            limit = 0.03 if scene == "builtin:pbr-spheres" else 0.01
            self.assertLessEqual(c["thresholdFlip"], limit, c["name"])
            self.assertTrue(c["thresholdReason"].strip(), c["name"])

    def test_every_render_case_has_probes(self):
        for c in self.cases.values():
            if "compareWith" not in c:
                self.assertTrue(c.get("probes"), c["name"])

    def test_pbr_probes_come_from_the_independent_model(self):
        for c in self.cases.values():
            if c.get("scene") == "builtin:pbr-spheres":
                expected = [{k: v for k, v in p.items() if k not in ("kind", "name")}
                            for p in model.probes(c["width"], c["height"])]
                got = [{k: v for k, v in p.items() if k not in ("kind", "name")} for p in c["probes"]]
                self.assertEqual(got, expected, f"{c['name']}: run python scripts/f02_pbr_model.py --probes")
                kinds = {p.get("kind") for p in c["probes"]}
                self.assertEqual(kinds, {"background", "unlit", "reflection", "sun"}, c["name"])

    def test_review_focus_5_case_writes_under_a_non_ascii_spaced_dir(self):
        utf8 = [c for c in self.cases.values() if "out" in c]
        self.assertEqual(len(utf8), 1)
        self.assertTrue(utf8[0]["out"].startswith("build/f2/çıktı klasörü/"), utf8[0]["out"])


class PngChannelsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="eng-rg-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_png_channels_reaches_headless(self):
        cmd = rg.headless_command(Path("headless"), render_case(pngChannels=3), Path("o.png"))
        self.assertEqual(cmd[cmd.index("--png-channels") + 1], "3")
        self.assertNotIn("--png-channels", rg.headless_command(Path("headless"), render_case(), Path("o.png")))

    def test_png_channels_must_be_3_or_4(self):
        with self.assertRaisesRegex(rg.DriverError, "pngChannels"):
            rg.load_manifest(write_cases(self.tmp, [render_case(pngChannels=2)]))
        rg.load_manifest(write_cases(self.tmp, [render_case(pngChannels=3)]))


class ExpectedFailTest(unittest.TestCase):
    """Plan F3 Task 6: "expectedFail": "<reason>" marks a case that still fails; its FAIL becomes XFAIL (not a gate
    failure); at most 10 % of the render cases may carry it, otherwise the gate fails."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="eng-rg-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_expected_fail_needs_a_reason(self):
        with self.assertRaisesRegex(rg.DriverError, "expectedFail"):
            rg.load_manifest(write_cases(self.tmp, [render_case(expectedFail="")]))
        with self.assertRaisesRegex(rg.DriverError, "expectedFail"):
            rg.load_manifest(write_cases(self.tmp, [render_case(expectedFail=True)]))
        rg.load_manifest(write_cases(self.tmp, [render_case(expectedFail="KHR_x not drawn")]))

    def test_failing_expected_fail_case_is_xfail_and_not_counted(self):
        results = {"cases": [{"name": "a", "status": "FAIL", "reason": "flip-above-threshold"},
                             {"name": "b", "status": "FAIL", "reason": "flip-above-threshold"},
                             {"name": "c", "status": "PASS"}]}
        cases = [render_case("a", expectedFail="known: x"), render_case("b"), render_case("c", expectedFail="y")]
        failed = rg.apply_expected_fail(results, cases)
        st = {c["name"]: c for c in results["cases"]}
        self.assertEqual(st["a"]["status"], "XFAIL")
        self.assertIn("known: x", st["a"]["detail"])
        self.assertEqual(st["b"]["status"], "FAIL")
        self.assertEqual(st["c"]["status"], "PASS")
        self.assertIn("expectedFail", st["c"]["detail"])  # an expected-fail case that passes is flagged
        self.assertEqual(failed, 1)

    def test_expected_fail_cap_is_10_percent_of_render_cases(self):
        cases = [render_case(f"c{i}") for i in range(20)]
        for c in cases[:2]:
            c["expectedFail"] = "r"
        self.assertTrue(rg.expected_fail_within_cap(cases)[2])
        cases[2]["expectedFail"] = "r"
        count, cap, ok = rg.expected_fail_within_cap(cases)
        self.assertEqual((count, cap, ok), (3, 2, False))


class ScenePathTest(unittest.TestCase):
    def test_file_scene_is_repo_relative_builtin_unchanged(self):
        self.assertEqual(rg.scene_arg({"scene": "builtin:triangle"}), "builtin:triangle")
        self.assertEqual(rg.scene_arg({"scene": "tests/assets/x y.glb"}), str(REPO / "tests" / "assets" / "x y.glb"))


class F03ReferenceCasesTest(unittest.TestCase):
    """tests/golden/f03/cases.json, the plan F3 Task 5 reference-mode cases (spec K-B)."""

    ASSETS = ["DamagedHelmet", "MetalRoughSpheres", "NormalTangentTest", "AlphaBlendModeTest", "TextureCoordinateTest"]

    @classmethod
    def setUpClass(cls):
        cls.path = REPO / "tests" / "golden" / "f03" / "cases.json"
        cls.manifest = rg.load_manifest(cls.path)
        cls.cases = {c["name"]: c for c in cls.manifest["cases"]}
        cls.settings = json.loads((REPO / "tests" / "golden" / "ref" / "sample-viewer" / "settings.json").read_text(
            encoding="utf-8"))

    def test_five_assets_on_vulkan_and_d3d11_at_512_in_reference_mode(self):
        for asset in self.ASSETS:
            for backend in ("vulkan", "d3d11"):
                c = self.cases.get(f"{asset}.{backend}")
                self.assertIsNotNone(c, f"{asset}.{backend}")
                self.assertEqual(c["backend"], backend)
                self.assertEqual((c["width"], c["height"]), (512, 512))
                self.assertIn("r.ReferenceMode=1", c.get("cvars", []))
                self.assertEqual(c.get("pngChannels"), 3)  # the references are RGB (refgen, canvas alpha: false)
                self.assertEqual(c["ref"], f"../ref/sample-viewer/{asset}.png")

    def test_scene_is_the_file_refgen_rendered(self):
        models = {m["name"]: m for m in self.settings["models"]}
        for asset in self.ASSETS:
            m = models[asset]
            for backend in ("vulkan", "d3d11"):
                c = self.cases[f"{asset}.{backend}"]
                self.assertEqual(c["scene"], f"tests/assets/glTF-Sample-Assets/Models/{asset}/{m['variant']}/{m['file']}")

    def test_thresholds_at_most_0_05_unless_reasoned_never_above_0_10(self):
        for c in self.cases.values():
            if c["name"].split(".")[0] not in self.ASSETS:
                continue
            thr = c.get("thresholdFlip", 0.05)
            self.assertLessEqual(thr, 0.10, c["name"])
            self.assertTrue(c.get("thresholdReason", "").strip(), c["name"])

    def test_references_are_the_committed_pngs(self):
        import hashlib
        models = {m["name"]: m for m in self.settings["models"]}
        for asset in self.ASSETS:
            png = REPO / "tests" / "golden" / "ref" / "sample-viewer" / f"{asset}.png"
            self.assertEqual(hashlib.sha256(png.read_bytes()).hexdigest(), models[asset]["pngSha256"], asset)


class LightsTest(unittest.TestCase):
    """Plan F3 Task 7: ratio probes, manifest "include" (tests/golden/f03/lights_cases.json runs under --phase f03)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="eng-rg-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_darker_than_ratio_probe(self):
        # (0, 0) = (0, 0, 200) sum 200; (3, 3) = (30, 30, 200) sum 260: 200 <= 0.8 * 260 = 208, not <= 0.7 * 260
        px = bytearray()
        for y in range(4):
            for x in range(4):
                px += bytes((10 * x, 10 * y, 200, 255))
        res = rg.check_probes(4, 4, bytes(px), [
            {"x": 0, "y": 0, "darkerThan": {"x": 3, "y": 3}, "ratio": 0.8},
            {"x": 0, "y": 0, "darkerThan": {"x": 3, "y": 3}, "ratio": 0.7}])
        self.assertEqual([r["pass"] for r in res], [True, False])

    def test_ratio_probe_schema(self):
        rg.load_manifest(write_cases(self.tmp, [render_case(probes=[
            {"x": 1, "y": 2, "darkerThan": {"x": 3, "y": 3}, "ratio": 0.6}])]))
        for bad in (0.0, 1.5, "x"):
            with self.assertRaisesRegex(rg.DriverError, "ratio"):
                rg.load_manifest(write_cases(self.tmp, [render_case(probes=[
                    {"x": 1, "y": 2, "darkerThan": {"x": 3, "y": 3}, "ratio": bad}])]))

    def test_include_merges_cases_with_refs_relative_to_the_included_file(self):
        sub = self.tmp / "more"
        sub.mkdir()
        (sub / "extra.json").write_text(json.dumps({"phase": "fx", "cases": [render_case("b", ref="r/b.png")]}),
                                        encoding="utf-8")
        p = self.tmp / "cases.json"
        p.write_text(json.dumps({"phase": "fx", "include": ["more/extra.json"], "cases": [render_case("a")]}),
                     encoding="utf-8")
        m = rg.load_manifest(p)
        self.assertEqual([c["name"] for c in m["cases"]], ["a", "b"])
        self.assertEqual((p.parent / m["cases"][1]["ref"]).resolve(), (sub / "r" / "b.png").resolve())
        # duplicate names across files are refused
        (sub / "extra.json").write_text(json.dumps({"phase": "fx", "cases": [render_case("a")]}), encoding="utf-8")
        with self.assertRaisesRegex(rg.DriverError, "duplicate case name"):
            rg.load_manifest(p)
        # a missing include is an error
        p.write_text(json.dumps({"phase": "fx", "include": ["nope.json"], "cases": []}), encoding="utf-8")
        with self.assertRaisesRegex(rg.DriverError, "nope.json"):
            rg.load_manifest(p)

    def test_f03_includes_the_lights_cases(self):
        m = rg.load_manifest(REPO / "tests" / "golden" / "f03" / "cases.json")
        names = {c["name"] for c in m["cases"]}
        for b in ("vulkan", "d3d11"):
            self.assertIn(f"lights.LightsPunctualLamp.punctual.{b}", names)
            self.assertIn(f"lights.DirectionalLight.punctual.{b}", names)
            for kind in ("directional", "spot", "point"):
                self.assertIn(f"lights.{kind}.shadow.{b}", names)
        for c in m["cases"]:
            if c["name"].startswith("lights.") and ".punctual." in c["name"]:
                self.assertIn("r.ReferenceMode=1", c["cvars"])
                self.assertIn("r.ReferenceMode.Punctual=1", c["cvars"])
                self.assertTrue(c["ref"].startswith("../ref/sample-viewer-punctual/"), c["ref"])
            if ".shadow." in c["name"] and "compareWith" not in c:
                ratio = [p for p in c["probes"] if "ratio" in p]
                self.assertTrue(ratio and all(p["ratio"] <= 0.6 for p in ratio), c["name"])  # >= 40 % darker


class PbrModelTest(unittest.TestCase):
    def test_unlit_colours_are_tonemap_identities(self):
        for c in model.UNLIT_COLORS:
            self.assertEqual(min(c), 0.0)
            self.assertLess(max(c), 0.76)
            self.assertEqual(model.pbr_neutral(list(c)), tuple(c))

    def test_sun_reflection_at_both_dielectric_centres(self):
        for row in (0, model.GRID_ROWS - 1):
            self.assertLess(model.angle_to_sun_deg(model.centre_reflection(0, row)), model.SUN_RADIUS_DEG - 4.0)

    def test_sun_is_off_every_axis(self):
        for a in range(3):
            self.assertGreater(abs(model.SUN_DIR[a]), 0.03)

    def test_background_is_asymmetric(self):
        p = {q["name"]: tuple(q["rgba"]) for q in model.probes(256, 256) if q["kind"] == "background"}
        self.assertNotEqual(p["background top-left"], p["background top-right"])      # X mirror
        self.assertNotEqual(p["background top-left"], p["background bottom-left"])    # Y flip


if __name__ == "__main__":
    unittest.main()
