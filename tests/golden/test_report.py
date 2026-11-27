"""Tests for tools/golden/report.py (stdlib unittest).

Run: python -m unittest tests/golden/test_report.py   (or: python -m unittest discover tests)
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REPORT = REPO / "tools" / "golden" / "report.py"
FIXTURES = REPO / "tests" / "golden" / "fixtures"


def run_report(results: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(REPORT), "--results", str(results), "--out", str(out)],
                          capture_output=True, text=True, encoding="utf-8")


class ReportTest(unittest.TestCase):
    def setUp(self) -> None:
        # Non-ASCII directory with spaces (Review Focus 4).
        self.tmp = Path(tempfile.mkdtemp(prefix="eng-report-")) / "ç ğ ü test"
        self.tmp.mkdir(parents=True)
        for name in ("solid_red_64.png", "solid_red_64_copy.png", "gray_64.png"):
            shutil.copy(FIXTURES / name, self.tmp / name)
        # Stand-in heatmap: any valid PNG works for the report.
        shutil.copy(FIXTURES / "gray_64.png", self.tmp / "same.diff.png")
        self.results = self.tmp / "results.json"
        self.out = self.tmp / "report.html"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp.parent, ignore_errors=True)

    def write_results(self, cases: list[dict], phase: str = "f00") -> None:
        self.results.write_text(json.dumps({"phase": phase, "cases": cases}, ensure_ascii=False),
                                encoding="utf-8")

    def pass_case(self, name: str = "same") -> dict:
        return {"name": name, "status": "PASS", "reason": "ok", "detail": "", "flipMean": 0.0,
                "psnr": "inf", "thresholdFlip": 0.05, "test": "solid_red_64.png",
                "ref": "solid_red_64_copy.png", "diff": "same.diff.png", "frameTimeMs": None}

    def missing_ref_case(self) -> dict:
        return {"name": "new_case", "status": "FAIL", "reason": "missing-reference",
                "detail": "cannot open 'ref/new_case.png'", "flipMean": None, "psnr": None,
                "thresholdFlip": 0.05, "test": "gray_64.png", "ref": "ref/new_case.png", "diff": None,
                "frameTimeMs": None}

    def test_pass_and_missing_reference(self) -> None:
        self.write_results([self.pass_case(), self.missing_ref_case()])
        proc = run_report(self.results, self.out)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        html = self.out.read_text(encoding="utf-8")

        self.assertIn("PASS", html)
        self.assertIn("missing-reference", html)
        self.assertEqual(html.count('data-case="'), 2)
        self.assertGreaterEqual(html.count("data:image/png;base64,"), 1)
        self.assertNotIn("http://", html)
        self.assertNotIn("https://", html)

        # 3 embedded images for the PASS case + 1 (test) for the FAIL case.
        self.assertEqual(html.count('src="data:image/png;base64,'), 4)
        # Missing reference and missing diff are labelled placeholders, not broken <img>.
        self.assertEqual(html.count('class="placeholder"'), 2)
        self.assertIn("Reference missing", html)
        # Header: phase and totals.
        self.assertIn("f00", html)
        self.assertRegex(html, r"<b>1</b>\s*PASS")
        self.assertRegex(html, r"<b>1</b>\s*FAIL")
        # Null metrics render as an em dash; infinite PSNR as infinity sign.
        self.assertIn("—", html)
        self.assertIn("∞", html)

    def test_strings_are_html_escaped(self) -> None:
        case = self.pass_case(name="a&b <i>x</i>")
        self.write_results([case], phase="f00 <script>")
        proc = run_report(self.results, self.out)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        html = self.out.read_text(encoding="utf-8")
        self.assertIn("a&amp;b &lt;i&gt;x&lt;/i&gt;", html)
        self.assertNotIn("<i>x</i>", html)
        self.assertNotIn("f00 <script>", html)

    def test_unreadable_image_is_placeholder(self) -> None:
        (self.tmp / "broken.png").write_bytes(b"not a png")
        case = self.pass_case()
        case["ref"] = "broken.png"
        self.write_results([case])
        proc = run_report(self.results, self.out)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        html = self.out.read_text(encoding="utf-8")
        self.assertEqual(html.count('class="placeholder"'), 1)
        self.assertIn("unreadable", html)

    def test_invalid_results_exit_2(self) -> None:
        self.results.write_text("{ not json", encoding="utf-8")
        proc = run_report(self.results, self.out)
        self.assertEqual(proc.returncode, 2)
        self.assertFalse(self.out.exists())

        proc = run_report(self.tmp / "nope.json", self.out)
        self.assertEqual(proc.returncode, 2)

    def test_no_external_references_at_all(self) -> None:
        self.write_results([self.pass_case(), self.missing_ref_case()])
        self.assertEqual(run_report(self.results, self.out).returncode, 0)
        html = self.out.read_text(encoding="utf-8")
        # No CDN / remote scripts, styles or images of any kind.
        self.assertIsNone(re.search(r"""(src|href)\s*=\s*["'](?!data:|#)""", html))
        self.assertNotIn("@import", html)
        self.assertNotIn("url(", html)


if __name__ == "__main__":
    unittest.main()
