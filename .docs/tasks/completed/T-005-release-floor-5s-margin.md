# T-005: Servo release deadline — 5 s floor or estimated travel + 10 %, whichever is greater

<!-- File: .docs/tasks/completed/T-005-release-floor-5s-margin.md (completed 2026-09-07; PR #59). Branch: feature/T-005-release-floor-5s-margin.
     PR title: "T-005: Servo release deadline — 5 s floor or estimated travel + 10 %, whichever is greater". -->

## Context

T-001 set the Maestro release deadline to `max(WorstCaseTravelMs, 20 s)`. The 20 s floor was
chosen to match what the old (broken) math had been doing at full speed (~19 s), because the
linear actuators had been living with that, and it doubled as the model's only margin after the
slack multiplier was dropped. Bench runs since (T-003/T-004, 2026-09-06/07) show the physical
model tracks the servos closely and every release lands 19.8–20.1 s after the last move, i.e.
the floor dominates for every speed ≥ 10.

Decision 2026-09-07 (Jeff): shorten the hold. Deadline = **the greater of a 5 s floor and the
estimated travel time plus 10 %**. The 10 % keeps a proportional margin on slow moves (the
guard range already gives 1.5× on distance); the 5 s floor covers full-speed moves and
fast-reacting loads. A linear actuator whose stroke takes longer than 5 s at full speed will
now be cut mid-stroke; the per-channel release-time setting (PLAN.md Backlog) is the fix for
that if it bites.

## Contract (pinned — do not change)

- Maestro wire protocol unchanged; `CheckServos` call site unchanged (`ServoReleaseDeadlineMs`
  is the only thing that changes).
- PURE-lib purity: `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp`, no ESP-IDF includes.
- `WorstCaseTravelMs` (the physical model) unchanged; the margin and floor live in
  `ServoReleaseDeadlineMs` only, as named constants.
- Integer arithmetic; ceiling on the margin so the deadline never rounds below travel + 10 %.

## Task

1. `MAESTRO_RELEASE_FLOOR_MS` 20 000 → 5 000. Add `MAESTRO_RELEASE_MARGIN_PERCENT = 10`.
2. `ServoReleaseDeadlineMs(speed, accel)` = `max(travel + ceil(travel × 10 / 100), 5000)` where
   `travel = WorstCaseTravelMs(speed, accel)`.
3. Update the comments on both constants and the function; update the QA plan's reference table
   and every "~20 s" expectation in `.docs/qa/maestro-servo-release.md` (cases 1–11, T-003/T-004
   notes).
4. Bench script: the `T-003 QA` script in the server DB probes the release tick at 20.0–20.3 s
   periods; add a `T-005 QA` script with the same shape at 5.0–5.3 s periods (deadline-race case)
   — the old one stays for the record.

## Acceptance criteria

- [x] Native tests: floor cases (0/0, 20/2, 10/2 → 5000), margin cases (5/1 → 26 840,
      5/0 → 26 400, 1/0 → 132 000, 20/0 → 6 600), and the crossover (a travel just under
      4 546 ms floors; just over exceeds 5 000).
- [x] `pio test -e test` green; both boards build clean; clang-format clean.
- [x] QA plan reference table and case expectations updated; `T-005 QA` script in the server DB.
- [x] Bench (2026-09-07, scripted via the server API, master console captured): boot homing
      released all 7 servos at 6.3 s uptime (~5 s after homing); `T-005 QA` deadline-race
      script — 40 moves, 40 releases, gaps 4.82–5.11 s after each move (window 4.7–5.1 s),
      0 stale releases, 0 WARN/ERROR; `T-001 QA` script — speed 11 released at 12.13 s
      (deadline 12.0 s), speed 5 at 26.25 s (26.4 s), speed 5 / accel 1 at 26.89 s (26.84 s).
- [ ] Bench (human-gated): linear actuators complete their stroke inside 5 s at full speed — if
      not, promote the per-channel release setting.

## Out of scope

- Per-channel release-time setting — PLAN.md Backlog.
- The 300 ms tick quantization (release lands −300 ms..+1 tick around the deadline) — accepted
  in T-001.

## Verification

```bash
pio test -e test
pio run -e lolin_d32_pro
pio run -e metro_s3
# bench: boot → each servo releases ~5.3 s after homing; slider move → ~5.3 s;
# speed 5 / accel 1 script → ~27 s; T-005 QA script → no release within its deadline after a move.
```

## Implementation checklist

- [x] RED: deadline tests rewritten to the new values, observed failing
- [x] GREEN: constants + formula; comments updated
- [x] `pio test -e test` green; both boards build clean; clang-format clean
- [x] QA plan updated (table + cases); `T-005 QA` script created in the server DB (`s1788789HJY`)
- [x] PLAN.md Status updated
- [x] bench (2026-09-07): floor, deadline-race, and margin cases passed via the server API
      (see acceptance); linear-actuator stroke check remains human-gated
