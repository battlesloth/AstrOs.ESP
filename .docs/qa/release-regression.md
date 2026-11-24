# Release regression

What to run on hardware before cutting a release. The steps live in the feature QA plans; this plan picks which of them to run, on which build, and in what order. Each release gets a record at the end.

## Preconditions

- **An RC.** `develop` is merged to `main`, and `v<version>-RC.N` is published (`.github/workflows/rc-build.yml`).
- **Two builds of the same commit:**
  - **RC build:** the published artifact. Per-command log lines are compiled out, as in the release.
  - **Verbose build:** check out the RC tag and build with `PLATFORMIO_BUILD_FLAGS="-D ASTROS_VERBOSE_COMMANDS"`, flashed over USB. For the cases that read per-command lines.
  - Both report the same version, so the boot line `Verbose command logs: on|off` is what tells them apart. Check it after every flash.
- **Master and at least one padawan,** both consoles captured with host timestamps.
- **AstrOs.Server** at the version that will ship with this firmware.
- **Someone at the droid, with panic within reach,** for any case that moves a servo.

## Choosing the cases

1. List what changed since the last release:

   `git diff --stat <last tag> origin/develop -- src lib lib_native components platformio.ini 'sdkconfig.*' 'partition_*.csv'`
2. Always run blocks A, B, C, E and F below.
3. For block D, add the rows whose area changed:

| Changed | Run |
|---|---|
| `MaestroModule`, `AstrOsServoUtils`, the servo-config parser, the serial-channel tasks | `maestro-servo-release.md`, all cases |
| `AstrOsRandomEffects`, `RandomEffectsController` | `random-span-events.md`, all bench cases |
| Dispatch loop, `AnimationController`, `AstrOsAnimationEngine` | `code-review-phase-c.md` § 2; `random-span-events.md` case 11 |
| `GpioModule` | `random-span-events.md` cases 1 and 11 |
| `I2cModule`, `I2cMaster`, display | `code-review-phase-c.md` §§ 1, 3 |
| `AstrOsEspNow*`, peer list | `espnow-handlers-extraction.md`, `poll-ack-variant.md`, all cases |
| `Ota*` | every `ota-*.md` plan and `master-ota-progress.md` |
| `AstrOsStorageManager` | `code-review-phase-c.md` § 5; `espnow-handlers-extraction.md` case 5 (formats the SD) |
| `sdkconfig`, `platformio.ini`, partitions, ESP-IDF version | everything, on both boards |

## Test cases

The order matters: A needs boards still on the previous release, and F leaves a board broken until it is reflashed.

### A. Upgrade (RC build, first)

1. Flash the previous release over USB to the master and a padawan, on SD cards synced under that release. Confirm both load their Maestro channels.
2. If this release raises the server's minimum firmware version, try a config sync. **Pass:** each board is refused with the reason, and its channels are unchanged.
3. OTA the RC to the padawan and the master in one deploy (`master-ota-progress.md`, happy path — master + padawan). Start from the previous release: an OTA to the version a board already reports proves nothing.
4. **Do not sync yet.** Run the release's upgrade case (for v1.3.0: `maestro-servo-release.md` case 20). **Pass:** every channel loads, homes and releases.
5. Sync. **Pass:** accepted, and every channel still loads.

### B. Smoke (RC build)

1. `poll-ack-variant.md` cases 1–2: version and variant for both boards.
2. `espnow-handlers-extraction.md` cases 1–4, 6 and 7 on the padawan.
3. `code-review-phase-c.md` § 1: the mixed serial + servo + I²C + GPIO script, ten times. **Pass:** no `Send … queue fail`, and the display and I²C devices respond.

### C. Panic (both builds; repeat on every RC)

1. `maestro-servo-release.md` cases 7, 7a, 7b and 7d on the RC build.
2. `random-span-events.md` cases 6 and 9 on the RC build. The per-command checks don't apply there: go by the `Panic:` lines and the hardware.
3. Case 7c and, if random spans are in the release, 6 and 9 again on the verbose build, with the per-command checks.

### D. Changed areas

Cases from the table above. Use the RC build unless a case reads a per-command line (`Setting servo`, `Queueing servo command`, `Sending Command`, `… command val`).

### E. Soak (RC build, overnight)

1. Loop one script that mixes scripted servo, GPIO and random spans, at least 100 runs, with both consoles captured. Use the same script throughout, because the last-loaded script stays in RAM.
2. **Pass:** no reboot, no task-watchdog line, no `Stack HWM` warning, no queue-fail line. (`Hardware Queue Stack HWM` logs at INFO by design; it fails only if the value falls below 500.)
3. **Pass:** idle `RAM left` falls by no more than 16 B per run (the known loss, `PLAN.md` Backlog).

### F. Rollback (last)

1. Take one synced board back to the previous release over OTA.
2. **Pass:** what happens matches the release notes exactly (for v1.3.0: the Maestro channels are gone at boot, and sync is refused).
3. Reflash the RC. **Pass:** the channels come back without a sync.

## Expected results

- **Blocks the release:** any panic failure; a servo left energized with no later release; a lost channel after the upgrade; a reboot, watchdog or stack warning; a `Send … queue fail`.
- **Anything else:** record it, and Jeff decides.
- **After a fix:** cut RC.N+1, then re-run A, C, the cases the fix touches, and the case that failed.

## Edge cases / negative tests

- **A bench board running an older `-dev` build** can pass the server's version gate without having the feature it gates. Flash every board before the first sync.
- **A console logger holds the USB port:** stop it before each flash.
- **Hardware nobody has:** list it under "Not covered" in the record and in the release notes. A build that compiles isn't coverage.

## Release records

### v1.3.0

**Since v1.2.2:** T-007–T-009 (random spans), T-010 (per-channel release), T-011 (verbose logs), T-012 (serial-channel drain).

**Unchanged since v1.2.2** (as of 801adfb; re-check at the RC): `AstrOsEspNow*`, `Ota*`, `AstrOsMessaging`, `AstrOsSerialProtocol`, `AnimationController`, `AstrOsAnimationEngine`, `I2cMaster`, `Pca9685`, `AstrOsDisplay`, `sdkconfig`, `platformio.ini`, partitions. So the full OTA and ESP-NOW plans are not re-run: blocks A and B cover them.

**Server:** AstrOs.Server with T-014, which cases 17–19, 21 and 23 and steps A.2 and F.2 need.

| Block | Cases | Build | Result |
|---|---|---|---|
| A | Upgrade from v1.2.2, case 20 | RC | |
| B | Smoke | RC | |
| C | Maestro 7, 7a, 7b, 7d; random spans 6, 9 | RC | |
| C | Maestro 7c; random spans 6, 9 | verbose | |
| D | Maestro 1, 2, 2b, 6, 9, 11, 22, 23 | RC | |
| D | Maestro 3–5, 10, 12–19, 21 | verbose | |
| D | Random spans 1–5b, 7, 8, 10, 11; `code-review-phase-c.md` § 2 | verbose | |
| E | Overnight soak | RC | |
| F | Rollback to v1.2.2 | RC → v1.2.2 → RC | |

Maestro 17–23 are T-010's joint bench with AstrOs.Server T-014: passing them here closes T-010.

**Not covered** (Jeff, 2026-11-23: none of this hardware is available). The release notes must say so:
- **`lolin_d32_pro`:** builds and ships, but isn't run on hardware.
- **Linear actuators:** T-010's reason for existing. Nearest check: case 17 with Maestro Control Center open, confirming the channel stays enabled until its release time.
- **Kangaroo:** no QA case, and T-012 rewrites the loop that feeds it.

**To confirm before the run:**
- A second Maestro for `maestro-servo-release.md` case 8; otherwise add it to "Not covered".
- A generic-serial device for step B.3. A USB-serial adapter on the channel's TX pin is enough to read the bytes.
