"""Generate docs/gltf-extensions.md from the pinned glTF extension registry (plan F0 Task 6).

Usage:
    python scripts/gen_extension_list.py [--registry-file README.md] [--out docs/gltf-extensions.md]

Inputs (all pinned in tests/assets/assets.lock.json):
  * KhronosGroup/glTF `extensions/README.md` at the "glTF-registry" commit (downloaded from
    raw.githubusercontent.com unless --registry-file is given);
  * the glTF-Sample-Assets checkout from scripts/fetch_assets.py: every .gltf / .glb under Models/
    is read and its `extensionsUsed` collected, so the "sample assets" column is measured, not guessed.

Status comes verbatim from the registry: "Ratified" (section "Ratified Khronos Extensions"), the
in-progress table's status ("Release Candidate", "Review Draft", ...), "Multi-Vendor", "Vendor",
"Archived". Differences against the preliminary list of spec §7.2 are computed, not hand-written.
Output is deterministic (sorted, no timestamps). Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import urllib.error
import urllib.request
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LOCK = REPO / "tests" / "assets" / "assets.lock.json"
SAMPLES = REPO / "tests" / "assets" / "glTF-Sample-Assets" / "Models"
OUT = REPO / "docs" / "gltf-extensions.md"
USER_AGENT = "engine-gen-extension-list/1.0"

# Spec §7.2 (docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md), preliminary list.
SPEC_KHR_TARGETS = [
    "KHR_draco_mesh_compression", "KHR_lights_punctual", "KHR_materials_anisotropy", "KHR_materials_clearcoat",
    "KHR_materials_diffuse_transmission", "KHR_materials_dispersion", "KHR_materials_emissive_strength",
    "KHR_materials_ior", "KHR_materials_iridescence", "KHR_materials_sheen", "KHR_materials_specular",
    "KHR_materials_transmission", "KHR_materials_unlit", "KHR_materials_variants", "KHR_materials_volume",
    "KHR_mesh_quantization", "KHR_meshopt_compression", "KHR_texture_basisu", "KHR_texture_transform",
    "KHR_animation_pointer", "KHR_xmp_json_ld",
]
SPEC_EXT_TARGETS = ["EXT_meshopt_compression", "EXT_mesh_gpu_instancing", "EXT_texture_webp", "EXT_texture_avif"]
SPEC_EXT_OPTIONAL = ["EXT_lights_image_based"]
SPEC_OUT_OF_SCOPE = {"KHR_interactivity": "spec §7.2: draft / pending ratification -> out of scope"}

LINK = re.compile(r"^\*\s+\[([A-Za-z0-9_]+)\]\(")
TABLE_ROW = re.compile(r"^\|\s*([A-Z][A-Za-z0-9]*_[A-Za-z0-9_]+)\s*\|\s*([^|]+?)\s*\|")


def parse_registry(text: str) -> dict[str, str]:
    """Returns {extension: status}."""
    status: dict[str, str] = {}
    section = None
    for line in text.splitlines():
        if line.startswith("## "):
            title = line[3:].strip()
            section = {
                "Ratified Khronos Extensions": "Ratified",
                "In-progress Khronos and Multi-Vendor Extensions and Projects": "in-progress",
                "Multi-Vendor Extensions": "Multi-Vendor",
                "Vendor Extensions": "Vendor",
                "Archived Extensions": "Archived",
            }.get(title)
            continue
        if line.startswith("# "):
            section = None
            continue
        if section is None:
            continue
        if section == "in-progress":
            m = TABLE_ROW.match(line)
            if m:
                status.setdefault(m.group(1), m.group(2))
        else:
            m = LINK.match(line.strip())
            if m:
                status.setdefault(m.group(1), section)
    return status


def gltf_json(path: Path) -> dict | None:
    try:
        data = path.read_bytes()
        if path.suffix.lower() == ".glb":
            if len(data) < 20 or data[:4] != b"glTF":
                return None
            length, ctype = struct.unpack_from("<I4s", data, 12)
            if ctype != b"JSON":
                return None
            return json.loads(data[20:20 + length].decode("utf-8"))
        return json.loads(data.decode("utf-8-sig"))
    except (OSError, ValueError, struct.error):
        return None


def scan_samples(models_dir: Path) -> tuple[dict[str, set[str]], int, list[str]]:
    """Returns ({extension: {model names}}, files scanned, unreadable files)."""
    used: dict[str, set[str]] = defaultdict(set)
    scanned = 0
    bad: list[str] = []
    for p in sorted(models_dir.rglob("*")):
        if p.suffix.lower() not in (".gltf", ".glb") or not p.is_file():
            continue
        model = p.relative_to(models_dir).parts[0]
        doc = gltf_json(p)
        scanned += 1
        if doc is None:
            bad.append(p.relative_to(models_dir).as_posix())
            continue
        for ext in doc.get("extensionsUsed", []) or []:
            if isinstance(ext, str):
                used[ext].add(model)
    return used, scanned, bad


def spec_target(ext: str) -> str:
    if ext in SPEC_KHR_TARGETS or ext in SPEC_EXT_TARGETS:
        return "yes"
    if ext in SPEC_EXT_OPTIONAL:
        return "optional"
    if ext in SPEC_OUT_OF_SCOPE:
        return "no (out of scope)"
    return "no"


def probe_spec_folders(names: list[str], lock: dict) -> dict[str, str]:
    """For names missing from the registry README: which extensions/2.0/<Khronos|Vendor>/<name>/README.md
    exists at the pinned commit (HTTP 200). Returns {name: "extensions/2.0/.../README.md"}."""
    reg = lock["glTF-registry"]
    raw = reg["repo"].replace("https://github.com/", "https://raw.githubusercontent.com/")
    found: dict[str, str] = {}
    for name in names:
        for folder in ("Khronos", "Vendor"):
            rel = f"extensions/2.0/{folder}/{name}/README.md"
            req = urllib.request.Request(f"{raw}/{reg['commit']}/{rel}", method="HEAD",
                                         headers={"User-Agent": USER_AGENT})
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    if resp.status == 200:
                        found[name] = rel
                        break
            except urllib.error.HTTPError as e:
                if e.code != 404:
                    raise
    return found


def render(registry: dict[str, str], used: dict[str, set[str]], scanned: int, bad: list[str], lock: dict,
           spec_folders: dict[str, str] | None = None) -> str:
    probed = spec_folders is not None
    spec_folders = spec_folders or {}
    reg = lock["glTF-registry"]
    sa = lock["glTF-Sample-Assets"]
    reg_url = f"{reg['repo']}/blob/{reg['commit']}/{reg['file']}"
    tree_url = f"{reg['repo']}/blob/{reg['commit']}/"

    def folder_note(ext: str) -> str:
        rel = spec_folders.get(ext)
        if rel:
            return f" A spec folder exists but is not in the list: [{rel}]({tree_url}{rel})."
        return " No spec folder under extensions/2.0/Khronos or Vendor either." if probed else ""
    lines = [
        "# glTF extensions: verified list",
        "",
        "Generated by `python scripts/gen_extension_list.py` -- do not edit by hand.",
        "",
        f"- Registry: [{reg['file']} @ `{reg['commit']}`]({reg_url}) (KhronosGroup/glTF)",
        f"- Sample assets: KhronosGroup/glTF-Sample-Assets @ `{sa['commit']}`; `extensionsUsed` read from "
        f"{scanned} `.gltf`/`.glb` files under `Models/` (all variants)"
        + (f", {len(bad)} unreadable" if bad else "") + ".",
        "- Status is the registry's own wording. \"Spec §7.2 target\" is the preliminary list in "
        "`docs/superpowers/specs/2026-11-27-cross-platform-gltf-renderer-design.md` §7.2.",
        "",
        "## Registry extensions",
        "",
        "| Extension | Prefix | Status | Spec §7.2 target? | Sample assets that exercise it |",
        "|---|---|---|---|---|",
    ]
    order = {"Ratified": 0, "Release Candidate": 1, "Review Draft": 2, "Initial Draft": 3, "Proposal": 4,
             "Multi-Vendor": 5, "Vendor": 6, "Archived": 7}
    for ext in sorted(registry, key=lambda e: (order.get(registry[e], 4), e)):
        models = sorted(used.get(ext, ()))
        sample = ", ".join(f"`{m}`" for m in models) if models else "none"
        lines.append(f"| `{ext}` | {ext.split('_', 1)[0]} | {registry[ext]} | {spec_target(ext)} | {sample} |")

    unlisted = sorted(e for e in used if e not in registry)
    lines += ["", "## Used by sample assets but not listed in the registry README", ""]
    if unlisted:
        lines += ["| Extension | Sample assets | Spec folder at the registry commit |", "|---|---|---|"]
        for ext in unlisted:
            rel = spec_folders.get(ext)
            folder = f"[{rel}]({tree_url}{rel})" if rel else "none found"
            lines.append(f"| `{ext}` | " + ", ".join(f"`{m}`" for m in sorted(used[ext])) + f" | {folder} |")
    else:
        lines.append("None.")

    # --- differences from spec §7.2 ---
    diffs: list[str] = []
    for ext in SPEC_KHR_TARGETS + SPEC_EXT_TARGETS:
        st = registry.get(ext)
        if st is None:
            diffs.append(f"- `{ext}`: spec §7.2 target, but **not listed** in the registry README at this commit."
                         + folder_note(ext))
        elif st != "Ratified":
            diffs.append(f"- `{ext}`: spec §7.2 target, registry status is **{st}** (not ratified).")
    for ext in SPEC_EXT_OPTIONAL:
        st = registry.get(ext)
        diffs.append(f"- `{ext}`: spec §7.2 optional (\"status to be verified\"); registry status: "
                     f"**{st or 'not listed'}**.")
    for ext, why in SPEC_OUT_OF_SCOPE.items():
        st = registry.get(ext)
        if st == "Ratified":
            diffs.append(f"- `{ext}`: {why}; registry status is now **Ratified**.")
    spec_all = set(SPEC_KHR_TARGETS + SPEC_EXT_TARGETS + SPEC_EXT_OPTIONAL) | set(SPEC_OUT_OF_SCOPE)
    for ext in sorted(e for e, st in registry.items() if st == "Ratified" and e not in spec_all):
        diffs.append(f"- `{ext}`: **Ratified** in the registry, not mentioned in spec §7.2.")
    lines += ["", "## Differences from spec §7.2", "",
              "To be folded back into the spec by the reviewer phase.", ""]
    lines += diffs or ["None."]
    if bad:
        lines += ["", "## Unreadable sample files", ""] + [f"- `{b}`" for b in bad]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--registry-file", help="local copy of extensions/README.md at the pinned commit")
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--no-probe", action="store_true",
                    help="do not probe the registry repo for spec folders of unlisted extensions (offline)")
    args = ap.parse_args(argv)

    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    reg = lock["glTF-registry"]
    if args.registry_file:
        text = Path(args.registry_file).read_text(encoding="utf-8")
    else:
        raw = reg["repo"].replace("https://github.com/", "https://raw.githubusercontent.com/")
        url = f"{raw}/{reg['commit']}/{reg['file']}"
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=60) as resp:
            text = resp.read().decode("utf-8")
    registry = parse_registry(text)
    if not registry:
        print("gen_extension_list: no extensions parsed from the registry README", file=sys.stderr)
        return 1
    if not SAMPLES.is_dir():
        print(f"gen_extension_list: '{SAMPLES}' missing; run scripts/fetch_assets.py first", file=sys.stderr)
        return 1
    used, scanned, bad = scan_samples(SAMPLES)
    unlisted = sorted({e for e in used if e not in registry} |
                      {e for e in SPEC_KHR_TARGETS + SPEC_EXT_TARGETS if e not in registry})
    folders = None if args.no_probe else probe_spec_folders(unlisted, lock)
    Path(args.out).write_text(render(registry, used, scanned, bad, lock, folders),encoding="utf-8", newline="\n")
    print(f"gen_extension_list: {len(registry)} registry extensions, {scanned} sample files -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
