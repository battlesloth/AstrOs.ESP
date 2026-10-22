# T-007: Parse random span commands (types 6 and 7)

<!-- File: .docs/tasks/T-007-random-span-command-parsers.md. Branch: feature/T-007-random-span-command-parsers. PR title: "T-007: Parse random span commands (types 6 and 7)". -->

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
- **Number parsing:** no `std::stoi` in new code (exceptions are disabled). Use `strtol` with a full-consumption `endptr` check, `errno == ERANGE` rejection, and an `int` range check.

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
- `RejectsNonNumeric`: `"abc"`, `"12x"`, an empty field, and `"99999999999"` give `NOT_A_NUMBER` (the last via ERANGE or the int bound).
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

- [ ] All new native tests are green, and every existing native test is green.
- [ ] Both boards build.
- [ ] The new code has no `std::stoi` and no ESP-IDF or FreeRTOS includes. The purity guard passes.
- [ ] `.docs/qa/random-span-events.md` exists with Preconditions, plus a "Parser" note: native coverage only, with bench cases arriving in T-009.

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
- [x] Implement `ParseRandomMaestroCommand` / `ParseRandomGpioCommand` (strtol, exact field counts, gap/span ranges); GREEN
- [x] Implement `RandomServoFieldsValid`; GREEN
- [x] Full `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build; clang-format clean on changed files; purity grep clean
- [x] Create `.docs/qa/random-span-events.md` (Preconditions + Parser note)
- [ ] Code review (`superpowers:requesting-code-review`); address Critical/Important

