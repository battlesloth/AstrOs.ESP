# CI Phase 2 — PR Validation Workflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (or superpowers:subagent-driven-development) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a single GitHub Actions workflow (`.github/workflows/pr-validation.yml`) that runs four checks against every pull request: native unit tests, both-board firmware compile, AstrOsMessaging native-purity guard, and changed-files clang-format.

**Architecture:** One workflow file with four parallel jobs and `workflow_dispatch:` for manual re-triggers. Each job runs on `ubuntu-latest`, installs PlatformIO/clang-format from pinned PyPI packages, and uses `actions/cache@v4` keyed on `(platformio.ini, sdkconfig.lolin_d32_pro, sdkconfig.metro_s3, dependencies.lock)` to keep warm runs under ~3 minutes per board. The clang-format job uses `pip install clang-format==18.1.8` (PyPI wrapper that bundles LLVM binaries) so the toolchain version is reproducible regardless of the runner image's apt state. Verification is performed by a deliberately-breaking canary PR (Task 7), since GitHub Actions YAML has no meaningful unit-test surface.

**Tech Stack:** GitHub Actions, PlatformIO 6.x (`pio test`, `pio run`), googletest (existing native test framework), clang-format 18.1.8, bash/grep.

---

## Background and locked decisions

This is Phase 2 of the CI pipeline laid out in `.docs/plans/20260411-0905-ci-pipeline-design.md`. Phase 1 (versioning foundation, `.clang-format` config, `version_gen.py` pre-build script) is already merged into `main` and reachable from the current `develop` branch HEAD `bfc5114`. Phase 2 adds **PR validation only** — no artifact publishing, no tagging, no release creation. Those land in Phase 3.

**Decisions locked during planning (do not re-litigate during execution):**

1. **Branch this work lives on:** `ci/phase2-pr-validation`, branched from `develop`. Already created at start of plan.
2. **PR target for the work itself:** `develop` (per the new merge etiquette established 2026-04-11 — see `memory/feedback_branching_workflow.md` and the CLAUDE.md update in Task 6 below).
3. **Trigger branches in the workflow:** `[main, develop, 'release/rel_*']`. The original design doc only listed `main` and `release/rel_*` because it predated the introduction of `develop` as the integration branch. Since feature work now flows through `develop`, the workflow must validate PRs targeting `develop` as well.
4. **clang-format binary source:** PyPI package `clang-format==18.1.8`, installed via `pip`. Pinned exactly. Avoids dependence on Ubuntu image apt repo state.
5. **Cache strategy:** `actions/cache@v4` on `~/.platformio` keyed by `hashFiles('platformio.ini', 'sdkconfig.lolin_d32_pro', 'sdkconfig.metro_s3', 'dependencies.lock')`. The build matrix prepends `${{ matrix.env }}` to the key so each board gets its own cache slot. No scheduled cache-warming job — that's deferred to Phase 3 per user direction.
6. **Concurrency:** `pr-validation-${{ github.event.pull_request.number }}` with `cancel-in-progress: true`. New commits to a PR cancel any in-flight runs to save minutes. (RC and release workflows in Phase 3 will use the *opposite* setting because they must serialize on tag counts.)
7. **`fetch-depth: 0`** is required on the `build` job (because `version_gen.py` calls `git describe --tags --abbrev=0`) and on the `clang-format` job (because the diff against `pull_request.base.sha` needs both endpoints in history). The `native-tests` and `messaging-purity` jobs do not need full history and can use the default shallow clone.
8. **`fail-fast: false`** on the build matrix so that one board failing doesn't cancel the other.

## File Structure

**New files (Phase 2 deliverables):**

- `.github/workflows/pr-validation.yml` — the entire workflow lives in this single file. Four jobs: `native-tests`, `messaging-purity`, `clang-format`, `build` (matrix). All four run in parallel on every triggering PR.

**Modified files:**

- `CLAUDE.md` — add a short section documenting the `develop` → PR → `main` workflow and the rule that direct commits to `main` are forbidden (Task 6). This change should be ~10 lines, slotted into the existing "Things that are *not* in the repo" or "Conventions worth knowing" section.
- `.docs/plans/20260411-1811-ci-phase2-pr-validation.md` — this plan file itself, with task checkboxes updated as work progresses (Task 0).

**Files referenced but unchanged:**

- `.clang-format` (already committed in Phase 1, commit `9118748`)
- `VERSION` (already committed in Phase 1)
- `scripts/version_gen.py` (already committed in Phase 1; consumed by the `build` job's `pio run` invocation via the `extra_scripts = pre:scripts/version_gen.py` line in `platformio.ini`)
- `lib/AstrOsMessaging/{include,src}/*` — the `messaging-purity` job greps these but does not modify them
- `test/test_native/*.cpp` — the `native-tests` job runs these but does not modify them

## Repository facts the implementer needs

These are facts about the current state of the repo as of plan-writing time. Verify them before starting Task 1 if the branch has moved.

- **Working directory:** `/home/jeff/Source/astros/AstrOs.ESP`
- **Current branch:** `ci/phase2-pr-validation` (already created, branched from `develop` at `bfc5114`)
- **Active sdkconfig files:** `sdkconfig.lolin_d32_pro`, `sdkconfig.metro_s3`. Ignore `sdkconfig.lolin_d32_pro.old`, `sdkconfig.metro_s3.old`, `sdkconfig.old`, and `sdkconfig.ESP32-S3-DevKitC-1` — these are stale/inactive and must NOT be in the cache hash key (they would create cache misses on every run for no reason).
- **PlatformIO envs:** `lolin_d32_pro`, `metro_s3`, `test` (in `platformio.ini`). All three reference `extra_scripts = pre:scripts/version_gen.py`, so even the `test` env will invoke `version_gen.py` at build time. This means `native-tests` also needs `version_gen.py` to run successfully; commit `e60e144` ("fix REPO_ROOT resolution when running as PlatformIO extra_script") and commit `447e423` ("harden version_gen.py against shell injection and missing git") already make the script tolerant of running without git history, so a shallow checkout for `native-tests` is acceptable.
- **AstrOsMessaging source files** (the grep target):
  - `lib/AstrOsMessaging/include/AstrOsMessaging.hpp`
  - `lib/AstrOsMessaging/src/AstrOsEspNowMessageService.{hpp,cpp}`
  - `lib/AstrOsMessaging/src/AstrOsSerialMessageService.{hpp,cpp}`
  - `lib/AstrOsMessaging/src/PacketTracker.{hpp,cpp}`
- **Native test files:**
  - `test/test_native/test_main.cpp`
  - `test/test_native/astros_file_utils_tests.cpp`
  - `test/test_native/astros_espnow_messages_tests.cpp`
  - `test/test_native/astros_servo_utils_tests.cpp`
  - `test/test_native/astros_string_utils_tests.cpp`
  - `test/test_native/astros_serial_messages_tests.cpp`
  - `test/test_native/packet_tracker_tests.cpp`
- **Existing `.clang-format` rules:** `BasedOnStyle: LLVM`, `IndentWidth: 4`, `ColumnLimit: 120`, `PointerAlignment: Left`. Do not modify.
- **No `.github/` directory exists yet.** The `workflows/` subdirectory will need to be created in Task 1.

---

## Task 0: Plan setup (DO THIS FIRST)

**Files:**
- Modify: `.docs/plans/20260411-1811-ci-phase2-pr-validation.md` (this file)

- [ ] **Step 1: Confirm branch state**

Run: `git branch --show-current && git status`
Expected: branch is `ci/phase2-pr-validation`, working tree clean.

- [ ] **Step 2: Confirm the plan file is committed**

Run: `git log --oneline -1 -- .docs/plans/20260411-1811-ci-phase2-pr-validation.md`
Expected: shows the commit that introduced this plan file. If empty, commit it now with:
```bash
git add .docs/plans/20260411-1811-ci-phase2-pr-validation.md
git commit -m "add Phase 2 PR validation plan"
```
(Per CLAUDE.md, the plan file must be committed before any implementation code is written.)

---

## Task 1: Scaffold the workflow file

**Files:**
- Create: `.github/workflows/pr-validation.yml`

The first task creates the workflow file with header, triggers, concurrency configuration, and an empty `jobs:` block. Subsequent tasks add one job at a time.

- [ ] **Step 1: Create the directory and skeleton file**

Run: `mkdir -p .github/workflows`

Then create `.github/workflows/pr-validation.yml` with this exact content:

```yaml
name: PR Validation

on:
  pull_request:
    branches:
      - main
      - develop
      - 'release/rel_*'
  workflow_dispatch:

# Cancel in-flight runs when a new commit is pushed to the same PR.
# (Phase 3 release/RC workflows will use cancel-in-progress: false to serialize on tag counts.)
concurrency:
  group: pr-validation-${{ github.event.pull_request.number || github.ref }}
  cancel-in-progress: true

jobs:
  # Jobs added in subsequent tasks:
  #   Task 2 — messaging-purity
  #   Task 3 — clang-format
  #   Task 4 — native-tests
  #   Task 5 — build (matrix over lolin_d32_pro + metro_s3)
  noop:
    runs-on: ubuntu-latest
    steps:
      - name: Placeholder
        run: echo "scaffolding only — replaced in Task 2"
```

The `noop` job is a temporary placeholder so the YAML is valid as a standalone commit. Task 2 deletes it as part of adding the first real job.

- [ ] **Step 2: Verify YAML syntax**

If `actionlint` is installed locally, run:
```bash
actionlint .github/workflows/pr-validation.yml
```
Expected: no output (success).

If `actionlint` is not installed, fall back to a Python YAML parse:
```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/pr-validation.yml'))"
```
Expected: no output (success). Any error here means the file has a syntax problem and must be fixed before committing.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/pr-validation.yml
git commit -m "scaffold pr-validation workflow with triggers and concurrency"
```

- [ ] **Step 4: Update plan**

Edit this plan file: change `- [ ]` to `- [x]` for each Task 1 step. Stage and commit:
```bash
git add .docs/plans/20260411-1811-ci-phase2-pr-validation.md
git commit -m "mark Phase 2 Task 1 complete"
```

(Plan-update commits can be batched at the end of each task or rolled into the implementation commit — implementer's choice. The important thing is that the plan file reflects reality before moving to the next task.)

---

## Task 2: Add the messaging-purity job

**Files:**
- Modify: `.github/workflows/pr-validation.yml`

This is the simplest job: a single grep over `lib/AstrOsMessaging/`. No toolchain install, no cache, runs in seconds. Doing it first builds confidence that the workflow file structure is correct.

- [ ] **Step 1: Replace the `noop` placeholder with the real `messaging-purity` job**

In `.github/workflows/pr-validation.yml`, delete the `noop:` block and replace it with:

```yaml
  messaging-purity:
    name: AstrOsMessaging native-purity guard
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Check for forbidden ESP-IDF / FreeRTOS / driver includes
        run: |
          set -euo pipefail
          # The lib/AstrOsMessaging/README rule: this library is unit-tested
          # on the native host, so it must not pull in any ESP-IDF, FreeRTOS,
          # or low-level driver headers. Catch violations as early as possible.
          if grep -rEn '#include[[:space:]]*[<"](freertos/|esp_|driver/)' lib/AstrOsMessaging/; then
            echo "::error::lib/AstrOsMessaging contains forbidden ESP-IDF/FreeRTOS/driver includes (see lines above)."
            echo "::error::This library must remain native-test compatible. Move ESP-specific code to a different lib."
            exit 1
          fi
          echo "messaging purity OK"
```

Notes for the implementer:
- The pattern intentionally matches both angle-bracket (`<freertos/...>`) and quoted (`"freertos/..."`) include forms.
- `set -euo pipefail` is important in `run:` blocks. Without it, an undefined variable or pipeline failure would silently succeed.
- The `if grep ... ; then ... ; fi` shape relies on grep's exit codes: 0 = match found (we want to fail), 1 = no match (we want to pass), 2 = error (we treat as pass — extremely unlikely on a clean checkout).

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/pr-validation.yml'))"
```
Expected: no output.

- [ ] **Step 3: Locally smoke-test the grep itself**

```bash
grep -rEn '#include[[:space:]]*[<"](freertos/|esp_|driver/)' lib/AstrOsMessaging/ && echo "FAIL" || echo "OK"
```
Expected: `OK` (no forbidden includes today). If it prints `FAIL`, that means the library already has a violation that must be cleaned up before this check can land — stop and ask the user.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/pr-validation.yml
git commit -m "add messaging-purity job to pr-validation workflow"
```

- [ ] **Step 5: Update plan checkboxes**

Mark Task 2 steps as `- [x]` in this plan file. Commit the plan update either now or roll it into the next task's commit.

---

## Task 3: Add the clang-format job

**Files:**
- Modify: `.github/workflows/pr-validation.yml`

This job installs a pinned clang-format and runs it against only the C/C++ files changed in the PR (grandfathering untouched files until a future full-tree reformat).

- [ ] **Step 1: Append the `clang-format` job to `.github/workflows/pr-validation.yml`**

Add this block underneath the `messaging-purity` job (note the two-space indentation matches the other job):

```yaml
  clang-format:
    name: clang-format (changed files only)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          # Need both PR base and head SHAs in history for the diff below.
          fetch-depth: 0

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.11'

      - name: Install pinned clang-format
        run: |
          set -euo pipefail
          # Pinned exactly. The PyPI 'clang-format' package is a wrapper that
          # bundles the upstream LLVM clang-format binary. Bumping this version
          # is intentional and PR-reviewable.
          pip install 'clang-format==18.1.8'
          clang-format --version

      - name: Check changed C/C++ files against .clang-format
        env:
          BASE_SHA: ${{ github.event.pull_request.base.sha }}
          HEAD_SHA: ${{ github.event.pull_request.head.sha }}
        run: |
          set -euo pipefail
          # workflow_dispatch fires don't have a pull_request context, so fall
          # back to checking the working tree against origin/main in that case.
          if [ -z "${BASE_SHA:-}" ] || [ -z "${HEAD_SHA:-}" ]; then
            echo "No PR context (workflow_dispatch). Comparing HEAD against origin/main."
            git fetch origin main --depth=1
            BASE_SHA=$(git rev-parse origin/main)
            HEAD_SHA=$(git rev-parse HEAD)
          fi

          changed=$(git diff --name-only --diff-filter=ACMR "$BASE_SHA" "$HEAD_SHA" -- \
            '*.c' '*.cpp' '*.h' '*.hpp')

          if [ -z "$changed" ]; then
            echo "No C/C++ files changed in this PR. Nothing to check."
            exit 0
          fi

          echo "Files to check:"
          echo "$changed" | sed 's/^/  - /'
          echo

          # --Werror turns format diffs into a non-zero exit, --dry-run avoids
          # touching the working tree. xargs preserves whitespace via -d '\n'.
          echo "$changed" | xargs -d '\n' clang-format --dry-run --Werror
```

Notes for the implementer:
- `--diff-filter=ACMR` excludes deleted files (D) and unmerged (U). Renames (R) and copies (C) are included on the new path.
- The fall-back path for `workflow_dispatch` is intentional: it lets you manually re-trigger the check via the Actions tab without a PR context. Without this branch, a manual run would crash on the empty `BASE_SHA`.
- The `xargs -d '\n'` form correctly handles file paths containing spaces. Paths in this repo don't have spaces today, but it's free protection.

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/pr-validation.yml'))"
```
Expected: no output.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/pr-validation.yml
git commit -m "add pinned clang-format check (changed files only) to pr-validation"
```

- [ ] **Step 4: Update plan checkboxes**

Mark Task 3 as complete in the plan file.

---

## Task 4: Add the native-tests job

**Files:**
- Modify: `.github/workflows/pr-validation.yml`

This job installs PlatformIO and runs `pio test -e test`, which builds the googletest-based suite under `test/test_native/` against host g++.

- [ ] **Step 1: Append the `native-tests` job to `.github/workflows/pr-validation.yml`**

Add this block underneath the `clang-format` job:

```yaml
  native-tests:
    name: Native unit tests (pio test -e test)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        # Shallow clone is fine; version_gen.py is hardened against missing
        # tags (commit 447e423) and produces a placeholder version string
        # when git history isn't available.

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.11'

      - name: Cache PlatformIO core
        uses: actions/cache@v4
        with:
          path: ~/.platformio
          key: pio-${{ runner.os }}-test-${{ hashFiles('platformio.ini', 'sdkconfig.lolin_d32_pro', 'sdkconfig.metro_s3', 'dependencies.lock') }}
          restore-keys: |
            pio-${{ runner.os }}-test-
            pio-${{ runner.os }}-

      - name: Install PlatformIO
        run: |
          set -euo pipefail
          pip install --upgrade platformio
          pio --version

      - name: Run native unit tests
        run: pio test -e test
```

Notes for the implementer:
- The `test` env in `platformio.ini` uses `platform = native` and `test_framework = googletest`. PlatformIO downloads googletest into `.pio/` (project-local), not `~/.platformio`. The cache will help with PlatformIO core but not with googletest itself — that's an acceptable speed tradeoff for Phase 2. Phase 3 may add `.pio/` to the cache.
- `pio test -e test` exits non-zero on test failure, which is exactly the behavior we want for a CI gate. No additional flags needed.
- The cache key segment `test` (between `${{ runner.os }}` and the hash) is what isolates this job's cache from the matrix-build job's caches in Task 5. Without per-job key segments, all jobs would fight over the same cache slot.

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/pr-validation.yml'))"
```
Expected: no output.

- [ ] **Step 3: Locally smoke-test the test invocation**

```bash
pio test -e test
```
Expected: tests pass (or, if you have `StringUtils.GetMessageAt` disabled per commit `c0ccfd3`, that test is skipped). If tests fail locally on a clean working tree, stop and ask the user — that means CI will be broken on day one.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/pr-validation.yml
git commit -m "add native-tests job (pio test -e test) to pr-validation"
```

- [ ] **Step 5: Update plan checkboxes**

Mark Task 4 as complete.

---

## Task 5: Add the build matrix job

**Files:**
- Modify: `.github/workflows/pr-validation.yml`

This is the most expensive job: matrix-builds both ESP32 firmware variants. Cold runs ~10 min per board; warm runs ~2-3 min per board (per the design doc estimate).

- [ ] **Step 1: Append the `build` job to `.github/workflows/pr-validation.yml`**

Add this block underneath the `native-tests` job:

```yaml
  build:
    name: Build ${{ matrix.env }}
    runs-on: ubuntu-latest
    strategy:
      # Don't cancel the other board if one fails — we want to see both results.
      fail-fast: false
      matrix:
        env: [lolin_d32_pro, metro_s3]
    steps:
      - uses: actions/checkout@v4
        with:
          # version_gen.py runs `git describe --tags --abbrev=0` to compute
          # the version string. fetch-depth: 0 ensures all tags are present.
          fetch-depth: 0

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.11'

      - name: Cache PlatformIO core
        uses: actions/cache@v4
        with:
          path: ~/.platformio
          key: pio-${{ runner.os }}-${{ matrix.env }}-${{ hashFiles('platformio.ini', 'sdkconfig.lolin_d32_pro', 'sdkconfig.metro_s3', 'dependencies.lock') }}
          restore-keys: |
            pio-${{ runner.os }}-${{ matrix.env }}-
            pio-${{ runner.os }}-

      - name: Install PlatformIO
        run: |
          set -euo pipefail
          pip install --upgrade platformio
          pio --version

      - name: Build ${{ matrix.env }}
        run: pio run -e ${{ matrix.env }}
```

Notes for the implementer:
- `fail-fast: false` is critical: without it, if `lolin_d32_pro` fails fast, we never see whether `metro_s3` would also fail. The user needs both signals.
- The cache key includes `${{ matrix.env }}` so each board gets its own cache slot, avoiding mutual eviction. Both still hash the same set of config files because a change to either sdkconfig invalidates both boards' state predictably.
- Both sdkconfig files are in the hash key intentionally — touching only `sdkconfig.metro_s3` will still bust the `lolin_d32_pro` cache, which is a slight over-invalidation but keeps the key simple and correct. Phase 3 can refine this if cold-build minutes become a problem.
- `pio run` (no `-t upload`, no `-t monitor`) builds only — does not flash or open serial. Correct for CI.

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/pr-validation.yml'))"
```
Expected: no output.

- [ ] **Step 3: Locally smoke-test both builds**

```bash
pio run -e lolin_d32_pro && pio run -e metro_s3
```
Expected: both builds succeed. If either fails on a clean working tree, stop and ask the user — CI will break on day one. (Note: `lolin_d32_pro` was just fixed in commit `bfc5114` for the SPIRAM IRAM overflow issue; verify that fix is still present.)

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/pr-validation.yml
git commit -m "add both-board build matrix job to pr-validation"
```

- [ ] **Step 5: Update plan checkboxes**

Mark Task 5 as complete.

---

## Task 6: Document the develop → main workflow in CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

The user established the new branching etiquette on 2026-04-11: no direct commits to `main`; work flows through `develop` and integrates via PR. This rule needs to be in CLAUDE.md so future Claude Code sessions (and human contributors reading CLAUDE.md) follow it.

- [ ] **Step 1: Read the existing CLAUDE.md to find the right insertion point**

Run: read `CLAUDE.md` end-to-end and identify the section "Conventions worth knowing" or a similar process-oriented section. The new content should slot in as a clearly-titled subsection near the top (so it's seen early), or under "Conventions worth knowing" if that flows better.

- [ ] **Step 2: Add the branching workflow section**

Insert this content (adjust the heading level to match surrounding sections):

```markdown
## Branching and merge workflow

- **`develop` is the integration branch.** Feature work branches off `develop` (use names like `feature/...`, `ci/...`, `fix/...`) and merges back via PR.
- **`main` is reserved for release-candidate cuts.** Direct commits to `main` are forbidden — they bypass the RC build pipeline that fires on `push: branches: [main]`. Always integrate through PR.
- **`release/rel_X.Y` branches** are cut from `main` when a version is ready to ship. Bug fixes for a released line target the relevant `release/rel_*` branch.
- **PR validation** (`.github/workflows/pr-validation.yml`) runs on PRs targeting `main`, `develop`, and `release/rel_*` branches. All four checks (native tests, both-board build, AstrOsMessaging native-purity, clang-format on changed files) must pass before merge.
- **When using Claude Code** to commit on this repo: always confirm `git branch --show-current` is not `main` before committing. If it is, stop and switch to `develop` (or a feature branch) first.
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "document develop branch workflow and PR validation in CLAUDE.md"
```

- [ ] **Step 4: Update plan checkboxes**

Mark Task 6 as complete.

---

## Task 7: Canary verification — break each check intentionally

**⚠️ Checkpoint:** This task pushes branches to GitHub and consumes Actions minutes. **Confirm with the user before starting.** Open the GitHub Actions tab in a browser before pushing so you can watch the runs.

**Files:**
- Touched temporarily on a throwaway branch only — nothing committed to `ci/phase2-pr-validation`, `develop`, or `main`.

**Goal:** Prove each of the four checks fails when its specific failure mode is introduced, and passes when nothing is wrong. The cleanest way to do this is one canary branch with four breaking commits, pushing each commit individually so each gets its own workflow run we can inspect by SHA.

- [ ] **Step 1: Push the `ci/phase2-pr-validation` branch first so a real PR is open**

```bash
git push -u origin ci/phase2-pr-validation
```

Then open a PR `ci/phase2-pr-validation` → `develop` (Task 8 covers this formally; this step just makes sure the branch exists on origin so canary diffs have something to compare against).

- [ ] **Step 2: Create the canary branch from `ci/phase2-pr-validation`**

```bash
git checkout -b test/phase2-ci-canary
```

This branch will be discarded — never merged.

- [ ] **Step 3: Canary commit A — break messaging-purity**

Edit `lib/AstrOsMessaging/include/AstrOsMessaging.hpp`. Add this line near the top of the file (just inside any existing `#pragma once` or include guard):

```cpp
#include <freertos/FreeRTOS.h>  // CANARY: triggers messaging-purity check failure
```

Then:
```bash
git add lib/AstrOsMessaging/include/AstrOsMessaging.hpp
git commit -m "canary: introduce forbidden FreeRTOS include"
git push -u origin test/phase2-ci-canary
```

Open a draft PR `test/phase2-ci-canary` → `develop` titled "Phase 2 CI canary — DO NOT MERGE". Watch the workflow run for this commit's SHA. **Expected outcome:**
- `messaging-purity`: ❌ FAIL with the `forbidden ESP-IDF/FreeRTOS/driver includes` error message
- `clang-format`: ✅ PASS (the added include is well-formatted)
- `native-tests`: ❌ FAIL or ✅ PASS depending on whether the include breaks the native build (likely FAIL because freertos headers don't exist on host)
- `build`: ✅ PASS (FreeRTOS is available on ESP32 builds, so this compiles cleanly)

If `messaging-purity` does NOT fail, the grep is misconfigured — stop and debug.

- [ ] **Step 4: Revert canary A and commit canary B — break clang-format**

```bash
git revert --no-edit HEAD
```

Then make a clearly badly-formatted edit to a small `.cpp` file that's also touched in the PR diff. For example, edit `lib/AstrOsMessaging/src/PacketTracker.cpp` and introduce something obviously wrong like an over-indented line, missing space after a comma, or a 200-column line:

```cpp
// CANARY: malformed line below — clang-format should reject
int               canary_var=42      ;     // wildly inconsistent spacing
```

Insert at the end of the file (or in a function body where it compiles). Then:
```bash
git add lib/AstrOsMessaging/src/PacketTracker.cpp
git commit -m "canary: introduce clang-format violation"
git push origin test/phase2-ci-canary
```

**Expected outcome for this push's workflow run:**
- `clang-format`: ❌ FAIL with a diff showing what clang-format wants
- `messaging-purity`: ✅ PASS
- `native-tests`: ✅ PASS or ❌ FAIL depending on whether the line compiles
- `build`: ✅ PASS or ❌ FAIL depending on whether the line compiles

If `clang-format` does NOT fail, either the file isn't in the diff (check `git diff origin/develop...HEAD --name-only`) or clang-format thinks the line is acceptable (try a more egregious violation).

- [ ] **Step 5: Revert canary B and commit canary C — break native-tests**

```bash
git revert --no-edit HEAD
```

Edit one of the existing test files to introduce a guaranteed-failing assertion. The smallest, most isolated option is to add a new failing test. Pick `test/test_native/astros_string_utils_tests.cpp` (it's small and self-contained) and add at the bottom:

```cpp
TEST(CanaryTest, AlwaysFails) {
    EXPECT_EQ(1, 2) << "CANARY: deliberate failure to verify CI catches test failures";
}
```

Then:
```bash
git add test/test_native/astros_string_utils_tests.cpp
git commit -m "canary: introduce failing native test"
git push origin test/phase2-ci-canary
```

**Expected outcome:**
- `native-tests`: ❌ FAIL with `CanaryTest.AlwaysFails` in the failure list
- `messaging-purity`: ✅ PASS
- `clang-format`: ✅ PASS (assuming the inserted code is properly formatted; format it via local clang-format first if needed)
- `build`: ✅ PASS (test files aren't part of the firmware build)

- [ ] **Step 6: Revert canary C and commit canary D — break the build**

```bash
git revert --no-edit HEAD
```

Edit `src/main.cpp` and introduce a deliberate compile error. The smallest invasive option is a stray token at the top of the file:

```cpp
// CANARY: deliberate compile error
THIS_IS_NOT_A_VALID_TOKEN;
```

Insert this *after* the existing `#include` block but *before* any function (file-scope, where it'll fail to parse). Then:
```bash
git add src/main.cpp
git commit -m "canary: introduce build break"
git push origin test/phase2-ci-canary
```

**Expected outcome:**
- `build`: ❌ FAIL — both `lolin_d32_pro` and `metro_s3` matrix cells should fail (matches `fail-fast: false`)
- `messaging-purity`: ✅ PASS
- `clang-format`: ❌ FAIL or ✅ PASS depending on the format of the inserted line
- `native-tests`: ❌ FAIL (the `test` env also pulls in `src/main.cpp`? Verify — if not, this should pass.)

If only ONE matrix cell fails (instead of both), `fail-fast: false` is misconfigured — fix it.

- [ ] **Step 7: Verify the all-checks-pass case by reverting back to clean**

```bash
git revert --no-edit HEAD
git push origin test/phase2-ci-canary
```

This push should produce a workflow run where **all four checks pass**. This is the critical positive control — without it, you might be looking at a workflow that fails everything regardless of the diff.

**Expected outcome:**
- `messaging-purity`: ✅ PASS
- `clang-format`: ✅ PASS
- `native-tests`: ✅ PASS
- `build`: ✅ PASS (both matrix cells)

If any check fails on the clean revert, debug before proceeding.

- [ ] **Step 8: Close the canary PR without merging and delete the branch**

Close the draft PR via the GitHub UI (do NOT click Merge). Then locally:
```bash
git checkout ci/phase2-pr-validation
git branch -D test/phase2-ci-canary
git push origin --delete test/phase2-ci-canary
```

(`-D` is required because the branch contains commits that won't be merged anywhere — this is the intended use of force-delete for a throwaway.)

- [ ] **Step 9: Update plan checkboxes**

Mark Task 7 as complete and add a one-line summary at the bottom of this plan file recording which workflow run SHAs you observed for each canary commit. Future debugging will appreciate knowing the exact runs.

```bash
git add .docs/plans/20260411-1811-ci-phase2-pr-validation.md
git commit -m "record Phase 2 canary verification results"
```

---

## Task 8: Open the real PR and request review

**⚠️ Checkpoint:** Confirm with the user before opening the PR.

**Files:**
- No file changes — this task is purely about the GitHub PR.

- [ ] **Step 1: Confirm branch is clean and pushed**

```bash
git status
git log --oneline origin/develop..ci/phase2-pr-validation
```
Expected: clean working tree, the log shows the Phase 2 commits in order (scaffold, messaging-purity, clang-format, native-tests, build, CLAUDE.md, plan updates, canary results).

- [ ] **Step 2: Open the PR**

```bash
gh pr create \
  --base develop \
  --head ci/phase2-pr-validation \
  --title "CI Phase 2: PR validation workflow" \
  --body "$(cat <<'EOF'
## Summary

Adds `.github/workflows/pr-validation.yml` with four checks that gate every PR into `main`, `develop`, or `release/rel_*`:

1. **Native unit tests** — `pio test -e test`
2. **Both-board compile matrix** — `pio run -e lolin_d32_pro` and `pio run -e metro_s3` in parallel (`fail-fast: false`)
3. **AstrOsMessaging native-purity guard** — grep block on forbidden `freertos/`, `esp_`, `driver/` includes
4. **clang-format check** — pinned `clang-format==18.1.8` from PyPI, runs against changed C/C++ files only (existing untouched files grandfathered)

Also documents the new `develop` → PR → `main` workflow in `CLAUDE.md`.

This is Phase 2 of the CI pipeline laid out in `.docs/plans/20260411-0905-ci-pipeline-design.md`. Phase 3 will add RC and release artifact publishing on top of this validation foundation.

## Verification

Verified via canary PR (since closed) on `test/phase2-ci-canary`. Each of the four checks was independently broken and observed to fail; reverting to clean produced an all-green run. Run SHAs recorded in the plan file.

## Test plan

- [x] All four checks pass on this PR (the workflow validates itself)
- [x] Local `pio test -e test`, `pio run -e lolin_d32_pro`, `pio run -e metro_s3` all green
- [x] Canary verification proved each check independently catches its failure mode
EOF
)"
```

- [ ] **Step 3: Update plan checkboxes**

Mark Task 8 as complete. Final plan-update commit:
```bash
git add .docs/plans/20260411-1811-ci-phase2-pr-validation.md
git commit -m "mark Phase 2 plan complete"
git push origin ci/phase2-pr-validation
```

---

## Verification summary (acceptance criteria for Phase 2)

Phase 2 is done when **all** of the following are true:

1. `.github/workflows/pr-validation.yml` exists on `ci/phase2-pr-validation` and is syntactically valid YAML.
2. The workflow has exactly four real jobs: `messaging-purity`, `clang-format`, `native-tests`, `build` (matrix). No leftover `noop` placeholder.
3. Triggers are `pull_request` on `[main, develop, 'release/rel_*']` plus `workflow_dispatch`.
4. Concurrency group is `pr-validation-${{ github.event.pull_request.number || github.ref }}` with `cancel-in-progress: true`.
5. Cache configuration uses `~/.platformio` keyed on the four files listed in the design doc, with per-job key segments.
6. clang-format is pinned at `==18.1.8` via pip.
7. CLAUDE.md documents the `develop` → PR → `main` workflow and references the new validation workflow.
8. The canary PR verified each of the four checks fails for its intended failure mode AND passes when the failure is reverted.
9. The real PR `ci/phase2-pr-validation` → `develop` is open and all four checks are green.

## Out of scope (deferred to Phase 3)

- RC build workflow (`.github/workflows/rc-build.yml`)
- Release build workflow (`.github/workflows/release-build.yml`)
- Tag creation, GitHub Releases, and artifact uploads
- `esptool.py merge_bin` for flashable images
- Any cache-warming scheduled job
- Browser flasher and GitHub Pages (Phase 4)
