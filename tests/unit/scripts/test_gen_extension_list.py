"""Tests for scripts/gen_extension_list.py registry parsing and diff logic (stdlib unittest, offline)."""
from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
_spec = importlib.util.spec_from_file_location("gen_extension_list", REPO / "scripts" / "gen_extension_list.py")
gen = importlib.util.module_from_spec(_spec)
sys.modules["gen_extension_list"] = gen
_spec.loader.exec_module(gen)

README = """# glTF 2.0 Extension Registry

## Ratified Khronos Extensions

* [KHR_lights_punctual](2.0/Khronos/KHR_lights_punctual/README.md)
* [KHR_interactivity](2.0/Khronos/KHR_interactivity/Specification.adoc)
* [EXT_meshopt_compression](2.0/Vendor/EXT_meshopt_compression/README.md)

## In-progress Khronos and Multi-Vendor Extensions and Projects

| Extensions | Status | Pull Request |
|------------|--------|--------------|
| KHR_materials_diffuse_transmission | Release Candidate | [Specification](x) |
| KHR_texture_video | Initial Draft | [PR #2285](y) |

| Projects | Status | Pull Request |
|----------|--------|--------------|
| glTF External References | Proposal | [Repository](z) |

## Multi-Vendor Extensions

* [EXT_lights_image_based](2.0/Vendor/EXT_lights_image_based/README.md)

## Vendor Extensions

* [MSFT_lod](2.0/Vendor/MSFT_lod/README.md)

## Archived Extensions

* [KHR_xmp](2.0/Archived/KHR_xmp/README.md)

# About glTF Extensions

* [NOT_an_entry](x)
"""


class GenExtensionListTest(unittest.TestCase):
    def test_parse_registry_sections(self) -> None:
        reg = gen.parse_registry(README)
        self.assertEqual(reg["KHR_lights_punctual"], "Ratified")
        self.assertEqual(reg["KHR_interactivity"], "Ratified")
        self.assertEqual(reg["EXT_meshopt_compression"], "Ratified")
        self.assertEqual(reg["KHR_materials_diffuse_transmission"], "Release Candidate")
        self.assertEqual(reg["KHR_texture_video"], "Initial Draft")
        self.assertEqual(reg["EXT_lights_image_based"], "Multi-Vendor")
        self.assertEqual(reg["MSFT_lod"], "Vendor")
        self.assertEqual(reg["KHR_xmp"], "Archived")
        self.assertNotIn("NOT_an_entry", reg)
        self.assertNotIn("glTF External References", reg)

    def test_scan_reads_gltf_and_glb(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            models = Path(d)
            (models / "A" / "glTF").mkdir(parents=True)
            (models / "A" / "glTF" / "A.gltf").write_text('{"extensionsUsed":["KHR_lights_punctual"]}',
                                                          encoding="utf-8")
            (models / "B" / "glTF-Binary").mkdir(parents=True)
            js = b'{"extensionsUsed":["EXT_meshopt_compression"]}  '
            glb = b"glTF" + struct.pack("<II", 2, 20 + len(js)) + struct.pack("<I4s", len(js), b"JSON") + js
            (models / "B" / "glTF-Binary" / "B.glb").write_bytes(glb)
            used, scanned, bad = gen.scan_samples(models)
        self.assertEqual(scanned, 2)
        self.assertEqual(bad, [])
        self.assertEqual(used["KHR_lights_punctual"], {"A"})
        self.assertEqual(used["EXT_meshopt_compression"], {"B"})

    def test_differences_are_computed(self) -> None:
        reg = gen.parse_registry(README)
        lock = {"glTF-registry": {"repo": "r", "commit": "c", "file": "f"}, "glTF-Sample-Assets": {"commit": "s"}}
        md = gen.render(reg, {}, 0, [], lock)
        self.assertIn("`KHR_materials_diffuse_transmission`: spec §7.2 target, registry status is **Release Candidate**", md)
        self.assertIn("`KHR_meshopt_compression`: spec §7.2 target, but **not listed**", md)
        self.assertIn("`KHR_interactivity`: spec §7.2: draft", md)
        self.assertIn("registry status is now **Ratified**", md)


if __name__ == "__main__":
    unittest.main()
