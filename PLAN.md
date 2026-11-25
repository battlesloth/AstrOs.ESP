# AstrOs ESP Firmware — Plan

Workflow rules: `CLAUDE.md` (Workflow section). Rationale and templates: `.docs/agentic-workflow.md`. Task IDs here are independent of AstrOs.Server's.

## Status

Active:  Per-channel servo release time (cross-repo with AstrOs.Server) — T-010; plus T-012 (serial-channel drain). Random span events' firmware side is done (T-007–T-009)
Now:     T-012's bench is complete (branch `feature/T-012-serial-channel-drain`, 2026-11-25): option A holds 30 ms after a pre-position frame (Jeff), no jumps in 36 moves; a scripted servo behind on/off spans 310 → 10–20 ms; reviewed, PR next. T-010 merged (PR #66, 2026-11-23), its bench pending; T-011 complete (PR #67). v1.3.0 is held (Jeff, 2026-11-22) for T-012 and the release regression
Next:    T-012 (serial-channel drain) + its bench, in parallel with AstrOs.Server T-016 → T-013 → T-014 → v1.3.0-RC.1 (`develop` → `main`) → release regression (`.docs/qa/release-regression.md`; includes T-010's joint bench) → release v1.3.0. Release notes must cover the setting, the upgrade order (server → flash 1.3.0 → sync) and the rollback hazard (a 1.3.0-synced board rolled back to 1.2.x drops its Maestro channels), and the untested hardware (`lolin_d32_pro`, linear actuators, Kangaroo)
Blocked: v1.3.0 waits on T-012, AstrOs.Server T-014 and the release regression
Last:    2026-11-23 — T-010 merged (PR #66); 2026-11-22 — T-011 complete (PR #67); T-009 complete, bench passed

## Random span events

Spec: AstrOs.Server `.docs/specs/20261022-0748-random-span-events-design.md` · wire format: `.docs/protocol.md` § C (identical in both repos) · server side: AstrOs.Server `PLAN.md` § Random span events (its T-006 database guard, T-007 data, T-008 conversion; then the scripter UI, then a min-firmware bump)

- [x] **T-007** — Parse random span commands (types 6 and 7) (`.docs/tasks/completed/T-007-random-span-command-parsers.md`) — done 2026-10-22
- [x] **T-008** — Random span math and span table (pure) (`.docs/tasks/completed/T-008-random-effects-math.md`) — done 2026-10-23
- [x] **T-009** — Run random spans on the controller (`.docs/tasks/completed/T-009-random-effects-task.md`) — done 2026-11-22 (merged PR #65 2026-10-31; bench 2026-11-22)

## Per-channel servo release time

Spec: AstrOs.Server `.docs/specs/20261122-0847-per-channel-servo-release-design.md` (approved by Jeff, 2026-11-22) · wire format: `.docs/protocol.md` § D (identical in both repos) · server side: AstrOs.Server `PLAN.md` § Per-channel servo release time

**What changes:**
- Each Maestro servo channel releases at max(its own minimum on-time, modeled travel + 10 %). The default is 5 s (today's T-005 behavior), and the server's range is 1–120 s.
- The server sends the value as an 8th channel-entry field, and refuses config sync to firmware below 1.3.0, which drops 8-field entries.

**History:** this is the "per-channel release-time setting" that the 2026-09 Log entries and the T-001/T-005 task files point to. Its Backlog bullet was never written.

- [ ] **T-010** — Release each Maestro servo channel on its own minimum time (`.docs/tasks/T-010-per-channel-release.md`) — merged 2026-11-23 (PR #66); open until the bench, joint with AstrOs.Server T-014

## main.cpp decomposition

Goal (Jeff, 2026-10-30): `main.cpp` keeps `app_main`, `init` and one-line touchpoints; the logic moves into domain libs. Recipe: CLAUDE.md "Extracting a domain from main.cpp". Pilot: `lib/RandomEffectsController` (T-009, PR #65). Unscheduled: each domain is its own task with no behavior change, verified by a diff of the moved bodies with the renames mapped back, both board builds, and the domain's QA cases. Candidates, largest first (line counts as of 2026-10-30, `main.cpp` at 2361 lines):

- [ ] Interface messages: `interfaceResponseQueueTask` and its handlers (`handleRegistrationSync`, `handleSetConfig`, `handleSaveScript`, `handleRunSctipt`, `handleRunCommand`, `handlePanicStop`, `handleFormatSD`, `handleServoTest`), ~500 lines
- [ ] Script dispatch: `animationDispatchTask` (the per-type switch and the serial `\n` / `\r` unescape) and `animationQueueTask`; likely joins `AnimationController`, ~310 lines
- [ ] ESP-NOW glue: `espnowQueueTask`, `pollingTimerCallback`, the send/recv callbacks, `cachePeer`, `updateSeviceConfig`; likely joins `AstrOsEspNow`, ~290 lines
- [ ] Maestro registry: `maestroModules` and its mutex, `loadMaestroConfigs`, `readMaestroChannel`, `servoQueueTask`, `servoShutdownTimerCallback`; panic step 3 and `RandomEffectsCtrl` call into it, ~240 lines
- [ ] Service and button: `serviceQueueTask`, `buttonListenerTask`, ~165 lines
- [ ] Serial channels: `serialCh1QueueTask`, `serialCh2QueueTask`, `astrosRxTask`, ~150 lines
- [ ] Thin consumer loops: `i2cQueueTask`, `gpioQueueTask` and the three OTA task loops move into their libs' `TaskEntry`, ~125 lines

## Standalone tasks

- [x] **T-001** — Fix CheckServos release math so scripted moves de-energize servos (`.docs/tasks/completed/T-001-checkservos-release-timeout.md`) — done 2026-09-06
- [x] **T-002** — Move Maestro channel state into MaestroModule instances (`.docs/tasks/completed/T-002-maestro-channels-per-instance.md`) — done 2026-09-06
- [x] **T-003** — Synchronize Maestro channel state between timer and command paths (`.docs/tasks/completed/T-003-maestro-channel-state-sync.md`) — done 2026-09-06
- [x] **T-004** — Make panic stop de-energize every configured Maestro channel (`.docs/tasks/completed/T-004-panic-stop-maestro-deenergize.md`) — done 2026-09-07 (servo channels only: panic is a stop, GPIO holds)
- [x] **T-005** — Servo release deadline: 5 s floor or estimated travel + 10 %, whichever is greater (`.docs/tasks/completed/T-005-release-floor-5s-margin.md`) — done 2026-09-07
- [x] **T-006** — Re-energize released Maestro servos at their last commanded position (`.docs/tasks/completed/T-006-maestro-reenergize-at-last-position.md`) — done 2026-09-14
- [x] **T-011** — Log per-command lines only in verbose bench builds (`.docs/tasks/completed/T-011-verbose-command-logs.md`) — done 2026-11-22 (merged PR #67; bench 2026-11-22)
- [ ] **T-012** — Send every queued frame per tick in the serial-channel tasks (`.docs/tasks/T-012-serial-channel-drain.md`) — before v1.3.0 (Jeff, 2026-11-22; found by the T-011 bench)
- [ ] **v1.3.0 gate** — Release regression on an RC build (`.docs/qa/release-regression.md` § v1.3.0), after T-012 and AstrOs.Server T-014 (Jeff, 2026-11-23)

## Backlog (unscheduled candidates)

- Known-fragile catalog: `.docs/code-review/code-review.md` (P0–P3) — headline items also listed in CLAUDE.md "Known-fragile areas" (timer-callback leaks/stack pressure, `peers` vector mutex, `AnimationController` unlocked reads, `setKeyId` peer-index assumption, cross-core globals). Promote individually as tasks.
- Queue consumers that fail to `free()` embedded pointers (same review catalog) — sweepable as one task or per-consumer.
- Upgrade to espressif32 7.x / ESP-IDF 6.x deliberately: migrate both `sdkconfig.<env>` files (IDF 6.1 kconfgen crashes on the committed IDF-5-era files — the 2026-08-31 CI outage), then lift the `espressif32@6.13.0` pin in `platformio.ini`. Own task; touches both boards.
- **Maestro `lastPos` is an estimate.** T-006 re-energizes a released servo at its last *commanded* target; a servo moved by hand or by load while off jumps back there before the smooth travel. Fix, if it ever bites: read the position back (`GET_SERVO_POSITION 0x90`) — needs the Maestro RX path. Found 2026-09-14. A panic does it too: the servo stops partway, and the next move snaps to the interrupted move's target before it travels (seen on the T-012 bench, 2026-11-24).
- **Panic stop does not reach PCA9685 (I²C) servo channels.** T-004 covers the Maestro; the I²C servo path has the same gap. Found 2026-09-06.
- **Panic relay latency to padawans.** The master's `SEND_PANIC_STOP` relays are queue entries behind its own `PANIC_STOP` on `interfaceResponseQueueTask`, so when the server lists the master's record first (`generatePanicStop` iterates `runCommand.configs` in whatever order they come) padawans wait for the master's Maestro offs (~150 ms per module healthy; worst case summing every bound — 5 s animation mutex + 1 s map mutex + 2.2 s send take + 14 × 500 ms queue waits — ≈ 15 s for one module, ≈ 24 s for two; measured healthy: relay out 30 ms after panic start). Candidate fix: `handlePanicStop` drains the queue and posts a service command; `serviceQueueTask` runs the per-module `Panic()` calls — that also serializes panic against `RELOAD_CONFIG` on the same task, removing the `LoadConfig`-holds-`stateMutex` collision. Decide after QA 7a measures the delay. Found 2026-09-06 (T-004 review).
- **Panic ordering hardening (future version).** Decided 2026-09-07 to document rather than fix in T-004 — edge cases needing a second operator action or mutex contention to overlap the panic within ~1 s, and in every case the servos still release on their normal deadline: (1) a config reload in progress homes every channel after the offs; (2) panic is not serialized with module initialization (may snapshot a module before its `LoadConfig`, or send offs before its homing); (3) `AnimationCtrl.panicStop()` can wait up to 5 s on `animationMutex` before the drain and offs; (4) panic skips a servo a reload disabled while it was energized — T-010's `CheckServos` reload release sends its off about one tick after the panic releases the send mutex, not on a normal deadline. Candidate fixes: a reload barrier mutex held across all of `loadMaestroConfigs` and around the panic snapshot + offs (not the service task — `FORMAT_SD` runs there and would block a kill); a split `haltDispatch()` so the order becomes halt → drain → offs → bounded queue clear. Related and unfixable at this layer: a controller that crashes and restarts during a panic does not know it was in panic. Not industrial control software; revisit if a real incident ever traces to one of these.
- **`PANIC_STOP` has no ACK/NAK.** The server never learns whether the kill reached hardware; `Panic()` returns `void` so `handlePanicStop` cannot aggregate either. Needs a wire-format contract pinned in both repos (AstrOs.Server + AstrOs.ESP) before implementing. Found 2026-09-06 (T-004 review).
- **`MaestroModule` hygiene** (all pre-existing, found by T-002 review 2026-09-06; mutex leak and half-built-object items fixed in T-003): `LoadConfig` ignores `loadMaestroServos`'s return and copies the parsed file over entries 0..maxId only, so entries above the file's highest id (and every entry when the file is missing/unparseable) keep stale state on reload (fix belongs with T-003's lock — clearing opens a window against the timer); `QueueCommand` lacks the `loading` gate `SetServoPosition` has and treats a pre-config channel as GPIO; neither of them checks `enabled`, so a script can drive a disabled channel, and a disabled digital one then stays driven (found 2026-11-23, PR #66 review); removing a whole module from the config destroys it without sending offs, so a servo energized at that moment stays powered (deleting and re-adding a module does this, because the server's `idx` is autoincrement) — T-010's reload release only covers modules that stay (found 2026-11-23, PR #66 review; Jeff: an edge case with no real use, not scheduled); the hard-coded `24`s (now more, `Panic` adds five) want one named channel-count constant; `UpdateConfig` still spins unbounded on the send mutex (T-003 bounded the command paths only — switch it to `takeSendMutex()` and decide what a dropped config update should do).
- **Existing per-type parsers use unchecked `std::stoi`** (`MaestroCommand.cpp`, `GpioCommand.cpp`, `SerialCommand.cpp`, `I2cCommand.cpp`). With exceptions disabled, a non-numeric field aborts and reboots the controller. An older AstrOs.Server reading a newer DB could send `position = undefined` this way; the server's T-006 guard covers future builds only. Harden to `strtol` like T-007's parsers. Found 2026-10-22 (random span events design).
- **`GpioModule::SendCommand` bounds check is off by one** (`command.channel > gpioChannels.size()`, ~line 78): `channel == size` passes and `.at()` throws. Quick fix on its own branch. Found 2026-10-22.
- **`.docs/protocol.md` copies have drifted between repos** (OTA sections: Server has master self-flash `FW_PROGRESS` rows; ESP has `OTA_FLASH_RESULT`). § C was added identically on 2026-10-22; reconcile the rest.
- **The master holds every server command for 1 s.**
  - Cause: `astrosRxTask` calls `uart_read_bytes(ASTRO_PORT, data, RX_BUF_SIZE = 1024, 1000 ms)` (`src/main.cpp` ~line 1158). ESP-IDF returns only when it has all 1024 bytes or one receive waits the full timeout, so a short command is handed over 1 s after its last byte.
  - Measured 2026-11-22: HTTP panic → master 1.09 s, then → padawan +9 ms; deploy and run ~1.2 s. Commands sent within the same second arrive in one read.
  - This is most of the kill switch's latency: the panic handler itself takes ~10 ms.
  - Fix candidates: a short timeout (e.g. 20 ms), or the UART event queue / pattern detect on the command terminator. Keep the 8 KB command-buffer assembly, which already joins partial reads.
  - Re-measure QA `maestro-servo-release.md` case 7 after.
  - Found 2026-11-22 (T-009 bench).
- ~~**Per-command INFO logging slows the queue tasks.**~~ → T-011 (Jeff, 2026-11-22: behind a `-D ASTROS_VERBOSE_COMMANDS` bench flag).
  - Under QA case 11's load (five spans at 20–40 ms gaps) the console ran at ~81 % of 115200 baud (95 % in bursts). The queue tasks drained one message per ~20 ms (GPIO) and ~40 ms (servo) instead of the ~10 ms tick: `Queueing servo command` + `Setting servo …` alone are ~240 B (~21 ms) per servo command.
  - Effect: a scripted servo command waits up to ~400 ms behind the random half of `servoQueue`, and the dispatcher's own INFO lines slow same-time command bursts.
  - Candidates: DEBUG level for the per-command lines (`Setting servo`, `Queueing servo command`, `Sending Command`, the dispatch `… command val`), or a faster console baud.
  - When that lands, fix the "drain about one message per 10 ms tick" comment at `lib_native/AstrOsRandomEffects/include/RandomSpanAdmission.hpp:133` and re-measure QA case 11's bounds.
  - Found 2026-11-22 (T-009 bench).
- **Same-time commands shift the rest of the script.**
  - The dispatch loop sleeps `max(delay, MIN_WAKE_MS = 10)` measured from when the previous command finished (`vTaskDelay`, `src/main.cpp` ~line 854; the relative timing is deliberate there). So N commands at one time push everything after them back by at least 10·N ms plus their processing: +290 ms after five span starts with a busy console (QA case 11). The offset is fixed and doesn't grow.
  - Fixing it needs absolute scheduling from the script start, without catch-up bursts. That is a design decision.
  - Pre-existing; found 2026-11-22 (T-009 bench).
- **The master heap loses 16 B per script run.**
  - Seven runs across three scripts each stepped idle `RAM left` down 16 B, spans or not; a GPIO-only control did the same. Idle otherwise holds flat (45 min overnight). ~16 KB per 1000 runs.
  - Suspects: the `RUN_SCRIPT` handling, `AnimationController`'s load, the ESP-NOW run relay.
  - Measure between runs of the same script: the last-loaded script stays resident, so switching scripts moves the baseline by their size difference.
  - Pre-existing; found 2026-11-22 (T-009 bench).

- **From the T-010 / T-011 reviews (2026-11-22)**, all pre-existing except where noted:
  - **SD save and read don't check their errors.** `saveFileSd` ignores `fwrite`/`fclose`, and `readFileSd` never checks `ferror`. A short write still logs "Saved"; a read error can truncate a config so its last entries silently vanish.
  - **`CheckServos` isn't native-testable.** A pure `ServoReleaseDue(channel, msSinceLastCheck)` helper would pin which channel's release time it uses (today only bench case 18 does).
  - **No CI guard keeps per-command INFO lines out of the hot path.** T-011's acceptance grep could become a `pr-validation.yml` step.
  - **INFO lines left on frequent paths:**
    - `NONE command queued, assume buffer?` (per NONE command)
    - `AstrOs RX: Read N bytes` (per server message)
    - `i2cQueueTask` silently drops a `message_id` other than 0 or 1.
  - **The serial channels share one mutex** (found 2026-11-23, T-012): `serialMutex` in `SerialModule.cpp` is file-static and the second `Init` leaks the first. A channel waiting for FIFO space or for a speed change holds off the other: up to ten messages since T-012, ~0.35 s if they alternate speeds. Make it a member.
  - **`ASTROS_INTERFACE_MESSAGE` queues the wrong struct** (found 2026-11-23, T-012): a `queue_msg_t` to `serialCh1Queue`, whose items are `queue_serial_msg_t`, so `xQueueSend` reads past it and the fields shift. Nothing posts the command today; fix or delete it.
  - **Fewer serial messages per change.** Coalescing a change's frames into one message, or skipping an unchanged speed/accel frame, was the next step if T-012's bench needed it. For on/off outputs it doesn't: a scripted servo waits 100–110 ms behind a full queue of them, and the limit is now `servoQueueTask`, one change per 10 ms tick. Servo changes still take ~40 ms each (the 30 ms hold after the pre-position frame), so ten random servo changes hold a scripted command ~400 ms, as before T-012. What would help there: skip the pre-position frame and its hold when the servo is already on.

Cross-repo: measured 2026-09-07 — ~1.0 s (±30 ms) between an HTTP call and the master receiving it, for `scripts/run` and `panicStop` alike. The 2026-09 note blamed the server's serial pipeline. The 2026-11-22 bench found the master's own serial read holds each command 1 s (Backlog above), which covers all but ~0.1 s of it, so it is firmware. AstrOs.Server's `PLAN.md` Backlog holds the server-side serial findings from the 2026-08-06 bench log (e.g., server discards master-emitted POLL_NAK). Wire-format changes, if any come out of that, pin their contract in both repos first.

## Completed projects

- **OTA upgrade pipeline** (2026-04 → 2026-08) — padawan + master OTA over ESP-NOW/serial, recovery via USB, receiver watchdog, master self-flash (stack overflow fixed in PR #47), progress reporting (PR #49). Shipped in rel_1.2. Plans archive: `.docs/completed-plans/`.

## Log

- 2026-11-22 T-011 complete — per-command log lines only in verbose bench builds (PR #67 → develop)
  - `ASTROS_LOG_CMD` (`lib/AstrOsUtility_ESP/include/AstrOsCommandLog.h`): ten per-command lines are INFO with `-D ASTROS_VERBOSE_COMMANDS` and compiled out otherwise; a boot line says which build it is; failure lines name their command (both Jeff's amendments)
  - bench (master, case 11): the verbose build reproduces the T-009 numbers; flag-off, scripted GPIO 10–20 ms (verbose 70), scripted servo 310 ms (verbose 410), same-time offset +40–50 ms (verbose +230–270)
  - the servo gate (≤ 250 ms) failed at 310 ms in every run, and Jeff closed the task on the measured numbers. The rest isn't logging: the serial-channel tasks send one Maestro frame per 10 ms tick → T-012, before v1.3.0
  - verification re-run 2026-11-23 on develop 7b42508 (T-010 + T-011): 631/631; both boards build flag-on and flag-off; the nine per-command strings are in each ELF flag-on and absent flag-off

- 2026-11-22 T-009 complete — random spans run on the controller (PR #65 → develop 2026-10-31; bench 2026-11-22)
  - bench: develop 71e556a on the master (body) and the core padawan; AstrOs.Server develop with spans inserted by SQL; both consoles captured with host timestamps; cases 9 and 11 driven by `curl` for timing. Cases 1–11 pass; results table in the task file
  - case 9 ran log-only on a padawan with no hardware: a Maestro configured with nothing attached keeps state and logs the same. The panic relay adds 9 ms; the master's serial read adds 1 s
  - case 11: a scripted servo command waited 420 ms behind the random half of `servoQueue` (the plan said ~300 ms). The queue tasks drain ~20 / 40 ms per message under load, not one per 10 ms tick. Jeff: pass, with the QA bound corrected to cap × measured drain
  - verification re-run on develop: 611/611, both boards build
  - found (Backlog, all pre-existing): the master holds every server command 1 s (`uart_read_bytes`), which corrects the 2026-09 "server serial pipeline" note; per-command INFO logging throttles the queue tasks; same-time commands shift the rest of a script; 16 B of heap lost per script run
  - open: the GPIO gap mean read 202 ms against 225 expected on the master (n = 130) but 225 on the padawan, so run a 5-minute GPIO span on the next bench; the padawan's first boot after flashing hit one SD mount timeout (0x102); the same-start-burst fence is reachable only in native tests through the server (the 1 s read batches run and panic)

- 2026-10-23 T-008 complete — random span math and span table, pure (PR #63 → develop)
  - PURE lib `AstrOsRandomEffects` with an injected `Rand32`: `UniformInt`, `NextGapMs`, `NextServoTarget` (§ C move rule: forced up near min, down near max, size capped by room and `maxMove`, clamped), `MoveTravelMs`, and `RandomSpanTable` (fixed capacity, same-key replace, `[[nodiscard]] Start` re-checks the gap rules, 20 ms end guard, wrap-safe `uint32` times, not copyable)
  - contract amended four times: `AstrOsServoUtils.hpp` split into `.hpp` + `.cpp` (non-`inline` header functions broke the link once a second file included it) plus `PercentFromPosition`; distance-aware `MoveTravelMs` (the Maestro speed/accel model at the move's distance, equal to `WorstCaseTravelMs` at 3000 µs; the linear scale underestimated short accelerated moves ~4×); `[[nodiscard]] Start`; move distance is floor + 1 µs (PR #63 review: `GetRelativeRequestedPosition` truncates each endpoint, so floor and ceil both fell short)
  - native 566/566, both boards; 86 mutations, all caught (the "which parity means up" flip is equivalent by design)
  - declined (PR #63): a span's first move starts from a rounded percent, so its travel estimate can be up to 0.5 % of the channel range short (≤ 10 µs measured); exact needs a `SpanStart` field
  - cross-repo: T-009's plan amended on develop (tick round-up on the wait, one table pass per lock with one `now`, reject servos with no usable range, reject spans ≤ the end guard, separate "table full" / "gap rules" logs, `HomeServos` records digital `lastPos`, bench cases 5b and 10); AstrOs.Server spec's travel estimate is distance-aware (f5399eea)
  - open: no release with T-007 + T-008 alone — nothing dispatches span commands until T-009

- 2026-10-22 T-007 complete — parse random span commands, types 6 and 7 (PR #62 → develop)
  - `MODULE_TYPE` `MAESTRO_RANDOM = 6` / `GPIO_RANDOM = 7`; PURE `RandomCommands` parsers return `{ error, cmd }`: exact field count with the type field first, strict base-10 ints via `strtoll` bounded to `int` (no `std::stoi`), indexes ≥ 0, § C gap rules; on error `cmd` names no target (module/channel −1)
  - `RandomServoFieldsValid` (self-contained: re-checks target + gaps, then the servo-only rules) and `RandomParseErrorName` for T-009's log lines
  - native 515/515, both boards; 23 mutations — 22 fail a native test, the 23rd (narrowing back to 32-bit `strtol`) fails the board build via `static_assert`
  - review-driven cross-repo changes: AstrOs.Server T-008 contract amended (round span/gaps to integer ms; skip spans with a negative index), § C updated in both repos; ESP T-008/T-009 plan amendments (typed queue message, panic/reload drain, table re-checks gaps)
  - open: firmware still skips types 6/7 (keeping the delta) until T-009 — no release with T-007 alone

- 2026-09-14 release v1.2.2 — patch on `release/rel_1.2`
  - `develop` merged into `release/rel_1.2` (clean; tree identical to develop), `VERSION` set to 1.2.2 on the release branch, pushed → release workflow tagged v1.2.2 and published both boards (run 34920852402, six artifacts)
  - contents: T-006 only (released servos re-energize at their last commanded target; quarter-µs encoding, `lastPos` recorded after every move, no pre-position frame on GPIO); no new features, so a patch
  - `main` untouched: it is the RC stream for the next minor (VERSION 1.3.0 on develop)

- 2026-09-14 T-006 complete — re-energize released Maestro servos at their last commanded position (PR #60 → develop)
  - diagnosis: the pre-position frame `setServoPosition` sends before speed/accel went out in raw µs where the Maestro expects quarter-µs (a 1500 home = 375 µs on the wire → slam to the low end), and `lastPos` was only ever written by config load / `HomeServos`; a released servo jumps straight to its first target, so both showed as an instant jump — full-range for inverted 0 → 100 and non-inverted 100 → 0
  - `EncodeMaestroTarget` (PURE, `AstrOsServoUtils.hpp`, native-tested) shared by both `SET_TARGET` frames; `recordLastPos` after a complete send on the script and slider paths (success-path twin of `reconcileLimits`); GPIO-type channels get no pre-position frame (it blipped them LOW before every level); `lastPos:` appended to the `Setting servo` line
  - native 495/495; both boards clean; bench cases 12–16 (`.docs/qa/maestro-servo-release.md`) passed 2026-09-14
  - open: `lastPos` is the last *commanded* target — a servo moved by hand while released still jumps back to it first; reading the Maestro position back (0x90) is the fix if it ever matters (Backlog)

- 2026-09-07 regression scripts on the droid's Pi instance (192.168.40.76)
  - three QA scripts created via `PUT /api/scripts/` (release race, panic, deadlines — ids in `.docs/qa/maestro-servo-release.md`); deployed and run through the Pi with the master console captured over USB: all pass, matching the dev-bench numbers. HTTP→board latency is ~1.0 s on the Pi too, so it is the server's serial pipeline, not the host

- 2026-09-07 release v1.2.1 — patch on `release/rel_1.2`
  - `develop` merged into `release/rel_1.2` (clean; tree identical to develop), `VERSION` set to 1.2.1 on the release branch, pushed → release workflow tags v1.2.1 and publishes both boards
  - contents: T-001..T-005 (release math, per-instance channel state, timer/command locking, panic → Maestro, 5 s floor + 10 %), CI espressif32 pin; no new features, so a patch rather than 1.3.0
  - `main` untouched: it is the RC stream for the next minor (VERSION 1.3.0 on develop)

- 2026-09-07 T-005 complete — release deadline = max(5 s floor, travel + 10 %) (PR #59 → develop)
  - `MAESTRO_RELEASE_FLOOR_MS` 20 s → 5 s; new `MAESTRO_RELEASE_MARGIN_PERCENT` 10; `WorstCaseTravelMs` unchanged
  - bench via the server API: boot releases ~5 s after homing; `T-005 QA` deadline-race script (5.0–5.3 s periods) 40/40, gaps 4.82–5.11 s, 0 stale; margin cases speed 11 → 12.13 s, speed 5 → 26.25 s, speed 5/accel 1 → 26.89 s
  - open (human-gated): linear actuators must finish their stroke inside 5 s at full speed — else promote the per-channel release setting

- 2026-09-07 T-004 complete — panic stop reaches the Maestro (PR #58 → develop)
  - `Panic()` rewritten as a T-003 command-path operation: per-channel 0x84 target-0 for enabled *servo* channels, tracking cleared only after the off was queued; GPIO-type channels held (panic is a stop, not a reset — driving a GPIO anywhere could itself move something). The old 0x9F frame was malformed and never sent
  - `handlePanicStop`: halt → drain `servoQueue` → snapshot modules (1 s, ERROR on miss) → `Panic()` each → second drain for a late-landing command
  - bench via the server API with both consoles captured: 9 panics, 0 failed offs, no release after, 0 WARN/ERROR, recovery normal; servos observed stopping; relay to padawan 30–50 ms; server serial pipeline adds ~1.0 s HTTP→board (cross-repo note)
  - accepted as documented limitations (Backlog "panic ordering hardening"): reload/init overlap, animation-mutex wait; fresh-context review added the GPIO residual and inverted-GPIO points that led to the servo-only decision
  - open: relay physically holding through a panic not yet observed (QA 7b); padawan Maestro case not coverable on this bench

- 2026-09-06 T-003 complete — Maestro channel-state synchronization (PR #57 → develop)
  - per-instance `stateMutex`; command paths hold the send mutex once per operation (state under `stateMutex`, frames enqueued with it released); bounded `takeSendMutex()` (20 × 100 ms, then error, no state touched); `CheckServos` decides and enqueues under `stateMutex` with zero-wait takes only and logs after releasing it
  - review-driven: stop at the first dropped frame and reconcile tracked speed/accel to the stage reached; `enqueueFrame` never logs; `IsValid()` so a module whose mutexes failed is dropped by `loadMaestroConfigs`; destructor deletes both semaphores
  - bench: `T-003 QA` script (server DB `s1788696QAq`) with the master console captured over USB Serial/JTAG — 89 moves, 22 releases, gaps 19.81–20.04 s, 0 stale releases, 0 WARN/ERROR; release window is deadline −300 ms..+1 tick (first tick counts as a full 300 ms)
  - QA plan cases 9–11 + reload / dropped-frame / wedged-serial edge cases; task file → `.docs/tasks/completed/`

- 2026-09-06 T-002 complete — Maestro channel state per instance (PR #56 → develop)
  - `channels[24]` is a private zero-initialized member of `MaestroModule`; copy ops deleted; Maestro log lines carry `on module M`
  - review found `MaestroModule::Panic()` has no caller and its 0x9F frame is malformed → T-004 authored; five Copilot rounds refined the T-003/T-004 designs (per-operation send mutex, zero-wait timer takes, bounded send-mutex take, panic clears state only after a successful enqueue)
  - QA plan: two-module case (padawan only), panic case documents current behavior; task file → `.docs/tasks/completed/`

- 2026-09-06 T-001 complete — CheckServos release math (PRs #52, #54, #55 → develop)
  - `CheckServos` accumulates integer ms per channel; deadline = max(physical trapezoid/triangle model of the Maestro speed + accel units, 20 s floor). No multiplier — the 3000 µs guard range vs ≤2000 µs real sweep is the margin
  - slider path (`SetServoPosition`) now arms release tracking (was never turned off after a release) and validates the channel as the parsed `int` before narrowing to the wire byte
  - bench: speed 20 / accel 2 releases ~20 s (floor), speed 5 / accel 1 ~24.7 s (model), slider re-arm on a released channel; QA plan `.docs/qa/maestro-servo-release.md`
  - declined: measured timer delta in `servoShutdownTimerCallback` (constant 300 ms undercounts → late release only, accepted)

- 2026-09-05 T-001 amendment (pre-PR review)
  - the accel-for-speed substitution carried over from the old code was a dimensional error: Maestro accel is speed-units per 80 ms, not a speed cap. Accel 2 reaches speed 25 in 1 s; modeling it as speed 2 made a speed-20/accel-2 move (physically ~6.8 s) wait 240 s
  - replaced with the physical trapezoid/triangle model (`WorstCaseTravelMs`) and added `ServoReleaseDeadlineMs` = max(model × slack, 20 s floor). The floor keeps full-speed release at ~20 s (old broken math gave ~19 s, which the linear actuators were living with; first-pass T-001 had cut it to 1.9 s)
  - per-channel release-time setting (absolute ms, floor semantics) added to Backlog; removes the global floor when it lands
  - second pass: dropped the ×4 slack multiplier inherited from the old `/ 4`. Error sources are additive and sub-second; the 3000 vs ≤2000 µs guard range already gives 1.5×; the floor covers everything at speed ≥ 10. Deadline = max(model, 20 s)
  - PR #52 review (Copilot) found the slider path (`SetServoPosition`) never armed release tracking — pre-existing; a slider move on a released channel was never turned off. Fixed by arming with the script-path block (bounds-checked, no INFO log so drags don't spam). Second review item — timer callback passes a constant 300 ms rather than a measured delta, so the accumulator undercounts (late release only) — declined: late release is acceptable, constant stays
  - PR #52 merged 2026-09-06 (8b54d77) at 55c3128, before the slider fix. Follow-up PR #54 from the same branch carries slider arming + a review fix: `SetServoPosition` validated after narrowing to `uint8_t`, so channel 256 wrapped to 0 and passed — now validates the parsed `int` first

- 2026-08-31 servo-release investigation
  - bench symptom: servos stay energized after scripted moves. Root cause: `CheckServos` accumulates a fractional double into the int `currentPos` — increments < 1 (effective speed ≤ 5, which includes any scripted accel 1–5 via the accel-substitution clamp) truncate to zero, so the Maestro off command never fires; modeled rate is also ~40× slower than physical travel
  - authored T-001 (fix math via pure `lib_native` helper + native tests), T-002 (de-globalize `channels[24]`), T-003 (timer-vs-command-path locking); noted stale-`lastPos` re-arm issue in Backlog

- 2026-08-16 workflow bootstrap
  - adopted the task-file workflow (`.docs/agentic-workflow.md`), ported from AstrOs.Server; `PLAN.md` is now the authoritative status view — agent memory is a cache
  - `.docs/plans/` retired for new work in favor of `.docs/tasks/`; historical plans remain in `.docs/completed-plans/`
