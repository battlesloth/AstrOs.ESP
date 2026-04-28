# CI Pipeline Design — AstrOs.ESP

## Context

AstrOs.ESP currently has no CI. The repo is hosted on GitHub (`battlesloth/AstrOs.ESP`), built locally via PlatformIO against two boards (`lolin_d32_pro` + `metro_s3`), and the firmware version is hard-coded at `lib/AstrOsUtility/src/AstrOsConstants.hpp:10` (`v1.0.0-dev2`) and displayed on the OLED via `lib/Modules/src/I2cModule.cpp:44`.

This plan adds a full CI/CD pipeline with semantic versioning, release branches for maintenance lines, flashable + OTA artifacts, and a browser-based one-click flasher for non-technical R2 builders. The primary consumer of the published artifacts in the future is the sibling `AstrOs.Server` repo, which will eventually push updates to ESP boards over serial or OTA without requiring USB.

**Scope guard check**: this work touches GitHub workflows, PlatformIO build scripts, a new generated header, `CLAUDE.md`, `.gitignore`, a new `VERSION` file, a new clang-format config, GitHub Pages setup, and documentation. The task count is above the ~8-task threshold in `CLAUDE.md`, so the implementation plan will be broken into phases when `writing-plans` is invoked. This design doc itself stays unified.

## Locked Decisions

### Versioning
- **Git tags are the single source of truth.** CI creates tags, never commits back. No PAT, no `[skip ci]`, no feedback-loop risk.
- **Version format is clean:** `1.0.0`, `1.0.0-RC.3`, `1.0.0-dev.5`. No SHA suffix in the user-visible string.
- **Resolution logic** (same for CI and local builds):
  ```bash
  BASE_TAG=$(git describe --tags --abbrev=0)        # e.g. v1.0.0-RC.3
  COUNT=$(git rev-list ${BASE_TAG}..HEAD --count)   # e.g. 5
  BASE_VERSION=$(cat VERSION)                        # e.g. 1.0.0
  if [ "$COUNT" = "0" ]; then
      VERSION="${BASE_TAG#v}"                        # on-tag: 1.0.0-RC.3
  else
      VERSION="${BASE_VERSION}-dev.${COUNT}"         # off-tag: 1.0.0-dev.5
  fi
  SHORT_SHA=$(git rev-parse --short HEAD)            # for console log only
  ```
- **`VERSION` file** (committed, repo root) holds just the base (e.g., `1.0.0`). Hand-bumped via normal PR when starting a new release line. One line, no JSON, no parsing ceremony.
- **Short SHA is logged to the serial console** on boot via `ESP_LOGI(TAG, "Version: %s (sha: %s)", Version, GitSha)`. Never appears on the OLED or in the version string.

### Branching model
- `main` is the integration branch. PRs target it. Pushes to main produce `X.Y.Z-RC.N` builds.
- `release/rel_<MAJOR>.<MINOR>` branches are cut from `main` when a version is ready to ship. The first push to the branch produces the `vX.Y.Z` release; subsequent pushes (bug fixes) produce `vX.Y.(Z+1)` patch releases.
- Multiple release branches can exist in parallel (e.g., `release/rel_1.0` and `release/rel_1.1` simultaneously).
- After cutting `release/rel_X.Y`, a follow-up PR hand-bumps `main`'s `VERSION` file to the next planned minor (e.g., `1.1.0`). This is a documented step, not automated.
- **Fix flow**: case-by-case. Backport or forward-port at developer discretion. Neither CI nor `CLAUDE.md` enforces a direction.

### Artifacts
Three files per board (`lolin_d32_pro`, `metro_s3`) per tag:
- `astros-esp-<version>-<board>-flash.bin` — merged image via `esptool.py merge_bin`, flashable at offset `0x0`. Primary artifact for humans and for server-driven serial re-flashing.
- `astros-esp-<version>-<board>-app.bin` — raw app partition (just the firmware). For OTA updates from AstrOs.Server later.
- `astros-esp-<version>-<board>.elf` — debug symbols. For decoding exception backtraces from field crashes.

Filenames are **stable and predictable** so AstrOs.Server can construct download URLs without listing releases:
```
https://github.com/battlesloth/AstrOs.ESP/releases/download/v<version>/astros-esp-<version>-<board>-flash.bin
```

Both board targets build on every tag (matrix). `lolin_d32_pro` stays in even though it's deprecated in practice, as insurance that the multi-board scaffolding still works. Adding a future board = one line in the matrix + a new `[env:]` in `platformio.ini`.

### PR validation (all four on day one)
1. **Native unit tests** — `pio test -e test` runs `test/test_native/` googletest suite.
2. **Both-board compile** — matrix runs `pio run -e lolin_d32_pro` + `pio run -e metro_s3` in parallel.
3. **AstrOsMessaging native-purity guard** — grep under `lib/AstrOsMessaging/` for `#include <freertos/`, `#include <esp_`, `#include <driver/`; fail on any match. Enforces the rule in `lib/AstrOsMessaging/README`.
4. **clang-format check (changed-files-only)** — `git diff --name-only origin/main...HEAD` → filter `.cpp`/`.hpp`/`.h`/`.c` → run `clang-format --dry-run --Werror`. First PR commits a `.clang-format` file (`BasedOnStyle: LLVM`, `IndentWidth: 4`, `ColumnLimit: 120`). Existing untouched files are grandfathered until a future full-tree reformat PR.

### Browser flasher (ESP Web Tools on GitHub Pages)
- A tiny static site at `docs/flasher/` containing `index.html` + `manifest.json` + minimal CSS.
- HTML uses `<esp-web-install-button>` from `esptool-js`.
- `manifest.json` references the latest release's `-flash.bin` URL on GitHub Releases.
- GitHub Pages serves from `main` branch, `docs/` directory. Free hosting.
- **CI step** on release-build workflow: after uploading artifacts, update `docs/flasher/manifest.json` with the new version and open a small PR (or direct commit on release branch — to be decided during implementation, whichever avoids the feedback-loop issue since GitHub Pages rebuild-on-main is different from normal workflow triggers).
- Result: `https://battlesloth.github.io/AstrOs.ESP/flasher/` → plug in board → click Install → done. Works in Chrome/Edge via WebSerial.

### Infrastructure
- **Cache**: `actions/cache@v4` on `~/.platformio/`. Key: `hash(platformio.ini, sdkconfig.*, dependencies.lock)`. Warm runs ~2-3min per board; cold ~10min.
- **`fetch-depth: 0`** on every `actions/checkout@v4` so `git describe` sees all tags.
- **Initial VERSION file**: `1.0.0`. First push to main tags `v1.0.0-RC.1`. First release branch cuts `v1.0.0`.
- **Rollout**: Land all three workflows at once. Every workflow includes `workflow_dispatch:` for manual re-triggers during the inevitable first-run debugging.

## Architecture

Three GitHub workflows plus local build-script changes:

### `.github/workflows/pr-validation.yml`
- Trigger: `pull_request: branches: [main, 'release/rel_*']`
- Jobs:
  - `native-tests` — checkout (shallow ok), set up Python, install PlatformIO, run `pio test -e test`.
  - `build` — matrix over `[lolin_d32_pro, metro_s3]`, full `fetch-depth: 0`, cache, `pio run -e ${{ matrix.env }}`. Each matrix cell runs the version-resolution script and produces build outputs (not uploaded on PR).
  - `messaging-purity` — grep check. Fast, no cache, <5s.
  - `clang-format` — checkout with `fetch-depth: 0`, install clang-format (apt), run the diff-based check.
- Playwright equivalent for firmware (on-hardware tests) doesn't exist and isn't in scope. Noted for future.

### `.github/workflows/rc-build.yml`
- Trigger: `push: branches: [main]` + `workflow_dispatch:`
- Job: `tag-and-build`
  - Checkout `fetch-depth: 0`.
  - Compute next RC tag: read `VERSION`, list existing `v<base>-RC.*` tags, find max N, pick N+1.
  - `git tag -a v<base>-RC.<N+1> -m "..."` + `git push --tags` (uses `GITHUB_TOKEN` with `contents: write`).
  - Matrix build both boards with version resolution → `-flash.bin`, `-app.bin`, `.elf` per board.
  - Create GitHub Pre-release with the tag + upload all 6 artifacts (3 per board × 2 boards).
- Concurrency group: `rc-build-${{ github.ref }}` with `cancel-in-progress: false` to serialize RC bumps and avoid two merges racing on the tag-count step.

### `.github/workflows/release-build.yml`
- Trigger: `push: branches: ['release/rel_*']` + `workflow_dispatch:`
- Job: `tag-and-build`
  - Checkout `fetch-depth: 0`.
  - Compute release/patch tag: read `VERSION`, list existing `v<base>.*` (non-prerelease) tags, pick next patch number. First push to a new release branch tags `v<base>`; subsequent pushes tag `v<base>` patch-bumped.
  - `git tag -a v<new-version> -m "..."` + `git push --tags`.
  - Matrix build both boards → artifacts.
  - Create GitHub **Release** (not pre-release) with the tag + upload artifacts.
  - Update `docs/flasher/manifest.json` to point at the new release's `-flash.bin` URLs (GitHub Pages picks up the change on next `main` push — see below).
- Concurrency group: `release-build-${{ github.ref }}`.

### Local build — PlatformIO pre-build script
- New file `scripts/version_gen.py` (referenced from `platformio.ini` via `extra_scripts = pre:scripts/version_gen.py`).
- Reads git state, resolves version per the logic above, writes `lib/AstrOsUtility/src/version_generated.hpp`:
  ```cpp
  // AUTO-GENERATED - do not edit
  #pragma once
  namespace AstrOsConstants {
      constexpr const char *Version = "1.0.0-dev.5";
      constexpr const char *GitSha = "0539b3e";
  }
  ```
- `lib/AstrOsUtility/src/AstrOsConstants.hpp`: replace the literal with `#include "version_generated.hpp"`.
- `.gitignore`: add `lib/AstrOsUtility/src/version_generated.hpp`.
- `lib/Modules/src/I2cModule.cpp:44`: unchanged — still reads `AstrOsConstants::Version`.
- Add `ESP_LOGI` call in `src/main.cpp` `init()` that logs `Version` + `GitSha` at boot.
- **CI uses the same script.** Version resolution logic exists in exactly one place.

### Version flow diagram
```
                    ┌─────────────────────────┐
                    │ VERSION file (committed)│
                    │       "1.0.0"           │
                    └──────────┬──────────────┘
                               │
                               ▼
        ┌──────────────────────────────────────────┐
        │  scripts/version_gen.py  (pre-build)     │
        │  - git describe --tags --abbrev=0        │
        │  - git rev-list COUNT..HEAD --count      │
        │  - cat VERSION                           │
        └──────────┬───────────────────────────────┘
                   │
                   ▼
    ┌──────────────────────────────────────────┐
    │ lib/AstrOsUtility/src/version_generated  │
    │   .hpp    (gitignored, always generated) │
    │                                          │
    │   Version = "1.0.0-dev.5"                │
    │   GitSha  = "0539b3e"                    │
    └──────────┬───────────────────────────────┘
               │
               ▼
    ┌──────────────────────────────────────────┐
    │ AstrOsConstants.hpp  #includes it        │
    │ I2cModule.cpp        displays it on OLED │
    │ main.cpp init()      logs it to console  │
    └──────────────────────────────────────────┘
```

## Critical files

**New:**
- `.github/workflows/pr-validation.yml`
- `.github/workflows/rc-build.yml`
- `.github/workflows/release-build.yml`
- `scripts/version_gen.py` — PlatformIO extra_script + CI-callable
- `VERSION` — one line, `1.0.0`
- `.clang-format` — LLVM base, 4-space, 120 col
- `docs/flasher/index.html` — ESP Web Tools page
- `docs/flasher/manifest.json` — generated/updated by release workflow
- `docs/flasher/style.css` — minimal styling
- `FLASHING.md` — user-facing flashing guide pointing at the Pages flasher + GUI utility fallback

**Modified:**
- `platformio.ini` — add `extra_scripts = pre:scripts/version_gen.py` to all envs except `[env:test]` (native tests don't need the header)
- `lib/AstrOsUtility/src/AstrOsConstants.hpp` — replace literal with `#include "version_generated.hpp"`
- `src/main.cpp` — add `ESP_LOGI` boot log for Version + GitSha in `init()`
- `.gitignore` — add `lib/AstrOsUtility/src/version_generated.hpp`
- `CLAUDE.md` — document the VERSION-file bump flow after cutting a release branch, and the `release/rel_*` convention

**Unchanged (confirmed consumers):**
- `lib/Modules/src/I2cModule.cpp:44` — still reads `AstrOsConstants::Version`, no change
- Partition tables — already OTA-ready

## Phasing

The plan has ~14 tasks which exceeds the `CLAUDE.md` scope-guard threshold. When `writing-plans` is invoked, break along the queue/task-seam recipe adapted for CI work:

- **Phase 1 — Versioning foundation**: `version_gen.py` pre-build script, `VERSION` file, generated header, `.gitignore`, `AstrOsConstants.hpp` include, main.cpp boot log, `.clang-format`. End state: local `pio run -e metro_s3` produces a correctly-versioned binary using the new flow. No CI yet.
- **Phase 2 — PR validation workflow**: `.github/workflows/pr-validation.yml` with all four checks, cache config, matrix builds. End state: open a test PR, verify all four checks pass/fail as expected.
- **Phase 3 — RC + Release build workflows + artifact publishing**: `rc-build.yml`, `release-build.yml`, concurrency groups, `fetch-depth: 0`, tag creation, merged-bin generation via `esptool.py merge_bin`, GitHub Release upload. End state: push to main creates an RC pre-release with artifacts; creating a `release/rel_1.0` branch creates a stable release.
- **Phase 4 — Browser flasher + docs**: `docs/flasher/` static site, `manifest.json` generation step, GitHub Pages enablement, `FLASHING.md`. End state: browser flasher works against the latest release.

Each phase is independently shippable and testable. Phase 1 produces correctly-versioned binaries even without CI. Phase 2 is pure CI validation with no artifact publishing. Phase 3 stands up the full publish flow. Phase 4 is polish for end users.

## Verification

- **Phase 1**: `pio run -e metro_s3`; verify `version_generated.hpp` exists, contains the expected version string, and that the built binary boots on hardware and shows the version on OLED + console log.
- **Phase 2**: Open a PR against `main` that deliberately breaks one of the four checks (e.g., adds `#include <freertos/FreeRTOS.h>` to `lib/AstrOsMessaging/include/AstrOsMessaging.hpp`); verify the PR fails with a clear message.
- **Phase 3**: Manually push a no-op commit to main; verify `v1.0.0-RC.1` tag is created, both boards build in the matrix, and a GitHub Pre-release appears with six artifacts (three per board). Branch a `release/rel_1.0` from main and push; verify `v1.0.0` release appears with artifacts.
- **Phase 4**: Visit `https://battlesloth.github.io/AstrOs.ESP/flasher/` in Chrome, plug in a board, click Install, verify it flashes successfully.

## Open minor sub-questions (settle during implementation)

- How exactly `docs/flasher/manifest.json` gets updated — direct commit on `release/rel_*` branch (works, but weird because it's not firmware code) vs. PR to main (cleaner separation, but delays GitHub Pages update until PR is merged) vs. a side branch dedicated to the flasher manifest. Recommendation: update on main via a tiny auto-PR from the release workflow; weigh during Phase 4.
- Exact clang-format rule set beyond base style — e.g., brace wrap style, pointer alignment. Default to LLVM defaults and tweak iff the first commits generate churn the user dislikes.
- Whether to also emit `.map` files for each build (useful for debugging flash/RAM usage). Cheap to add; default to yes unless size of release artifacts becomes a concern.
