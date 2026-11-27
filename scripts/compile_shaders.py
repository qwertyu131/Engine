"""Compile every glTF PBR permutation for every target and validate it (plan F1 Task 4).

Usage:
    python scripts/compile_shaders.py --out build/shaders [--jobs N] [--shaderc <shaderc.exe>]
                                      [--permutations shaders/pbr/permutations.json]
                                      [--dxbc-path direct|cross]

For every (target, define set, entry) of shaders/pbr/permutations.json, runs
    shaderc compile --src <source> --entry <e> --stage <s> --target <t> --define K=V... --out <dir> --validate
with one output directory per target and define set: <out>/<target>/<key>/ (key e.g.
T2_SKIN1_MORPH0_UNLIT0), so parallel jobs never write the same file. Defines are passed in a canonical
order (axes, then fixed ones) so the same permutation always produces the same command.

Writes <out>/summary.json:
    {"total", "ok", "failed": [{"id", "target", "stage", "key", "error"}], "pendingCi": [id...], "msTotal",
     "msCompileSum", "shaderc", "dxbcPath", "items": [{"id", "target", "entry", "stage", "key", "defines",
     "status", "ms", "bytes", "output"}]}
    ok        = compiled and passed its validator;
    pendingCi = compiled, validation runs only on the macOS CI job (MSL, spec K2) - never counted as ok;
    failed    = compile or validation failure, or shaderc exit 2 (usage / tool missing);
    msTotal   = wall-clock time of the whole run; ms per item = wall-clock time of its shaderc process.

Exit codes: 0 no failure, 1 any failure, 2 usage error / shaderc not found / invalid permutations file.
Stdlib only.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import itertools
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[1]
DEFAULT_PERMUTATIONS = REPO / "shaders" / "pbr" / "permutations.json"
TARGETS = ["dxil", "dxbc", "spirv", "msl", "glsl-es300", "glsl-es320", "wgsl"]  # shaderc order
MAX_PER_TARGET_PER_STAGE = 8  # plan F1 Task 4 / spec K-A budget
KEY_NAMES = {"TIER": "T", "FG_SKIN": "SKIN", "FG_MORPH": "MORPH", "FG_UNLIT": "UNLIT"}
RESULT_LINE = re.compile(r"-> (?P<path>.+?) \[(?P<status>passed|PENDING-CI)\]\s*$")

Runner = Callable[[list[str]], "tuple[int, str, str]"]


@dataclass
class Job:
    target: str
    entry: str
    stage: str
    key: str
    defines: dict[str, str] = field(default_factory=dict)

    @property
    def id(self) -> str:
        return f"{self.target}/{self.key}/{self.entry}"


def load_permutations(path: Path) -> dict:
    perms = json.loads(Path(path).read_text(encoding="utf-8"))
    check_permutations(perms)
    return perms


def check_permutations(perms: dict) -> None:
    """Raises ValueError unless 'excluded' is exactly the complement of each target's 'tiers'."""
    all_tiers = set(perms["axes"]["TIER"])
    unknown = set(perms["targets"]) - set(TARGETS)
    if unknown:
        raise ValueError(f"unknown targets in permutations file: {sorted(unknown)}")
    other = 1
    for axis, values in perms["axes"].items():
        if axis != "TIER":
            other *= len(values)
    for target, spec in perms["targets"].items():
        tiers = set(spec["tiers"])
        if not tiers <= all_tiers:
            raise ValueError(f"{target}: tiers {sorted(tiers - all_tiers)} are not on the TIER axis")
        excluded: set[int] = set()
        for e in perms.get("excluded", []):
            if e["target"] == target:
                excluded |= set(e["TIER"])
        if excluded & tiers or excluded | tiers != all_tiers:
            raise ValueError(f"{target}: 'excluded' TIER {sorted(excluded)} is not the complement of tiers "
                             f"{sorted(tiers)}")
        if len(tiers) * other > MAX_PER_TARGET_PER_STAGE:
            raise ValueError(f"{target}: {len(tiers) * other} define sets per stage > {MAX_PER_TARGET_PER_STAGE}")


def permutation_key(defines: dict[str, str]) -> str:
    return "_".join(f"{KEY_NAMES[k]}{v}" for k, v in defines.items() if k in KEY_NAMES)


def expand_jobs(perms: dict) -> list[Job]:
    """Every (target, define set, entry), targets in shaderc order, define sets in axis order."""
    axes = list(perms["axes"].items())
    jobs: list[Job] = []
    for target in [t for t in TARGETS if t in perms["targets"]]:
        tiers = set(perms["targets"][target]["tiers"])
        for values in itertools.product(*[v for _, v in axes]):
            defines = {k: str(v) for (k, _), v in zip(axes, values)}
            if int(defines["TIER"]) not in tiers:
                continue
            defines.update({k: str(v) for k, v in perms.get("fixed", {}).items()})
            key = permutation_key(defines)
            for e in perms["entries"]:
                jobs.append(Job(target, e["entry"], e["stage"], key, dict(defines)))
    # Stable order: target (shaderc order), permutation key, entry.
    jobs.sort(key=lambda j: (TARGETS.index(j.target), j.key, j.entry))
    return jobs


def default_shaderc() -> Path | None:
    candidates = [REPO / "build" / "win-msvc" / "tools" / "shaderc" / cfg / "shaderc.exe"
                  for cfg in ("Release", "RelWithDebInfo", "Debug")]
    existing = [c for c in candidates if c.is_file()]
    return max(existing, key=lambda p: p.stat().st_mtime) if existing else None


def run_process(cmd: list[str]) -> tuple[int, str, str]:
    p = subprocess.run(cmd, capture_output=True)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def command(shaderc: Path, src: Path, job: Job, out: Path, dxbc_path: str | None) -> list[str]:
    cmd = [str(shaderc), "compile", "--src", str(src), "--entry", job.entry, "--stage", job.stage,
           "--target", job.target]
    for k, v in job.defines.items():
        cmd += ["--define", f"{k}={v}"]
    cmd += ["--out", str(out), "--validate"]
    if dxbc_path and job.target == "dxbc":
        cmd += ["--dxbc-path", dxbc_path]
    return cmd


def run_job(runner: Runner, shaderc: Path, src: Path, job: Job, out_root: Path, dxbc_path: str | None) -> dict:
    out = out_root / job.target / job.key
    start = time.perf_counter()
    rc, stdout, stderr = runner(command(shaderc, src, job, out, dxbc_path))
    ms = (time.perf_counter() - start) * 1000.0
    item = {"id": job.id, "target": job.target, "entry": job.entry, "stage": job.stage, "key": job.key,
            "defines": job.defines, "ms": round(ms, 1)}
    m = None
    for line in stdout.splitlines():
        m = RESULT_LINE.search(line) or m
    if rc == 0 and m:
        path = Path(m.group("path"))
        item["status"] = "pendingCi" if m.group("status") == "PENDING-CI" else "ok"
        item["bytes"] = path.stat().st_size if path.is_file() else None
        try:
            item["output"] = path.resolve().relative_to(out_root.resolve()).as_posix()
        except ValueError:
            item["output"] = str(path)
    else:
        item["status"] = "failed"
        reason = f"shaderc exit {rc}" if rc != 0 else "shaderc printed no validation status"
        item["error"] = (reason + ":\n" + (stderr + stdout).strip()).strip()
    return item


def main(argv: list[str] | None = None, runner: Runner = run_process) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True, type=Path, help="output directory (created)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--shaderc", type=Path, help="shaderc executable (default: newest build/win-msvc one)")
    ap.add_argument("--permutations", type=Path, default=DEFAULT_PERMUTATIONS)
    ap.add_argument("--dxbc-path", choices=["direct", "cross"], help="passed to shaderc for the dxbc target")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 2 if e.code else 0

    shaderc = args.shaderc or default_shaderc()
    if shaderc is None or not Path(shaderc).is_file():
        print("compile_shaders: tool not found: shaderc (build the 'shaderc' target, or pass --shaderc)",
              file=sys.stderr)
        return 2
    try:
        perms = load_permutations(args.permutations)
    except (OSError, ValueError, KeyError) as e:
        print(f"compile_shaders: invalid permutations file '{args.permutations}': {e}", file=sys.stderr)
        return 2
    src = (REPO / perms["source"]).resolve()  # "source" is repo-relative
    jobs = expand_jobs(perms)
    out_root: Path = args.out
    out_root.mkdir(parents=True, exist_ok=True)

    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        items = list(pool.map(lambda j: run_job(runner, Path(shaderc), src, j, out_root, args.dxbc_path), jobs))
    ms_total = (time.perf_counter() - start) * 1000.0

    failed = [{"id": i["id"], "target": i["target"], "stage": i["stage"], "key": i["key"], "error": i["error"]}
              for i in items if i["status"] == "failed"]
    summary = {
        "total": len(items),
        "ok": sum(1 for i in items if i["status"] == "ok"),
        "failed": failed,
        "pendingCi": [i["id"] for i in items if i["status"] == "pendingCi"],
        "msTotal": round(ms_total, 1),
        "msCompileSum": round(sum(i["ms"] for i in items), 1),
        "shaderc": str(shaderc),
        "dxbcPath": args.dxbc_path or "shaderc default",
        "items": items,
    }
    (out_root / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for f in failed:
        print(f"FAILED {f['id']}: {f['error']}", file=sys.stderr)
    print(f"compile_shaders: total {summary['total']}, ok {summary['ok']}, failed {len(failed)}, "
          f"pendingCi {len(summary['pendingCi'])}, {summary['msTotal'] / 1000.0:.1f} s -> {out_root / 'summary.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
