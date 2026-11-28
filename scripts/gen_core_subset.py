"""Core reference subset of glTF-Sample-Assets (plan F3 Task 6, spec §7.3.2).

Usage:
    python scripts/gen_core_subset.py [--check]          writes / checks tests/golden/f03/core_subset.json
    python scripts/gen_core_subset.py --refgen-models    prints the refgen --models list (models without a reference)
    python scripts/gen_core_subset.py --cases [--check]   writes / checks the subset cases of tests/golden/f03/cases.json

The subset is every model of tests/assets/glTF-Sample-Assets/Models (the pinned commit of tests/assets/assets.lock.json)
whose plain `glTF` variant uses no extension other than ALLOWED (extensionsUsed and extensionsRequired of the file):
the F3 decoders and KHR_lights_punctual (rendered with punctual lights off, as the references). Each such model is one
case (the plain glTF variant). Every other variant directory whose file uses at least one decoder extension and nothing
outside ALLOWED is one extra case, compared to the same reference. Models above MAX_BYTES or listed in EXCLUDED are left
out with their reason in the file; nothing else is excluded. Models whose plain variant uses another extension are
listed under "otherExtensions" (they belong to later phases), with the extensions named.

Cases (--cases): per subset entry one case per backend (vulkan, d3d11), names <case>.<variant>.<backend> with <case> the
model name with every character outside [A-Za-z0-9_.-] replaced by '_'. The reference is the model's
tests/golden/ref/sample-viewer/<model>.png (refgen renders glTF-Binary when it exists, else glTF). Per-case thresholds,
reasons and expectedFail entries are kept from the existing cases.json (THRESHOLDS below seeds new cases). The plan
F3 Task 5 cases (<asset>.<backend>, without a variant part) are kept as they are.

Stdlib only. Output is UTF-8 (model names are not all ASCII).
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MODELS = REPO / "tests" / "assets" / "glTF-Sample-Assets" / "Models"
OUT = REPO / "tests" / "golden" / "f03" / "core_subset.json"
CASES = REPO / "tests" / "golden" / "f03" / "cases.json"
REF_DIR = REPO / "tests" / "golden" / "ref" / "sample-viewer"
LOCK = REPO / "tests" / "assets" / "assets.lock.json"

DECODERS = {"KHR_draco_mesh_compression", "EXT_meshopt_compression", "KHR_mesh_quantization", "KHR_texture_basisu",
            "EXT_texture_webp"}
ALLOWED = DECODERS | {"KHR_lights_punctual"}
MAX_BYTES = 64 << 20
BACKENDS = ("vulkan", "d3d11")
# Model -> reason. Only models above MAX_BYTES or with a known Sample-Viewer limit (plan F3 Task 6).
EXCLUDED: dict[str, str] = {
    "TwoSidedPlane": "known Sample-Viewer limit, no reference: its only mesh lies in the XZ plane (POSITION min "
                     "(-1, 0, -1), max (1, 0, 1)) and UserCamera.resetView looks down -Z, so the plane is edge-on; "
                     "tools/refgen refuses it (tests/golden/ref/sample-viewer/settings.json models[TwoSidedPlane]: "
                     "status FAIL, 'model not visible: 0.000% of pixels differ from the environment-only frame')",
}
DEFAULT_THRESHOLD = 0.05
DEFAULT_REASON = "plan F3 Task 6 default gate (mean FLIP <= 0.05)"
CASE_CHARS = re.compile(r"[^A-Za-z0-9_.-]")


def gltf_json(path: Path) -> dict:
    data = path.read_bytes()
    if path.suffix.lower() == ".glb":
        (length,) = struct.unpack("<I", data[12:16])
        return json.loads(data[20:20 + length].decode("utf-8"))
    return json.loads(data.decode("utf-8-sig"))


def model_file(variant_dir: Path) -> Path | None:
    files = sorted(p for p in variant_dir.iterdir() if p.suffix.lower() in (".gltf", ".glb"))
    return files[0] if files else None


def extensions(doc: dict) -> list[str]:
    return sorted(set(doc.get("extensionsUsed", [])) | set(doc.get("extensionsRequired", [])))


def dir_bytes(d: Path) -> int:
    return sum(p.stat().st_size for p in d.rglob("*") if p.is_file())


def case_name(model: str) -> str:
    return CASE_CHARS.sub("_", model)


def rel(p: Path) -> str:
    return p.relative_to(REPO).as_posix()


def build() -> dict:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    subset, excluded, other = [], [], []
    for d in sorted((p for p in MODELS.iterdir() if p.is_dir()), key=lambda p: p.name):
        name = d.name
        plain = d / "glTF"
        f = model_file(plain) if plain.is_dir() else None
        if f is None:
            other.append({"name": name, "reason": "no plain glTF variant"})
            continue
        ext = extensions(gltf_json(f))
        outside = [e for e in ext if e not in ALLOWED]
        if outside:
            other.append({"name": name, "extensions": outside})
            continue
        if name in EXCLUDED:
            excluded.append({"name": name, "reason": EXCLUDED[name]})
            continue
        size = dir_bytes(plain)
        if size > MAX_BYTES:
            excluded.append({"name": name, "reason": f"plain glTF variant is {size} bytes (> 64 MiB)"})
            continue
        entry = {"name": name, "case": case_name(name), "variant": "glTF", "file": rel(f), "extensions": ext,
                 "decoderVariants": []}
        for v in sorted(p for p in d.iterdir() if p.is_dir() and p.name not in ("glTF", "screenshot")):
            vf = model_file(v)
            if vf is None:
                continue
            vext = extensions(gltf_json(vf))
            if not (set(vext) & DECODERS) or any(e not in ALLOWED for e in vext):
                continue
            vsize = dir_bytes(v)
            if vsize > MAX_BYTES:
                excluded.append({"name": f"{name}/{v.name}", "reason": f"{vsize} bytes (> 64 MiB)"})
                continue
            entry["decoderVariants"].append({"variant": v.name, "file": rel(vf), "extensions": vext})
        subset.append(entry)
    return {
        "generator": "scripts/gen_core_subset.py",
        "comment": "Plan F3 Task 6 core reference subset (spec 7.3.2). Generated: do not edit, run the script.",
        "sampleAssetsCommit": lock["glTF-Sample-Assets"]["commit"],
        "allowedExtensions": sorted(ALLOWED),
        "maxBytes": MAX_BYTES,
        "models": subset,
        "excluded": excluded,
        "otherExtensions": other,
    }


def dumps(doc: dict) -> str:
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def subset_cases(subset: dict, previous: dict[str, dict]) -> list[dict]:
    cases = []
    for m in subset["models"]:
        ref = f"../ref/sample-viewer/{m['name']}.png"
        for variant, file in [(m["variant"], m["file"])] + [(v["variant"], v["file"]) for v in m["decoderVariants"]]:
            for b in BACKENDS:
                name = f"{m['case']}.{variant}.{b}"
                c = {"name": name, "backend": b, "scene": file, "width": 512, "height": 512,
                     "cvars": ["r.ReferenceMode=1"], "pngChannels": 3, "ref": ref,
                     "thresholdFlip": DEFAULT_THRESHOLD, "thresholdReason": DEFAULT_REASON}
                old = previous.get(name)
                if old:
                    for k in ("thresholdFlip", "thresholdReason", "expectedFail"):
                        if k in old:
                            c[k] = old[k]
                cases.append(c)
    return cases


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed file is stale")
    ap.add_argument("--cases", action="store_true", help="write / check the subset cases of cases.json")
    ap.add_argument("--refgen-models", action="store_true", help="print the models that have no reference yet")
    args = ap.parse_args(argv)
    subset = build()
    if args.refgen_models:
        print(",".join(m["name"] for m in subset["models"] if not (REF_DIR / f"{m['name']}.png").is_file()))
        return 0
    if args.cases:
        manifest = json.loads(CASES.read_text(encoding="utf-8"))
        previous = {c["name"]: c for c in manifest["cases"]}
        names = {f"{m['case']}.{v}.{b}" for m in subset["models"]
                 for v in [m["variant"]] + [x["variant"] for x in m["decoderVariants"]] for b in BACKENDS}
        task5 = [c for c in manifest["cases"] if c["name"].count(".") == 1]
        stale_subset = [c for c in manifest["cases"] if c["name"].count(".") != 1 and c["name"] not in names]
        new = dict(manifest)
        new["cases"] = task5 + subset_cases(subset, previous)
        text = json.dumps(new, indent=2, ensure_ascii=False) + "\n"
        if args.check:
            if text != CASES.read_text(encoding="utf-8") or stale_subset:
                print("gen_core_subset: tests/golden/f03/cases.json subset cases are stale "
                      "(run python scripts/gen_core_subset.py --cases)", file=sys.stderr)
                return 1
            print(f"gen_core_subset: {len(new['cases'])} cases up to date")
            return 0
        CASES.write_text(text, encoding="utf-8", newline="\n")
        print(f"gen_core_subset: wrote {len(new['cases'])} cases to {CASES}")
        return 0
    text = dumps(subset)
    if args.check:
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != text:
            print("gen_core_subset: tests/golden/f03/core_subset.json is stale (run python scripts/gen_core_subset.py)",
                  file=sys.stderr)
            return 1
        print(f"gen_core_subset: {len(subset['models'])} models up to date")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    n = sum(1 + len(m["decoderVariants"]) for m in subset["models"])
    print(f"gen_core_subset: {len(subset['models'])} models, {n} subset entries, {len(subset['excluded'])} excluded, "
          f"{len(subset['otherExtensions'])} with other extensions -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
