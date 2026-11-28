"""scripts/gen_core_subset.py and the plan F3 Task 6 core reference set (tests/golden/f03/core_subset.json, the subset
cases of tests/golden/f03/cases.json, the Sample-Viewer references).

Run: python -m unittest tests/unit/scripts/test_gen_core_subset.py   (needs the glTF-Sample-Assets checkout)
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


gen = _load("gen_core_subset", REPO / "scripts" / "gen_core_subset.py")
rg = _load("run_golden_for_subset", REPO / "scripts" / "run_golden.py")


@unittest.skipUnless(gen.MODELS.is_dir(), "glTF-Sample-Assets not fetched (scripts/fetch_assets.py)")
class CoreSubsetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.subset = json.loads(gen.OUT.read_text(encoding="utf-8"))
        cls.cases = {c["name"]: c for c in rg.load_manifest(gen.CASES)["cases"]}
        cls.settings = json.loads((gen.REF_DIR / "settings.json").read_text(encoding="utf-8"))

    def test_committed_subset_and_cases_are_what_the_script_writes(self):
        self.assertEqual(gen.main(["--check"]), 0, "run python scripts/gen_core_subset.py")
        self.assertEqual(gen.main(["--cases", "--check"]), 0, "run python scripts/gen_core_subset.py --cases")

    def test_only_allowed_extensions_and_every_exclusion_has_a_reason(self):
        allowed = set(self.subset["allowedExtensions"])
        self.assertEqual(allowed, gen.ALLOWED)
        for m in self.subset["models"]:
            self.assertLessEqual(set(m["extensions"]), allowed, m["name"])
            for v in m["decoderVariants"]:
                self.assertLessEqual(set(v["extensions"]), allowed, f"{m['name']}/{v['variant']}")
                self.assertTrue(set(v["extensions"]) & gen.DECODERS, f"{m['name']}/{v['variant']}")
        for e in self.subset["excluded"]:
            self.assertTrue(e["reason"].strip(), e["name"])
        for o in self.subset["otherExtensions"]:
            self.assertTrue(o.get("extensions") or o.get("reason"), o["name"])

    def test_every_subset_entry_has_a_case_per_backend_against_the_model_reference(self):
        models = {m["name"]: m for m in self.settings["models"]}
        for m in self.subset["models"]:
            self.assertEqual(models[m["name"]]["status"], "OK", m["name"])
            png = gen.REF_DIR / f"{m['name']}.png"
            self.assertEqual(hashlib.sha256(png.read_bytes()).hexdigest(), models[m["name"]]["pngSha256"], m["name"])
            for variant in [m["variant"]] + [v["variant"] for v in m["decoderVariants"]]:
                for b in ("vulkan", "d3d11"):
                    c = self.cases[f"{m['case']}.{variant}.{b}"]
                    self.assertEqual(c["ref"], f"../ref/sample-viewer/{m['name']}.png")
                    self.assertIn("r.ReferenceMode=1", c["cvars"])
                    self.assertEqual((c["width"], c["height"], c["pngChannels"]), (512, 512, 3))

    def test_thresholds_and_expected_fail_cap(self):
        entries = sum(1 + len(m["decoderVariants"]) for m in self.subset["models"])
        xfail_entries = set()
        for c in self.cases.values():
            self.assertLessEqual(c.get("thresholdFlip", 0.05), 0.10, c["name"])
            self.assertTrue(c.get("thresholdReason", "").strip(), c["name"])
            if c.get("thresholdFlip", 0.05) > 0.05:
                self.assertNotIn("default gate", c["thresholdReason"], c["name"])
            if "expectedFail" in c:
                self.assertTrue(c["expectedFail"].strip(), c["name"])
                xfail_entries.add(c["name"].rsplit(".", 1)[0])
        self.assertLessEqual(len(xfail_entries), entries // 10, f"{len(xfail_entries)} of {entries} entries")
        self.assertTrue(rg.expected_fail_within_cap(list(self.cases.values()))[2])

    def test_case_names_are_valid_for_non_ascii_and_spaced_models(self):
        self.assertEqual(gen.case_name("Box With Spaces"), "Box_With_Spaces")
        self.assertRegex(gen.case_name("Unicode❤♻Test"), r"^[A-Za-z0-9_.-]+$")


if __name__ == "__main__":
    unittest.main()
