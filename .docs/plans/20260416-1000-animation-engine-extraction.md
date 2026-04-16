# AnimationController Pure Logic Extraction (Tier 2)

## Context

AnimationController is a MIXED lib that orchestrates animation scripts. The previous phase extracted the command parsers into `AstrOsAnimationCommands`. This phase extracts the remaining pure logic — script parsing, circular queue, and event dispatch — into a new `lib_native/AstrOsAnimationEngine`, leaving AnimationController as a thin MIXED adapter owning only the FreeRTOS mutex, atomics, and file I/O.

## Design

**New lib:** `lib_native/AstrOsAnimationEngine`

Three pure components:

1. **`parseAnimationScript(const std::string& script)`** — splits on `;`, skips empties, creates `AnimationCommand` per part, reverses. Returns `std::vector<AnimationCommand>`.

2. **`ScriptQueue`** — circular buffer for script IDs (strings). Pure index math.
   - `bool push(const std::string& id)` — returns false if full
   - `std::string pop()` — returns front item, advances index
   - `bool isEmpty() / isFull()` / `void clear()`
   - Capacity as template or constructor param (currently `QUEUE_CAPACITY = 30`)

3. **`NextCommandResult getNextCommand(std::vector<AnimationCommand>& events)`** — pops the back event, returns the command template + delay + whether the script is done. Pure: no mutex, no atomics.
   ```cpp
   struct NextCommandResult {
       std::unique_ptr<CommandTemplate> command;
       int delayMs;
       bool scriptDone;
   };
   ```

**AnimationController adapter** becomes: mutex take → call pure function → set atomics → mutex give.

## Task checklist

- [ ] Task 1: Create `lib_native/AstrOsAnimationEngine` with README + the three components
- [ ] Task 2: Rewrite AnimationController as thin adapter using the new lib
- [ ] Task 3: Add native tests for all three components
- [ ] Task 4: Register with CI purity guard + update CLAUDE.md
- [ ] Task 5: Verify tests + builds

## Verification

- `pio test -e test` — 126 existing + ~15-20 new cases pass
- `pio run -e metro_s3` + `pio run -e lolin_d32_pro` — clean builds
