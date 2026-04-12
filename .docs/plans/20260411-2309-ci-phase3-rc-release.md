# CI Phase 3 — RC & Release Build Workflows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (or superpowers:subagent-driven-development) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two GitHub Actions workflows that automatically tag, build, package, and publish firmware artifacts — `rc-build.yml` creates pre-releases on pushes to `main`, and `release-build.yml` creates full releases on pushes to `release/rel_*` branches.

**Architecture:** Each workflow follows a three-job pipeline: (1) a `compute-tag` job computes the next semantic version tag and pushes it, (2) a `build` matrix job compiles both boards and packages three artifacts per board (merged flash image, raw app binary, ELF debug symbols), and (3) a `release` job creates the GitHub Release/Pre-release and attaches all six artifacts. The tag computation uses the `VERSION` file + existing git tags; the build reuses `version_gen.py` (Phase 1) which reads the newly-created tag to produce the correct version string; `esptool.py merge_bin` produces the flashable merged image. Concurrency groups with `cancel-in-progress: false` serialize runs to prevent tag-count races.

**Tech Stack:** GitHub Actions, PlatformIO 6.x, esptool.py (PyPI), `gh` CLI for release creation, `scripts/version_gen.py` (Phase 1).

---

## Background and locked decisions

This is Phase 3 of the CI pipeline designed in `.docs/plans/20260411-0905-ci-pipeline-design.md`. Phase 1 (versioning) and Phase 2 (PR validation + clang-format baseline) are merged to `develop`. Phase 3 adds the artifact-publishing workflows.

**Decisions locked during planning (do not re-litigate during execution):**

1. **Branch:** `ci/phase3-rc-release`, branched from `develop`. Already created.
2. **Three-job pipeline:** `compute-tag` → `build` (matrix) → `release`. Keeps tag creation serialized, builds parallel, release creation atomic. The alternative (single job doing everything) would work but doesn't scale to adding boards and doesn't clearly separate concerns.
3. **Tag computation is inline bash, not a shared script.** RC and release tag patterns differ enough that extracting a shared script adds abstraction without reducing complexity. ~15 lines of bash each.
4. **`version_gen.py` is NOT modified.** The workflow derives the version string from the tag name (`${TAG#v}`). The PlatformIO build invokes `version_gen.py` automatically via `extra_scripts`, which sees the tag and produces the correct `version_generated.hpp`. No `--version-only` flag needed.
5. **`esptool.py` installed via `pip install esptool`** on the runner. PlatformIO bundles esptool internally but doesn't put it on PATH; a direct pip install is simpler and explicit.
6. **Artifact naming:** `astros-esp-<version>-<board>-flash.bin`, `astros-esp-<version>-<board>-app.bin`, `astros-esp-<version>-<board>.elf` per the design doc.
7. **`GITHUB_TOKEN` with `contents: write`** permission for tag pushing. Tags pushed with `GITHUB_TOKEN` do NOT trigger other workflows (GitHub prevents this to avoid loops).
8. **Concurrency:** `cancel-in-progress: false` on both workflows. Two merges racing must serialize on the tag-count step. This is the opposite of PR validation (which uses `cancel-in-progress: true`).
9. **Cache:** Same `actions/cache@v4` pattern as Phase 2 PR validation, keyed per board.
10. **`docs/flasher/manifest.json` update is deferred to Phase 4.** The release-build workflow does NOT update the browser flasher manifest.
11. **Cache-warming scheduled workflow** was deferred from Phase 2 per user direction and is included as Task 6 in this plan.

## Repository facts the implementer needs

- **Working directory:** `/home/jeff/Source/astros/AstrOs.ESP`
- **Current branch:** `ci/phase3-rc-release` (branched from `develop` at merge commit `edaefe2`)
- **`VERSION` file contents:** `1.0.0`
- **Existing tags:** none (`git tag -l 'v*'` returns empty). First RC will be `v1.0.0-RC.1`.
- **`scripts/version_gen.py`** — already exists (Phase 1). Runs as PlatformIO pre-build hook AND standalone CLI. Reads git tags via `git describe --tags --abbrev=0`.
- **PlatformIO build artifacts** are in `.pio/build/<env>/`:
  - `bootloader.bin`, `partitions.bin`, `ota_data_initial.bin`, `firmware.bin`, `firmware.elf`

### Board-specific merge_bin parameters

These values come from the partition tables (`partition_8mb.csv`, `partition_16mb.csv`), PlatformIO flash args, and `platformio.ini`:

| Parameter | `lolin_d32_pro` | `metro_s3` |
|-----------|----------------|------------|
| Chip | `esp32` | `esp32s3` |
| Flash mode | `dio` | `dio` |
| Flash freq | `40m` | `80m` |
| Flash size | `8MB` | `16MB` |
| Bootloader offset | `0x1000` | `0x0` |
| Partition table offset | `0x8000` | `0x8000` |
| OTA data offset | `0xf000` | `0xe000` |
| App (ota_0) offset | `0x20000` | `0x10000` |

**Note:** The sdkconfig files currently set `CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y` for both boards, which is a pre-existing misconfiguration (the boards have 8MB and 16MB respectively). The merge_bin commands use the correct sizes from `platformio.ini`'s `board_upload.flash_size`. Fixing the sdkconfig is out of scope for Phase 3.

## File structure

**New files:**

- `.github/workflows/rc-build.yml` — RC pre-release workflow (triggers on push to `main`)
- `.github/workflows/release-build.yml` — stable release workflow (triggers on push to `release/rel_*`)
- `.github/workflows/cache-warm.yml` — scheduled cache-warming workflow (weekly cron)

**Modified files:**

- `CLAUDE.md` — document the VERSION-file bump flow and release branch conventions
- `.docs/plans/20260411-2309-ci-phase3-rc-release.md` — this plan file (checkboxes updated as work progresses)

**Unchanged but consumed:**

- `scripts/version_gen.py` — invoked by `pio run` via `extra_scripts`; sees the tag created by the workflow
- `VERSION` — read by the tag computation step
- `.github/workflows/pr-validation.yml` — not modified; continues to gate PRs independently

---

## Task 1: Scaffold `rc-build.yml` with tag computation

**Files:**
- Create: `.github/workflows/rc-build.yml`

This task creates the workflow file with triggers, concurrency, permissions, and the `compute-tag` job that figures out the next RC number and pushes the tag. No build yet — just tagging.

- [ ] **Step 1: Create `.github/workflows/rc-build.yml`**

```yaml
name: RC Build

on:
  push:
    branches:
      - main
  workflow_dispatch:

permissions:
  contents: write

# Serialize RC builds — two merges racing must not compute the same tag number.
concurrency:
  group: rc-build
  cancel-in-progress: false

jobs:
  compute-tag:
    name: Compute next RC tag
    runs-on: ubuntu-latest
    outputs:
      tag: ${{ steps.tag.outputs.tag }}
      version: ${{ steps.tag.outputs.version }}
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Compute and push next RC tag
        id: tag
        run: |
          set -euo pipefail

          BASE_VERSION=$(cat VERSION)
          echo "Base version from VERSION file: ${BASE_VERSION}"

          # Find the highest existing RC tag for this base version.
          LAST_RC=$(git tag -l "v${BASE_VERSION}-RC.*" | sort -t. -k4 -n | tail -1)

          if [ -z "$LAST_RC" ]; then
            NEXT_N=1
          else
            CURRENT_N=$(echo "$LAST_RC" | grep -oP 'RC\.\K\d+')
            NEXT_N=$((CURRENT_N + 1))
          fi

          TAG="v${BASE_VERSION}-RC.${NEXT_N}"
          VERSION="${TAG#v}"
          echo "Creating tag: ${TAG} (version: ${VERSION})"

          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git tag -a "${TAG}" -m "RC build ${VERSION}"
          git push origin "${TAG}"

          echo "tag=${TAG}" >> "$GITHUB_OUTPUT"
          echo "version=${VERSION}" >> "$GITHUB_OUTPUT"
```

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/rc-build.yml')); print('YAML OK')"
```
Expected: `YAML OK`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/rc-build.yml
git commit -m "scaffold rc-build workflow with tag computation"
```

---

## Task 2: Add matrix build and artifact packaging to `rc-build.yml`

**Files:**
- Modify: `.github/workflows/rc-build.yml`

This task adds the `build` job: PlatformIO setup, cache, matrix build for both boards, `esptool.py merge_bin`, and artifact upload (to the workflow run, not the release — that's Task 3).

- [ ] **Step 1: Add the `build` job after `compute-tag`**

Append this job to `.github/workflows/rc-build.yml`:

```yaml
  build:
    name: Build ${{ matrix.env }}
    needs: compute-tag
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        include:
          - env: lolin_d32_pro
            chip: esp32
            flash_mode: dio
            flash_freq: 40m
            flash_size: 8MB
            bootloader_offset: "0x1000"
            partition_offset: "0x8000"
            otadata_offset: "0xf000"
            app_offset: "0x20000"
          - env: metro_s3
            chip: esp32s3
            flash_mode: dio
            flash_freq: 80m
            flash_size: 16MB
            bootloader_offset: "0x0"
            partition_offset: "0x8000"
            otadata_offset: "0xe000"
            app_offset: "0x10000"
    steps:
      - uses: actions/checkout@v4
        with:
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

      - name: Install PlatformIO and esptool
        run: |
          set -euo pipefail
          pip install --upgrade platformio esptool
          pio --version
          esptool.py version

      - name: Fetch tags so version_gen.py sees the new RC tag
        run: git fetch --tags origin

      - name: Build ${{ matrix.env }}
        run: pio run -e ${{ matrix.env }}

      - name: Package artifacts
        env:
          VERSION: ${{ needs.compute-tag.outputs.version }}
          ENV: ${{ matrix.env }}
          CHIP: ${{ matrix.chip }}
          FLASH_MODE: ${{ matrix.flash_mode }}
          FLASH_FREQ: ${{ matrix.flash_freq }}
          FLASH_SIZE: ${{ matrix.flash_size }}
          BOOT_OFF: ${{ matrix.bootloader_offset }}
          PART_OFF: ${{ matrix.partition_offset }}
          OTA_OFF: ${{ matrix.otadata_offset }}
          APP_OFF: ${{ matrix.app_offset }}
        run: |
          set -euo pipefail
          BUILD=".pio/build/${ENV}"
          OUT="artifacts"
          mkdir -p "${OUT}"

          # Merged flash image — flashable at offset 0x0.
          esptool.py --chip "${CHIP}" merge_bin \
            --flash_mode "${FLASH_MODE}" \
            --flash_freq "${FLASH_FREQ}" \
            --flash_size "${FLASH_SIZE}" \
            -o "${OUT}/astros-esp-${VERSION}-${ENV}-flash.bin" \
            "${BOOT_OFF}" "${BUILD}/bootloader.bin" \
            "${PART_OFF}" "${BUILD}/partitions.bin" \
            "${OTA_OFF}"  "${BUILD}/ota_data_initial.bin" \
            "${APP_OFF}"  "${BUILD}/firmware.bin"

          # Raw app binary — for OTA updates.
          cp "${BUILD}/firmware.bin" "${OUT}/astros-esp-${VERSION}-${ENV}-app.bin"

          # ELF with debug symbols — for exception decoding.
          cp "${BUILD}/firmware.elf" "${OUT}/astros-esp-${VERSION}-${ENV}.elf"

          echo "Artifacts for ${ENV}:"
          ls -lh "${OUT}"/astros-esp-*

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: artifacts-${{ matrix.env }}
          path: artifacts/
          retention-days: 5
```

Notes for the implementer:
- `needs: compute-tag` ensures the tag exists before the build starts.
- `git fetch --tags origin` after checkout is critical — the checkout happened with `fetch-depth: 0` but before the tag was pushed. The tag was pushed by the `compute-tag` job, so we need to fetch it for `version_gen.py` to see it.
- All `esptool.py merge_bin` parameters are passed via env vars (safe pattern per the security reminder hook).
- `actions/upload-artifact@v4` stores the per-board artifacts temporarily (5-day retention). The `release` job (Task 3) downloads them.

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/rc-build.yml')); print('YAML OK')"
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/rc-build.yml
git commit -m "add matrix build and artifact packaging to rc-build"
```

---

## Task 3: Add GitHub Pre-release creation to `rc-build.yml`

**Files:**
- Modify: `.github/workflows/rc-build.yml`

This task adds the `release` job that downloads all build artifacts and creates a GitHub Pre-release.

- [ ] **Step 1: Add the `release` job after `build`**

Append this job to `.github/workflows/rc-build.yml`:

```yaml
  release:
    name: Create GitHub Pre-release
    needs: [compute-tag, build]
    runs-on: ubuntu-latest
    steps:
      - name: Download all build artifacts
        uses: actions/download-artifact@v4
        with:
          path: artifacts/
          merge-multiple: true

      - name: List artifacts
        run: ls -lhR artifacts/

      - name: Create pre-release
        env:
          GH_TOKEN: ${{ github.token }}
          TAG: ${{ needs.compute-tag.outputs.tag }}
          VERSION: ${{ needs.compute-tag.outputs.version }}
        run: |
          set -euo pipefail
          gh release create "${TAG}" \
            --repo "${GITHUB_REPOSITORY}" \
            --prerelease \
            --title "${TAG}" \
            --notes "Release candidate ${VERSION}." \
            artifacts/*
```

Notes for the implementer:
- `needs: [compute-tag, build]` ensures both boards have finished building before creating the release.
- `actions/download-artifact@v4` with `merge-multiple: true` downloads artifacts from both matrix cells into a single `artifacts/` directory. The files don't collide because they have board-specific names.
- `gh release create` with `--prerelease` creates a pre-release. The `GH_TOKEN` env var is how `gh` authenticates.
- The `--repo` flag uses `GITHUB_REPOSITORY` (e.g., `battlesloth/AstrOs.ESP`) to be explicit.
- `artifacts/*` globs all six files (3 per board × 2 boards).

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/rc-build.yml')); print('YAML OK')"
```

- [ ] **Step 3: Verify the complete workflow structure**

```bash
python3 -c "
import yaml
with open('.github/workflows/rc-build.yml') as f:
    d = yaml.safe_load(f)
jobs = list(d['jobs'].keys())
print('Jobs:', jobs)
assert jobs == ['compute-tag', 'build', 'release'], f'Expected 3 jobs, got {jobs}'
print('Structure OK')
"
```

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/rc-build.yml
git commit -m "add GitHub pre-release creation to rc-build"
```

---

## Task 4: Create `release-build.yml`

**Files:**
- Create: `.github/workflows/release-build.yml`

This workflow is structurally similar to `rc-build.yml` but differs in:
- Trigger: `push: branches: ['release/rel_*']` instead of `main`
- Tag computation: `v<MAJOR>.<MINOR>.<PATCH>` (auto-incremented) instead of `v<BASE>-RC.<N>`
- Release type: full release instead of pre-release

- [ ] **Step 1: Create `.github/workflows/release-build.yml`**

```yaml
name: Release Build

on:
  push:
    branches:
      - 'release/rel_*'
  workflow_dispatch:

permissions:
  contents: write

# Serialize release builds per branch — patches on the same release line must not race.
concurrency:
  group: release-build-${{ github.ref }}
  cancel-in-progress: false

jobs:
  compute-tag:
    name: Compute release tag
    runs-on: ubuntu-latest
    outputs:
      tag: ${{ steps.tag.outputs.tag }}
      version: ${{ steps.tag.outputs.version }}
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Compute and push release tag
        id: tag
        run: |
          set -euo pipefail

          # Extract MAJOR.MINOR from the branch name: release/rel_1.0 → 1.0
          BRANCH="${GITHUB_REF#refs/heads/}"
          MAJOR_MINOR="${BRANCH#release/rel_}"
          echo "Release line: ${MAJOR_MINOR}"

          # Find the highest existing release tag for this MAJOR.MINOR
          # (exclude RC/pre-release tags).
          LAST_RELEASE=$(git tag -l "v${MAJOR_MINOR}.*" | grep -vE '(RC|rc|alpha|beta)' | sort -V | tail -1)

          if [ -z "$LAST_RELEASE" ]; then
            TAG="v${MAJOR_MINOR}.0"
          else
            CURRENT_PATCH=$(echo "$LAST_RELEASE" | rev | cut -d. -f1 | rev)
            NEXT_PATCH=$((CURRENT_PATCH + 1))
            TAG="v${MAJOR_MINOR}.${NEXT_PATCH}"
          fi

          VERSION="${TAG#v}"
          echo "Creating tag: ${TAG} (version: ${VERSION})"

          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git tag -a "${TAG}" -m "Release ${VERSION}"
          git push origin "${TAG}"

          echo "tag=${TAG}" >> "$GITHUB_OUTPUT"
          echo "version=${VERSION}" >> "$GITHUB_OUTPUT"

  build:
    name: Build ${{ matrix.env }}
    needs: compute-tag
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        include:
          - env: lolin_d32_pro
            chip: esp32
            flash_mode: dio
            flash_freq: 40m
            flash_size: 8MB
            bootloader_offset: "0x1000"
            partition_offset: "0x8000"
            otadata_offset: "0xf000"
            app_offset: "0x20000"
          - env: metro_s3
            chip: esp32s3
            flash_mode: dio
            flash_freq: 80m
            flash_size: 16MB
            bootloader_offset: "0x0"
            partition_offset: "0x8000"
            otadata_offset: "0xe000"
            app_offset: "0x10000"
    steps:
      - uses: actions/checkout@v4
        with:
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

      - name: Install PlatformIO and esptool
        run: |
          set -euo pipefail
          pip install --upgrade platformio esptool
          pio --version
          esptool.py version

      - name: Fetch tags so version_gen.py sees the new release tag
        run: git fetch --tags origin

      - name: Build ${{ matrix.env }}
        run: pio run -e ${{ matrix.env }}

      - name: Package artifacts
        env:
          VERSION: ${{ needs.compute-tag.outputs.version }}
          ENV: ${{ matrix.env }}
          CHIP: ${{ matrix.chip }}
          FLASH_MODE: ${{ matrix.flash_mode }}
          FLASH_FREQ: ${{ matrix.flash_freq }}
          FLASH_SIZE: ${{ matrix.flash_size }}
          BOOT_OFF: ${{ matrix.bootloader_offset }}
          PART_OFF: ${{ matrix.partition_offset }}
          OTA_OFF: ${{ matrix.otadata_offset }}
          APP_OFF: ${{ matrix.app_offset }}
        run: |
          set -euo pipefail
          BUILD=".pio/build/${ENV}"
          OUT="artifacts"
          mkdir -p "${OUT}"

          esptool.py --chip "${CHIP}" merge_bin \
            --flash_mode "${FLASH_MODE}" \
            --flash_freq "${FLASH_FREQ}" \
            --flash_size "${FLASH_SIZE}" \
            -o "${OUT}/astros-esp-${VERSION}-${ENV}-flash.bin" \
            "${BOOT_OFF}" "${BUILD}/bootloader.bin" \
            "${PART_OFF}" "${BUILD}/partitions.bin" \
            "${OTA_OFF}"  "${BUILD}/ota_data_initial.bin" \
            "${APP_OFF}"  "${BUILD}/firmware.bin"

          cp "${BUILD}/firmware.bin" "${OUT}/astros-esp-${VERSION}-${ENV}-app.bin"
          cp "${BUILD}/firmware.elf" "${OUT}/astros-esp-${VERSION}-${ENV}.elf"

          echo "Artifacts for ${ENV}:"
          ls -lh "${OUT}"/astros-esp-*

      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: artifacts-${{ matrix.env }}
          path: artifacts/
          retention-days: 5

  release:
    name: Create GitHub Release
    needs: [compute-tag, build]
    runs-on: ubuntu-latest
    steps:
      - name: Download all build artifacts
        uses: actions/download-artifact@v4
        with:
          path: artifacts/
          merge-multiple: true

      - name: List artifacts
        run: ls -lhR artifacts/

      - name: Create release
        env:
          GH_TOKEN: ${{ github.token }}
          TAG: ${{ needs.compute-tag.outputs.tag }}
          VERSION: ${{ needs.compute-tag.outputs.version }}
        run: |
          set -euo pipefail
          gh release create "${TAG}" \
            --repo "${GITHUB_REPOSITORY}" \
            --title "${TAG}" \
            --notes "Stable release ${VERSION}." \
            artifacts/*
```

Key differences from `rc-build.yml`:
- Tag computation uses `MAJOR.MINOR` from branch name, auto-increments PATCH
- `grep -vE '(RC|rc|alpha|beta)'` filters out pre-release tags when finding the last release
- `CURRENT_PATCH` extraction uses `rev | cut -d. -f1 | rev` — reverses the string, takes the first dot-delimited field, reverses back. This correctly extracts the last numeric segment regardless of MAJOR.MINOR.PATCH length.
- `gh release create` does NOT include `--prerelease` — this is a full release
- Concurrency group includes `${{ github.ref }}` so different release lines (`release/rel_1.0` vs `release/rel_1.1`) can build in parallel

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/release-build.yml')); print('YAML OK')"
```

- [ ] **Step 3: Verify job structure**

```bash
python3 -c "
import yaml
with open('.github/workflows/release-build.yml') as f:
    d = yaml.safe_load(f)
jobs = list(d['jobs'].keys())
print('Jobs:', jobs)
assert jobs == ['compute-tag', 'build', 'release'], f'Expected 3 jobs, got {jobs}'
print('Structure OK')
"
```

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/release-build.yml
git commit -m "add release-build workflow for stable releases"
```

---

## Task 5: Add cache-warming scheduled workflow

**Files:**
- Create: `.github/workflows/cache-warm.yml`

Deferred from Phase 2 per user direction. A weekly cron job that builds both boards to keep the PlatformIO cache warm. This ensures the first PR after an idle period doesn't hit a cold-cache 10-minute build.

- [ ] **Step 1: Create `.github/workflows/cache-warm.yml`**

```yaml
name: Cache Warm

on:
  schedule:
    # Every Monday at 06:00 UTC — keeps the weekly cache from expiring
    # (GitHub Actions caches expire after 7 days of no access).
    - cron: '0 6 * * 1'
  workflow_dispatch:

jobs:
  warm:
    name: Warm cache (${{ matrix.env }})
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        env: [lolin_d32_pro, metro_s3]
    steps:
      - uses: actions/checkout@v4
        with:
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
- Monday 06:00 UTC ensures the cache is touched within the 7-day GitHub Actions cache TTL.
- `workflow_dispatch` allows manual triggering if the cache needs immediate warming.
- The cache key matches the PR validation and RC/release build keys exactly — a warm run here benefits all other workflows.
- No artifact packaging or release creation — this is purely about cache maintenance.

- [ ] **Step 2: Verify YAML syntax**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/cache-warm.yml')); print('YAML OK')"
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/cache-warm.yml
git commit -m "add weekly cache-warming workflow"
```

---

## Task 6: Update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

Document the release workflow, VERSION-file bump process, and release branch conventions so future sessions follow the correct workflow.

- [ ] **Step 1: Read CLAUDE.md and find the insertion point**

Read `CLAUDE.md` to find the "Branching and merge workflow" section added in Phase 2. The new content extends that section with release-specific guidance.

- [ ] **Step 2: Add release workflow documentation**

After the existing "Branching and merge workflow" section's bullet about `release/rel_X.Y` branches, add:

```markdown
### Release workflow

1. **RC builds** — every push to `main` triggers `.github/workflows/rc-build.yml`, which auto-tags `v<BASE>-RC.<N+1>` and publishes a GitHub Pre-release with six artifacts (3 per board × 2 boards).
2. **Cutting a release** — create a `release/rel_X.Y` branch from `main` and push. `.github/workflows/release-build.yml` auto-tags `vX.Y.0` and publishes a full GitHub Release with the same six artifacts.
3. **Patch releases** — push a bug-fix commit to `release/rel_X.Y`. The workflow auto-tags `vX.Y.<N+1>`.
4. **After cutting a release branch** — open a PR on `develop` that bumps the `VERSION` file to the next planned minor (e.g., `1.0.0` → `1.1.0`). This is a manual step, not automated.
5. **Cache warming** — `.github/workflows/cache-warm.yml` runs weekly (Monday 06:00 UTC) to keep the PlatformIO cache warm. Can also be triggered manually.
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "document release workflow and VERSION bump process in CLAUDE.md"
```

---

## Task 7: Verification — test RC and release flows

**⚠️ Checkpoint:** This task merges to `develop` and then to `main`, creates a release branch, and triggers real CI workflows. **Confirm with the user before each merge/push.** All steps require the user to push since git push via HTTPS doesn't work from this environment.

**Goal:** Prove end-to-end that:
1. Merging to `main` creates an RC pre-release with correct artifacts
2. Creating a `release/rel_1.0` branch creates a stable release with correct artifacts

- [ ] **Step 1: Push `ci/phase3-rc-release` to origin and open a PR → `develop`**

```bash
git push -u origin ci/phase3-rc-release
gh pr create --base develop --head ci/phase3-rc-release \
  --title "CI Phase 3: RC & release build workflows" \
  --body "..."
```

Wait for PR validation to pass (all four checks from Phase 2).

- [ ] **Step 2: Merge Phase 3 PR to `develop`**

Merge the PR via GitHub UI or `gh pr merge`.

- [ ] **Step 3: Merge `develop` → `main` to trigger the RC build**

Create a PR from `develop` → `main`, merge it. This push to `main` triggers `rc-build.yml`.

Watch the workflow run. **Expected outcome:**
- `compute-tag` job: creates and pushes `v1.0.0-RC.1` tag
- `build` job: both `lolin_d32_pro` and `metro_s3` matrix cells succeed
- `release` job: creates a GitHub Pre-release titled `v1.0.0-RC.1` with 6 artifacts:
  - `astros-esp-1.0.0-RC.1-lolin_d32_pro-flash.bin`
  - `astros-esp-1.0.0-RC.1-lolin_d32_pro-app.bin`
  - `astros-esp-1.0.0-RC.1-lolin_d32_pro.elf`
  - `astros-esp-1.0.0-RC.1-metro_s3-flash.bin`
  - `astros-esp-1.0.0-RC.1-metro_s3-app.bin`
  - `astros-esp-1.0.0-RC.1-metro_s3.elf`

Verify the pre-release exists and all 6 artifacts are downloadable.

- [ ] **Step 4: Create `release/rel_1.0` branch to trigger the release build**

```bash
git checkout main && git pull
git checkout -b release/rel_1.0
git push -u origin release/rel_1.0
```

This push to `release/rel_1.0` triggers `release-build.yml`. **Expected outcome:**
- `compute-tag` job: creates and pushes `v1.0.0` tag
- `build` job: both boards succeed
- `release` job: creates a full GitHub Release (not pre-release) titled `v1.0.0` with 6 artifacts

Verify the release exists, is NOT marked as pre-release, and all 6 artifacts are downloadable.

- [ ] **Step 5: Verify artifact naming and download URLs**

Check that the stable download URL pattern works:
```
https://github.com/battlesloth/AstrOs.ESP/releases/download/v1.0.0/astros-esp-1.0.0-metro_s3-flash.bin
```

- [ ] **Step 6: Bump VERSION for the next release line**

After verifying the release, open a PR on `develop` that bumps `VERSION` from `1.0.0` to `1.1.0`:
```bash
git checkout develop && git pull
git checkout -b chore/bump-version-1.1.0
echo "1.1.0" > VERSION
git add VERSION
git commit -m "bump VERSION to 1.1.0 for next release line"
git push -u origin chore/bump-version-1.1.0
```

Open a PR → `develop`, merge it. This ensures subsequent RC builds from `main` produce `v1.1.0-RC.1` (after the next develop→main merge).

- [ ] **Step 7: Update plan checkboxes and record verification results**

Mark Task 7 as complete and add a verification summary at the bottom of this plan file. Commit and push.

---

## Verification summary (acceptance criteria for Phase 3)

Phase 3 is done when **all** of the following are true:

1. `.github/workflows/rc-build.yml` exists with 3 jobs: `compute-tag`, `build` (matrix), `release`.
2. `.github/workflows/release-build.yml` exists with the same 3-job structure.
3. `.github/workflows/cache-warm.yml` exists with a weekly cron schedule.
4. Pushing to `main` creates an RC pre-release with 6 correctly-named artifacts.
5. Creating and pushing a `release/rel_X.Y` branch creates a full release with 6 correctly-named artifacts.
6. Both matrix cells (`lolin_d32_pro`, `metro_s3`) build successfully in both workflows.
7. `fail-fast: false` is set on both matrix strategies.
8. Concurrency groups prevent tag-count races (`cancel-in-progress: false`).
9. `CLAUDE.md` documents the release workflow and VERSION bump process.
10. The `VERSION` file is bumped for the next release line after verification.

## Out of scope (deferred to Phase 4)

- Browser flasher (`docs/flasher/`) and GitHub Pages
- `docs/flasher/manifest.json` auto-update from release workflow
- `FLASHING.md` user-facing guide
