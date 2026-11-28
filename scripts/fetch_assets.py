"""Fetch the pinned glTF test assets and the Khronos glTF-Validator (plan F0 Task 6).

Usage:
    python scripts/fetch_assets.py [--only sample-assets|validator|environments] [--verify]

Everything is pinned in tests/assets/assets.lock.json:
    glTF-Sample-Assets  git repo + exact commit + sparse-checkout patterns
                        -> tests/assets/glTF-Sample-Assets/  (shallow, blob-filtered, sparse)
    gltf-validator      release version + archive url + sha256 (win64; "linux64" sub-entry for Linux)
                        -> tests/assets/tools/<archive>  and  tests/assets/tools/gltf_validator-<ver>-<platform>/
Environment files (plan F3 Task 5) are pinned in tests/golden/ref/sample-viewer/settings.json "pinnedFiles"
(url at a commit + sha256, the files refgen renders the references with):
    Cannon_Exterior.hdr, lut_sheen_E.png -> tests/assets/environments/<name>

Idempotent:
  * checkout already at the pinned commit -> "up to date", no network access;
  * checkout at another commit, or a clone interrupted before checkout -> fetch + re-checkout of the pin;
  * archive present with the pinned sha256 and extracted -> "up to date", no download;
  * a leftover <archive>.part from an interrupted download is deleted and the download restarts;
  * an archive whose sha256 differs is deleted and downloaded again; a freshly downloaded file with
    the wrong sha256 is deleted and the run fails with "sha256 mismatch" (environment files alike).
--verify checks the pins without any network access (exit 1 if something is missing or wrong).

Exit codes: 0 ok, 1 fetch / verify failure, 2 usage error. Stdlib only (needs `git` on PATH).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOT = REPO / "tests" / "assets"
DEFAULT_SETTINGS = REPO / "tests" / "golden" / "ref" / "sample-viewer" / "settings.json"
USER_AGENT = "engine-fetch-assets/1.0"
CHUNK = 1 << 20


class FetchError(Exception):
    pass


GitRunner = Callable[[list[str], Path], str]
Downloader = Callable[[str, Path], None]


def run_git(args: list[str], cwd: Path) -> str:
    # GIT_CEILING_DIRECTORIES = parent of cwd: git never searches above `cwd` for a repository, so a
    # broken checkout can never make git operate on an enclosing repo (e.g. the Engine repo itself).
    env = dict(os.environ)
    env["GIT_CEILING_DIRECTORIES"] = str(Path(cwd).resolve().parent)
    proc = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", env=env)
    if proc.returncode != 0:
        raise FetchError(f"git {' '.join(args)} failed ({proc.returncode}): {proc.stderr.strip()}")
    return proc.stdout


def http_download(url: str, dest: Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as resp, open(dest, "wb") as out:
        while True:
            chunk = resp.read(CHUNK)
            if not chunk:
                break
            out.write(chunk)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def host_platform_key() -> str | None:
    system = platform.system()
    if system == "Windows":
        return "win64"
    if system == "Linux":
        return "linux64"
    return None


class Fetcher:
    def __init__(self, root: Path = DEFAULT_ROOT, lock_path: Path | None = None, git: GitRunner = run_git,
                 download: Downloader = http_download, log: Callable[[str], None] = print,
                 platform_key: str | None = None, settings_path: Path | None = None):
        self.root = Path(root)
        self.settings_path = Path(settings_path) if settings_path else DEFAULT_SETTINGS
        self.lock_path = Path(lock_path) if lock_path else self.root / "assets.lock.json"
        try:
            self.lock = json.loads(self.lock_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise FetchError(f"cannot read lock file '{self.lock_path}': {e}") from e
        self.git = git
        self.download = download
        self.log = log
        self.platform_key = platform_key or host_platform_key()

    # --- glTF-Sample-Assets ---------------------------------------------------------------------

    @property
    def sample_dir(self) -> Path:
        return self.root / "glTF-Sample-Assets"

    def _is_own_repo(self) -> bool:
        """True only if sample_dir is itself a valid repository root (not a broken .git that would
        make git fall through to an enclosing repository)."""
        if not (self.sample_dir / ".git").exists():
            return False
        try:
            top = self.git(["rev-parse", "--show-toplevel"], self.sample_dir).strip()
        except FetchError:
            return False
        try:
            return Path(top).resolve() == self.sample_dir.resolve()
        except OSError:
            return False

    def _head(self) -> str | None:
        if not self._is_own_repo():
            return None
        try:
            return self.git(["rev-parse", "HEAD"], self.sample_dir).strip()
        except FetchError:
            return None  # unborn HEAD (interrupted before checkout)

    def sample_assets(self, verify: bool = False) -> None:
        entry = self.lock["glTF-Sample-Assets"]
        repo, commit = entry["repo"], entry["commit"]
        sparse = entry.get("sparse", [])
        head = self._head()
        if head == commit:
            self.log(f"glTF-Sample-Assets: up to date ({commit})")
            return
        if verify:
            raise FetchError(f"glTF-Sample-Assets: expected commit {commit}, found {head or 'no checkout'}")

        d = self.sample_dir
        if d.exists() and not self._is_own_repo():
            self.log(f"glTF-Sample-Assets: '{d}' is not a valid git checkout, recreating it")
            shutil.rmtree(d)
        if not d.exists():
            d.mkdir(parents=True)
            self.git(["init", "-q"], d)
            self.git(["remote", "add", "origin", repo], d)
        else:
            self.log(f"glTF-Sample-Assets: at {head or 'no commit'}, re-checking out {commit}")
            try:
                self.git(["remote", "set-url", "origin", repo], d)
            except FetchError:  # run interrupted between `init` and `remote add`
                self.git(["remote", "add", "origin", repo], d)
        if sparse:
            self.git(["sparse-checkout", "set", "--no-cone", *sparse], d)
        else:
            self.git(["sparse-checkout", "disable"], d)
        self.log(f"glTF-Sample-Assets: fetching {commit} (shallow, blob-filtered) ...")
        self.git(["fetch", "--depth", "1", "--filter=blob:none", "origin", commit], d)
        self.git(["checkout", "-q", "--force", "--detach", commit], d)
        head = self._head()
        if head != commit:
            raise FetchError(f"glTF-Sample-Assets: checkout ended at {head}, expected {commit}")
        self.log(f"glTF-Sample-Assets: checked out {commit}")

    # --- glTF-Validator -------------------------------------------------------------------------

    def _validator_entry(self) -> tuple[str, str, str]:
        entry = self.lock["gltf-validator"]
        if self.platform_key == "win64":
            return entry["version"], entry["url"], entry["sha256"].lower()
        if self.platform_key in entry:
            sub = entry[self.platform_key]
            return entry["version"], sub["url"], sub["sha256"].lower()
        raise FetchError(f"gltf-validator: no pinned archive for platform '{self.platform_key}'")

    @property
    def tools_dir(self) -> Path:
        return self.root / "tools"

    def _paths(self) -> tuple[Path, Path, Path]:
        version, url, _ = self._validator_entry()
        archive = self.tools_dir / url.rsplit("/", 1)[-1]
        part = archive.with_name(archive.name + ".part")
        extract = self.tools_dir / f"gltf_validator-{version}-{self.platform_key}"
        return archive, part, extract

    def validator_exe(self) -> Path:
        _, _, extract = self._paths()
        for name in ("gltf_validator.exe", "gltf_validator"):
            if self.platform_key == "win64" and not name.endswith(".exe"):
                continue
            for p in sorted(extract.rglob(name)):
                if p.is_file():
                    return p
        raise FetchError(f"gltf-validator: executable not found under '{extract}'")

    def _extracted_ok(self, extract: Path, sha: str) -> bool:
        marker = extract / ".archive-sha256"
        try:
            return marker.read_text(encoding="utf-8").strip() == sha
        except OSError:
            return False

    def _extract(self, archive: Path, extract: Path, sha: str) -> None:
        tmp = extract.with_name(extract.name + ".tmp")
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(extract, ignore_errors=True)
        tmp.mkdir(parents=True)
        if archive.name.endswith(".zip"):
            with zipfile.ZipFile(archive) as z:
                for info in z.infolist():
                    target = (tmp / info.filename).resolve()
                    if not target.is_relative_to(tmp.resolve()):
                        raise FetchError(f"gltf-validator: unsafe path in archive: {info.filename}")
                z.extractall(tmp)
        else:
            with tarfile.open(archive) as t:
                t.extractall(tmp, filter="data")
        if self.platform_key != "win64":
            for p in tmp.rglob("gltf_validator"):
                p.chmod(p.stat().st_mode | 0o111)
        (tmp / ".archive-sha256").write_text(sha + "\n", encoding="utf-8")
        os.replace(tmp, extract)

    def validator(self, verify: bool = False) -> None:
        version, url, sha = self._validator_entry()
        archive, part, extract = self._paths()

        if part.exists() and not verify:
            self.log(f"gltf-validator: removing partial download '{part.name}'")
            part.unlink()

        if archive.exists():
            actual = sha256_file(archive)
            if actual == sha:
                if self._extracted_ok(extract, sha):
                    self.log(f"gltf-validator: up to date ({version}, {self.validator_exe()})")
                    return
                if verify:
                    raise FetchError(f"gltf-validator: archive ok but not extracted in '{extract}'")
                self._extract(archive, extract, sha)
                self.log(f"gltf-validator: extracted {version} -> {self.validator_exe()}")
                return
            if verify:
                raise FetchError(f"gltf-validator: sha256 mismatch for '{archive.name}': expected {sha}, got {actual}")
            self.log(f"gltf-validator: '{archive.name}' has sha256 {actual}, expected {sha}; downloading again")
            archive.unlink()
        elif verify:
            raise FetchError(f"gltf-validator: '{archive}' missing")

        self.tools_dir.mkdir(parents=True, exist_ok=True)
        self.log(f"gltf-validator: downloading {url}")
        try:
            self.download(url, part)
        except Exception:
            part.unlink(missing_ok=True)
            raise
        actual = sha256_file(part)
        if actual != sha:
            part.unlink()
            raise FetchError(f"gltf-validator: sha256 mismatch for '{archive.name}': expected {sha}, got {actual}")
        os.replace(part, archive)
        self._extract(archive, extract, sha)
        self.log(f"gltf-validator: {version} -> {self.validator_exe()}")

    # --- environment files (settings.json pinnedFiles) -----------------------------------------

    def environment_pins(self) -> dict:
        try:
            pins = json.loads(self.settings_path.read_text(encoding="utf-8"))["pinnedFiles"]
        except (OSError, ValueError, KeyError) as e:
            raise FetchError(f"cannot read pinnedFiles from '{self.settings_path}': {e}") from e
        if not isinstance(pins, dict):
            raise FetchError(f"'{self.settings_path}': pinnedFiles must be an object")
        return pins

    @property
    def environments_dir(self) -> Path:
        return self.root / "environments"

    def environments(self, verify: bool = False) -> None:
        for name, pin in self.environment_pins().items():
            if "/" in name or "\\" in name or name in (".", ".."):
                raise FetchError(f"environments: invalid file name {name!r}")
            url, sha = pin["url"], pin["sha256"].lower()
            dest = self.environments_dir / name
            part = dest.with_name(dest.name + ".part")
            if part.exists() and not verify:
                part.unlink()
            if dest.exists():
                actual = sha256_file(dest)
                if actual == sha:
                    self.log(f"environments: {name} up to date")
                    continue
                if verify:
                    raise FetchError(f"environments: sha256 mismatch for '{name}': expected {sha}, got {actual}")
                self.log(f"environments: '{name}' has sha256 {actual}, expected {sha}; downloading again")
                dest.unlink()
            elif verify:
                raise FetchError(f"environments: '{dest}' missing")
            self.environments_dir.mkdir(parents=True, exist_ok=True)
            self.log(f"environments: downloading {url}")
            try:
                self.download(url, part)
            except Exception:
                part.unlink(missing_ok=True)
                raise
            actual = sha256_file(part)
            if actual != sha:
                part.unlink()
                raise FetchError(f"environments: sha256 mismatch for '{name}': expected {sha}, got {actual}")
            os.replace(part, dest)
            self.log(f"environments: {name} -> {dest}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", choices=["sample-assets", "validator", "environments"], help="fetch only one item")
    ap.add_argument("--verify", action="store_true", help="check the pins only, no network access")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    try:
        f = Fetcher()
        if args.only in (None, "sample-assets"):
            f.sample_assets(verify=args.verify)
        if args.only in (None, "validator"):
            f.validator(verify=args.verify)
        if args.only in (None, "environments"):
            f.environments(verify=args.verify)
    except FetchError as e:
        print(f"fetch_assets: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"fetch_assets: IO / network error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
