# Code Review Remediation Phase A — Memory Leaks + Buffer Safety

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (or superpowers:subagent-driven-development) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the highest-priority memory leaks and buffer safety issues identified in `.docs/code-review/code-review.md`. This phase covers 7 implementation tasks (sprintf overflow, malloc leak in timer callback, queue-failure leaks, semaphore cleanup, NVS bounds validation, ESP-NOW name overflow, and raw-pointer ownership) plus a QA test plan.

**Architecture:** All changes are surgical fixes to existing code. No new files except the QA plan. No new queues, tasks, or timers. The `unique_ptr` migration (Task 7) touches the most files but does not change runtime behavior — it converts a caller-owns-delete contract into RAII ownership. Every other task is a 1-5 line change in a single file.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, PlatformIO 6.x, C++20 (`-std=gnu++2a`), googletest (native tests).

---

## Locked decisions

1. **Branch:** Create `fix/code-review-phase-a` from `develop`. PR target is `develop`.
2. **Commit strategy:** One commit per task. Each commit must compile on both board envs before moving to the next task.
3. **No behavioral changes.** Every fix preserves the existing runtime behavior; only failure/edge-case paths change.
4. **Task order:** Tasks 1-6 are independent single-file changes. Task 7 (unique_ptr) spans multiple files and must be done as a single atomic commit. Task 8 (QA plan) is documentation only.

## Repository facts

- **Working directory:** `/home/jeff/Source/astros/AstrOs.ESP`
- **PlatformIO binary:** `~/.platformio/penv/bin/pio`
- **Build verification:** `~/.platformio/penv/bin/pio run -e metro_s3` after each task; `~/.platformio/penv/bin/pio run -e lolin_d32_pro` and `~/.platformio/penv/bin/pio test -e test` after all tasks.
- **Style:** Allman braces, 4-space indent, clang-format enforced via `.clang-format`.

---

## Task 0: Branch setup

**Files:** None (git operations only)

- [ ] **Step 1: Create feature branch from develop**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git checkout develop && git pull && git checkout -b fix/code-review-phase-a
```

- [ ] **Step 2: Commit this plan file**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add .docs/plans/20260412-1250-code-review-phase-a.md && git commit -m "plan: code review remediation Phase A — memory leaks + buffer safety"
```

---

## Task 1: guid.h sprintf to snprintf

**Files:**
- Modify: `lib/Uuid/guid.h`

**What:** `sprintf` into a 64-byte stack buffer with no length guard. Each `%x` expander can produce 1-2 hex chars for a `uint8_t`, so the current code is safe in practice, but `snprintf` costs nothing and prevents future regressions.

- [ ] **Step 1: Replace sprintf with snprintf**

In `lib/Uuid/guid.h`, replace:

```cpp
        sprintf(buff, "%x%x%x%x-%x%x-%x%x-%x%x-%x%x%x%x%x%x", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
                raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
```

with:

```cpp
        snprintf(buff, sizeof(buff), "%x%x%x%x-%x%x-%x%x-%x%x-%x%x%x%x%x%x", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
                 raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
```

- [ ] **Step 2: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 3: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add lib/Uuid/guid.h && git commit -m "fix: replace sprintf with snprintf in guid.h for buffer safety"
```

---

## Task 2: pollingTimerCallback fingerprint leak

**Files:**
- Modify: `src/main.cpp`

**What:** `pollingTimerCallback` mallocs 37 bytes for a fingerprint string every 2 seconds and never frees it. Over 30 minutes that leaks ~33 KB. Fix: use a stack-allocated `char[37]` instead — `getControllerFingerprint` takes a `char*` regardless of allocation source.

- [ ] **Step 1: Replace malloc with stack allocation**

In `src/main.cpp`, replace:

```cpp
            char *fingerprint = (char *)malloc(37);
            AstrOs_Storage.getControllerFingerprint(fingerprint);
            AstrOs_SerialMsgHandler.sendPollAckNak("00:00:00:00:00:00", "master", std::string(fingerprint), true);
```

with:

```cpp
            char fingerprint[37];
            AstrOs_Storage.getControllerFingerprint(fingerprint);
            AstrOs_SerialMsgHandler.sendPollAckNak("00:00:00:00:00:00", "master", std::string(fingerprint), true);
```

- [ ] **Step 2: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 3: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add src/main.cpp && git commit -m "fix: eliminate 37-byte malloc leak in pollingTimerCallback by using stack allocation"
```

---

## Task 3: DisplayService queue failure leaks

**Files:**
- Modify: `lib/AstrOsDisplay/src/DisplayService.cpp`

**What:** Both `displayUpdate` and `displayClear` malloc a buffer for `i2cMsg.data` but never free it when `xQueueSend` fails. The queue consumer owns the buffer on success; the producer must free on failure.

- [ ] **Step 1: Add free on queue send failure in displayUpdate**

In `lib/AstrOsDisplay/src/DisplayService.cpp`, replace:

```cpp
    if (xQueueSend(AstrOsDisplayService::i2cQqueue, &i2cMsg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display command to hardware queue");
    }
}
```

with:

```cpp
    if (xQueueSend(AstrOsDisplayService::i2cQqueue, &i2cMsg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display command to hardware queue");
        free(i2cMsg.data);
    }
}
```

- [ ] **Step 2: Add free on queue send failure in displayClear**

In `lib/AstrOsDisplay/src/DisplayService.cpp`, replace:

```cpp
    if (xQueueSend(AstrOsDisplayService::i2cQqueue, &i2cMsg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display clear command to hardware queue");
    }
}
```

with:

```cpp
    if (xQueueSend(AstrOsDisplayService::i2cQqueue, &i2cMsg, pdMS_TO_TICKS(100)) == pdFALSE)
    {
        ESP_LOGE(TAG, "Failed to send display clear command to hardware queue");
        free(i2cMsg.data);
    }
}
```

- [ ] **Step 3: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 4: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add lib/AstrOsDisplay/src/DisplayService.cpp && git commit -m "fix: free i2cMsg.data on queue send failure in DisplayService"
```

---

## Task 4: AnimationController semaphore cleanup

**Files:**
- Modify: `lib/AnimationController/src/AnimationController.cpp`

**What:** The destructor is empty but the constructor creates a FreeRTOS mutex via `xSemaphoreCreateMutex()`. The mutex is never deleted. In practice the singleton lives forever, but correctness requires cleanup in the destructor.

- [ ] **Step 1: Add vSemaphoreDelete to destructor**

In `lib/AnimationController/src/AnimationController.cpp`, replace:

```cpp
AnimationController::~AnimationController() {}
```

with:

```cpp
AnimationController::~AnimationController()
{
    vSemaphoreDelete(this->animationMutex);
}
```

- [ ] **Step 2: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 3: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add lib/AnimationController/src/AnimationController.cpp && git commit -m "fix: delete animationMutex in AnimationController destructor"
```

---

## Task 5: NvsManager setKeyId bounds validation

**Files:**
- Modify: `lib/AstrOsStorageManager/src/NvsManager.c`

**What:** `setKeyId` hardcodes `key[2]` and `key[3]` in the else branch, ignoring `startPos`. It also only handles ids 10-19 correctly — ids 20-99 produce wrong digits, and ids >= 100 could overflow. Fix: compute tens/units digits arithmetically, guard against id >= 100. Note: this is a C file, so use `ESP_LOGE` (already available via includes in the file).

- [ ] **Step 1: Replace setKeyId implementation**

In `lib/AstrOsStorageManager/src/NvsManager.c`, replace:

```c
static void setKeyId(char *key, uint8_t id, uint8_t startPos)
{
    if (id < 10)
    {
        key[startPos + 1] = (id + '0');
    }
    else
    {
        key[2] = (1 + '0');
        key[3] = ((id - 10) + '0');
    }
}
```

with:

```c
static void setKeyId(char *key, uint8_t id, uint8_t startPos)
{
    if (id > 99)
    {
        ESP_LOGE(TAG, "Peer index %d exceeds maximum of 99", id);
        return;
    }
    key[startPos] = '0' + (id / 10);
    key[startPos + 1] = '0' + (id % 10);
}
```

- [ ] **Step 2: Verify that ESP_LOGE is available**

The file already includes `<AstrOsUtility_ESP.h>` and defines `static const char *TAG = "NvsManager";` at line 10. `ESP_LOGE` is provided transitively via `esp_log.h`. Confirm by checking compilation in the next step.

- [ ] **Step 3: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 4: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add lib/AstrOsStorageManager/src/NvsManager.c && git commit -m "fix: correct setKeyId to use startPos and handle ids 0-99"
```

---

## Task 6: ESP-NOW peer name bounds check

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

**What:** `memcpy(newPeer.name, name.c_str(), name.length() + 1)` copies the full string including null terminator into `char name[16]`. If `name.length() >= 16`, this overflows the field. Fix: clamp to 15 chars and null-terminate. The file already includes `<algorithm>` so `std::min` is available.

- [ ] **Step 1: Replace unbounded memcpy with clamped copy**

In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, replace:

```cpp
    memcpy(newPeer.name, name.c_str(), name.length() + 1);
```

with:

```cpp
    size_t nameLen = std::min(name.length(), (size_t)15);
    memcpy(newPeer.name, name.c_str(), nameLen);
    newPeer.name[nameLen] = '\0';
```

- [ ] **Step 2: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 3: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add lib/AstrOsEspNow/src/AstrOsEspNowService.cpp && git commit -m "fix: clamp ESP-NOW peer name to 15 chars to prevent buffer overflow"
```

---

## Task 7: CommandTemplate unique_ptr ownership

**Files:**
- Modify: `lib/AnimationController/src/AnimationCommand.hpp`
- Modify: `lib/AnimationController/src/AnimationCommand.cpp`
- Modify: `lib/AnimationController/include/AnimationController.hpp`
- Modify: `lib/AnimationController/src/AnimationController.cpp`
- Modify: `src/main.cpp`

**What:** `GetCommandTemplatePtr()` and `getNextCommandPtr()` return raw `CommandTemplate*` with a caller-owns-delete contract. The caller in `main.cpp` calls `delete(cmd)` at the end. Converting to `std::unique_ptr<CommandTemplate>` makes ownership explicit and eliminates the risk of leaks on early-return paths. The `nullptr` check at line 453 of `main.cpp` still works because `unique_ptr` is contextually convertible to `bool` and compares equal to `nullptr` when empty.

- [ ] **Step 1: Update AnimationCommand.hpp — return type + include**

In `lib/AnimationController/src/AnimationCommand.hpp`, replace:

```cpp
#ifndef ANIMATIONCOMMAND_HPP
#define ANIMATIONCOMMAND_HPP

#include <AnimationCommon.hpp>
#include <AstrOsEnums.h>
```

with:

```cpp
#ifndef ANIMATIONCOMMAND_HPP
#define ANIMATIONCOMMAND_HPP

#include <AnimationCommon.hpp>
#include <AstrOsEnums.h>
#include <memory>
```

In the same file, replace:

```cpp
    CommandTemplate *GetCommandTemplatePtr();
```

with:

```cpp
    std::unique_ptr<CommandTemplate> GetCommandTemplatePtr();
```

- [ ] **Step 2: Update AnimationCommand.cpp — implementation**

In `lib/AnimationController/src/AnimationCommand.cpp`, replace:

```cpp
CommandTemplate *AnimationCommand::GetCommandTemplatePtr()
{
    CommandTemplate *ct = new CommandTemplate(commandType, module, commandTemplate);
    return ct;
}
```

with:

```cpp
std::unique_ptr<CommandTemplate> AnimationCommand::GetCommandTemplatePtr()
{
    return std::make_unique<CommandTemplate>(commandType, module, commandTemplate);
}
```

- [ ] **Step 3: Update AnimationController.hpp — return type + include**

In `lib/AnimationController/include/AnimationController.hpp`, replace:

```cpp
#ifndef ANIMATIONCONTROLLER_HPP
#define ANIMATIONCONTROLLER_HPP

#include <AnimationCommand.hpp>

#include <array>
#include <string>
```

with:

```cpp
#ifndef ANIMATIONCONTROLLER_HPP
#define ANIMATIONCONTROLLER_HPP

#include <AnimationCommand.hpp>

#include <array>
#include <memory>
#include <string>
```

In the same file, replace:

```cpp
    CommandTemplate *getNextCommandPtr();
```

with:

```cpp
    std::unique_ptr<CommandTemplate> getNextCommandPtr();
```

- [ ] **Step 4: Update AnimationController.cpp — implementation**

In `lib/AnimationController/src/AnimationController.cpp`, replace:

```cpp
CommandTemplate *AnimationController::getNextCommandPtr()
{
    CommandTemplate *cmd = nullptr;
    auto retrieved = false;

    while (!retrieved)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {

            if (this->scriptEvents.empty())
            {
                this->scriptLoaded = false;
                cmd = new CommandTemplate(MODULE_TYPE::NONE, 0, "");
            }
            else if (this->scriptEvents.size() == 1)
            {

                CommandTemplate *lastCmd = scriptEvents.back().GetCommandTemplatePtr();
                this->scriptEvents.pop_back();

                this->scriptLoaded = false;
                cmd = lastCmd;
            }
            else
            {
                this->delayTillNextEvent = scriptEvents.back().duration;

                // 10 milliseconds minimum between events?
                if (this->delayTillNextEvent < 10)
                {
                    this->delayTillNextEvent = 10;
                }

                cmd = scriptEvents.back().GetCommandTemplatePtr();
                this->scriptEvents.pop_back();
            }
            retrieved = true;
            xSemaphoreGive(this->animationMutex);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return cmd;
}
```

with:

```cpp
std::unique_ptr<CommandTemplate> AnimationController::getNextCommandPtr()
{
    std::unique_ptr<CommandTemplate> cmd;
    auto retrieved = false;

    while (!retrieved)
    {
        if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
        {

            if (this->scriptEvents.empty())
            {
                this->scriptLoaded = false;
                cmd = std::make_unique<CommandTemplate>(MODULE_TYPE::NONE, 0, "");
            }
            else if (this->scriptEvents.size() == 1)
            {
                auto lastCmd = scriptEvents.back().GetCommandTemplatePtr();
                this->scriptEvents.pop_back();

                this->scriptLoaded = false;
                cmd = std::move(lastCmd);
            }
            else
            {
                this->delayTillNextEvent = scriptEvents.back().duration;

                // 10 milliseconds minimum between events?
                if (this->delayTillNextEvent < 10)
                {
                    this->delayTillNextEvent = 10;
                }

                cmd = scriptEvents.back().GetCommandTemplatePtr();
                this->scriptEvents.pop_back();
            }
            retrieved = true;
            xSemaphoreGive(this->animationMutex);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return cmd;
}
```

- [ ] **Step 5: Update main.cpp — call site**

In `src/main.cpp`, replace:

```cpp
        CommandTemplate *cmd = AnimationCtrl.getNextCommandPtr();
```

with:

```cpp
        auto cmd = AnimationCtrl.getNextCommandPtr();
```

In the same file, remove the explicit delete. Replace:

```cpp
        delete (cmd);

        ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, AnimationCtrl.msTillNextServoCommand() * 1000));
```

with:

```cpp
        ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, AnimationCtrl.msTillNextServoCommand() * 1000));
```

- [ ] **Step 6: Verify compilation**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3
```

- [ ] **Step 7: Commit**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add lib/AnimationController/src/AnimationCommand.hpp lib/AnimationController/src/AnimationCommand.cpp lib/AnimationController/include/AnimationController.hpp lib/AnimationController/src/AnimationController.cpp src/main.cpp && git commit -m "fix: convert CommandTemplate to unique_ptr ownership for leak-safe animation commands"
```

---

## Task 8: QA test plan

**Files:**
- Create: `.docs/qa/code-review-phase-a.md`

- [ ] **Step 1: Create QA test plan**

Create `.docs/qa/code-review-phase-a.md` with the following content:

```markdown
# QA Test Plan — Code Review Phase A: Memory Leaks + Buffer Safety

## Preconditions

- Firmware built from `fix/code-review-phase-a` branch with all Phase A commits applied.
- One master node and at least one padawan node available.
- Serial monitor connected at 115200 baud (`pio device monitor -e metro_s3`).
- Both nodes flashed and able to communicate via ESP-NOW.

---

## Test 1: Polling Timer Heap Soak (Task 2 — fingerprint leak fix)

**Target:** Master node

**Steps:**
1. Flash master node with Phase A firmware.
2. Open serial monitor and note the initial free heap value from boot logs.
3. Let the master node run idle for 30 minutes (the polling timer fires every 2 seconds, so this is ~900 poll cycles).
4. After 30 minutes, trigger a heap report (reboot the node and compare boot-time free heap, or add a temporary `ESP_LOGI` logging `esp_get_free_heap_size()` in the polling callback).

**Expected result:** Free heap should remain stable (within normal jitter of ~100-200 bytes). Before this fix, the leak was 37 bytes per poll cycle = ~33 KB over 30 minutes.

---

## Test 2: Animation Queue Stress (Task 7 — unique_ptr ownership)

**Target:** Any node with animation capability

**Steps:**
1. Flash node with Phase A firmware.
2. Queue 5 animation scripts in rapid succession via the serial interface.
3. Let all 5 scripts run to completion.
4. Queue another animation script and let it complete.
5. Monitor serial output for any crash, guru meditation, or memory corruption messages.

**Expected result:** All scripts execute normally. No crashes, no memory corruption. The `unique_ptr` change is transparent to runtime behavior — commands are created and destroyed identically, just with automatic cleanup.

---

## Test 3: Display Command Under Queue-Full Conditions (Task 3 — DisplayService leak fix)

**Target:** Any node with OLED display

**Steps:**
1. Flash node with Phase A firmware.
2. Trigger a burst of display updates that is likely to saturate the I2C queue (e.g., rapidly send 20+ display update commands via animation scripts with 0ms delays).
3. Monitor serial output for "Failed to send display command to hardware queue" log messages.
4. If the log messages appear, note the free heap before and after the burst.

**Expected result:** If queue send failures occur, the `free(i2cMsg.data)` cleanup runs and heap does not leak. The error log messages themselves are expected under queue saturation — the fix ensures they are no longer accompanied by a memory leak.

---

## Test 4: ESP-NOW Peer Registration with Long Name (Task 6 — name bounds check)

**Target:** Master node

**Steps:**
1. Flash master node with Phase A firmware.
2. Using the configuration interface, attempt to register a padawan peer with a name longer than 15 characters (e.g., "VeryLongPeerName123").
3. Verify the peer appears in the peer list.
4. Verify the peer name is truncated to 15 characters ("VeryLongPeerNam").
5. Verify the node does not crash or exhibit stack corruption.

**Expected result:** Peer registers successfully with a truncated name. No crash, no buffer overflow. Names of 15 characters or fewer are stored unchanged.

---

## Test 5: GUID Format Visual Check (Task 1 — snprintf)

**Target:** Any node

**Steps:**
1. Flash node with Phase A firmware.
2. Trigger an action that generates a GUID (e.g., sending a command that creates a message with an origination ID).
3. Observe the GUID in serial monitor output.

**Expected result:** GUID appears in standard hex-dash format (e.g., `a1b2c3d4-e5f6-a7b8-c9d0-e1f2a3b4c5d6`). No truncation, no garbage characters. This is a smoke test — the `snprintf` change does not alter output for well-formed data, it only adds safety against hypothetical overflows.

---

## Edge Cases / Negative Tests

- **NvsManager setKeyId (Task 5):** If a peer index >= 100 is ever passed (currently impossible with the 20-peer limit, but defensive), the function logs an error and returns without writing to the key buffer. Verify by code inspection — no runtime test needed unless the peer limit is raised.
- **AnimationController destructor (Task 4):** The `AnimationCtrl` singleton is never destroyed during normal operation. The semaphore delete is a correctness fix for hypothetical teardown. No runtime test needed.
- **unique_ptr nullptr path:** Queue an animation, then immediately trigger a panic stop. The early return at the `cmd == nullptr` check (line 453 of main.cpp) should still work correctly — `unique_ptr` compares equal to `nullptr` when default-constructed. Verify by code inspection that the early return path does not call `delete`.
```

- [ ] **Step 2: Commit QA plan**

Run:
```bash
cd /home/jeff/Source/astros/AstrOs.ESP && git add .docs/qa/code-review-phase-a.md && git commit -m "qa: add manual test plan for code review Phase A fixes"
```

---

## Final Verification

- [ ] **Step 1: Build both board environments**

Run:
```bash
~/.platformio/penv/bin/pio run -e metro_s3 && ~/.platformio/penv/bin/pio run -e lolin_d32_pro
```

- [ ] **Step 2: Run native tests**

Run:
```bash
~/.platformio/penv/bin/pio test -e test
```

- [ ] **Step 3: Update plan — mark complete**

Check off this section's boxes in this plan file and commit the update.
