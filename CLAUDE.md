# Build and test rules for agents (minimal rebuilds)

Goal: build only what changed. A full rebuild costs tens of minutes; an incremental one costs seconds to minutes.

## Implementers (working tree C:\Users\Heros\Desktop\Engine)

- Never delete `build/`, `build/<preset>/`, `third_party/tools/` or `tests/assets/glTF-Sample-Assets/`.
- Never use `--clean-first`, `--fresh`, `cmake --build ... --target clean`, or `git clean -x` here.
- Configure (`cmake --preset ...`) only once per build dir. After that, `cmake --build` re-configures by itself
  when a CMake file changes.
- While iterating, build only the target you need (`cmake --build build/win-msvc --config Debug --target <t>`)
  and run only the matching tests (`ctest --test-dir build/win-msvc -C Debug -R <regex>`).
- Run the full build plus the full `ctest` only once, just before the commit that closes the task.
- Build Release or ASan only if your task's gate asks for it. Reuse `build/asan` incrementally, the same way.

## Batch reviewers (per-task review / re-review)

This rule replaces the workflow prompt's "fresh clone, delete afterwards" step for batch reviews.

- Use ONE persistent review tree at `C:\tmp\eng-rev` and do not delete it afterwards. If it does not exist yet,
  `git clone https://github.com/qwertyu131/Engine C:\tmp\eng-rev`.
- Sync the tree to exactly the pushed commit:
  ```
  git -C C:\tmp\eng-rev fetch origin
  git -C C:\tmp\eng-rev checkout -f --detach origin/<branch>
  git -C C:\tmp\eng-rev clean -ffdx -e build/ -e third_party/tools/ -e tests/assets/glTF-Sample-Assets/ -e tests/assets/tools/
  ```
  After the sync, `git -C C:\tmp\eng-rev status --porcelain` must print nothing, and `git rev-parse HEAD` must
  equal `origin/<branch>`. This proves the source matches origin; only the build outputs are reused.
- Then run the fetch scripts. They reuse what is already present: the Tint build dir is keyed by the repo path,
  so a fixed path means no Tint rebuild.
- Then do an incremental `cmake --build`, followed by the full gate tests. Re-run configure only if the build
  dir is missing.
- The FINAL phase gate is the exception. It still uses a fresh clone at a new short path (e.g. `C:\tmp\<phase>g`),
  builds from scratch, and deletes that clone afterwards. This proves a clean build works.

## General

- MSVC builds run in parallel (`/MP` in CMakeLists.txt, `-maxCpuCount` in Directory.Build.rsp). Do not add `-m:1`.
- Use `ctest -j 8`.
- Never kill processes you did not start.
