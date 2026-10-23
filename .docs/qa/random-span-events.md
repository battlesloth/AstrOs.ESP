# QA: Random span events

This plan covers script commands 6 (`MAESTRO_RANDOM`) and 7 (`GPIO_RANDOM`). Each is sent once, at a span's start. The controller then improvises toggles or servo moves until `start + spanMs`. The grammar and valid ranges are in `.docs/protocol.md` § C. The design is AstrOs.Server `.docs/specs/20261022-0748-random-span-events-design.md`.

The feature lands in three tasks:

- **T-007, parsers.** Native tests only. Nothing dispatches types 6 and 7 yet; firmware skips them and keeps their delta.
- **T-008, move math and span table.** Native tests only.
- **T-009, runtime.** The random-effects task and the panic/reload hooks. Bench cases are added here.

## Preconditions

- **Firmware:**
  - For native coverage: `pio test -e test` is green.
  - For bench cases: built from a branch that contains T-009.
- **Server:** AstrOs.Server with its T-007 and T-008, so a script can carry a span. Until the scripter UI lands, span rows are inserted by hand. AstrOs.Server T-008 adds that SQL to its own `.docs/qa/random-span-events.md`.
- **Hardware:** a master with a Maestro wired to at least one servo and one digital output (an LED), plus one ESP GPIO with an LED. Bench cases that cover padawans need one padawan with the same wiring.
- **Logging:** a serial monitor on the master (`pio device monitor -e metro_s3`).

## Parser (T-007): native coverage

`test/test_native/astros_random_commands_tests.cpp` covers parsing and the firmware-side rejects:

- valid type 6 and type 7 strings parse field by field (every field distinct, so a swapped index shows)
- `maxMove 0` means "no max"; a negative `maxMove` is rejected
- **accepted boundaries:** `minGapMs` 20, `maxGapMs == minGapMs`, `spanMs == minGapMs`, a span shorter than `maxGapMs`, module 0 and channel 0 (both parsers)
- **`FIELD_COUNT` rejects:**
  - wrong field counts (12 or 14 fields for type 6; 5 or 7 for type 7; an empty string)
  - a type field that isn't this parser's, checked before the other fields
- **`NOT_A_NUMBER` rejects:** empty fields, non-numeric fields, int overflow in both directions, and these non-plain forms: a leading or trailing space, a leading `+`, a lone `-`, an embedded NUL. (Leading zeros and `-0` are accepted.)
- **`OUT_OF_RANGE` rejects:**
  - a negative module or channel
  - `minGapMs < 20`
  - `maxGapMs < minGapMs`
  - `spanMs < minGapMs`
- **on every error**, the result's command names no real target (module and channel are -1)
- servo-field boundaries (`RandomServoFieldsValid`):
  - `0 ≤ minPos < maxPos ≤ 100`, including extreme values that would overflow the range arithmetic
  - `minMove` from 1 up to half the position range (even and odd ranges)
  - `maxMove` from `minMove` up to the full range
  - `minGapMs ≥ 100`
  - speed and acceleration 0–255
  - it also re-checks the parser's rules, so a `true` stands on its own
- a digital channel's zeroed fields parse, but never pass the servo check
- `RandomParseErrorName` gives every error a distinct name
- `AnimationCommand` maps `6|…` and `7|…` to the new types, and its duration is the delta, not the span
- the animation engine hands each command to the dispatcher with its delta and the full command string

Overflow checks behave the same on the device and the native host: fields are parsed as 64-bit `long long` on both, then bounded to `int`. A `static_assert` keeps the parse wider than `int`, so narrowing it would fail the board builds.

There are no bench cases for T-007 alone. On hardware it behaves like the previous firmware: types 6 and 7 are skipped, and the rest of the script plays on time.

Malformed commands aren't bench-tested at any stage. AstrOs.Server T-008 rounds span and gap values to whole milliseconds and skips invalid spans before they're sent (§ C); these native tests cover the firmware rejects. Unrounded, a float such as `2009.9999999999998` would be rejected here as `NOT_A_NUMBER`.

## Move rule and span table (T-008): native coverage

`test/test_native/astros_random_effects_tests.cpp` covers every random decision, with a scripted RNG (exact draws) or a seeded one (property loops):

- **draws:** `UniformInt` is inclusive at both ends, works across the full `int` range, and returns `lo` without drawing when `hi <= lo`; `NextGapMs` draws over `minGapMs..maxGapMs`
- **servo move rule (§ C):**
  - forced up at `minPos` and at `minPos + minMove`; forced down at `maxPos` and at `maxPos − minMove`; neither consumes a direction draw
  - in between, including the first positions past each forced band, the draw's parity picks the direction
  - the size is bounded by the room in that direction (`maxMove 0`), by `maxMove`, and by the room when it is smaller than `maxMove`
  - a position outside the range jumps to a uniform position inside it, ignoring `minMove`
  - a move that would pass a limit stops at it (only reachable when `minMove` is more than half the range, which § C forbids)
  - 10,000 seeded moves for each of four configurations (including the exact half-range edge) stay in range, move at least `minMove` and at most `maxMove`
- **travel stretch:**
  - `MoveTravelMs` runs `WorstCaseTravelMs`'s Maestro model at the move's distance. It equals `WorstCaseTravelMs` at 3000 µs for every speed and acceleration (out-of-range values clamp the same way), and is linear at accel 0
  - with acceleration it isn't linear: the shape that never reaches the speed cap grows with √distance (200 µs at speed 0, accel 5 is 716 ms, where linear scaling gives 185), and the shape that does reach it keeps its full ramps; the two shapes meet at the boundary (within 1 ms after rounding up), and the boundary itself counts as the second shape
  - it saturates instead of overflowing, and large distances with acceleration compute in 64 bits
  - a servo span's next change waits for the larger of the drawn gap and the move's travel, in either direction, with and without acceleration, with the distance computed in 64 bits; a span starting outside its range measures its first move from where it actually is
- **`PercentFromPosition`:** round-trips `GetRelativeRequestedPosition` within ±1 %, plain and inverted (undoing MaestroModule's `100 − p`), on full and trimmed channels; clamps to 0–100; a channel with `maxUs <= minUs` reads as 0 (100 inverted)
- **span table:**
  - the first change is at the start; toggles alternate from the initial level; servo changes carry the target, speed and acceleration, and each move starts from the last target
  - gaps are drawn for every change, toggle and servo; a servo change draws direction, size, then gap
  - the next change is scheduled from when the pass ran, so a late pass never causes a burst of catch-up changes
  - a span with the same key (type, module, channel) replaces the old one, even behind a free slot or when the table is full, and starts from its own initial position; a key that differs in any of type, module or channel makes a new span
  - a full table rejects a new key and leaves every span running; a span that ends frees its slot
  - `Start` rejects `minGapMs < 20`, `maxGapMs < minGapMs` and `spanMs < minGapMs`, leaving the table unchanged; each rule reads the right field (a span shorter than its max gap is valid), and the table accepts exactly the gap triples T-007's parser accepts
  - `Collect` appends to the caller's vector, and the table can't be copied (a copy would fork the schedule)
  - no change at or after `deadline − 20 ms`: a late pass inside the guard drops the span with no change, and a span whose next change would land inside it is dropped right after emitting
  - `MsUntilNext`, `Clear`, and a span that crosses the `uint32` millisecond wrap, including a check made just before the wrap while the next change is due after it

T-008 adds no bench cases either: nothing on the controller calls the table until T-009.
