"""Committed MSL outputs for the macOS CI job (plan F1 Task 5).

Usage:
    python scripts/msl_artifacts.py [--shaderc <shaderc.exe>] [--dest tests/shaders/msl] [--check]

Compiles every msl permutation of shaders/pbr/permutations.json (8 define sets x VSMain/PSMain = 16 files)
with shaderc, exactly as scripts/compile_shaders.py does, into a temporary directory, then
    default : writes them to <dest>/pbr.<key>.<entry>.metal (stale *.metal files removed);
    --check : compares them byte for byte with the committed files; any missing, extra or different file
              -> exit 1 with the file names (the macOS job would otherwise validate stale MSL).
The macOS job (.github/workflows/ci.yml, job macos-metal) runs scripts/check_metal.py on <dest>, so it
needs no Windows tool.

Exit codes: 0 ok, 1 check mismatch or compile failure, 2 usage error / shaderc not found.
Stdlib only.
"""
from __future__ import annotations

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_DEST = REPO / "tests" / "shaders" / "msl"


def _compile_shaders():
    spec = importlib.util.spec_from_file_location("compile_shaders", REPO / "scripts" / "compile_shaders.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("compile_shaders", mod)
    spec.loader.exec_module(mod)
    return mod


cs = _compile_shaders()


def artifact_name(job) -> str:
    return f"pbr.{job.key}.{job.entry}.metal"


def generate(shaderc: Path, out_root: Path, runner=cs.run_process,
             permutations: Path = cs.DEFAULT_PERMUTATIONS) -> tuple[dict[str, bytes], list[str]]:
    """Returns ({artifact name: MSL bytes}, [errors])."""
    perms = cs.load_permutations(permutations)
    src = (REPO / perms["source"]).resolve()
    files: dict[str, bytes] = {}
    errors: list[str] = []
    for job in [j for j in cs.expand_jobs(perms) if j.target == "msl"]:
        item = cs.run_job(runner, shaderc, src, job, out_root, None)
        if item["status"] != "pendingCi":
            errors.append(f"{job.id}: {item.get('error', item['status'])}")
            continue
        files[artifact_name(job)] = (out_root / item["output"]).read_bytes()
    return files, errors


def compare(files: dict[str, bytes], dest: Path) -> list[str]:
    committed = {p.name: p.read_bytes() for p in sorted(dest.glob("*.metal"))} if dest.is_dir() else {}
    problems = [f"missing in {dest}: {n}" for n in sorted(set(files) - set(committed))]
    problems += [f"not produced by shaderc (stale): {n}" for n in sorted(set(committed) - set(files))]
    problems += [f"differs from fresh shaderc output: {n}" for n in sorted(set(files) & set(committed))
                 if files[n] != committed[n]]
    return problems


def write(files: dict[str, bytes], dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for stale in dest.glob("*.metal"):
        if stale.name not in files:
            stale.unlink()
    for name, data in sorted(files.items()):
        (dest / name).write_bytes(data)


def main(argv: list[str] | None = None, runner=cs.run_process) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--shaderc", type=Path, help="shaderc executable (default: newest build/win-msvc one)")
    ap.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    ap.add_argument("--permutations", type=Path, default=cs.DEFAULT_PERMUTATIONS)
    ap.add_argument("--check", action="store_true", help="compare instead of writing")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0
    shaderc = args.shaderc or cs.default_shaderc()
    if shaderc is None or not Path(shaderc).is_file():
        print("msl_artifacts: tool not found: shaderc (build the 'shaderc' target, or pass --shaderc)",
              file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="msl-artifacts-") as tmp:
        files, errors = generate(Path(shaderc), Path(tmp), runner, args.permutations)
    for e in errors:
        print(f"msl_artifacts: FAILED {e}", file=sys.stderr)
    if errors:
        return 1
    if args.check:
        problems = compare(files, args.dest)
        for p in problems:
            print(f"msl_artifacts: {p}", file=sys.stderr)
        if problems:
            print("msl_artifacts: run `python scripts/msl_artifacts.py` to refresh", file=sys.stderr)
            return 1
        print(f"msl_artifacts: {len(files)} files in {args.dest} match fresh shaderc output")
        return 0
    write(files, args.dest)
    print(f"msl_artifacts: wrote {len(files)} files to {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
