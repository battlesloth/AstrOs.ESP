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
- **The existing `AstrOsServoUtils.hpp` functions are unchanged.** One new helper is added.

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
// = ceil(WorstCaseTravelMs(speed, acceleration) * distanceUs / 3000); 0 when distanceUs <= 0

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
    RandomSpanTable(size_t capacity, Rand32 rand);
    bool Start(const SpanStart &start);                        // false when full (table unchanged)
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
  - The first change is due at `startMs`. The deadline is `startMs + spanMs`.
- **`Collect(now)`:** for each entry whose next change is due:
  - If `now` is within `END_GUARD_MS` of the deadline, or past it, the entry is dropped **with no change**.
  - Otherwise it emits one change:
    - **Servo:** `targetPct = NextServoTarget(currentPct, …)`. Then `currentPct = targetPct`, and next is `now + max(NextGapMs, MoveTravelMs(|Δpct| × rangeUs / 100, speed, accel))`.
    - **Toggle:** `level = !level`, and next is `now + NextGapMs`.
  - An entry whose next change would land within `END_GUARD_MS` of its deadline is dropped after emitting.
- **Time comparisons** are wrap-safe: `(int32_t)(a - b)`.

### Helper in `lib_native/AstrOsUtility/src/AstrOsServoUtils.hpp`

`int PercentFromPosition(int minUs, int maxUs, int us, bool inverted)` is the inverse of `GetRelativeRequestedPosition`:

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
  - `NextServoTargetPropertyHolds`: 10,000 seeded draws for each of (20,100,15,60), (35,65,2,8), (0,100,5,0), and (40,60,10,0, the exact half-range edge).
    - Every result is in [minPos, maxPos].
    - Every move is ≥ minMove from the previous result.
    - Every move is ≤ maxMove when it is set.
    - The servo never stays put.
- **`MoveTravelMs`:**
  - `MoveTravelMsScalesWorstCase`: 3000 µs equals `WorstCaseTravelMs`, 0 µs gives 0, and 1500 µs gives `ceil(half)`.
- **`PercentFromPosition`:**
  - `PercentFromPositionRoundTrips`: for p in 0..100, `PercentFromPosition(500, 2500, GetRelativeRequestedPosition(500, 2500, p), false)` is within ±1 of p.
  - `PercentFromPositionInverted`.
- **`RandomSpanTable`:**
  - `TableFirstChangeAtStart`: `Collect(startMs)` emits exactly one change.
  - `TableToggleAlternatesFromInitialLevel`: initialLevel low gives high, low, high…
  - `TableReplacesSameKey`
  - **Review focus:** `TableFullRejectsAndKeepsExisting`. Capacity 2 with 2 active: `Start` returns false, `ActiveCount` stays 2, and both still emit.
  - `TableNoChangeInsideEndGuard`: no change is emitted at or after `deadline − 20`, and the entry is gone after.
  - `TableServoGapStretchedByTravel`: speed 1, accel 0, rangeUs 2000, and a forced 80 % move. The next change is ≥ `MoveTravelMs(1600, 1, 0)` later, not the drawn gap.
  - `TableMsUntilNext`: UINT32_MAX when empty, and 0 when due.
  - `TableClearEmpties`
  - `TableWrapsAround`: startMs = UINT32_MAX − 50 with spanMs 1000 still emits and expires correctly.

## Acceptance criteria

- [ ] All new native tests are green, and every existing native test is green.
- [ ] Both boards build.
- [ ] `lib_native/AstrOsRandomEffects` is listed in `PURE_LIBS`, and the purity guard passes locally: `grep -rEn '#include[[:space:]]*[<"](freertos/|esp_|driver/|nvs_|sdmmc_)' lib_native/AstrOsRandomEffects` returns nothing.
- [ ] `.docs/qa/random-span-events.md` notes that the move rule and table are covered natively. Bench cases come in T-009.

## Out of scope

- FreeRTOS wiring: queues, the task, the mutex, panic and reload hooks, and reading channel state from modules (T-009).
- Parsing (T-007).
- Any change to `WorstCaseTravelMs` or to `ServoReleaseDeadlineMs`.

## Verification

- `pio test -e test` → green, including `astros_random_effects_tests`.
- `pio run -e lolin_d32_pro` and `pio run -e metro_s3` → both build.
