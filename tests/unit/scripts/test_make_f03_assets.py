"""scripts/make_f03_assets.py (plan F3 Task 2): the committed loader fixtures under tests/assets/f03 are what the
script writes (decoded PNG pixels, other files byte for byte), and bad/expected.json lists every bad/ file.

Run: python -m unittest tests/unit/scripts/test_make_f03_assets.py
"""
from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load():
    spec = importlib.util.spec_from_file_location("make_f03_assets", REPO / "scripts" / "make_f03_assets.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["make_f03_assets"] = mod
    spec.loader.exec_module(mod)
    return mod


mk = _load()


class Fixtures(unittest.TestCase):
    def test_committed_fixtures_are_fresh(self):
        self.assertEqual(mk.main(["--check"]), 0, "run python scripts/make_f03_assets.py")

    def test_expected_json_lists_every_bad_file(self):
        bad = REPO / "tests" / "assets" / "f03" / "bad"
        listed = {f["file"] for f in json.loads((bad / "expected.json").read_text(encoding="utf-8"))["fixtures"]}
        on_disk = {p.name for p in bad.iterdir() if p.name != "expected.json"}
        self.assertEqual(listed, on_disk)

    def test_review_focus_1_kinds_present(self):
        names = set(mk.fixtures())
        for kind in ("truncated.glb", "accessor_out_of_range.gltf", "index_out_of_range.gltf", "bad_base64.gltf",
                     "bad_image.gltf", "accessor_count_2pow31.gltf"):
            self.assertIn(f"bad/{kind}", names)


if __name__ == "__main__":
    unittest.main()
