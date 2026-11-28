"""K-D (spec §14, plan F3 Task 4): apps/headless and apps/viewer use only the public facade include/engine/*.h.

Mechanical check, two parts:
  - includes: every #include of apps/** names a standard header, a public facade header <engine/...>, Dear ImGui
    (<imgui.h>, the facade's debug UI), an app-local header, or a tools/golden header (PNG / UTF-8 helpers of the test
    tooling); never an engine-internal path (core/, rhi/, asset/, scene/, renderer/, platform/, ui/) or a moved
    built-in scene header;
  - CMake: the apps link engine::facade and no other engine:: target.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
APPS = REPO / "apps"

INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
INTERNAL = ("core/", "rhi/", "asset/", "scene/", "renderer/", "platform/", "ui/")
MOVED = {"Scenes.h", "SceneUtil.h", "Ibl.h"}  # the F2 built-in scenes, now engine/renderer/builtin
GOLDEN_TOOLS = {"Image.h", "Utf8.h"}           # tools/golden (test tooling, not the engine)


def violations() -> list[str]:
    bad = []
    for f in sorted(APPS.rglob("*")):
        if f.suffix not in (".cpp", ".h", ".hpp"):
            continue
        local = {p.name for p in f.parent.iterdir()}
        for m in INCLUDE.finditer(f.read_text(encoding="utf-8")):
            kind, name = m.group(1), m.group(2)
            close = '"' if kind == '"' else ">"
            rel = f"{f.relative_to(REPO).as_posix()}: #include {kind}{name}{close}"
            if name.startswith(INTERNAL) or name in MOVED:
                bad.append(rel + " (engine-internal)")
            elif kind == '"' and name not in local and name not in GOLDEN_TOOLS:
                bad.append(rel + " (not an app-local or tools/golden header)")
            elif kind == "<" and "/" in name and not name.startswith("engine/"):
                bad.append(rel + " (a path outside include/engine)")
    return bad


def engine_targets() -> list[str]:
    out = []
    for cm in sorted(APPS.rglob("CMakeLists.txt")):
        text = cm.read_text(encoding="utf-8")
        for t in re.findall(r"\bengine(?:_|::)[A-Za-z_]+", text):
            if t not in ("engine::facade", "engine_facade"):
                out.append(f"{cm.relative_to(REPO).as_posix()}: {t}")
    return out


class FacadeIncludes(unittest.TestCase):
    def test_apps_include_only_the_facade(self):
        self.assertEqual(violations(), [])

    def test_apps_link_only_the_facade(self):
        self.assertEqual(engine_targets(), [])

    def test_check_catches_an_internal_include(self):
        # The pattern itself (a regression in the regexes would make the two checks above vacuous).
        m = INCLUDE.search('#include "rhi/Rhi.h"\n')
        self.assertIsNotNone(m)
        self.assertTrue(m.group(2).startswith(INTERNAL))


if __name__ == "__main__":
    unittest.main()
