# T-007: Parse random span commands (types 6 and 7)

<!-- File: .docs/tasks/completed/T-007-random-span-command-parsers.md (completed 2026-10-22; PR #62). Branch: feature/T-007-random-span-command-parsers. PR title: "T-007: Parse random span commands (types 6 and 7)". -->

## Context

This is the first firmware task of the random span events project. See `PLAN.md` § Random span events, and the spec in AstrOs.Server, `.docs/specs/20261022-0748-random-span-events-design.md`.

A random span is one script command sent at the span's start. The ESP then improvises GPIO toggles or servo moves until the span ends. This task is the wire-format seam: enum values and pure parsers, with native tests. Nothing dispatches the new types yet (T-009). Firmware without T-009 skips them and keeps their delta, which is today's `default:` behavior.

**The wire format is pinned in `.docs/protocol.md` § C.** It is identical in both repos. The server side is AstrOs.Server T-008.

## Contract (pinned — do not change)

- **`.docs/protocol.md` § C, exactly:**
  - `6|delta|maestroIdx|channel|spanMs|minGapMs|maxGapMs|minPos|maxPos|minMove|maxMove|speed|accel` (13 fields)
  - `7|delta|gpioChannel|spanMs|minGapMs|maxGapMs` (6 fields)
  - All fields are base-10 integers.
- **`MODULE_TYPE`** (`lib_native/AstrOsUtility/src/AstrOsEnums.h`) **appends** `MAESTRO_RANDOM = 6, GPIO_RANDOM = 7`. `NONE`…`GPIO` are unchanged.
- **PURE-lib purity:** no ESP-IDF or FreeRTOS includes in `lib_native/`.
- **Number parsing:** no `std::stoi` in new code (exceptions are disabled). Use `strtoll` with a full-consumption `endptr` check and an `int` range check. *(Amended 2026-10-22 with Jeff's approval: originally `strtol` plus `errno == ERANGE`. `long` is 32-bit on the ESP32, so that overflow check could only run on the device. `long long` is 64-bit everywhere, so the int bounds reject overflow identically on the device and the native test host, and saturation past `long long` is rejected by the same bounds.)*

## Task

New `lib_native/AstrOsAnimationCommands/include/RandomCommands.hpp` and `src/RandomCommands.cpp`. These are free functions with rich results, per CLAUDE.md "prefer rich return values".

```cpp
enum class RandomParseError { NONE, FIELD_COUNT, NOT_A_NUMBER, OUT_OF_RANGE };

struct RandomMaestroCommand {
    int module; int channel; int spanMs; int minGapMs; int maxGapMs;
    int minPos; int maxPos; int minMove; int maxMove; /* 0 = no max */ int speed; int acceleration;
};
struct RandomGpioCommand { int channel; int spanMs; int minGapMs; int maxGapMs; };

struct RandomMaestroParseResult { RandomParseError error; RandomMaestroCommand cmd; };
struct RandomGpioParseResult { RandomParseError error; RandomGpioCommand cmd; };

RandomMaestroParseResult ParseRandomMaestroCommand(const std::string &val);
RandomGpioParseResult ParseRandomGpioCommand(const std::string &val);

// Servo-only rules; T-009 calls this once it knows the channel is a servo.
bool RandomServoFieldsValid(const RandomMaestroCommand &cmd);
```

**What the parsers check:**

- an exact field count (13 / 6)
- every field numeric (`NOT_A_NUMBER` otherwise)
- the type field matches (6 / 7, else `FIELD_COUNT`)
- `module`, `channel` and `gpioChannel` are ≥ 0
- `minGapMs ≥ 20`
- `maxGapMs ≥ minGapMs`
- `spanMs ≥ minGapMs`

A violation of those range rules gives `OUT_OF_RANGE`.

**What `RandomServoFieldsValid` checks:**

- `minGapMs ≥ 100`
- `0 ≤ minPos < maxPos ≤ 100`
- `1 ≤ minMove`, and `2 × minMove ≤ maxPos − minPos`
- `maxMove == 0`, or `minMove ≤ maxMove ≤ maxPos − minPos`
- `0 ≤ speed, acceleration ≤ 255`

`AstrOsEnums.h` appends the two values. `AnimationCommand`'s header parse already casts the type int, so `6|…` and `7|…` get the new enum values with no change there. The tests pin that.

**Tests**, written first. New `test/test_native/astros_random_commands_tests.cpp`:

- `ParsesValidMaestroRandom`: `"6|14000|0|3|14000|400|2000|20|100|15|60|0|0"` gives `NONE`, and every field matches.
- `ParsesValidGpioRandom`: `"7|14000|2|14000|50|400"` gives `NONE`, and every field matches.
- `MaxMoveZeroMeansNoMax`: parses with `NONE`, `maxMove == 0`, and `RandomServoFieldsValid` true.
- `RejectsWrongFieldCount`: 12 and 14 fields for type 6, and 5 fields for type 7, give `FIELD_COUNT`.
- `RejectsNonNumeric`: `"abc"`, `"12x"`, an empty field, and `"99999999999"` give `NOT_A_NUMBER` (the last via the int bound).
- `RejectsGapAndSpanRules`: `minGapMs` 19, `maxGapMs < minGapMs`, `spanMs < minGapMs`, and a negative channel give `OUT_OF_RANGE`.
- `AcceptsGapBoundaries`: `minGapMs` 20, `maxGapMs == minGapMs`, and `spanMs == minGapMs` give `NONE`.
- `ServoFieldBoundaries`:
  - on a 20–100 range, `minMove` 40 is true and 41 is false
  - `maxMove` 80 is true and 81 is false
  - `maxMove` 10 with `minMove` 15 is false
  - `minGapMs` 100 is true and 99 is false
  - speed 255 is true and 256 is false
  - `minPos == maxPos` is false
- `DigitalZerosAreParseable`: `"6|0|0|3|5000|50|200|0|0|0|0|0|0"` gives `NONE`, and `RandomServoFieldsValid` is false. A digital channel is never checked with it.
- `AnimationCommandTypesNewValues`: an `AnimationCommand` built from each valid string has type `MAESTRO_RANDOM` / `GPIO_RANDOM` and `duration` 14000.

## Acceptance criteria

- [x] All new native tests are green, and every existing native test is green.
- [x] Both boards build.
- [x] The new code has no `std::stoi` and no ESP-IDF or FreeRTOS includes. The purity guard passes.
- [x] `.docs/qa/random-span-events.md` exists with Preconditions, plus a "Parser" note: native coverage only, with bench cases arriving in T-009.

## Out of scope

- Dispatching the new types, the random-effects task, and panic handling (T-009).
- Move math and the span table (T-008).
- Hardening the existing parsers' `std::stoi` (Backlog item).

## Verification

- `pio test -e test` → green, including `astros_random_commands_tests`.
- `pio run -e lolin_d32_pro` and `pio run -e metro_s3` → both build.

## Implementation checklist

- [x] Write `astros_random_commands_tests.cpp` (all tests in the Task section); RED: compile error on the missing `MAESTRO_RANDOM` / `RandomCommands.hpp`
- [x] Append `MAESTRO_RANDOM = 6, GPIO_RANDOM = 7` to `MODULE_TYPE`; add `RandomCommands.hpp` declarations + sentinel stubs; RED: every parser test fails on its assertions
- [x] Implement `ParseRandomMaestroCommand` / `ParseRandomGpioCommand` (strtol at first, `strtoll` after the second review; exact field counts, gap/span ranges); GREEN
- [x] Implement `RandomServoFieldsValid`; GREEN
- [x] Full `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build; clang-format clean on changed files; purity grep clean
- [x] Create `.docs/qa/random-span-events.md` (Preconditions + Parser note)
- [x] Code review (`superpowers:requesting-code-review`); address Critical/Important — 0 Critical; Important #2 fixed (accept-side zero boundaries), doc drift fixed; Important #1 resolved by the AstrOs.Server T-008 amendment (118eb50d)

## Review notes (2026-10-22)

### First branch review (opus): 0 Critical, 2 Important

- **Important #2, fixed in 41e7a5b:** accept-side tests for zero boundaries (minPos 0, module and channel 0). The doc drift was fixed in the same commit (lib README, the CLAUDE.md lib table, a test comment).
- **Important #1:** the producer must send integer milliseconds. A 2.01 s gap × 1000 is `2009.9999999999998`, which this parser rejects. Jeff approved the amendment to AstrOs.Server T-008 in AstrOs.Server 118eb50d: span and gap values are rounded with `Math.round` before validating, and the integer rule covers all spans. It's mirrored in `.docs/protocol.md` § C by 2b77da1 on develop (this branch predates it).
- **The deferred test-gap minors**, done at Jeff's request in 5110feb: `minMove` 1, `maxMove == minMove`, the odd 19–100 range, speed −1, INT_MIN overflow, the type-6 gap and span rules, distinct field values, and a strict `ToInt` (`RejectsLooseNumbers`, RED → GREEN).

### Second review (pr-review-toolkit, five agents): 0 Critical

Fixed in the next commit, test-first:

- **Span shorter than its max gap:** now accepted in a test (`AcceptsSpanShorterThanMaxGap`). Tightening the span rule used to pass every test.
- **Results of a rejected parse could be misused:** on any error, `cmd` now names no target (module and channel −1). Before, it read as module 0 / channel 0, a real target. Tested by `ErrorResultsCarryNoTarget`.
- **`RandomServoFieldsValid` now stands on its own.** It also re-checks module, channel and the gap rules, so it no longer approves out-of-range commands (`ServoCheckIsSelfContained`).
- **The type field is checked before the other fields,** so a wrong type is always `FIELD_COUNT`, whatever the other fields hold (`RejectsWrongTypeField`).
- **`RandomParseErrorName`**, for T-009's log lines (`ParseErrorNamesAreDistinct`).
- **`[[nodiscard]]`** on the result structs, the servo check and the name helper.
- **Explicit `MAESTRO_RANDOM = 6` and `GPIO_RANDOM = 7`.**
- **More tests:**
  - the overflow guard, with an INT_MIN `maxPos`
  - a negative `maxMove`
  - type-6 gap boundaries
  - the animation engine hands off the delta and the full string
  - the delta differs from the span in `AnimationCommandTypesNewValues`
- **Renamed tests:** `RejectsOutOfRangeFields` and `DigitalZerosParseButFailServoCheck`.
- **Comments and docs corrected:**
  - the overflow comment (since superseded by the `strtoll` switch below)
  - the `ToInt` attribution
  - the header error-code comments
  - the README, the CLAUDE.md row, and the QA plan (SQL pointer, exact coverage list)

**Verification after the fixes:**

- 22 code-breaks were applied one at a time against the final code, and each was restored. 21 failed a test.
- The one survivor, as expected: deleting `errno == ERANGE` couldn't be caught where `long` is 64-bit.
- **Jeff approved switching to `strtoll`** (Contract amended above), so that gap is closed:
  - The int bounds are now the one overflow guard on every target, and the host tests exercise them. Deleting either bound fails `RejectsNonNumeric`.
  - New assertions cover values past `long long` (20 digits, both signs) and the int edges, which are accepted as numbers.
  - A `static_assert(sizeof(value) > sizeof(int))` covers narrowing the parse back to `long`. That change is invisible on a 64-bit host, but it fails the ESP32 board build, which CI runs. Shown by applying it and building `lolin_d32_pro`.
  - Final run: 23 code-breaks. 22 fail a test, and the 23rd fails the device build.

**Moved to the T-009 and ESP T-008 task files (planning, on develop):**

- The queue message carries the command type.
- Panic and reload drain `randomQueue` and reject spans dispatched before the clear.
- The dispatch `default:` logs unknown types.
- The GPIO `SpanKey.module` convention.
- One snapshot for `isServo`.
- A warning when a digital channel gets non-zero servo fields.
- `RandomSpanTable::Start` re-checks the gap rules (defense in depth).

**Still deferred:**

- Counting `|` before splitting, so oversized input isn't allocated first. This is defense in depth; the server is trusted, and the existing parsers split the same way.
- A reason code from `RandomServoFieldsValid`, and a separate `WRONG_TYPE` error. Both are contract changes, and neither is recommended now.
