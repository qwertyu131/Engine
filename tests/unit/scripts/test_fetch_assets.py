"""Tests for scripts/fetch_assets.py (stdlib unittest; git and network are mocked).

Run: python -m unittest discover tests   (or: python -m unittest tests/unit/scripts/test_fetch_assets.py)
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load_module():
    spec = importlib.util.spec_from_file_location("fetch_assets", REPO / "scripts" / "fetch_assets.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["fetch_assets"] = mod  # dataclasses need the module registered
    spec.loader.exec_module(mod)
    return mod


fa = _load_module()

COMMIT = "1111111111111111111111111111111111111111"
OTHER = "2222222222222222222222222222222222222222"
VERSION = "9.9.9-test"


def make_zip() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("gltf_validator.exe", b"fake validator")
        z.writestr("gltf_validator", b"fake validator")
    return buf.getvalue()


class FakeGit:
    """Records git invocations; `head` is what `rev-parse HEAD` reports (None = unborn / broken)."""

    def __init__(self, head: str | None):
        self.head = head
        self.calls: list[list[str]] = []

    def __call__(self, args: list[str], cwd: Path) -> str:
        self.calls.append(list(args))
        if args[:2] == ["rev-parse", "HEAD"]:
            if self.head is None:
                raise fa.FetchError("fatal: ambiguous argument 'HEAD'")
            return self.head + "\n"
        if args and args[0] == "init":
            (cwd / ".git").mkdir(parents=True, exist_ok=True)
        if args and args[0] == "checkout":
            self.head = COMMIT
        return ""

    def ran(self, sub: str) -> bool:
        return any(c and c[0] == sub for c in self.calls)


class FetchAssetsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="eng-fetch-")) / "ç ğ ü test"
        self.root.mkdir(parents=True)
        self.zip_bytes = make_zip()
        self.archive_name = f"gltf_validator-{VERSION}-win64.zip"
        lock = {
            "glTF-Sample-Assets": {"repo": "https://example.invalid/glTF-Sample-Assets", "commit": COMMIT,
                                   "sparse": ["/Models/"]},
            "gltf-validator": {"version": VERSION,
                               "url": f"https://example.invalid/{self.archive_name}",
                               "sha256": hashlib.sha256(self.zip_bytes).hexdigest(),
                               "linux64": {"url": f"https://example.invalid/gltf_validator-{VERSION}-linux64.zip",
                                           "sha256": hashlib.sha256(self.zip_bytes).hexdigest()}},
        }
        self.lock_path = self.root / "assets.lock.json"
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")
        self.downloads: list[str] = []
        self.log: list[str] = []

    def tearDown(self) -> None:
        shutil.rmtree(self.root.parent, ignore_errors=True)

    def fake_download(self, payload: bytes):
        def download(url: str, dest: Path) -> None:
            self.downloads.append(url)
            dest.write_bytes(payload)
        return download

    def fetcher(self, git: FakeGit, payload: bytes | None = None) -> "fa.Fetcher":
        return fa.Fetcher(root=self.root, lock_path=self.lock_path, git=git,
                          download=self.fake_download(self.zip_bytes if payload is None else payload),
                          log=self.log.append, platform_key="win64")

    # --- sample assets --------------------------------------------------------------------------

    def test_sample_assets_second_run_at_pinned_commit_does_no_fetch(self) -> None:
        (self.root / "glTF-Sample-Assets" / ".git").mkdir(parents=True)
        git = FakeGit(head=COMMIT)
        self.fetcher(git).sample_assets()
        self.assertFalse(git.ran("fetch"), git.calls)
        self.assertFalse(git.ran("checkout"), git.calls)
        self.assertFalse(git.ran("init"), git.calls)
        self.assertTrue(any("up to date" in line for line in self.log), self.log)

    def test_sample_assets_wrong_commit_is_rechecked_out(self) -> None:
        (self.root / "glTF-Sample-Assets" / ".git").mkdir(parents=True)
        git = FakeGit(head=OTHER)
        self.fetcher(git).sample_assets()
        fetches = [c for c in git.calls if c[0] == "fetch"]
        self.assertTrue(fetches and COMMIT in fetches[0] and "--depth" in fetches[0], git.calls)
        self.assertTrue(git.ran("checkout"))
        self.assertEqual(git.head, COMMIT)

    def test_sample_assets_interrupted_clone_is_resumed(self) -> None:
        (self.root / "glTF-Sample-Assets" / ".git").mkdir(parents=True)
        git = FakeGit(head=None)  # unborn HEAD: a previous fetch never reached checkout
        self.fetcher(git).sample_assets()
        self.assertTrue(git.ran("fetch"))
        self.assertEqual(git.head, COMMIT)

    def test_sample_assets_fresh_clone_is_shallow_sparse(self) -> None:
        git = FakeGit(head=None)
        self.fetcher(git).sample_assets()
        self.assertTrue(git.ran("init"))
        sparse = [c for c in git.calls if c[:2] == ["sparse-checkout", "set"]]
        self.assertTrue(sparse and "/Models/" in sparse[0], git.calls)
        fetch = [c for c in git.calls if c[0] == "fetch"][0]
        self.assertIn("--filter=blob:none", fetch)
        self.assertIn("--depth", fetch)

    def test_sample_assets_verify_mode_never_fetches(self) -> None:
        (self.root / "glTF-Sample-Assets" / ".git").mkdir(parents=True)
        git = FakeGit(head=OTHER)
        with self.assertRaises(fa.FetchError):
            self.fetcher(git).sample_assets(verify=True)
        self.assertFalse(git.ran("fetch"))

    # --- validator ------------------------------------------------------------------------------

    def test_validator_second_run_does_not_download(self) -> None:
        f = self.fetcher(FakeGit(head=COMMIT))
        f.validator()
        self.assertEqual(len(self.downloads), 1)
        self.log.clear()
        f.validator()
        self.assertEqual(len(self.downloads), 1, "second run must not download again")
        self.assertTrue(any("up to date" in line for line in self.log), self.log)
        self.assertTrue(f.validator_exe().is_file())

    def test_leftover_part_file_is_removed(self) -> None:
        tools = self.root / "tools"
        tools.mkdir()
        part = tools / (self.archive_name + ".part")
        part.write_bytes(b"truncated garbage from an interrupted run")
        self.fetcher(FakeGit(head=COMMIT)).validator()
        self.assertFalse(part.exists())
        self.assertEqual((tools / self.archive_name).read_bytes(), self.zip_bytes)

    def test_sha_mismatch_raises_and_leaves_nothing(self) -> None:
        f = self.fetcher(FakeGit(head=COMMIT), payload=b"tampered")
        with self.assertRaisesRegex(fa.FetchError, "sha256 mismatch"):
            f.validator()
        tools = self.root / "tools"
        self.assertFalse((tools / self.archive_name).exists())
        self.assertFalse((tools / (self.archive_name + ".part")).exists())

    def test_corrupt_existing_archive_is_redownloaded(self) -> None:
        tools = self.root / "tools"
        tools.mkdir()
        (tools / self.archive_name).write_bytes(b"corrupt")
        self.fetcher(FakeGit(head=COMMIT)).validator()
        self.assertEqual(len(self.downloads), 1)
        self.assertEqual((tools / self.archive_name).read_bytes(), self.zip_bytes)


if __name__ == "__main__":
    unittest.main()
