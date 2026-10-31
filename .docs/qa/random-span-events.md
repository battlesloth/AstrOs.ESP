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
  - a servo span's next change waits for the larger of the drawn gap and the move's travel, in either direction, with and without acceleration, with the distance computed in 64 bits as `floor(|Δpct| × rangeUs / 100) + 1` µs, which is never short of what `GetRelativeRequestedPosition` maps (checked for every forced 1 % and 10 % move on eight channel ranges, plain and inverted); `rangeUs <= 0` disables the stretch; a span starting outside its range measures its first move from where it actually is
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

## Runtime (T-009): native coverage

`test/test_native/astros_random_span_admission_tests.cpp` covers the random-effects task's pure decisions (`RandomSpanAdmission.hpp`):

- **building a span from a Maestro command and the channel's snapshot:**
  - a servo copies every field, takes `rangeUs` from the *channel's* µs range (not the span's percents), and starts from `PercentFromPosition(lastPos)`, inversion undone (20 % plain is 80 % inverted)
  - a digital channel starts from `lastPos >= 1500` (2500, 1500 high; 1499, 500, 0 low) and leaves every servo field zero, even when the command carries some; any one non-zero servo field flags the config mismatch, and the span still runs as a toggle
  - rejects, in order: disabled channel (servo and digital); servo with `maxPos <= minPos` (1 µs of range is accepted; reported before any field check); a digital span on a servo channel (positions and moves all 0, whatever speed and accel say); invalid servo fields (one non-zero position or move is "invalid", not "mismatch"); a span of 20 ms or less (the end guard), Maestro and GPIO
- **GPIO:** key `{GPIO_RANDOM, 0, channel}`, starting from the level `GpioModule` last set
- **too late:** a span dispatched at 1000 for 5000 ms is accepted until 5979 and rejected from 5980; the same across the `uint32` wrap
- **clear fence:** fences nothing before the first clear; fences a dispatch at or before the latest clear, wrap-safe; keeps a clear for 2^30 ms and then forgets it, so a span dispatched 2^31 ms after a clear isn't read as "before" it
- **change strings:** servo `1|0|m|ch|pct|speed|accel`, digital `1|0|m|ch|2500 or 500|0|0`, GPIO `5|0|ch|1 or 0`; each round-trips through the real `MaestroCommand` / `GpioCommand` parsers the queue consumers use; a non-span type or a buffer too small returns 0 (never a truncated command); the longest possible string fits `SPAN_CHANGE_MAX_LEN`
- **dropped-change log throttle:** logs a span's first drop only; keys differ by type, module and channel; a reset (new span on the key) logs again; a full throttle still logs a new key and forgets the oldest; capacity 0 logs every drop
- **ms → ticks:** rounds up (1–10 ms is 1 tick at 100 Hz) without overflowing
- **change-queue share:** random changes may fill at most half of a change queue (10 of 20, 5 of 10, 3 of 5); a zero-depth queue has no room
- **send-time expiry:** every change carries its span's end (`Collect` stamps it); `SpanChangeExpired` refuses a change from `deadline − 20 ms` on, the same boundary `Collect` uses, wrap-safe. The random task re-checks it with a fresh clock just before each send

28 mutations of `RandomSpanAdmission.cpp` (2026-10-30), each caught by a test.

## Runtime (T-009): bench cases

### Bench preconditions

- Firmware from a branch with T-009 on the master (and on the padawan for case 9). Monitor the master with `pio device monitor -e metro_s3`.
- AstrOs.Server with its T-007, T-008 and T-009. Insert span rows by hand with the SQL shape in AstrOs.Server `.docs/qa/random-span-events.md` case 8 (GPIO span: `module_type 3`, `module_sub_type 301`; Maestro span: `module_type 1`, `module_sub_type 104`, data JSON with `channel`, gaps, positions, moves, speed and acceleration). Set `scripts.duration_ds` to the latest span end, and keep every span within `0 ≤ time < end_time ≤ 6000`. Deploy the script to the controllers, then run it.
- Useful log lines (master console):
  - `Random span command val: 6|…` / `7|…`: the dispatch loop handed the span over
  - `Random span started: GPIO ch C, N ms, gaps A-B ms` / `Maestro M ch C servo, … speed S, accel A` / `… digital …`
  - every change then shows as a normal command: `Sending Command => 5|0|C|1` (GpioModule) or `Queueing servo command => 1|0|M|C|…` plus `Setting servo …` (MaestroModule)
  - `Random span skipped (<reason>): <raw command>`: any reject
  - `Panic: stopped N random span(s), dropped M queued` / `Config reload: stopped …`
  - `RAM left <bytes>` every 10 s (heap check)

### 1. GPIO flicker

1. A GPIO span of 10 s with gaps 50–400 ms on an LED channel, followed by a GPIO point event at the span's end time setting the channel low.
2. Run the script.
3. **Pass:** `Random span started: GPIO ch …, 10000 ms, gaps 50-400 ms`; the LED blinks irregularly for 10 s; the last `5|0|C|…` from the span is logged before the end time, then the end-time event turns the LED off and it stays off.

### 2. Maestro digital flicker

1. The same span on a Maestro channel configured `isServo = false` (an LED on the Maestro output).
2. **Pass:** as case 1, with `… digital …` in the start line and `1|0|M|C|2500|0|0` / `1|0|M|C|500|0|0` commands.

### 3. Servo twitch

1. A servo span: positions 35–65 %, move 2–8 %, gaps 200–800 ms, speed 0, 20 s.
2. **Pass:** small irregular twitches; every `1|0|M|C|<pct>|0|0` has a percent from 35 to 65; consecutive targets differ by 2–8 % (except the first move if the servo started outside 35–65 %). After the span the servo releases on its normal deadline.
3. **Load check** (acceptance criterion): repeat with a 100 ms min gap (gaps 100–200 ms) for 60 s. **Pass:** no `Random Effects Stack HWM`, `Animation Dispatch Stack HWM` or task-watchdog line, and `RAM left` stays flat (within a few hundred bytes) across the minute.

### 4. Panel flap with an end event

1. A servo span over 0–100 %, move 30 % to no max, gaps 400–2000 ms, then a Home (or fixed-position) event at the span's end time.
2. **Pass:** random flapping; the end-time event's `Queueing servo command` is the last servo command for that channel, and no `1|0|M|C|…` from the span follows it.

### 5. Slow servo

1. A servo span with speed 5, gaps 100–200 ms, move 20 %.
2. **Pass:** each move visibly completes before the next starts; the logged commands are spaced by the travel time, far more than 200 ms.

### 5b. Accelerated short moves

1. Speed 0, accel 3, move 5–10 %, gaps 100–200 ms.
2. **Pass:** each move still completes before the next starts (a 10 % move on a 2000 µs channel is modeled at ~925 ms).

### 6. Panic mid-span

1. Run cases 1 and 3 together (one script, both spans), then hit panic from the server while both are active.
2. **Pass:** `Panic: stopped 2 random span(s), dropped 0 queued` appears before `Panic: dropped … queued servo commands`; no `Queueing servo command => 1|…` from a span after the panic lines; the servo de-energizes (Maestro offs as in `maestro-servo-release.md`); the LED stops flickering. Panic doesn't drain `gpioQueue` (GPIO holds, T-004), so **at most one** `Sending Command => 5|…` that was already queued may appear right at the panic lines. A second one, or any later, is a fail.
3. **Same-start burst:** a script with several spans at the same start time; panic within ~10 ms of that start (start the script and panic in one shell line, e.g. two `curl` calls joined with `;`). **Pass:** no random servo command after the panic lines, and at most one already-queued GPIO change per GPIO span right at them. A `Random span skipped (dispatched before a panic or config reload stopped the spans)` line is the fence catching an in-flight span, a pass.

### 7. Reload mid-span

1. While a long span runs, deploy the controller config from the server.
2. **Pass:** `Config reload: stopped 1 random span(s), …`, then the normal Maestro reload and homing lines; no command from the span after it; the new config applies.

### 8. Table full

1. A script with 33 concurrent spans (e.g. 10 GPIO channels + 23 Maestro channels, or any 33 distinct channels), all starting together and lasting 10 s.
2. **Pass:** 32 `Random span started` lines and one `Random span skipped (table full (32/32 active)): …`; the 32 others run. Expect `Random change dropped, … half full` lines at this load: random changes may fill at most half of each change queue (5 of 10 GPIO, 10 of 20 servo), and each span logs only its first drop.

### 9. Padawan

1. Cases 1 and 3 on a dome padawan's channels, then panic from the server.
2. **Pass:** as cases 1, 3 and 6, on the padawan's console; the panic arrives through the ESP-NOW relay and the padawan logs `Panic: stopped …`.

### 10. Channel with no usable range

1. Set a servo channel's min and max to the same µs, deploy the config, then run a servo span on it.
2. **Pass:** `Random span skipped (Maestro M ch C has no usable range (min X us >= max X us), fix the channel config): 6|…` and nothing moves. Restore the channel afterwards.

### 11. Script commands keep their timing under fast spans

The queue tasks drain about one message per 10 ms tick. Random changes may fill at most half of each change queue, so the script's own commands always find room.

1. A 20 s script with three GPIO spans and two Maestro digital spans, every one with gaps 20–40 ms, plus scripted point events during the spans: a GPIO channel set high at 5 s and low at 10 s, a servo move at 7 s, and a servo Home at the end time.
2. Run it with the master console captured (the ESP log prefix carries milliseconds since boot).
3. **Pass:**
   - each scripted event's dispatch line (`GPIO command val: 5|…` / `Maestro command val: 1|…`) is followed by its execution line (`Sending Command => 5|…` / `Queueing servo command => 1|…` with the same string) within about 100 ms for GPIO and 300 ms for servo
   - no `Send gpio queue fail` or `Send servo queue fail`
   - the end-time Home runs at the end time (within the same bounds) and the script finishes on time
   - `Random change dropped, … half full` lines appear: the spans are asking for more than the queue tasks can run
4. **Fail:** a scripted event 2 s late, a `Send … queue fail`, or a script that ends late.

### Edge cases and negative checks

- **Unknown command type** (optional: the server never sends one, so it needs a script file edited by hand on the SD card): a type 8 command logs `Unknown command type 8 skipped (delta kept)` and the rest plays on time.
- **Manual commands don't stop a span:** during case 3, send a `SERVO_TEST` to the same channel. The slider move lands; the span keeps going (§ C).
- **Disabled channel:** a span on a disabled Maestro channel logs `Random span skipped (Maestro M ch C is disabled)`.
- **Config mismatch:** a digital span (positions 0) sent for a servo channel logs `config mismatch: the server sent a digital span for servo channel …`; a servo span for a digital channel logs the toggle mismatch warning and toggles. Both mean the server's channel config is out of sync with the controller's; re-sync.
