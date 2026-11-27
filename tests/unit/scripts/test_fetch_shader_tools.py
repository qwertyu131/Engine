"""Tests for scripts/fetch_shader_tools.py (stdlib unittest; git, network and the CMake build are mocked).

Run: python -m unittest discover tests   (or: python -m unittest tests/unit/scripts/test_fetch_shader_tools.py)
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _load_module():
    spec = importlib.util.spec_from_file_location("fetch_shader_tools", REPO / "scripts" / "fetch_shader_tools.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules["fetch_shader_tools"] = mod
    spec.loader.exec_module(mod)
    return mod


fst = _load_module()

DAWN_COMMIT = "3333333333333333333333333333333333333333"
DXC_TAG = "v9.9.9999"

FAKE_DEPS = """
vars = {
  'chromium_git': 'https://chromium.googlesource.com',
}
deps = {
  'third_party/abseil-cpp': {
    'url': '{chromium_git}/chromium/src/third_party/abseil-cpp@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'condition': 'dawn_standalone',
  },
  'third_party/spirv-headers/src': {
    'url': '{chromium_git}/external/github.com/KhronosGroup/SPIRV-Headers@bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
    'condition': 'dawn_standalone',
  },
  'third_party/spirv-tools/src': {
    'url': '{chromium_git}/external/github.com/KhronosGroup/SPIRV-Tools@cccccccccccccccccccccccccccccccccccccccc',
    'condition': 'dawn_standalone',
  },
  'third_party/jinja2': {
    'url': '{chromium_git}/chromium/src/third_party/jinja2@dddddddddddddddddddddddddddddddddddddddd',
  },
  'third_party/markupsafe': {
    'url': '{chromium_git}/chromium/src/third_party/markupsafe@eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',
  },
  'third_party/webgpu-headers/src': {
    'url': '{chromium_git}/external/github.com/webgpu-native/webgpu-headers@ffffffffffffffffffffffffffffffffffffffff',
  },
  'third_party/unused-big-dep': {
    'url': '{chromium_git}/external/big@9999999999999999999999999999999999999999',
  },
}
"""


def make_dxc_zip() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name in ("dxc.exe", "dxcompiler.dll", "dxil.dll"):
            z.writestr(f"bin/x64/{name}", b"fake " + name.encode())
        z.writestr("inc/dxcapi.h", b"// fake")
    return buf.getvalue()


class FakeGit:
    """Fake git: `fetch` + `checkout` of a repo makes HEAD the fetched commit; the Dawn checkout
    gets a DEPS file."""

    def __init__(self) -> None:
        self.calls: list[tuple[list[str], Path]] = []
        self.heads: dict[Path, str] = {}
        self.fetched: dict[Path, str] = {}

    def __call__(self, args: list[str], cwd: Path) -> str:
        cwd = Path(cwd)
        self.calls.append((list(args), cwd))
        if args[:2] == ["rev-parse", "--show-toplevel"]:
            if not (cwd / ".git").exists():
                raise fst.FetchError("fatal: not a git repository")
            return str(cwd) + "\n"
        if args[:2] == ["rev-parse", "HEAD"]:
            if cwd not in self.heads:
                raise fst.FetchError("fatal: ambiguous argument 'HEAD'")
            return self.heads[cwd] + "\n"
        if args and args[0] == "init":
            (cwd / ".git").mkdir(parents=True, exist_ok=True)
        if args and args[0] == "fetch":
            self.fetched[cwd] = args[-1]
        if args and args[0] == "checkout":
            self.heads[cwd] = self.fetched[cwd]
            if cwd.name == "dawn":
                (cwd / "DEPS").write_text(FAKE_DEPS, encoding="utf-8")
        return ""

    def ran(self, sub: str) -> bool:
        return any(c and c[0] == sub for c, _ in self.calls)


class FakeRunner:
    """Fake CMake: the build step 'produces' tint.exe in the Release output dir."""

    def __init__(self) -> None:
        self.calls: list[list[str]] = []

    def __call__(self, args: list[str], cwd: Path) -> str:
        self.calls.append(list(args))
        if "--build" in args:
            build_dir = Path(args[args.index("--build") + 1])
            out = build_dir / "Release"
            out.mkdir(parents=True, exist_ok=True)
            (out / "tint.exe").write_bytes(b"fake tint")
        return ""


class FetchShaderToolsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="eng-shtools-")) / "ç ğ ü test" / "tools"
        self.root.mkdir(parents=True)
        self.zip_bytes = make_dxc_zip()
        self.zip_name = "dxc_9999_99_99.zip"
        self.lock = {
            "dxc": {"tag": DXC_TAG, "url": f"https://example.invalid/{self.zip_name}",
                    "sha256": hashlib.sha256(self.zip_bytes).hexdigest()},
            "dawn": {"repo": "https://example.invalid/dawn", "commit": DAWN_COMMIT},
            "spirv-cross": {"commit": "4444444444444444444444444444444444444444"},
        }
        self.lock_path = self.root / "tools.lock.json"
        self.lock_path.write_text(json.dumps(self.lock), encoding="utf-8")
        self.downloads: list[str] = []
        self.log: list[str] = []

    def tearDown(self) -> None:
        shutil.rmtree(self.root.parents[1], ignore_errors=True)

    def fake_download(self, payload: bytes):
        def download(url: str, dest: Path) -> None:
            self.downloads.append(url)
            dest.write_bytes(payload)
        return download

    def fetcher(self, payload: bytes | None = None, git: FakeGit | None = None,
                run: FakeRunner | None = None, env: dict[str, str] | None = None) -> "fst.ShaderToolsFetcher":
        return fst.ShaderToolsFetcher(
            root=self.root, lock_path=self.lock_path, git=git or FakeGit(), run=run or FakeRunner(),
            download=self.fake_download(self.zip_bytes if payload is None else payload),
            log=self.log.append, env=env if env is not None else {})

    # --- lock -----------------------------------------------------------------------------------

    def test_committed_lock_has_plan_schema(self) -> None:
        lock = json.loads((REPO / "third_party" / "tools" / "tools.lock.json").read_text(encoding="utf-8"))
        self.assertEqual(set(lock), {"dxc", "dawn", "spirv-cross"})
        self.assertEqual(set(lock["dxc"]), {"tag", "url", "sha256"})
        self.assertEqual(set(lock["dawn"]), {"repo", "commit"})
        self.assertEqual(set(lock["spirv-cross"]), {"commit"})
        for sha in (lock["dawn"]["commit"], lock["spirv-cross"]["commit"]):
            self.assertRegex(sha, r"^[0-9a-f]{40}$")
        self.assertRegex(lock["dxc"]["sha256"], r"^[0-9a-f]{64}$")

    # --- DXC ------------------------------------------------------------------------------------

    def test_dxc_second_run_does_not_download(self) -> None:
        f = self.fetcher()
        f.dxc()
        self.assertEqual(len(self.downloads), 1)
        for name in ("dxc.exe", "dxcompiler.dll", "dxil.dll"):
            self.assertTrue((self.root / "dxc" / "bin" / "x64" / name).is_file(), name)
        self.log.clear()
        f.dxc()
        self.assertEqual(len(self.downloads), 1, "second run must not download again")
        self.assertTrue(any("up to date" in line for line in self.log), self.log)

    def test_dxc_sha_mismatch_raises_and_leaves_nothing(self) -> None:
        f = self.fetcher(payload=b"tampered")
        with self.assertRaisesRegex(fst.FetchError, "sha256 mismatch"):
            f.dxc()
        dl = self.root / "downloads"
        self.assertFalse((dl / self.zip_name).exists())
        self.assertFalse((dl / (self.zip_name + ".part")).exists())
        self.assertFalse((self.root / "dxc" / "bin" / "x64" / "dxc.exe").exists())

    def test_dxc_archive_without_dxil_is_rejected(self) -> None:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("bin/x64/dxc.exe", b"x")
        payload = buf.getvalue()
        self.lock["dxc"]["sha256"] = hashlib.sha256(payload).hexdigest()
        self.lock_path.write_text(json.dumps(self.lock), encoding="utf-8")
        with self.assertRaisesRegex(fst.FetchError, "dxil.dll"):
            self.fetcher(payload=payload).dxc()

    def test_dxc_verify_never_downloads(self) -> None:
        with self.assertRaises(fst.FetchError):
            self.fetcher().dxc(verify=True)
        self.assertEqual(self.downloads, [])

    # --- Tint (Dawn) ----------------------------------------------------------------------------

    def test_parse_deps_picks_pinned_commits(self) -> None:
        deps = fst.parse_deps(FAKE_DEPS, ["third_party/abseil-cpp", "third_party/spirv-tools/src"])
        self.assertEqual(deps["third_party/abseil-cpp"],
                         ("https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp",
                          "a" * 40))
        self.assertEqual(deps["third_party/spirv-tools/src"][1], "c" * 40)
        with self.assertRaisesRegex(fst.FetchError, "third_party/missing"):
            fst.parse_deps(FAKE_DEPS, ["third_party/missing"])

    def test_tint_build_uses_only_needed_options_and_deps(self) -> None:
        git, run = FakeGit(), FakeRunner()
        f = self.fetcher(git=git, run=run)
        f.tint()
        self.assertTrue((self.root / "tint" / "tint.exe").is_file())
        configure = run.calls[0]
        for opt in ("-DTINT_BUILD_CMD_TOOLS=ON", "-DTINT_BUILD_SPV_READER=ON", "-DTINT_BUILD_WGSL_WRITER=ON",
                    "-DTINT_BUILD_TESTS=OFF", "-DTINT_BUILD_MSL_WRITER=OFF", "-DTINT_BUILD_HLSL_WRITER=OFF",
                    "-DDAWN_ENABLE_VULKAN=OFF", "-DDAWN_BUILD_SAMPLES=OFF"):
            self.assertIn(opt, configure)
        fetched = {str(cwd.relative_to(self.root / "src" / "dawn")).replace("\\", "/"): sha
                   for cwd, sha in git.fetched.items() if cwd.name != "dawn"}
        self.assertEqual(fetched["third_party/spirv-tools/src"], "c" * 40)
        self.assertNotIn("third_party/unused-big-dep", fetched)
        self.assertEqual(git.fetched[self.root / "src" / "dawn"], DAWN_COMMIT)
        self.assertTrue(any("--depth" in c for c, _ in git.calls if c[0] == "fetch"))

    def test_tint_second_run_does_no_git_or_build(self) -> None:
        self.fetcher().tint()
        git, run = FakeGit(), FakeRunner()
        self.log.clear()
        self.fetcher(git=git, run=run).tint()
        self.assertEqual(git.calls, [])
        self.assertEqual(run.calls, [])
        self.assertTrue(any("up to date" in line for line in self.log), self.log)

    def test_tint_rebuilt_when_pin_changes(self) -> None:
        self.fetcher().tint()
        self.lock["dawn"]["commit"] = "5" * 40
        self.lock_path.write_text(json.dumps(self.lock), encoding="utf-8")
        git, run = FakeGit(), FakeRunner()
        self.fetcher(git=git, run=run).tint()
        self.assertTrue(git.ran("fetch"))
        self.assertTrue(run.calls)

    # --- tool lookup ----------------------------------------------------------------------------

    def test_missing_tool_message(self) -> None:
        f = self.fetcher(env={})
        with self.assertRaises(fst.ToolNotFound) as cm:
            f.tool("spirv-val")
        self.assertEqual(str(cm.exception), "tool not found: spirv-val (run scripts/fetch_shader_tools.py)")
        with self.assertRaisesRegex(fst.ToolNotFound,
                                    r"^tool not found: dxc \(run scripts/fetch_shader_tools.py\)$"):
            f.tool("dxc")

    def test_tools_found_after_fetch_and_from_vulkan_sdk(self) -> None:
        sdk = self.root.parent / "Vulkan SDK"
        (sdk / "Bin").mkdir(parents=True)
        for name in ("spirv-val.exe", "glslangValidator.exe", "dxc.exe"):
            (sdk / "Bin" / name).write_bytes(b"x")
        f = self.fetcher(env={"VULKAN_SDK": str(sdk)})
        f.dxc()
        f.tint()
        self.assertEqual(f.tool("spirv-val"), sdk / "Bin" / "spirv-val.exe")
        self.assertEqual(f.tool("glslangValidator"), sdk / "Bin" / "glslangValidator.exe")
        # DXC must be the pinned one, never the Vulkan SDK copy.
        self.assertEqual(f.tool("dxc"), self.root / "dxc" / "bin" / "x64" / "dxc.exe")
        self.assertEqual(f.tool("tint"), self.root / "tint" / "tint.exe")

    def test_verify_with_missing_tool_exits_2_with_message(self) -> None:
        err = io.StringIO()
        code = fst.main(["--verify"], fetcher_factory=lambda: self.fetcher(env={}), stderr=err)
        self.assertEqual(code, 2)
        self.assertIn("tool not found: ", err.getvalue())
        self.assertIn("(run scripts/fetch_shader_tools.py)", err.getvalue())

    # --- helpers --------------------------------------------------------------------------------

    def test_rmtree_force_removes_read_only_files(self) -> None:
        d = self.root / "ro"
        (d / "objects" / "pack").mkdir(parents=True)
        p = d / "objects" / "pack" / "pack-1.pack"
        p.write_bytes(b"x")
        os.chmod(p, stat.S_IREAD)
        fst.rmtree_force(d)
        self.assertFalse(d.exists())
        fst.rmtree_force(d)  # missing directory is fine


if __name__ == "__main__":
    unittest.main()
