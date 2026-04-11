# CI Phase 1 — Versioning Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hard-coded `AstrOsConstants::Version = "v1.0.0-dev2"` with a git-tag-derived version resolver that works identically in local PlatformIO builds and (later) in CI, and lay down the `.clang-format` config that Phase 2 will enforce.

**Architecture:** A single Python script (`scripts/version_gen.py`) runs as a PlatformIO `pre:` build hook. It reads a new committed `VERSION` file (base version only) plus `git describe --tags --abbrev=0` + `git rev-list --count` to resolve a clean semver string (`1.0.0`, `1.0.0-RC.3`, or `1.0.0-dev.5`). It writes `lib/AstrOsUtility/src/version_generated.hpp` (gitignored) on every build. `AstrOsConstants.hpp` `#include`s that generated header. Boot-time `ESP_LOGI` logs the version and short SHA to the serial console; the OLED display shows the clean version via the existing `AstrOsConstants::Version` path (no change to `I2cModule.cpp`).

**Tech Stack:** Python 3 (for the build script — already required by PlatformIO), PlatformIO `extra_scripts` API, C++17 (existing toolchain), ESP-IDF logging, clang-format 16+ (LLVM base).

**Parent design doc:** `.docs/plans/20260411-0905-ci-pipeline-design.md`

---

## File Structure

**New files:**
- `VERSION` — single-line base version (`1.0.0\n`). Committed. Hand-bumped via PR when starting a new release line.
- `scripts/version_gen.py` — Python script. Runs as PlatformIO pre-build hook AND can be invoked standalone (`python3 scripts/version_gen.py`) for local testing. Reads git state, writes `version_generated.hpp`.
- `scripts/README.md` — one paragraph explaining what lives in `scripts/` and that `version_gen.py` is called by `platformio.ini`.
- `lib/AstrOsUtility/src/version_generated.hpp` — auto-generated C++ header. **Gitignored.** Never committed. Always regenerated.
- `.clang-format` — LLVM base with 4-space indent + 120-column limit. No enforcement in Phase 1 (Phase 2 adds the CI check); just committing the config.

**Modified files:**
- `lib/AstrOsUtility/src/AstrOsConstants.hpp` — replace hard-coded `Version` literal with `#include "version_generated.hpp"`.
- `platformio.ini` — add `extra_scripts = pre:scripts/version_gen.py` to **all three** envs (`lolin_d32_pro`, `metro_s3`, and `test`). The test env also needs it because `test/test_native/astros_serial_messages_tests.cpp` transitively includes `AstrOsConstants.hpp` via `AstrOsUtility.h`.
- `src/main.cpp` — add one `ESP_LOGI` call in `init()` immediately after the existing `"init called"` log (around line 215) that logs `AstrOsConstants::Version` and `AstrOsConstants::GitSha`.
- `.gitignore` — add `lib/AstrOsUtility/src/version_generated.hpp`.

**Unchanged:**
- `lib/Modules/src/I2cModule.cpp:44` — continues to read `AstrOsConstants::Version` and pass it to the OLED. No edit required.
- Partition tables, test files themselves, everything under `lib/AstrOsMessaging/`.

---

## Task 1: Create the `VERSION` file

**Files:**
- Create: `VERSION`

- [ ] **Step 1: Create the VERSION file**

```bash
echo "1.0.0" > /home/jeff/Source/astros/AstrOs.ESP/VERSION
```

- [ ] **Step 2: Verify the file is exactly one line with `1.0.0`**

Run: `cat /home/jeff/Source/astros/AstrOs.ESP/VERSION`
Expected output: `1.0.0`

Also verify no trailing blank lines: `wc -l /home/jeff/Source/astros/AstrOs.ESP/VERSION` should print `1`.

- [ ] **Step 3: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add VERSION
git commit -m "$(cat <<'EOF'
add VERSION file as base-version source of truth

First step of the CI pipeline: a single-line file holding only the
base version (major.minor.patch). Build-time scripts read this to
construct the full version string. Hand-bumped via PR when starting
a new release line.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Create `scripts/version_gen.py`

**Files:**
- Create: `scripts/version_gen.py`
- Create: `scripts/README.md`

- [ ] **Step 1: Create the `scripts/` directory and write `version_gen.py`**

File: `/home/jeff/Source/astros/AstrOs.ESP/scripts/version_gen.py`

```python
#!/usr/bin/env python3
"""
Version resolver for AstrOs.ESP firmware builds.

Runs as a PlatformIO pre-build hook (wired from platformio.ini via
`extra_scripts = pre:scripts/version_gen.py`) AND as a standalone CLI
script (`python3 scripts/version_gen.py`). Both entry points produce
the same output: lib/AstrOsUtility/src/version_generated.hpp.

Version resolution rules (see .docs/plans/20260411-0905-ci-pipeline-design.md):

  BASE_VERSION = contents of VERSION file (e.g. "1.0.0")
  BASE_TAG     = `git describe --tags --abbrev=0`  (e.g. "v1.0.0-RC.3")
  COUNT        = `git rev-list BASE_TAG..HEAD --count`
  SHORT_SHA    = `git rev-parse --short HEAD`

  If no tags exist:       Version = "{BASE_VERSION}-dev.0"
  If COUNT == 0 (on tag): Version = BASE_TAG with leading 'v' stripped
  If COUNT >  0 (off tag):Version = "{BASE_VERSION_clean}-dev.{COUNT}"
                          where BASE_VERSION_clean has any -RC.N suffix stripped

Git SHA is logged to the serial console at boot but never appears in
the user-visible version string.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO_ROOT / "VERSION"
OUTPUT_FILE = REPO_ROOT / "lib" / "AstrOsUtility" / "src" / "version_generated.hpp"


def _run(cmd: str, default: str = "") -> str:
    """Run a shell command in REPO_ROOT. Return stripped stdout or default on failure."""
    try:
        return subprocess.check_output(
            cmd,
            shell=True,
            cwd=str(REPO_ROOT),
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except subprocess.CalledProcessError:
        return default


def resolve_version() -> tuple[str, str]:
    """Return (version_string, short_sha). Never raises — falls back to placeholders."""
    if VERSION_FILE.exists():
        base_version = VERSION_FILE.read_text().strip()
    else:
        base_version = "0.0.0"

    base_tag = _run("git describe --tags --abbrev=0")
    short_sha = _run("git rev-parse --short HEAD", default="unknown")

    # No tags in the repo yet — first-ever local build or CI on a fresh clone with shallow depth
    if not base_tag:
        return f"{base_version}-dev.0", short_sha

    count_str = _run(f"git rev-list {base_tag}..HEAD --count", default="0")
    try:
        count = int(count_str)
    except ValueError:
        count = 0

    if count == 0:
        # Exact tag — strip leading 'v' if present: v1.0.0-RC.3 -> 1.0.0-RC.3
        return base_tag.lstrip("v"), short_sha

    # Off-tag dev build — strip any -RC.N / -rc.N suffix from the base
    clean_base = re.sub(r"-RC\.\d+$", "", base_version, flags=re.IGNORECASE)
    return f"{clean_base}-dev.{count}", short_sha


def write_header(version: str, sha: str) -> bool:
    """Write version_generated.hpp. Returns True if file contents changed."""
    content = (
        "// AUTO-GENERATED by scripts/version_gen.py - do not edit.\n"
        "// Regenerated on every build from the VERSION file + git state.\n"
        "#pragma once\n"
        "\n"
        "namespace AstrOsConstants\n"
        "{\n"
        f'    constexpr const char *Version = "{version}";\n'
        f'    constexpr const char *GitSha = "{sha}";\n'
        "}\n"
    )
    OUTPUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    if OUTPUT_FILE.exists() and OUTPUT_FILE.read_text() == content:
        return False
    OUTPUT_FILE.write_text(content)
    return True


def main() -> None:
    version, sha = resolve_version()
    changed = write_header(version, sha)
    rel_out = OUTPUT_FILE.relative_to(REPO_ROOT)
    marker = "(updated)" if changed else "(unchanged)"
    print(f"[version_gen] Version={version} GitSha={sha} -> {rel_out} {marker}")


# Detect PlatformIO context: the `Import` builtin is injected when
# platformio loads this script via extra_scripts. Standalone runs
# get a NameError, which we treat as "run main() if __main__".
try:
    Import("env")  # type: ignore[name-defined]  # noqa: F821
    main()
except NameError:
    if __name__ == "__main__":
        main()
```

- [ ] **Step 2: Make the script executable**

```bash
chmod +x /home/jeff/Source/astros/AstrOs.ESP/scripts/version_gen.py
```

- [ ] **Step 3: Write `scripts/README.md`**

File: `/home/jeff/Source/astros/AstrOs.ESP/scripts/README.md`

```markdown
# scripts/

Build-support scripts that run outside the normal C++ build.

## version_gen.py

Called automatically by PlatformIO before every build via `extra_scripts = pre:scripts/version_gen.py` in `platformio.ini`. Resolves the firmware version from `../VERSION` + git state and writes `../lib/AstrOsUtility/src/version_generated.hpp` (gitignored).

Can also be run standalone for debugging:

```
python3 scripts/version_gen.py
```

See `.docs/plans/20260411-0905-ci-pipeline-design.md` for the full versioning strategy.
```

- [ ] **Step 4: Run the script standalone and verify it produces a valid header**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
python3 scripts/version_gen.py
```

Expected output (version number will vary based on git state — the repo currently has NO tags, so expect `1.0.0-dev.0`):
```
[version_gen] Version=1.0.0-dev.0 GitSha=<some 7-char sha> -> lib/AstrOsUtility/src/version_generated.hpp (updated)
```

- [ ] **Step 5: Verify the generated header file exists and has the expected shape**

```bash
cat /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/version_generated.hpp
```

Expected content (SHA will differ):
```cpp
// AUTO-GENERATED by scripts/version_gen.py - do not edit.
// Regenerated on every build from the VERSION file + git state.
#pragma once

namespace AstrOsConstants
{
    constexpr const char *Version = "1.0.0-dev.0";
    constexpr const char *GitSha = "xxxxxxx";
}
```

- [ ] **Step 6: Run it a second time and verify the `(unchanged)` marker**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
python3 scripts/version_gen.py
```

Expected output:
```
[version_gen] Version=1.0.0-dev.0 GitSha=<same sha> -> lib/AstrOsUtility/src/version_generated.hpp (unchanged)
```

This confirms the idempotent-write logic works: PlatformIO won't trigger unnecessary rebuilds from mtime churn.

- [ ] **Step 7: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add scripts/version_gen.py scripts/README.md
git commit -m "$(cat <<'EOF'
add version_gen.py pre-build script and scripts/README

Resolves the firmware version from the VERSION file + git state and
writes lib/AstrOsUtility/src/version_generated.hpp. Designed to run
both as a PlatformIO extra_script (auto-invoked on every build) and
as a standalone CLI script for debugging. Idempotent writes avoid
triggering unnecessary rebuilds when the version is unchanged.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

Note: the generated `lib/AstrOsUtility/src/version_generated.hpp` is NOT staged — it will be gitignored in Task 4.

---

## Task 3: Wire `version_gen.py` into `AstrOsConstants.hpp`

**Files:**
- Modify: `lib/AstrOsUtility/src/AstrOsConstants.hpp` (remove the hard-coded Version literal, add the include)

- [ ] **Step 1: Read the current state of `AstrOsConstants.hpp`**

Run: `cat /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/AstrOsConstants.hpp`

Expected content (lines 1-12 are what we're modifying):
```cpp
#ifndef ASTROSCONSTANTS_H
#define ASTROSCONSTANTS_H

namespace AstrOsConstants
{

    /***********************************
     *  Module
     ***********************************/
    constexpr const char *Version = "v1.0.0-dev2";

    constexpr const char *ModuleName = "AstrOs-esp32";
```

- [ ] **Step 2: Apply the edit**

Use the `Edit` tool on `/home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/AstrOsConstants.hpp`:

old_string:
```cpp
#ifndef ASTROSCONSTANTS_H
#define ASTROSCONSTANTS_H

namespace AstrOsConstants
{

    /***********************************
     *  Module
     ***********************************/
    constexpr const char *Version = "v1.0.0-dev2";

    constexpr const char *ModuleName = "AstrOs-esp32";
```

new_string:
```cpp
#ifndef ASTROSCONSTANTS_H
#define ASTROSCONSTANTS_H

// Version and GitSha are defined in version_generated.hpp, which is
// written by scripts/version_gen.py on every PlatformIO build and is
// gitignored. Do not redefine them here.
#include "version_generated.hpp"

namespace AstrOsConstants
{

    /***********************************
     *  Module
     ***********************************/
    constexpr const char *ModuleName = "AstrOs-esp32";
```

- [ ] **Step 3: Verify the edit landed correctly**

Run: `head -15 /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/AstrOsConstants.hpp`

Expected output:
```cpp
#ifndef ASTROSCONSTANTS_H
#define ASTROSCONSTANTS_H

// Version and GitSha are defined in version_generated.hpp, which is
// written by scripts/version_gen.py on every PlatformIO build and is
// gitignored. Do not redefine them here.
#include "version_generated.hpp"

namespace AstrOsConstants
{

    /***********************************
     *  Module
     ***********************************/
    constexpr const char *ModuleName = "AstrOs-esp32";
```

Also verify the old literal is gone:

```bash
grep -n "v1.0.0-dev2" /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/AstrOsConstants.hpp
```

Expected: no output (exit code 1).

- [ ] **Step 4: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add lib/AstrOsUtility/src/AstrOsConstants.hpp
git commit -m "$(cat <<'EOF'
replace hard-coded Version constant with generated header include

AstrOsConstants.hpp now pulls Version and GitSha from the generated
version_generated.hpp instead of hard-coding them. The generated
header is produced by scripts/version_gen.py on every build (wired
in platformio.ini in the next task).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Gitignore the generated header + wire the script into platformio.ini

**Files:**
- Modify: `.gitignore`
- Modify: `platformio.ini` (add `extra_scripts` to all three envs)

- [ ] **Step 1: Append to `.gitignore`**

Use the `Edit` tool on `/home/jeff/Source/astros/AstrOs.ESP/.gitignore`. First check the current tail of the file to find a unique anchor:

```bash
tail -5 /home/jeff/Source/astros/AstrOs.ESP/.gitignore
```

Then append these lines at the end of `.gitignore`:

```
# Generated by scripts/version_gen.py on every build
lib/AstrOsUtility/src/version_generated.hpp
```

The exact append strategy depends on the current end-of-file; use `Edit` with the last ~3 lines of the file as `old_string` and the same plus the new block as `new_string`. If `.gitignore` is very long, a safer alternative is to read the last 20 lines first and construct a unique anchor.

- [ ] **Step 2: Verify the pattern is in place and matches the generated file**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git check-ignore lib/AstrOsUtility/src/version_generated.hpp
```

Expected output: `lib/AstrOsUtility/src/version_generated.hpp`

This confirms git ignores the generated file.

- [ ] **Step 3: Apply the `platformio.ini` edits — `[env:lolin_d32_pro]`**

Read the current env block first:

```bash
sed -n '11,43p' /home/jeff/Source/astros/AstrOs.ESP/platformio.ini
```

Use `Edit` to add `extra_scripts = pre:scripts/version_gen.py` as a new line immediately after `framework = espidf` in the `[env:lolin_d32_pro]` block.

old_string:
```
[env:lolin_d32_pro]
platform = espressif32
board = lolin_d32_pro
framework = espidf
monitor_speed = 115200
```

new_string:
```
[env:lolin_d32_pro]
platform = espressif32
board = lolin_d32_pro
framework = espidf
extra_scripts = pre:scripts/version_gen.py
monitor_speed = 115200
```

- [ ] **Step 4: Apply the `platformio.ini` edits — `[env:metro_s3]`**

old_string:
```
[env:metro_s3]
board = adafruit_metro_esp32s3
platform = espressif32
framework = espidf
monitor_speed = 115200
```

new_string:
```
[env:metro_s3]
board = adafruit_metro_esp32s3
platform = espressif32
framework = espidf
extra_scripts = pre:scripts/version_gen.py
monitor_speed = 115200
```

- [ ] **Step 5: Apply the `platformio.ini` edits — `[env:test]`**

The test env also needs the script because `test/test_native/astros_serial_messages_tests.cpp` includes `AstrOsUtility.h`, which transitively includes `AstrOsConstants.hpp`, which now includes `version_generated.hpp`. Without the script running, the native test build will fail with "file not found".

old_string:
```
[env:test]
platform = native
test_framework = googletest
test_ignore = embedded
debug_test = test_native
build_unflags = -std=gnu++11
build_flags = -std=gnu++2a
```

new_string:
```
[env:test]
platform = native
test_framework = googletest
test_ignore = embedded
debug_test = test_native
extra_scripts = pre:scripts/version_gen.py
build_unflags = -std=gnu++11
build_flags = -std=gnu++2a
```

- [ ] **Step 6: Verify all three envs have the hook**

```bash
grep -n "extra_scripts" /home/jeff/Source/astros/AstrOs.ESP/platformio.ini
```

Expected output: three lines, one per env:
```
15:extra_scripts = pre:scripts/version_gen.py
49:extra_scripts = pre:scripts/version_gen.py
85:extra_scripts = pre:scripts/version_gen.py
```

(Line numbers may be off by one or two but the count must be exactly 3.)

- [ ] **Step 7: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add .gitignore platformio.ini
git commit -m "$(cat <<'EOF'
wire version_gen.py into all three PlatformIO envs

Adds extra_scripts to lolin_d32_pro, metro_s3, and test envs so the
generated header is always produced before any compile. The test env
needs it too because test/test_native includes AstrOsUtility.h which
transitively pulls in AstrOsConstants.hpp. Also gitignores the
generated header.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: End-to-end build verification

**Files:**
- No file changes — this task verifies the previous four tasks work together.

- [ ] **Step 1: Delete any stale generated header to force a clean run**

```bash
rm -f /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/version_generated.hpp
```

- [ ] **Step 2: Run the native test env — this is the fastest check that the header is generated and compiles**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
pio test -e test 2>&1 | tee /tmp/pio-test.log
```

Expected in the output stream, early in the run:
```
[version_gen] Version=1.0.0-dev.0 GitSha=<sha> -> lib/AstrOsUtility/src/version_generated.hpp (updated)
```

Expected final state: all tests PASS. If tests pass, the header was generated and `AstrOsConstants.hpp`'s new include worked.

If the tests fail with `'version_generated.hpp' file not found`, something is wrong with the `extra_scripts` wiring — recheck Task 4. If they fail with a compile error in `version_generated.hpp`, the script's output format is wrong — recheck Task 2 Step 1.

- [ ] **Step 3: Verify the generated header exists after the test run**

```bash
cat /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsUtility/src/version_generated.hpp
```

Expected: valid C++ header with `Version` and `GitSha` constants.

- [ ] **Step 4: Run a metro_s3 compile (firmware build, not test)**

This is slower (~5-10 minutes cold, since the ESP-IDF toolchain may need to download) but it's the real test that the firmware builds with the new version plumbing.

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
pio run -e metro_s3 2>&1 | tail -40
```

Expected: final lines include `SUCCESS` / `========== [SUCCESS] Took <time> ==========`. The build should show the `[version_gen]` log line near the top.

- [ ] **Step 5: Grep the built binary to confirm the version string is embedded**

```bash
strings /home/jeff/Source/astros/AstrOs.ESP/.pio/build/metro_s3/firmware.elf | grep -E "1\.0\.0-dev\." | head -3
```

Expected: at least one line matching `1.0.0-dev.<N>`. This confirms the version string made it from the Python script, through the generated header, through the compiler, into the linked binary.

- [ ] **Step 6: No commit for this task**

This is a verification-only task. No files changed. If everything passes, move on to Task 6. If anything fails, back up to the responsible task and fix it before continuing.

---

## Task 6: Add boot log for version + SHA in `main.cpp`

**Files:**
- Modify: `src/main.cpp` (add one `ESP_LOGI` line in `init()`)

- [ ] **Step 1: Read the current init() header to locate the insertion point**

```bash
sed -n '213,220p' /home/jeff/Source/astros/AstrOs.ESP/src/main.cpp
```

Expected output:
```cpp
void init(void)
{
    ESP_LOGI(TAG, "init called");

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_POSEDGE; // Interrupt on rising edge
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << RESET_PIN;
```

- [ ] **Step 2: Apply the edit**

Use `Edit` on `/home/jeff/Source/astros/AstrOs.ESP/src/main.cpp`:

old_string:
```cpp
void init(void)
{
    ESP_LOGI(TAG, "init called");

    gpio_config_t io_conf;
```

new_string:
```cpp
void init(void)
{
    ESP_LOGI(TAG, "init called");
    ESP_LOGI(TAG, "AstrOs.ESP version %s (sha: %s)", AstrOsConstants::Version, AstrOsConstants::GitSha);

    gpio_config_t io_conf;
```

- [ ] **Step 3: Verify the edit**

```bash
sed -n '213,220p' /home/jeff/Source/astros/AstrOs.ESP/src/main.cpp
```

Expected output includes both log lines.

- [ ] **Step 4: Rebuild metro_s3 to confirm the new log line compiles**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
pio run -e metro_s3 2>&1 | tail -20
```

Expected: `SUCCESS`. This build will be fast (~30s) because only `src/main.cpp` changed.

- [ ] **Step 5: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add src/main.cpp
git commit -m "$(cat <<'EOF'
log firmware version and git sha at boot

Adds a second ESP_LOGI in init() that prints the resolved version
string and the short git sha. The version also appears on the OLED
display via the existing AstrOsConstants::Version consumer in
I2cModule.cpp; the sha is serial-console only so the display stays
uncluttered for non-technical R2 builders.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Create `.clang-format`

**Files:**
- Create: `.clang-format`

- [ ] **Step 1: Write the config**

File: `/home/jeff/Source/astros/AstrOs.ESP/.clang-format`

```yaml
---
# AstrOs.ESP C/C++ formatting rules.
# Based on LLVM defaults with three overrides:
#   - 4-space indent (matches existing codebase)
#   - 120-column limit (fits two panes side-by-side on a typical monitor)
#   - PointerAlignment: Left (matches `char *foo` pattern used throughout)
#
# Phase 1 only lays down this file. Phase 2 adds a CI check that
# runs `clang-format --dry-run --Werror` against changed files only
# (grandfathering untouched existing files).
BasedOnStyle: LLVM
IndentWidth: 4
ColumnLimit: 120
PointerAlignment: Left
---
```

- [ ] **Step 2: Verify the config is parseable by clang-format if available**

```bash
which clang-format && clang-format --style=file --dump-config /home/jeff/Source/astros/AstrOs.ESP/.clang-format 2>&1 | head -5
```

If `clang-format` is not installed locally, skip this step — Phase 2 CI will exercise it. If it IS installed, expected output is a valid config dump (not an error).

If clang-format is installed, also try formatting a real file as a smoke test:

```bash
clang-format --style=file --dry-run --Werror /home/jeff/Source/astros/AstrOs.ESP/src/main.cpp 2>&1 | head -5
```

This WILL produce warnings about formatting differences (that's expected — we're not reformatting the existing codebase in Phase 1). The important thing is that the config itself parses and the tool runs without crashing. Non-zero exit is OK for this smoke test.

- [ ] **Step 3: Commit**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git add .clang-format
git commit -m "$(cat <<'EOF'
add .clang-format config for Phase 2 CI check

LLVM base with IndentWidth=4, ColumnLimit=120, PointerAlignment=Left
to match the existing codebase conventions. Phase 1 only lays down
the config file; Phase 2 adds the CI check that enforces it on
changed files (existing untouched files are grandfathered).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 1 acceptance checklist

Run these commands top-to-bottom after all seven tasks are complete. Every step must pass before declaring Phase 1 done.

- [ ] **1. Clean state**

```bash
cd /home/jeff/Source/astros/AstrOs.ESP
git status
```

Expected: `nothing to commit, working tree clean`. (The generated `version_generated.hpp` should NOT appear — it's gitignored.)

- [ ] **2. Native tests pass**

```bash
pio test -e test 2>&1 | tail -20
```

Expected: all tests PASS, no compilation errors.

- [ ] **3. Firmware builds for metro_s3**

```bash
pio run -e metro_s3 2>&1 | tail -5
```

Expected: `[SUCCESS]`.

- [ ] **4. Firmware builds for lolin_d32_pro**

```bash
pio run -e lolin_d32_pro 2>&1 | tail -5
```

Expected: `[SUCCESS]`. This board is deprecated in practice but must still build — it's our insurance that the multi-board scaffolding works.

- [ ] **5. Version string is embedded in the firmware binary**

```bash
strings .pio/build/metro_s3/firmware.elf | grep -E "^1\.0\.0-dev\.[0-9]+$"
```

Expected: one line like `1.0.0-dev.N` where N is the commit count since Task 1's commit.

- [ ] **6. Short git SHA is embedded in the firmware binary**

```bash
strings .pio/build/metro_s3/firmware.elf | grep -E "^[0-9a-f]{7}$" | head -3
```

Expected: at least one 7-character hex string. (Other 7-char hex values may appear incidentally — the important thing is that GitSha isn't empty.)

- [ ] **7. `version_gen.py` is idempotent**

```bash
python3 scripts/version_gen.py
python3 scripts/version_gen.py
```

Expected: first run prints `(updated)` or `(unchanged)`; second run prints `(unchanged)`.

- [ ] **8. Verify on hardware (manual)**

Flash the built `metro_s3` firmware to a real board and:
- Confirm the OLED displays the new version string at startup (no more `v1.0.0-dev2`).
- Monitor serial at 115200 and confirm the log line `I (xxx) AstrOs-esp32: AstrOs.ESP version 1.0.0-dev.<N> (sha: <sha>)` appears during boot.

If on-hardware testing isn't possible right now, mark this step as deferred and note it in `.docs/qa/versioning.md` for later verification (create the QA file if it doesn't exist).

---

## Self-review — issues found and fixed inline

- **Gap**: Initial draft had `extra_scripts` only for ESP envs, not `[env:test]`. Found during writing that `test/test_native/astros_serial_messages_tests.cpp:4` includes `AstrOsUtility.h` which transitively pulls in `AstrOsConstants.hpp`. Fixed: Task 4 now adds `extra_scripts` to all three envs and Task 5 Step 2 verifies via `pio test -e test`.
- **Gap**: `scripts/README.md` wasn't in the original file list but is worth having so a future session discovering `scripts/` isn't confused. Added to Task 2.
- **Idempotence**: `version_gen.py` only writes when content changes, avoiding rebuild churn. Verified via Task 2 Step 6 and the acceptance checklist step 7.
- **Type consistency**: `Version` and `GitSha` are both `constexpr const char *` in the generated header and in `main.cpp`'s use sites. Checked.
- **Scope check**: No CI yet, no workflow YAML, no artifact publishing. All deferred to Phase 2+. Phase 1 is strictly local/build plumbing.
