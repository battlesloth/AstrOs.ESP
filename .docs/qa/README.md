# QA Test Plans

This directory holds manual, hands-on-hardware QA test plans for features in AstrOs.ESP. QA plans are the primary verification mechanism for anything that touches FreeRTOS tasks, ESP-IDF drivers, or real hardware — code that cannot be exercised by the native `[env:test]` googletest suite.

## Relationship to native tests

QA plans **complement**, not replace, the native tests in `test/test_native/`. For any feature that touches code in `lib/AstrOsMessaging` or pure utility code in `lib/AstrOsUtility`, add or extend native unit tests first — those run on every `pio test -e test` invocation and are the first line of defense. QA plans then cover the end-to-end hardware behavior that native tests cannot reach:

- ESP-NOW mesh behavior between real devices
- Servo movement on PCA9685 boards
- I²C bus contention under load
- Serial channel baud rate changes between master/padawan roles
- OLED display rendering
- NVS / SD card persistence across reboots

## Filename convention

Use descriptive kebab-case filenames — no timestamp prefix, since QA plans track a feature rather than a specific point in time:

```
ota-upgrade.md
animation-queueing.md
maestro-hotplug.md
peer-discovery.md
```

Commit the QA plan alongside the feature work it covers.

## Plan structure

Each QA plan should contain:

### Preconditions
Required state and setup before testing — firmware version, number of nodes, hardware attached, master/padawan role assignment, etc.

### Test cases
Numbered, step-by-step user actions. Each step should be specific enough that a different person could execute it without asking questions. Group related steps under subheadings when a plan covers multiple scenarios.

### Expected results
What should happen after each step or group of steps. Where relevant, reference specific log output, display content, or hardware behavior.

### Edge cases / negative tests
Invalid inputs, error states, boundary conditions, and recovery scenarios. These are often the most valuable part of the plan — they exercise the paths that native tests can't reach.
