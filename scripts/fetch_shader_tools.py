"""Fetch / build the pinned offline shader toolchain (plan F1 Task 1).

Usage:
    python scripts/fetch_shader_tools.py [--only dxc|tint|all] [--verify]

Everything is pinned in third_party/tools/tools.lock.json (committed; the rest of third_party/tools/
is git-ignored):
    dxc          official DirectXShaderCompiler release zip (tag + url + sha256)
                 -> third_party/tools/dxc/bin/x64/{dxc.exe,dxcompiler.dll,dxil.dll}
    dawn         Dawn repo + exact commit. Tint's CLI is built from it with only the SPIR-V reader and
                 the WGSL writer/reader (the WGSL reader is what lets `tint` validate WGSL). Dawn's own
                 sub-dependencies are taken from the DEPS file of that exact commit (only the ones the
                 Tint CLI build needs), so the Dawn commit pins them transitively.
                 -> third_party/tools/tint/tint.exe
    spirv-cross  commit used by cmake/Dependencies.cmake (FetchContent, library); not fetched here.

Not downloaded (found on the machine): spirv-val + glslangValidator from $VULKAN_SDK/Bin, fxc.exe from
Windows SDK 10.0.26100 (bin/x64).

Idempotent:
  * dxc zip present with the pinned sha256 and extracted -> "up to date", no download;
  * a leftover .part download is deleted; a freshly downloaded zip with the wrong sha256 is deleted and
    the run fails with "sha256 mismatch";
  * tint.exe present and built from the pinned Dawn commit (marker file) -> "up to date", no git, no build.
--verify checks the pins and every tool without network access or building.

Exit codes: 0 ok, 1 fetch / verify failure, 2 usage error or a tool not found
("tool not found: <name> (run scripts/fetch_shader_tools.py)"). Stdlib only; needs git and CMake.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Callable, Mapping, TextIO

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fetch_assets import FetchError, http_download, run_git, sha256_file  # noqa: E402  (reused F0 helpers)

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = REPO / "third_party" / "tools"
WINDOWS_SDK_VERSION = "10.0.26100.0"
CMAKE_GENERATOR = "Visual Studio 18 2026"

# Dawn sub-dependencies needed by a Tint-CLI-only build (everything else in DEPS is skipped).
DAWN_DEPS = [
    "third_party/abseil-cpp",
    "third_party/spirv-headers/src",
    "third_party/spirv-tools/src",
    "third_party/jinja2",
    "third_party/markupsafe",
    "third_party/webgpu-headers/src",
]

# Tint CLI: SPIR-V reader + WGSL writer (+ WGSL reader for validation). Every Dawn backend, test,
# sample and other Tint reader/writer is off.
TINT_CMAKE_OPTIONS = [
    "-DTINT_BUILD_CMD_TOOLS=ON",
    "-DTINT_BUILD_SPV_READER=ON",
    "-DTINT_BUILD_WGSL_WRITER=ON",
    "-DTINT_BUILD_WGSL_READER=ON",
    "-DTINT_BUILD_SPV_WRITER=OFF",
    "-DTINT_BUILD_HLSL_WRITER=OFF",
    "-DTINT_BUILD_MSL_WRITER=OFF",
    "-DTINT_BUILD_GLSL_WRITER=OFF",
    "-DTINT_BUILD_GLSL_VALIDATOR=OFF",
    "-DTINT_BUILD_NULL_WRITER=OFF",
    "-DTINT_BUILD_IR_BINARY=OFF",
    "-DTINT_BUILD_TESTS=OFF",
    "-DTINT_BUILD_BENCHMARKS=OFF",
    "-DDAWN_FETCH_DEPENDENCIES=OFF",
    "-DDAWN_BUILD_SAMPLES=OFF",
    "-DDAWN_BUILD_TESTS=OFF",
    "-DDAWN_BUILD_BENCHMARKS=OFF",
    "-DDAWN_BUILD_PROTOBUF=OFF",
    "-DDAWN_BUILD_MONOLITHIC_LIBRARY=OFF",
    "-DDAWN_ENABLE_D3D11=OFF",
    "-DDAWN_ENABLE_D3D12=OFF",
    "-DDAWN_ENABLE_METAL=OFF",
    "-DDAWN_ENABLE_NULL=OFF",
    "-DDAWN_ENABLE_DESKTOP_GL=OFF",
    "-DDAWN_ENABLE_OPENGLES=OFF",
    "-DDAWN_ENABLE_VULKAN=OFF",
    "-DDAWN_ENABLE_WEBGPU_ON_WEBGPU=OFF",
    "-DDAWN_ENABLE_SPIRV_VALIDATION=OFF",
    "-DDAWN_USE_GLFW=OFF",
    "-DDAWN_USE_WINDOWS_UI=OFF",
]
TINT_TARGET = "tint_cmd_tint_cmd"

TOOL_NAMES = ["dxc", "tint", "spirv-val", "glslangValidator", "fxc"]

Runner = Callable[[list[str], Path], str]


class ToolNotFound(FetchError):
    def __init__(self, name: str):
        super().__init__(f"tool not found: {name} (run scripts/fetch_shader_tools.py)")
        self.name = name


def run_cmd(args: list[str], cwd: Path) -> str:
    proc = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        tail = "\n".join((proc.stdout + proc.stderr).strip().splitlines()[-30:])
        raise FetchError(f"{Path(args[0]).name} {' '.join(args[1:3])} ... failed ({proc.returncode}):\n{tail}")
    return proc.stdout


def find_cmake() -> str:
    # Prefer the official CMake install over other cmake.exe copies on PATH (e.g. Strawberry Perl's).
    official = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "CMake" / "bin" / "cmake.exe"
    if official.is_file():
        return str(official)
    found = shutil.which("cmake")
    if not found:
        raise ToolNotFound("cmake")
    return found


def _on_rm_error(func, path, exc) -> None:
    # Git pack/idx files are read-only on Windows: clear the flag and retry once.
    os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
    func(path)


def rmtree_force(path: Path) -> None:
    """shutil.rmtree that also deletes read-only files (git objects on Windows). Missing path = no-op."""
    path = Path(path)
    if not path.exists():
        return
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=_on_rm_error)
    else:  # pragma: no cover
        shutil.rmtree(path, onerror=lambda f, p, e: _on_rm_error(f, p, e[1]))


_DEP_RE = re.compile(r"'(?P<path>[^']+)'\s*:\s*\{\s*'url'\s*:\s*'(?P<url>[^'@]+)@(?P<sha>[0-9a-f]{40})'")


def parse_deps(deps_text: str, names: list[str]) -> dict[str, tuple[str, str]]:
    """Read pinned (url, commit) for `names` from a Dawn DEPS file (text parsing only; DEPS is never
    executed). `{var}` placeholders are expanded from its `vars` block."""
    variables = dict(re.findall(r"'(\w+)'\s*:\s*'(https?://[^']+)'", deps_text))
    found: dict[str, tuple[str, str]] = {}
    for m in _DEP_RE.finditer(deps_text):
        url = re.sub(r"\{(\w+)\}", lambda v: variables.get(v.group(1), v.group(0)), m.group("url"))
        found[m.group("path")] = (url, m.group("sha"))
    missing = [n for n in names if n not in found]
    if missing:
        raise FetchError(f"Dawn DEPS has no pinned entry for: {', '.join(missing)}")
    return {n: found[n] for n in names}


class ShaderToolsFetcher:
    def __init__(self, root: Path = DEFAULT_ROOT, lock_path: Path | None = None, git=run_git,
                 download=http_download, run: Runner = run_cmd, log: Callable[[str], None] = print,
                 env: Mapping[str, str] | None = None, cmake: str | None = None):
        self.root = Path(root)
        self.lock_path = Path(lock_path) if lock_path else self.root / "tools.lock.json"
        try:
            import json
            self.lock = json.loads(self.lock_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise FetchError(f"cannot read lock file '{self.lock_path}': {e}") from e
        self.git = git
        self.download = download
        self.run = run
        self.log = log
        self.env = os.environ if env is None else env
        self._cmake = cmake

    # --- DXC ------------------------------------------------------------------------------------

    @property
    def dxc_dir(self) -> Path:
        return self.root / "dxc"

    def _dxc_paths(self) -> tuple[Path, Path]:
        url = self.lock["dxc"]["url"]
        archive = self.root / "downloads" / url.rsplit("/", 1)[-1]
        return archive, archive.with_name(archive.name + ".part")

    def _dxc_extracted_ok(self, sha: str) -> bool:
        try:
            ok = (self.dxc_dir / ".archive-sha256").read_text(encoding="utf-8").strip() == sha
        except OSError:
            return False
        return ok and all((self.dxc_dir / "bin" / "x64" / n).is_file() for n in ("dxc.exe", "dxcompiler.dll", "dxil.dll"))

    def _dxc_extract(self, archive: Path, sha: str) -> None:
        tmp = self.dxc_dir.with_name("dxc.tmp")
        rmtree_force(tmp)
        tmp.mkdir(parents=True)
        with zipfile.ZipFile(archive) as z:
            names = set(z.namelist())
            missing = [n for n in ("bin/x64/dxc.exe", "bin/x64/dxcompiler.dll", "bin/x64/dxil.dll") if n not in names]
            if missing:
                rmtree_force(tmp)
                raise FetchError(f"dxc: archive '{archive.name}' has no {', '.join(missing)}")
            for info in z.infolist():
                target = (tmp / info.filename).resolve()
                if not target.is_relative_to(tmp.resolve()):
                    raise FetchError(f"dxc: unsafe path in archive: {info.filename}")
            z.extractall(tmp)
        (tmp / ".archive-sha256").write_text(sha + "\n", encoding="utf-8")
        rmtree_force(self.dxc_dir)
        os.replace(tmp, self.dxc_dir)

    def dxc(self, verify: bool = False) -> None:
        entry = self.lock["dxc"]
        tag, url, sha = entry["tag"], entry["url"], entry["sha256"].lower()
        archive, part = self._dxc_paths()
        if part.exists() and not verify:
            self.log(f"dxc: removing partial download '{part.name}'")
            part.unlink()
        if archive.exists():
            actual = sha256_file(archive)
            if actual == sha:
                if self._dxc_extracted_ok(sha):
                    self.log(f"dxc: up to date ({tag}, {self.dxc_dir / 'bin' / 'x64' / 'dxc.exe'})")
                    return
                if verify:
                    raise FetchError(f"dxc: archive ok but not extracted in '{self.dxc_dir}'")
                self._dxc_extract(archive, sha)
                self.log(f"dxc: extracted {tag}")
                return
            if verify:
                raise FetchError(f"dxc: sha256 mismatch for '{archive.name}': expected {sha}, got {actual}")
            self.log(f"dxc: '{archive.name}' has sha256 {actual}, expected {sha}; downloading again")
            archive.unlink()
        elif verify:
            raise FetchError(f"dxc: '{archive}' missing")

        archive.parent.mkdir(parents=True, exist_ok=True)
        self.log(f"dxc: downloading {url}")
        try:
            self.download(url, part)
        except Exception:
            part.unlink(missing_ok=True)
            raise
        actual = sha256_file(part)
        if actual != sha:
            part.unlink()
            raise FetchError(f"dxc: sha256 mismatch for '{archive.name}': expected {sha}, got {actual}")
        os.replace(part, archive)
        self._dxc_extract(archive, sha)
        self.log(f"dxc: {tag} -> {self.dxc_dir / 'bin' / 'x64' / 'dxc.exe'}")

    # --- Tint (built from Dawn) -----------------------------------------------------------------

    @property
    def tint_dir(self) -> Path:
        return self.root / "tint"

    @property
    def dawn_src(self) -> Path:
        return self.root / "src" / "dawn"

    @property
    def dawn_build(self) -> Path:
        return self.root / "build" / "dawn"

    def _tint_marker(self) -> Path:
        return self.tint_dir / ".dawn-commit"

    def _tint_ok(self, commit: str) -> bool:
        try:
            marker = self._tint_marker().read_text(encoding="utf-8").strip()
        except OSError:
            return False
        return marker == commit and (self.tint_dir / "tint.exe").is_file()

    def _checkout(self, d: Path, repo: str, commit: str) -> None:
        """Shallow checkout of exactly `commit` into `d` (reused when already there)."""
        def head() -> str | None:
            if not (d / ".git").exists():
                return None
            try:
                top = self.git(["rev-parse", "--show-toplevel"], d).strip()
                if Path(top).resolve() != d.resolve():
                    return None
                return self.git(["rev-parse", "HEAD"], d).strip()
            except FetchError:
                return None

        if head() == commit:
            return
        if d.exists():
            rmtree_force(d)
        d.mkdir(parents=True)
        self.git(["init", "-q"], d)
        self.git(["remote", "add", "origin", repo], d)
        self.git(["fetch", "-q", "--depth", "1", "origin", commit], d)
        self.git(["checkout", "-q", "--force", "--detach", "FETCH_HEAD"], d)
        got = head()
        if got != commit:
            raise FetchError(f"checkout of {repo} ended at {got}, expected {commit}")

    def tint(self, verify: bool = False) -> None:
        entry = self.lock["dawn"]
        repo, commit = entry["repo"], entry["commit"]
        if self._tint_ok(commit):
            self.log(f"tint: up to date (Dawn {commit}, {self.tint_dir / 'tint.exe'})")
            return
        if verify:
            raise FetchError(f"tint: '{self.tint_dir / 'tint.exe'}' not built from Dawn {commit}")

        self.log(f"tint: checking out Dawn {commit} (shallow) ...")
        self._checkout(self.dawn_src, repo, commit)
        deps = parse_deps((self.dawn_src / "DEPS").read_text(encoding="utf-8"), DAWN_DEPS)
        for path, (url, sha) in deps.items():
            self.log(f"tint: dependency {path} @ {sha}")
            self._checkout(self.dawn_src / path, url, sha)

        cmake = self._cmake or find_cmake()
        self.log("tint: configuring (Tint CLI only) ...")
        self.run([cmake, "-S", str(self.dawn_src), "-B", str(self.dawn_build), "-G", CMAKE_GENERATOR,
                  "-A", "x64", *TINT_CMAKE_OPTIONS], self.root)
        self.log("tint: building Release (takes a while) ...")
        self.run([cmake, "--build", str(self.dawn_build), "--config", "Release", "--target", TINT_TARGET,
                  "--parallel"], self.root)
        built = self.dawn_build / "Release" / "tint.exe"
        if not built.is_file():
            raise FetchError(f"tint: build finished but '{built}' does not exist")
        self.tint_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(built, self.tint_dir / "tint.exe")
        self._tint_marker().write_text(commit + "\n", encoding="utf-8")
        self.log(f"tint: Dawn {commit} -> {self.tint_dir / 'tint.exe'}")

    # --- tool lookup ----------------------------------------------------------------------------

    def tool(self, name: str) -> Path:
        """Absolute path of a toolchain executable, or ToolNotFound. dxc and tint come only from
        third_party/tools (never the Vulkan SDK's dxc.exe)."""
        candidates: list[Path] = []
        if name == "dxc":
            candidates = [self.dxc_dir / "bin" / "x64" / "dxc.exe"]
        elif name == "tint":
            candidates = [self.tint_dir / "tint.exe"]
        elif name in ("spirv-val", "glslangValidator"):
            sdk = self.env.get("VULKAN_SDK")
            if sdk:
                candidates = [Path(sdk) / "Bin" / f"{name}.exe", Path(sdk) / "bin" / name]
        elif name == "fxc":
            kits = self.env.get("ENG_WINDOWS_KITS",
                                str(Path(self.env.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10"))
            candidates = [Path(kits) / "bin" / WINDOWS_SDK_VERSION / "x64" / "fxc.exe"]
        else:
            raise ToolNotFound(name)
        for c in candidates:
            if c.is_file():
                return c
        raise ToolNotFound(name)


def main(argv: list[str] | None = None, fetcher_factory: Callable[[], ShaderToolsFetcher] | None = None,
         stderr: TextIO | None = None) -> int:
    err = stderr or sys.stderr
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", choices=["dxc", "tint", "all"], default="all", help="fetch/build only one item")
    ap.add_argument("--verify", action="store_true", help="check pins and tools only; no network, no build")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    try:
        f = (fetcher_factory or ShaderToolsFetcher)()
        names = TOOL_NAMES if args.only == "all" else [args.only]
        if args.verify:  # presence first: a missing tool is "tool not found" (exit 2), not a pin error
            for name in names:
                f.tool(name)
        if args.only in ("all", "dxc"):
            f.dxc(verify=args.verify)
        if args.only in ("all", "tint"):
            f.tint(verify=args.verify)
        for name in names:
            f.log(f"{name}: {f.tool(name)}")
    except ToolNotFound as e:
        print(f"fetch_shader_tools: {e}", file=err)
        return 2
    except FetchError as e:
        print(f"fetch_shader_tools: {e}", file=err)
        return 1
    except OSError as e:
        print(f"fetch_shader_tools: IO / network error: {e}", file=err)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
