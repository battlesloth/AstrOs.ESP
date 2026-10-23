# T-008: Random span math and span table (pure)

<!-- File: .docs/tasks/T-008-random-effects-math.md. Branch: feature/T-008-random-effects-math. PR title: "T-008: Random span math and span table (pure)". -->

## Context

This is the random span events project. See `PLAN.md` § Random span events, the AstrOs.Server spec `.docs/specs/20261022-0748-random-span-events-design.md` § "Servo random span" and § "Firmware", and `.docs/protocol.md` § C.

Every random decision lives here, as pure code with an injected RNG, so native tests are deterministic:

- the servo move rule (Jeff's direction rule plus the clamp)
- gap draws
- the travel-time gap stretch (Jeff: option A)
- the table of active spans

T-009 wires it to FreeRTOS. This task has no dependency on T-007 beyond the `MODULE_TYPE` values. It may land before or after T-007, because it does not parse.

## Contract (pinned — do not change)

- **PURE-lib purity.** The new lib lives under `lib_native/` and is registered in the `PURE_LIBS` array in `.github/workflows/pr-validation.yml`.
- **The move rule, exactly as `.docs/protocol.md` § C:**
  - Direction: if `current ≤ minPos + minMove` → up. Else if `current ≥ maxPos − minMove` → down. Else random.
  - Size: uniform in `[minMove, min(maxMove or ∞, room in that direction)]`.
  - Clamp the result to `[minPos, maxPos]`.
  - Current position outside the range: uniform position in `[minPos, maxPos]`.
- **The gap after a servo move** is `max(NextGapMs(min, max), MoveTravelMs(distanceUs, speed, accel))`. GPIO and digital gaps are never stretched.
- **`MoveTravelMs` is distance-aware:** the same trapezoid/triangle model as `WorstCaseTravelMs`, with the move's distance `d` in place of the 3000 µs guard range. With `cap` = speed (0 → 255), cruise is `ceil(40·d / cap)` ms. With acceleration it adds `ceil(80·cap / accel)` when `2·cap² ≤ d·accel` (trapezoid); otherwise it is `ceilSqrt(ceil(12800·d / accel))` (triangle). (Amended 2026-10-22, Jeff, from the T-008 review: the original `WorstCaseTravelMs × d / 3000` is exact only at accel 0. A triangle move's time grows as √d, so short moves with acceleration were underestimated, by about 4× for 200 µs at accel 5, and the next change would cut them off mid-travel.)
- **`Start` is `[[nodiscard]]`** (amended 2026-10-22, Jeff, from the pre-push type-design review): a dropped `false` is a span that never runs and is never logged. No behavior change.
- **The existing `AstrOsServoUtils.hpp` functions keep their behavior.** Their bodies move verbatim to a new `lib_native/AstrOsUtility/src/AstrOsServoUtils.cpp`; the header keeps the declarations and doc comments, the constants, `constexpr CeilDiv` and `inline EncodeMaestroTarget`. One new helper is added. (Amended 2026-10-22, Jeff: the header defined non-`inline` functions, so a second including file — `RandomEffects.cpp`, the new test file, T-009's `main.cpp` — fails to link with "multiple definition".)

## Task

### New PURE lib `lib_native/AstrOsRandomEffects/`

Contents: `include/RandomEffects.hpp`, `src/RandomEffects.cpp`, and a `README` stating the purity rule.

```cpp
using Rand32 = std::function<uint32_t()>;

int UniformInt(int lo, int hi, const Rand32 &rand);          // inclusive; returns lo when hi <= lo
int NextGapMs(int minGapMs, int maxGapMs, const Rand32 &rand);
int NextServoTarget(int currentPct, int minPos, int maxPos, int minMove, int maxMove /*0 = none*/,
                    const Rand32 &rand);
int MoveTravelMs(int distanceUs, int speed, int acceleration);
// WorstCaseTravelMs's Maestro model evaluated at distanceUs (it equals WorstCaseTravelMs at 3000 us);
// 0 when distanceUs <= 0

struct SpanKey { int type; int module; int channel; };        // type = MODULE_TYPE value
struct SpanStart {
    SpanKey key; uint32_t startMs; int spanMs; int minGapMs; int maxGapMs;
    bool isServo;                                              // false = toggle (GPIO or digital Maestro)
    int minPos, maxPos, minMove, maxMove, speed, acceleration; // servo only
    int rangeUs;                                               // servo only: channel maxPos - minPos in us
    int initialPct;                                            // servo only: un-inverted percent at start
    bool initialLevel;                                         // toggle only: level at start
};
struct SpanChange { SpanKey key; bool isServo; int targetPct; bool level; int speed; int acceleration; };

class RandomSpanTable {
public:
    static constexpr int END_GUARD_MS = 20;
    RandomSpanTable(size_t capacity, Rand32 rng);
    [[nodiscard]] bool Start(const SpanStart &start);          // false when full or the all-span gap rules fail (table unchanged)
    void Clear();
    void Collect(uint32_t nowMs, std::vector<SpanChange> &out);// appends due changes, reschedules, drops ended spans
    uint32_t MsUntilNext(uint32_t nowMs) const;                // UINT32_MAX when empty
    size_t ActiveCount() const;
};
```

### Table semantics, pinned for the tests

- **`Start`:**
  - An entry with an equal key (type, module, channel) is **replaced** in place.
  - Otherwise `Start` takes a free slot.
  - When the table is full, it returns false and existing spans are untouched.
  - It also returns false, leaving the table unchanged, when § C's all-span gap rules fail: `minGapMs < 20`, `maxGapMs < minGapMs`, or `spanMs < minGapMs`. This is defense in depth (added 2026-10-22, from the T-007 review). A zero gap would make `Collect` emit on every pass and spin the task. T-009 validates first, but the table must not depend on it.
  - The first change is due at `startMs`. The deadline is `startMs + spanMs`.
- **`Collect(now)`:** for each entry whose next change is due:
  - If `now` is within `END_GUARD_MS` of the deadline, or past it, the entry is dropped **with no change**.
  - Otherwise it emits one change:
    - **Servo:** `targetPct = NextServoTarget(currentPct, …)`. Then `currentPct = targetPct`, and next is `now + max(NextGapMs, MoveTravelMs(|Δpct| × rangeUs / 100, speed, accel))`.
    - **Toggle:** `level = !level`, and next is `now + NextGapMs`.
  - An entry whose next change would land within `END_GUARD_MS` of its deadline is dropped after emitting.
- **Time comparisons** are wrap-safe: `(int32_t)(a - b)`.

### Helper in `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp` / `.cpp`

`int PercentFromPosition(int minUs, int maxUs, int us, bool inverted)`, declared in the header and defined in the `.cpp`, is the inverse of `GetRelativeRequestedPosition`:

- it rounds to the nearest percent
- it clamps to 0–100
- when `inverted`, it returns `100 − p`

### Tests

Written first. New `test/test_native/astros_random_effects_tests.cpp` uses a scripted `Rand32` (it returns a queued sequence) and, for property loops, a seeded `std::mt19937`.

- `UniformIntBoundsInclusive`, and `UniformIntLoEqualsHi`.
- **`NextServoTarget`:**
  - `NextServoTargetForcedUpNearMin`: current = minPos and current = minPos + minMove both move up by ≥ minMove.
  - `NextServoTargetForcedDownNearMax`: current = maxPos and current = maxPos − minMove both move down.
  - `NextServoTargetMiddleUsesRandomDirection`: even and odd draws give opposite directions.
  - `NextServoTargetNoMaxUsesRoom`: with maxMove 0, the move is bounded by the room in that direction.
  - `NextServoTargetOutsideRangeJumpsInside`: current 0 with range 20–100 gives a result in [20, 100], and minMove is ignored.
  - `NextServoTargetPropertyHolds`: 10,000 seeded moves for each of (20,100,15,60), (35,65,2,8), (0,100,5,0), and (40,60,10,0, the exact half-range edge).
    - Every result is in [minPos, maxPos].
    - Every move is ≥ minMove from the previous result.
    - Every move is ≤ maxMove when it is set.
    - The servo never stays put.
- **`MoveTravelMs`:**
  - `MoveTravelMsMatchesWorstCaseAtFullRange`: at 3000 µs it equals `WorstCaseTravelMs` for every speed and acceleration 0–255. 0 µs and negative distances give 0.
  - `MoveTravelMsAccelZeroIsLinear`: 1500 µs at speed 1 gives 60000; at speed 0 it gives `ceil(60000 / 255)` = 236.
  - `MoveTravelMsTriangleGrowsWithSqrtDistance`: 200 µs at speed 0, accel 5 gives 716, where linear scaling would give 185.
  - `MoveTravelMsTrapezoidKeepsFullRamps`: 1500 µs at speed 20, accel 2 gives 3000 + 800.
  - `MoveTravelMsRegimeBoundaryIsContinuous`: speed 10, accel 1 at 199, 200 and 201 µs gives 1596, 1600, 1604.
- **`PercentFromPosition`:**
  - `PercentFromPositionRoundTrips`: for p in 0..100, `PercentFromPosition(500, 2500, GetRelativeRequestedPosition(500, 2500, p), false)` is within ±1 of p.
  - `PercentFromPositionInverted`.
- **`RandomSpanTable`:**
  - `TableFirstChangeAtStart`: `Collect(startMs)` emits exactly one change.
  - `TableToggleAlternatesFromInitialLevel`: initialLevel low gives high, low, high…
  - `TableReplacesSameKey`
  - `TableStartRejectsBadGaps`: `minGapMs` 0, `minGapMs` 19, `maxGapMs < minGapMs`, and `spanMs < minGapMs` each return false. `ActiveCount` doesn't change, and an existing span with the same key is not replaced.
  - **Review focus:** `TableFullRejectsAndKeepsExisting`. Capacity 2 with 2 active: `Start` returns false, `ActiveCount` stays 2, and both still emit.
  - `TableNoChangeInsideEndGuard`: no change is emitted at or after `deadline − 20`, and the entry is gone after.
  - `TableServoGapStretchedByTravel`: speed 1, accel 0, rangeUs 2000, and a forced 80 % move. The next change is ≥ `MoveTravelMs(1600, 1, 0)` later, not the drawn gap.
  - `TableMsUntilNext`: UINT32_MAX when empty, and 0 when due.
  - `TableClearEmpties`
  - `TableWrapsAround`: startMs = UINT32_MAX − 50 with spanMs 1000 still emits and expires correctly.

## Acceptance criteria

- [x] All new native tests are green, and every existing native test is green.
- [x] Both boards build.
- [x] `lib_native/AstrOsRandomEffects` is listed in `PURE_LIBS`, and the purity guard passes locally: `grep -rEn '#include[[:space:]]*[<"](freertos/|esp_|driver/|nvs_|sdmmc_)' lib_native/AstrOsRandomEffects` returns nothing.
- [x] `.docs/qa/random-span-events.md` notes that the move rule and table are covered natively. Bench cases come in T-009.

## Out of scope

- FreeRTOS wiring: queues, the task, the mutex, panic and reload hooks, and reading channel state from modules (T-009).
- Parsing (T-007).
- Any change to `WorstCaseTravelMs` or to `ServoReleaseDeadlineMs`.

## Verification

- `pio test -e test` → green, including `astros_random_effects_tests`.
- `pio run -e lolin_d32_pro` and `pio run -e metro_s3` → both build.

## Implementation checklist

- [x] Split `AstrOsServoUtils.hpp` into `.hpp` + `.cpp` (bodies verbatim); RED first: a second including file fails to link; GREEN: existing servo-utils tests pass unchanged
- [x] Write `astros_random_effects_tests.cpp` (all tests in the Task section); RED: compile error on the missing `RandomEffects.hpp` / `PercentFromPosition`
- [x] Add the `lib_native/AstrOsRandomEffects` skeleton (README, header, sentinel stubs) and the `PercentFromPosition` stub; RED: the tests fail on their assertions
- [x] Implement `UniformInt`, `NextGapMs`, `NextServoTarget`, `MoveTravelMs`, `PercentFromPosition`; GREEN
- [x] Implement `RandomSpanTable`; GREEN
- [x] Register the lib in `PURE_LIBS`; purity grep clean; clang-format 18.1.8 clean; CLAUDE.md lib table row
- [x] Full `pio test -e test` green; `pio run -e lolin_d32_pro` and `pio run -e metro_s3` build
- [x] Mutation pass on the new code (each mutation fails a native test) — 47/47, then 19/19 after the review fixes
- [x] `.docs/qa/random-span-events.md` note (move rule + table covered natively)
- [x] Code review (`superpowers:requesting-code-review`); address Critical/Important — 0 Critical; all 4 Important addressed (see Review notes); fix-pass mutations 19/19 killed

## Rulings (2026-10-22, during implementation)

Calls the contract left open. Each is pinned by a test where it changes behavior.

- **`UniformInt` draws nothing when `hi <= lo`** (`UniformIntLoEqualsHi`). Fixed gaps then cost no RNG call, and scripted tests stay in step. The count is computed in 64 bits, so the full `int` range works.
- **Direction parity:** an odd draw goes up. Not pinned: the tests only require that the two parities differ.
- **Draw order for one servo change:** direction (middle band only), then size, then the gap.
- **The clamp is reachable only upward**, with `minMove > range / 2`, which § C forbids. The up check runs first, so a downward move always has more than `minMove` of room (`NextServoTargetClampsToLimit`).
- **`MoveTravelMs` saturates at `INT_MAX`** (`MoveTravelMsSaturates`). It computes in 64 bits and clamps speed and acceleration to 0–255, as `WorstCaseTravelMs` does. The model is duplicated rather than shared, because `WorstCaseTravelMs` is out of scope; `MoveTravelMsMatchesWorstCaseAtFullRange` keeps the two from drifting. The table computes `|Δpct| × rangeUs / 100` in 64 bits. `rangeUs` comes from channel config, so § C doesn't bound the product. A saturated gap simply drops the span after that change.
- **`PercentFromPosition` with `maxUs <= minUs` returns 0** (100 when inverted) instead of dividing by zero (`PercentFromPositionDegenerateRange`). It clamps before rounding, because `lround` returns a 32-bit `long` on the ESP32.
- **`AstrOsServoUtils.hpp` keeps its includes** (`<algorithm>`, `<cstdint>`, `<math.h>`), so any file that relied on them through the header still compiles. `CeilSqrt` moved to the `.cpp`, still `static`.
- **`NextServoTarget` doesn't check its inputs for overflow.** They are § C percents, validated by T-009 with `RandomServoFieldsValid` before `Start`, as its header comment says.
- **Tests added beyond the Task list:**
  - `NextGapMsDrawsInclusiveRange`
  - `NextServoTargetMaxMoveCapsSize`
  - `NextServoTargetClampsToLimit`
  - `MoveTravelMsSaturates`
  - `PercentFromPositionDegenerateRange`
  - `TableServoChangeCarriesTargetAndMotion`
  - `TableServoMovesFromLastTarget`
  - `TableReplacesSameKeyBehindAFreeSlot`: found while planning mutations. Without it, a `Start` that took the first free slot before finding a same-key span would pass.
  - `TableServoGapKeepsDrawWhenTravelIsShorter`: pins max(), not a sum.
  - From the review: `TableToggleDrawsEachGap`, `TableServoDrawsDirectionSizeThenGap`, `TableSchedulesFromCollectTime`, `TableReplacementStartsFromItsInitialPct` and `TableServoTravelProductIs64Bit`, plus a check in `TableWrapsAround` made just before the wrap. The four distance-aware `MoveTravelMs` tests come from the contract amendment.
- **The direction-parity flip is an equivalent mutant by design**, so the mutation passes don't run it.

## Review notes (2026-10-22)

**Fresh-context review (opus) of the uncommitted work: 0 Critical, 4 Important, 6 Minor.**

- **Important 1, fixed.** No table test drew a gap: every helper set `minGapMs == maxGapMs`, so `NextGapMs(max, min)` and `gap = minGapMs` both passed. Fixed by `TableToggleDrawsEachGap` and `TableServoDrawsDirectionSizeThenGap`.
- **Important 2, fixed.** "next = now + gap" wasn't pinned; catch-up scheduling from `nextMs` passed. Fixed by `TableSchedulesFromCollectTime` and an `ActiveCount` check in `TableNoChangeInsideEndGuard`.
- **Important 3, contract amended (Jeff: distance-aware).** Linear `d / 3000` travel scaling underestimates moves with acceleration. See the Contract.
- **Important 4, T-009 plan amended on develop.** At 100 Hz, `pdMS_TO_TICKS` truncates a 1–9 ms `MsUntilNext` to 0 ticks, and the priority-8 task would spin until the change was due. T-009 now rounds the wait up. The `MsUntilNext` comment warns callers.
- **Minors 5–7, fixed as tests.** The wrap-safe check before the wrap, servo replacement resetting `currentPct`, and the 64-bit distance product.
- **Minor 8, fixed.** Checkboxes ticked.
- **Minor 9, noted in T-009 (develop).** Log a span that arrives after its deadline, and sample `now` after the queue wait.
- **Minor 10, fixed.** The constructor parameter is now `rng` (no shadowing), and `MsUntilNext` says "earliest scheduled change".
- **Declined by the reviewer, accepted here:**
  - `END_GUARD_MS` sizing: pinned, and bench case 4 checks it.
  - Modulo bias: negligible.
  - Pre-existing warnings and UB carried verbatim by the split: out of scope.
  - A replacement starting from T-009's `lastPos` snapshot: the replacement semantics are pinned.
  - `bool Start` can't say why it failed: the interface is pinned, and T-009 validates first.
