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
- **Server:** AstrOs.Server with its T-007 and T-008, so a script can carry a span. Until the scripter UI lands, span rows are inserted by hand. The SQL is in AstrOs.Server `.docs/qa/random-span-events.md`.
- **Hardware:** a master with a Maestro wired to at least one servo and one digital output (an LED), plus one ESP GPIO with an LED. Bench cases that cover padawans need one padawan with the same wiring.
- **Logging:** a serial monitor on the master (`pio device monitor -e metro_s3`).

## Parser (T-007): native coverage

`test/test_native/astros_random_commands_tests.cpp` covers parsing and the firmware-side rejects:

- valid type 6 and type 7 strings parse field by field
- `maxMove 0` means "no max"
- **`FIELD_COUNT` rejects:**
  - wrong field counts (12 or 14 fields for type 6; 5 or 7 for type 7)
  - the wrong type field
- **`NOT_A_NUMBER` rejects:** empty fields, non-numeric fields and int overflow
- **`OUT_OF_RANGE` rejects:**
  - a negative module or channel
  - `minGapMs < 20`
  - `maxGapMs < minGapMs`
  - `spanMs < minGapMs`
- servo-field boundaries (`RandomServoFieldsValid`):
  - `minMove` up to half the position range
  - `maxMove` up to the full range
  - `minGapMs ≥ 100`
  - speed and acceleration 0–255
- a digital channel's zeroed fields parse, but never pass the servo check
- `AnimationCommand` maps `6|…` and `7|…` to the new types, and keeps the delta

There are no bench cases for T-007 alone. On hardware it behaves like the previous firmware: types 6 and 7 are skipped, and the rest of the script plays on time.

Malformed commands aren't bench-tested at any stage. AstrOs.Server T-008's validation is meant to skip invalid spans before they're sent, and these native tests cover the firmware rejects. One known gap: the server must round span and gap values to integer milliseconds. A float such as `2009.9999999999998` is rejected here as `NOT_A_NUMBER`, and that rounding is pending an amendment to server T-008.
