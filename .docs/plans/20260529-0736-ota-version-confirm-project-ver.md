# Fix: OTA version-confirm always times out — wire PROJECT_VER to version_gen.py

## Problem

Master-side OTA version-confirm (`OtaForwarder::checkPeerVersionForCurrentPadawan`,
`lib/OtaForwarder/src/OtaForwarder.cpp:1678`) records SUCCESS only when the
padawan's reported running version equals `expectedNewVersion_`. The master
derives `expectedNewVersion_` by parsing the staged `.bin`'s
`esp_app_desc_t.version` (`OtaForwarder.cpp:726-730`) — i.e. ESP-IDF
`PROJECT_VER`. The padawan reports `AstrOsConstants::Version`
(`lib/AstrOsEspNow/src/AstrOsEspNowService.cpp:1020`), produced by
`scripts/version_gen.py` (e.g. `1.2.0-dev.390`).

`PROJECT_VER` is never wired: `CMakeLists.txt` is a bare `project(AstrOs.ESP)`,
there is no `version.txt`, and `CONFIG_APP_PROJECT_VER_FROM_CONFIG` is unset, so
ESP-IDF falls back to `git describe` (`v1.1.0-RC.1-390-gf7169a6`). The two
strings never match, so every padawan OTA ends in `FAILED(version_unconfirmed)`
after the 15 s `versionConfirmTimer` despite a fully successful flash + reboot +
rollback-cancel.

This realizes an assumption already baked into the PR-set-2 design
(`.docs/plans/20260528-0709-firmware-ota-pr-set-2-design.md`): "master parses
esp_app_desc_t from the staged .bin ... padawan reports its version" — only
correct if both strings come from one source.

## Fix

Have `scripts/version_gen.py` also emit `${PROJECT_DIR}/version.txt` with the
same resolved version string it writes into `version_generated.hpp`. ESP-IDF
auto-reads `version.txt` for `PROJECT_VER` at CMake configure time, and the
pre-build hook runs before configure, so `esp_app_desc.version` becomes
`1.2.0-dev.390`, matching what the padawan announces.

## Tasks

- [x] `scripts/version_gen.py`: add `write_version_txt(version)` (write-if-changed,
      mirroring `write_header`) and call it from `main()`, targeting
      `${PROJECT_DIR}/version.txt`.
- [x] `.gitignore`: ignore `version.txt` (generated, changes every commit, like
      `version_generated.hpp`).
- [x] Verify producer: run `python3 scripts/version_gen.py`; confirm `version.txt`
      == the `Version` value in `version_generated.hpp`.
- [x] Verify no regression: `pio test -e test`.
- [ ] Bench confirm (manual, on-device): next deploy's master log shows
      `Parsed expected new version '1.2.0-dev.390' from staged .bin` and the
      padawan row reaches `VERSION_CONFIRMED` / `status=OK`. Note in
      `.docs/qa/ota-upgrade-pr-set-2.md`.

## Notes / risks

- If PlatformIO's ESP-IDF integration does not auto-pick-up `version.txt`,
  fall back to an explicit `set(PROJECT_VER ...)` in `CMakeLists.txt` that reads
  `version.txt`. Decide from the bench-confirm step.
- `esp_app_desc.version` is capped at 32 chars; `1.2.0-dev.390` (13) fits.
- Version-confirm only proves "moved to new image" when old vs new differ; the
  version embeds the commit count, so a build from a different commit (or bumped
  VERSION) yields a distinct string — consistent with the QA plan.
