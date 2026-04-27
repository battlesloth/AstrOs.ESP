# Code Review — Phase C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Spec:** `.docs/plans/20260414-0954-code-review-phase-c.md` — read it first for design rationale.
**Goal:** Land the remaining P1/P2/P3 resilience items from the code review as a set of small, independently reviewable commits on branch `fix/code-review-phase-c`.
**Architecture:** Eight localized refactors; no cross-cutting semantic decisions. Biggest change (Task 2) replaces the animation `esp_timer` callback with a dedicated FreeRTOS task using `vTaskDelayUntil`.
**Tech Stack:** ESP-IDF / PlatformIO, FreeRTOS, C++17 (gnu++2a). Host-side native tests via googletest under `[env:test]`.

---

## Repository facts

- Current branch: `fix/code-review-phase-c` (already created)
- Primary files: `src/main.cpp` (7 tasks touch it), `lib/I2cMaster/src/I2cMaster.cpp`, `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp` (note: filename has typo, missing 'r'), `lib/AstrOsUtility/src/AstrOsStringUtils.hpp`
- Build commands: `pio run -e metro_s3`, `pio run -e lolin_d32_pro`, `pio test -e test`
- Commit convention: scoped short subject (e.g., `fix:`, `perf:`, `refactor:`), body explaining *why*, Co-Authored-By trailer

## Task execution order

Front-load the small mechanical wins so the big animation refactor (Task 2) lands on a branch that's already been partially validated:

1. Task 1 — queue depth bumps (mechanical)
2. Task 4 — button task stack bump (one-liner)
3. Task 6 — `stringFormat` `snprintf` (one-liner)
4. Task 3 — I2C leak fixes (bounded)
5. Task 7 — silent-error counters (additive)
6. Task 5 — storage manager error hardening
7. Task 2 — animation dispatch refactor (largest; kept last so prior commits are already green)
8. Task 8 — QA test plan

---

## Task 1: Queue depth bumps

**Files:**
- Modify: `src/main.cpp:236-244`

- [ ] **Step 1: Replace the queue creation block**

`src/main.cpp` line 236 is currently:

```cpp
    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_ani_cmd_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_svc_cmd_t));
    interfaceResponseQueue = xQueueCreate(QUEUE_LENGTH, sizeof(astros_interface_response_t));
    serialCh1Queue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_serial_msg_t));
    serialCh2Queue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_serial_msg_t));
    servoQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    i2cQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    gpioQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_msg_t));
    espnowQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_espnow_msg_t));
```

Replace with:

```cpp
    // Dispatch queues sized for fan-in from animationTimerCallback + producer bursts.
    // Control queues stay at QUEUE_LENGTH (5) since they carry low-rate coordination.
    animationQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_ani_cmd_t));
    serviceQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_svc_cmd_t));
    interfaceResponseQueue = xQueueCreate(QUEUE_LENGTH, sizeof(astros_interface_response_t));
    serialCh1Queue = xQueueCreate(10, sizeof(queue_serial_msg_t));
    serialCh2Queue = xQueueCreate(10, sizeof(queue_serial_msg_t));
    servoQueue = xQueueCreate(20, sizeof(queue_msg_t));
    i2cQueue = xQueueCreate(16, sizeof(queue_msg_t));
    gpioQueue = xQueueCreate(10, sizeof(queue_msg_t));
    espnowQueue = xQueueCreate(QUEUE_LENGTH, sizeof(queue_espnow_msg_t));
```

- [ ] **Step 2: Build both board environments**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: both complete with no new warnings attributable to the change.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
perf: bump dispatch queue depths to reduce send-fail under bursty animation frames

servoQueue 5→20 (highest churn, fan-in from animation + Maestro),
i2cQueue 5→16 (shared by OLED writes and animation I2C),
serialCh1/Ch2/gpio 5→10 (double current for slack).
Control queues unchanged — they carry low-rate coordination traffic.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `button_listener_task` stack bump

**Files:**
- Modify: `src/main.cpp:199`

- [ ] **Step 1: Change the stack size**

Line 199 is currently:

```cpp
    xTaskCreatePinnedToCore(&buttonListenerTask, "button_listener_task", 2048, (void *)serviceQueue, 5, NULL, 1);
```

Change `2048` to `4096`:

```cpp
    xTaskCreatePinnedToCore(&buttonListenerTask, "button_listener_task", 4096, (void *)serviceQueue, 5, NULL, 1);
```

- [ ] **Step 2: Build both envs**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
```

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
fix: bump button_listener_task stack 2048→4096 to match other I/O tasks

Task does GPIO reads, formatted logging, and queue sends; 2 KB is tight
and the existing 500-byte HWM warning suggests it sits close to the edge.
4 KB aligns with the other I/O consumer tasks in the codebase.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: `stringFormat` `std::sprintf` → `std::snprintf`

**Files:**
- Modify: `lib/AstrOsUtility/src/AstrOsStringUtils.hpp:155`

- [ ] **Step 1: Change `sprintf` to `snprintf` with explicit bound**

Line 151-157 is currently:

```cpp
    template <typename... Args> static std::string stringFormat(const std::string &format, Args &&...args)
    {
        auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
        std::string output(size + 1, '\0');
        std::sprintf(&output[0], format.c_str(), std::forward<Args>(args)...);
        return output;
    }
```

Replace line 155 (`std::sprintf(...)` call) with a bounded `std::snprintf`:

```cpp
    template <typename... Args> static std::string stringFormat(const std::string &format, Args &&...args)
    {
        auto size = std::snprintf(nullptr, 0, format.c_str(), std::forward<Args>(args)...);
        std::string output(size + 1, '\0');
        std::snprintf(&output[0], size + 1, format.c_str(), std::forward<Args>(args)...);
        return output;
    }
```

- [ ] **Step 2: Build both envs + run native tests**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
pio test -e test
```

Expected: native tests pass (`stringFormat` is used transitively in `lib/AstrOsMessaging` which has unit test coverage).

- [ ] **Step 3: Commit**

```bash
git add lib/AstrOsUtility/src/AstrOsStringUtils.hpp
git commit -m "$(cat <<'EOF'
fix: replace std::sprintf with bounded std::snprintf in stringFormat

The preceding snprintf(nullptr, 0, ...) computes the exact needed size,
but using sprintf on the output buffer offers no bounds protection if
a future edit ever miscomputes size. Defense in depth; no behavior change
when size is correct.

Note: lib/Uuid/guid.h already uses snprintf — the code review's #28 flag
for that file was stale. This commit addresses the real remaining sprintf
site in the codebase.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: I2C `cmd` handle leak fixes

**Files:**
- Modify: `lib/I2cMaster/src/I2cMaster.cpp`

### Step-by-step

- [ ] **Step 1: Fix `DeviceExists` — add missing delete (lines 44-59)**

Current:

```cpp
bool I2cMaster::DeviceExists(uint8_t addr)
{
    esp_err_t res = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, 1 /* expect ack */);
        i2c_master_stop(cmd);

        res = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
        xSemaphoreGive(i2cBusMutext);
    }
    return res == ESP_OK;
}
```

Replace with:

```cpp
bool I2cMaster::DeviceExists(uint8_t addr)
{
    esp_err_t res = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, 1 /* expect ack */);
        i2c_master_stop(cmd);

        res = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        xSemaphoreGive(i2cBusMutext);
    }
    return res == ESP_OK;
}
```

- [ ] **Step 2: Fix `ReadWord` — remove stray create + double-delete (lines 190-243)**

Current shape (abbreviated — see file for full context):

```cpp
        // ... first cmd: write register ...
        i2c_cmd_link_delete(cmd);                     // line 202
        if (err != ESP_OK) { ...return false; }
        cmd = i2c_cmd_link_create();                  // line 209 — LEAK: never deleted
        i2c_master_start(cmd);                        // line 210

        uint8_t buffer1;
        uint8_t buffer2;

        cmd = i2c_cmd_link_create();                  // line 215 — overwrites leaked handle
        i2c_master_start(cmd);
        // ... read both bytes ...
        err = i2c_master_cmd_begin(...);              // line 221
        i2c_cmd_link_delete(cmd);                     // line 222
        if (err != ESP_OK) { ...return false; }
        *data = (buffer1 << 8) | buffer2;
        i2c_cmd_link_delete(cmd);                     // line 233 — DOUBLE-FREE
```

Remove lines 209, 210, and 233. The cleaned function body (replacing lines 190-243) is:

```cpp
bool I2cMaster::ReadWord(uint8_t addr, uint8_t registerAddr, uint16_t *data)
{
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        uint8_t buffer1;
        uint8_t buffer2;

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, &buffer1, (i2c_ack_type_t)ACK_VAL);
        i2c_master_read_byte(cmd, &buffer2, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        *data = (buffer1 << 8) | buffer2;

        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to read from device: %s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}
```

- [ ] **Step 3: Fix `ReadTwoWords` — same pattern (lines 245-316)**

The function has the same leak+double-free shape: stray create at 264-265 (leak) and duplicate delete at line 306. Replace the function body (lines 245-316) with:

```cpp
bool I2cMaster::ReadTwoWords(uint8_t addr, uint8_t registerAddr, uint16_t *data1, uint16_t *data2)
{
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(i2cBusMutext, pdMS_TO_TICKS(SEMAPHORE_TIMEOUT)) == pdTRUE)
    {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, ACK_CHECK_EN);
        i2c_master_write_byte(cmd, registerAddr, ACK_CHECK_EN);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to write to device: %s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        uint8_t buffer1;
        uint8_t buffer2;

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, &buffer1, (i2c_ack_type_t)ACK_VAL);
        i2c_master_read_byte(cmd, &buffer2, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device:%s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        *data1 = (buffer1 << 8) | buffer2;

        cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, ACK_CHECK_EN);
        i2c_master_read_byte(cmd, &buffer1, (i2c_ack_type_t)ACK_VAL);
        i2c_master_read_byte(cmd, &buffer2, (i2c_ack_type_t)NACK_VAL);
        i2c_master_stop(cmd);
        err = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (err != ESP_OK)
        {
            ESP_LOGE("I2C", "Failed to read from device:%s", esp_err_to_name(err));
            xSemaphoreGive(i2cBusMutext);
            return false;
        }

        *data2 = (buffer1 << 8) | buffer2;

        xSemaphoreGive(i2cBusMutext);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE("I2C", "Failed to read from device:%s", esp_err_to_name(err));
    }

    return err == ESP_OK;
}
```

- [ ] **Step 4: Verify no other `i2c_cmd_link_create` sites have unpaired cleanup**

Run the verification grep (via the Grep tool, not shell `grep`):

- Pattern: `i2c_cmd_link_create|i2c_cmd_link_delete`
- Path: `lib/I2cMaster/src/I2cMaster.cpp`

Expected: every `i2c_cmd_link_create` (excluding the commented-out migration block starting at line 318) has a matching `i2c_cmd_link_delete` on every path. `WriteByte`, `WriteWord`, `WriteTwoWords`, `ReadByte` already satisfy this.

- [ ] **Step 5: Build both envs**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build; no warnings about unused handles or double-free.

- [ ] **Step 6: Commit**

```bash
git add lib/I2cMaster/src/I2cMaster.cpp
git commit -m "$(cat <<'EOF'
fix: eliminate I2C cmd-handle leaks and double-free in I2cMaster

Three specific bugs fixed:
- DeviceExists never called i2c_cmd_link_delete on its handle.
- ReadWord leaked one cmd (line 209 create never deleted, overwritten by
  another create at 215) and then double-freed another at line 233.
- ReadTwoWords had the same leak-plus-double-free shape at 264/306.

No change to control flow on the success path; error-path semantics
preserved (log, give semaphore, return false).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Silent-error counters

**Files:**
- Modify: `src/main.cpp` (globals block around line 88, `maintenanceTimerCallback` at line 474, `astrosRxTask` at line 908, `espnowRecvCallback` at line 1632)

- [ ] **Step 1: Add the counter globals near the other timer globals**

In the globals block starting at `src/main.cpp:88`, after the existing `static esp_timer_handle_t ...;` declarations (around line 95), insert:

```cpp
// Silent-error counters — incremented from cross-task contexts (Wi-Fi driver task
// for espnowRecvCallback, astrosRxTask on core 0). Read from maintenanceTimerCallback
// (esp_timer task). std::atomic<uint32_t> is single-word on ESP32 and provides the
// memory visibility we need without a mutex on the hot paths.
static std::atomic<uint32_t> astrosRxOverflowCount{0};
static std::atomic<uint32_t> espnowMallocFailureCount{0};
```

Verify `<atomic>` is already included in `main.cpp` (it should be — Phase B added it for other atomic globals). If not, add `#include <atomic>` with the other includes at the top.

- [ ] **Step 2: Update `maintenanceTimerCallback` to log the counters**

Current (line 474):

```cpp
static void maintenanceTimerCallback(void *arg)
{
    ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());
}
```

Replace with:

```cpp
static void maintenanceTimerCallback(void *arg)
{
    ESP_LOGI(TAG, "RAM left %lu", esp_get_free_heap_size());

    uint32_t rxOverflow = astrosRxOverflowCount.load(std::memory_order_relaxed);
    uint32_t espnowMallocFail = espnowMallocFailureCount.load(std::memory_order_relaxed);
    if (rxOverflow != 0 || espnowMallocFail != 0)
    {
        ESP_LOGW(TAG, "err-counters rx-overflow=%" PRIu32 " espnow-malloc-fail=%" PRIu32,
                 rxOverflow, espnowMallocFail);
    }
}
```

If `<cinttypes>` is not already included (it provides `PRIu32`), add `#include <cinttypes>` at the top of the file.

- [ ] **Step 3: Increment on buffer overflow in `astrosRxTask`**

Current (line 944-948):

```cpp
                    if (bufferIndex >= bufferLength)
                    {
                        ESP_LOGW("AstrOs RX", "Buffer overflow");
                        bufferIndex = 0;
                    }
```

Replace with:

```cpp
                    if (bufferIndex >= bufferLength)
                    {
                        ESP_LOGW("AstrOs RX", "Buffer overflow");
                        astrosRxOverflowCount.fetch_add(1, std::memory_order_relaxed);
                        bufferIndex = 0;
                    }
```

- [ ] **Step 4: Increment on malloc failure in `espnowRecvCallback`**

Current (line 1650-1655):

```cpp
    msg.data = (uint8_t *)malloc(len);
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
```

Replace with:

```cpp
    msg.data = (uint8_t *)malloc(len);
    if (msg.data == NULL)
    {
        ESP_LOGE(TAG, "Malloc receive data fail");
        espnowMallocFailureCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
```

- [ ] **Step 5: Build both envs**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
fix: add silent-error counters for RX overflow and ESP-NOW malloc failure

Two sites previously dropped errors with only a log line, observable only
if someone happened to be watching the serial monitor:
- astrosRxTask buffer overflow (oversized incoming message discarded)
- espnowRecvCallback malloc failure (incoming packet dropped)

Counters are atomic<uint32_t> (single-word on ESP32, cross-task safe with
relaxed ordering) and are logged as a single line from the existing 10-second
maintenance timer when either is non-zero. Zero-count case is silent to
avoid log noise on a healthy system.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `AstrOsStorageManager` error hardening

**Files:**
- Modify: `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp` (note: filename is missing an 'r' — historical typo)
- Modify: `lib/AstrOsStorageManager/include/AstrOsStorageManager.hpp` (signature change for `formatSdCard`)
- Modify: `src/main.cpp` (single caller of `formatSdCard` — find via grep)

### Step-by-step

- [ ] **Step 1: Add `isPathSafe` helper — header declaration**

In `lib/AstrOsStorageManager/include/AstrOsStorageManager.hpp`, in the class's private section (or public if other code calls it — private is preferred since it's a defensive check internal to the storage manager), declare:

```cpp
    // Returns true if the supplied (potentially externally-sourced) path is safe
    // to combine with the mount prefix. Rejects ".." traversal, absolute paths,
    // "//" collapse, and paths that would overflow the downstream buffer.
    static bool isPathSafe(const std::string &path);
```

Place the declaration with the other private static helpers.

- [ ] **Step 2: Add `isPathSafe` implementation**

In `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp`, near the top of the file with the other static helpers, add:

```cpp
bool AstrOsStorageManager::isPathSafe(const std::string &path)
{
    if (path.empty())
    {
        ESP_LOGE(TAG, "isPathSafe: empty path rejected");
        return false;
    }
    if (path[0] == '/')
    {
        ESP_LOGE(TAG, "isPathSafe: absolute path rejected: %s", path.c_str());
        return false;
    }
    if (path.find("..") != std::string::npos)
    {
        ESP_LOGE(TAG, "isPathSafe: traversal component rejected: %s", path.c_str());
        return false;
    }
    if (path.find("//") != std::string::npos)
    {
        ESP_LOGE(TAG, "isPathSafe: double-slash rejected: %s", path.c_str());
        return false;
    }
    // Cap at a conservative length leaving room for the mount prefix and null.
    // The mount prefix is MOUNT_POINT (usually "/sdcard") plus a '/' separator.
    constexpr size_t MAX_PATH_LEN = 128;
    if (path.size() > MAX_PATH_LEN)
    {
        ESP_LOGE(TAG, "isPathSafe: path too long (%zu > %zu): %s",
                 path.size(), MAX_PATH_LEN, path.c_str());
        return false;
    }
    return true;
}
```

- [ ] **Step 3: Gate every externally-sourced path call through `isPathSafe`**

The path-constructing helper `setFilePath` is called by `saveFileSd`, `deleteFileSd`, `fileExistsSd`, `readFileSd`, `saveFileSpiffs`, `deleteFileSpiffs`, `fileExistsSpiffs`, `readFileSpiffs`, `listFilesSpiffs`. For each of the SD-card and SPIFFS entrypoints that take a `filename` or `folder` parameter from external input, insert `isPathSafe` at the top before `setFilePath`:

Example for `saveFileSd` (around line 570):

```cpp
bool AstrOsStorageManager::saveFileSd(std::string filename, std::string data)
{
    if (!isPathSafe(filename))
    {
        return false;
    }
    FILE *fd = NULL;
    std::string path = AstrOsStorageManager::setFilePath(filename);
    // ... rest unchanged ...
}
```

Do the same at the top of each of:
- `saveFileSd`, `deleteFileSd`, `fileExistsSd`, `readFileSd` (SD card entrypoints)
- `saveFileSpiffs`, `deleteFileSpiffs`, `fileExistsSpiffs`, `readFileSpiffs`, `listFilesSpiffs` (SPIFFS entrypoints)

For `readFileSd` and `readFileSpiffs` which return `std::string` instead of `bool`, return `"error"` on reject (matches their existing `"error"` failure contract).

For `listFilesSd` and `listFilesSpiffs` (list operations), also gate on `isPathSafe` — return empty vector on reject.

- [ ] **Step 4: Check `mkdir` return values in `formatSdCard`**

Current (lines 493-495):

```cpp
    mkdir(SCRIPTS_FOLDER, 0777);
    mkdir(MAESTRO_FOLDER, 0777);
    mkdir(GPIO_FOLDER, 0777);
```

Replace with:

```cpp
    for (const char *folder : {SCRIPTS_FOLDER, MAESTRO_FOLDER, GPIO_FOLDER})
    {
        if (mkdir(folder, 0777) != 0 && errno != EEXIST)
        {
            ESP_LOGE(TAG, "Failed to create folder %s: errno=%d (%s)",
                     folder, errno, strerror(errno));
            free(workbuf);
            return ESP_FAIL;  // will become esp_err_t after Step 5
        }
    }
```

Add `#include <errno.h>` and `#include <string.h>` at the top of the file if not already present (likely already present).

- [ ] **Step 5: Change `formatSdCard` signature to `esp_err_t`**

In `lib/AstrOsStorageManager/include/AstrOsStorageManager.hpp` line 61, change:

```cpp
    bool formatSdCard();
```

to:

```cpp
    esp_err_t formatSdCard();
```

In `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp` line 461, replace the function definition:

```cpp
esp_err_t AstrOsStorageManager::formatSdCard()
{
    char drv[3] = {'0', ':', 0};
    const size_t workbuf_size = 4096;
    void *workbuf = NULL;
    ESP_LOGI(TAG, "Formatting the SD card");

    size_t allocation_unit_size = 16 * 1024;

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL)
    {
        ESP_LOGE(TAG, "Error formatting SD card: work-buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(card->csd.sector_size, allocation_unit_size);

    MKFS_PARM param;
    param.fmt = FM_ANY;
    param.au_size = alloc_unit_size;

    FRESULT res = f_mkfs(drv, &param, workbuf, workbuf_size);
    if (res != FR_OK)
    {
        ESP_LOGE(TAG, "Error formatting SD card: f_mkfs failed (%d)", res);
        free(workbuf);
        return ESP_FAIL;
    }

    free(workbuf);

    for (const char *folder : {SCRIPTS_FOLDER, MAESTRO_FOLDER, GPIO_FOLDER})
    {
        if (mkdir(folder, 0777) != 0 && errno != EEXIST)
        {
            ESP_LOGE(TAG, "Failed to create folder %s: errno=%d (%s)",
                     folder, errno, strerror(errno));
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Successfully formatted the SD card");
    return ESP_OK;
}
```

Note: the previous error path at line 488 returned `false` *without* freeing `workbuf` — that was a pre-existing leak. The replacement above fixes it.

- [ ] **Step 6: Update the single `formatSdCard` caller in `main.cpp`**

Find the caller via a Grep for `formatSdCard`. It lives in `handleFormatSD` (called from the service queue task). The caller currently expects `bool` — update it to handle `esp_err_t`:

Locate the call site, currently of the form:

```cpp
    if (AstrOs_Storage.formatSdCard())
    {
        // success path
    }
    else
    {
        // failure path
    }
```

Replace with:

```cpp
    esp_err_t formatErr = AstrOs_Storage.formatSdCard();
    if (formatErr == ESP_OK)
    {
        // success path
    }
    else
    {
        ESP_LOGE(TAG, "formatSdCard failed: %s", esp_err_to_name(formatErr));
        // failure path — surface distinct error if the interface response carries one
    }
```

If `handleFormatSD` sends an interface response with a message field, include `esp_err_to_name(formatErr)` so the web UI sees which failure occurred. Read the current `handleFormatSD` body to adapt to the exact shape of the response it sends.

- [ ] **Step 7: Audit `fopen`/`fclose` sites — confirm no regression**

Grep for `fopen|fclose` in `lib/AstrOsStorageManager/src/AstOsStorageManager.cpp`. Current state (from pre-plan exploration):

- Line 584 (`saveFileSd`): `if (!fd) return false;` ✓
- Line 631 (`readFileSd`): `if (f == NULL) return "error";` ✓
- Line 712 (`saveFileSpiffs`): `if (f == NULL) return false;` ✓
- Line 804 (`readFileSpiffs`): `if (file == NULL) return "error";` ✓

All are protected. This step is a confirmation audit — no change to files unless a new unprotected site is discovered during the audit. If so, add the `NULL` check before use.

- [ ] **Step 8: Build both envs**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build.

- [ ] **Step 9: Commit**

```bash
git add lib/AstrOsStorageManager/ src/main.cpp
git commit -m "$(cat <<'EOF'
fix: AstrOsStorageManager error hardening — path safety, mkdir checks, format error codes

Four code-review items consolidated into one pass:

- Add isPathSafe helper rejecting .. traversal, absolute paths, //,
  and overlong paths. Gate every externally-sourced path entrypoint
  through it (SD and SPIFFS load/save/delete/exists/list).
- Check mkdir return values in formatSdCard; previously ignored.
- Convert formatSdCard bool→esp_err_t so callers can distinguish
  work-buffer OOM from format failure.
- Fix pre-existing workbuf leak on the f_mkfs-failed path.

Update the single caller in main.cpp's handleFormatSD to match.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Animation dispatch refactor — drop `esp_timer`

**Files:**
- Modify: `src/main.cpp` (delete `animationTimer` global and `animationTimerCallback`; add `animationDispatchTask`; wire up task creation; remove timer setup)

Port the existing `animationTimerCallback` body verbatim into a new FreeRTOS task, driven by `vTaskDelayUntil` using `AnimationCtrl.msTillNextServoCommand()` as the wake delta. Preserve all malloc/free ownership semantics.

### Step-by-step

- [ ] **Step 1: Remove the `animationTimer` global (line 92)**

Delete line 92:

```cpp
static esp_timer_handle_t animationTimer;
```

- [ ] **Step 2: Remove the forward declaration of `animationTimerCallback` (line 150)**

Delete line 150:

```cpp
static void animationTimerCallback(void *arg);
```

- [ ] **Step 3: Add forward declaration of the new task**

In the task declarations section near the other `void <name>Task(void *arg);` forward decls, add:

```cpp
void animationDispatchTask(void *arg);
```

- [ ] **Step 4: Remove the `animationTimer` creation and start in `initTimers` (lines 363-367 and 386-388)**

Delete the timer-args block for `animation`:

```cpp
    const esp_timer_create_args_t aTimerArgs = {.callback = &animationTimerCallback,
                                                .arg = NULL,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "animation",
                                                .skip_unhandled_events = true};
```

And delete the create + start + log lines:

```cpp
    ESP_ERROR_CHECK(esp_timer_create(&aTimerArgs, &animationTimer));
    ESP_ERROR_CHECK(esp_timer_start_once(animationTimer, 5 * 1000 * 1000));
    ESP_LOGI("init_timer", "Started animation timer");
```

- [ ] **Step 5: Replace the `animationTimerCallback` body with `animationDispatchTask`**

Delete the entire function body (lines 479-626, starting `static void animationTimerCallback` and ending at its closing `}`), and replace with the new task. Drop-in replacement (keep at approximately the same file location so diffs are tidy):

```cpp
void animationDispatchTask(void *arg)
{
    TickType_t lastWake = xTaskGetTickCount();
    constexpr uint32_t MIN_WAKE_MS = 10;
    constexpr uint32_t IDLE_WAKE_MS = 250;

    while (1)
    {
        auto highWaterMark = uxTaskGetStackHighWaterMark(NULL);
        if (highWaterMark < 500)
        {
            ESP_LOGW(TAG, "Animation Dispatch Stack HWM: %d", highWaterMark);
        }

        uint32_t nextDelayMs = IDLE_WAKE_MS;

        if (AnimationCtrl.scriptIsLoaded())
        {
            auto cmd = AnimationCtrl.getNextCommandPtr();

            if (cmd == nullptr)
            {
                // getNextCommandPtr returns nullptr only on mutex timeout, in which
                // case it has also set scriptLoaded=false (halting the current
                // sequence — see AnimationController.cpp for the safety rationale).
                // We loop normally; on the next iteration scriptIsLoaded() returns
                // false and we take the idle wake interval until a future queueScript
                // resumes animation. Without this, a single transient mutex
                // contention would require a device reboot to restore animation
                // — bad, because third-party hardware may be in a non-safe state
                // that a recovery script (not a power-cycle) needs to address.
                ESP_LOGE(TAG, "Animation command pointer is null — script halted, dispatch task idling for recovery");
                nextDelayMs = IDLE_WAKE_MS;
            }
            else
            {
                MODULE_TYPE ct = cmd->type;
                std::string val = cmd->val;
                int module = cmd->module;

                switch (ct)
                {
                case MODULE_TYPE::NONE:
                {
                    ESP_LOGI(TAG, "NONE command queued, assume buffer?");
                    break;
                }
                case MODULE_TYPE::KANGAROO:
                case MODULE_TYPE::GENERIC_SERIAL:
                {
                    ESP_LOGI(TAG, "Serial command val: %s", val.c_str());

                    // replace any occurances of \n with actual new line character
                    std::string formatted;
                    for (size_t i = 0; i < val.size(); i++)
                    {
                        if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'n')
                        {
                            formatted += '\n';
                            i++;
                        }
                        else if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'r')
                        {
                            formatted += '\r';
                            i++;
                        }
                        else
                        {
                            formatted += val[i];
                        }
                    }

                    queue_serial_msg_t serialMsg;
                    serialMsg.message_id = 0;
                    serialMsg.data = (uint8_t *)malloc(formatted.size() + 1);
                    memcpy(serialMsg.data, formatted.c_str(), formatted.size());
                    serialMsg.data[formatted.size()] = '\0';

                    if (module == 1)
                    {
                        if (xQueueSend(serialCh1Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                        {
                            ESP_LOGW(TAG, "Send serial queue fail");
                            free(serialMsg.data);
                        }
                    }
                    else if (module == 2)
                    {
                        if (xQueueSend(serialCh2Queue, &serialMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                        {
                            ESP_LOGW(TAG, "Send serial queue fail");
                            free(serialMsg.data);
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Invalid serial module %d", module);
                        free(serialMsg.data);
                    }
                    break;
                }
                case MODULE_TYPE::MAESTRO:
                {
                    ESP_LOGI(TAG, "Maestro command val: %s", val.c_str());
                    queue_msg_t servoMsg;
                    servoMsg.message_id = module;
                    servoMsg.data = (uint8_t *)malloc(val.size() + 1);
                    memcpy(servoMsg.data, val.c_str(), val.size());
                    servoMsg.data[val.size()] = '\0';

                    if (xQueueSend(servoQueue, &servoMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send servo queue fail");
                        free(servoMsg.data);
                    }
                    break;
                }
                case MODULE_TYPE::I2C:
                {
                    ESP_LOGI(TAG, "I2C command val: %s", val.c_str());
                    queue_msg_t i2cMsg;
                    i2cMsg.message_id = 0;
                    i2cMsg.data = (uint8_t *)malloc(val.size() + 1);
                    memcpy(i2cMsg.data, val.c_str(), val.size());
                    i2cMsg.data[val.size()] = '\0';

                    if (xQueueSend(i2cQueue, &i2cMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send i2c queue fail");
                        free(i2cMsg.data);
                    }
                    break;
                }
                case MODULE_TYPE::GPIO:
                {
                    ESP_LOGI(TAG, "GPIO command val: %s", val.c_str());
                    queue_msg_t gpioMsg;
                    gpioMsg.data = (uint8_t *)malloc(val.size() + 1);
                    memcpy(gpioMsg.data, val.c_str(), val.size());
                    gpioMsg.data[val.size()] = '\0';

                    if (xQueueSend(gpioQueue, &gpioMsg, pdMS_TO_TICKS(2000)) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Send gpio queue fail");
                        free(gpioMsg.data);
                    }
                    break;
                }
                default:
                    break;
                }

                uint32_t scriptDelay = AnimationCtrl.msTillNextServoCommand();
                nextDelayMs = (scriptDelay < MIN_WAKE_MS) ? MIN_WAKE_MS : scriptDelay;
            }
        }
        else
        {
            nextDelayMs = IDLE_WAKE_MS;
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(nextDelayMs));
    }
}
```

- [ ] **Step 6: Create the task during setup**

Find the task creation cluster (near the other `xTaskCreatePinnedToCore` calls, around lines 195-215). Add:

```cpp
    xTaskCreatePinnedToCore(&animationDispatchTask, "animation_dispatch_task", 4096, NULL, 5, NULL, 1);
```

Pin to core 1 (same as most other I/O tasks), priority 5, stack 4 KB. The task takes no argument.

- [ ] **Step 7: Verify no stale references to `animationTimer`**

Run a Grep for `animationTimer` in `src/main.cpp`. Expected: zero matches after this task. If any remain (e.g., in a `servoMoveTimerCallback` or other callback), evaluate case-by-case — likely they were already unrelated references that only matched the global.

- [ ] **Step 8: Build both envs**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
```

Expected: clean build.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "$(cat <<'EOF'
refactor: move animation dispatch off esp_timer onto a dedicated FreeRTOS task

Animation dispatch previously ran in the shared esp_timer service task via
animationTimerCallback — heavy malloc/memcpy/string-format/xQueueSend work
on a stack the esp_timer task sizes for quick-return callbacks. Replace
with animationDispatchTask pinned to core 1, 4 KB stack, loop driven by
vTaskDelayUntil using msTillNextServoCommand() as the wake delta.

Behavior preserved verbatim: malloc/free ownership, null-cmd recovery
branch, 250 ms idle wake, all five dispatch channels. Drops one
esp_timer handle from the system.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: QA test plan

**Files:**
- Create: `.docs/qa/code-review-phase-c.md`

- [ ] **Step 1: Write the QA test plan**

Create `.docs/qa/code-review-phase-c.md` with the following structure:

```markdown
# Phase C QA Test Plan

**Prerequisites:**
- A flashed master node and at least one padawan node.
- SD card mounted with deployed scripts covering all five channel types (serial, servo/Maestro, I2C, GPIO, NONE).
- Serial monitor open on both boards, `esp32_exception_decoder` filter active.
- Web UI accessible for script deploy / panic-stop.

## 1. Queue-depth smoke test (Task 1)

**Goal:** Confirm bumped queue depths eliminate transient "Send X queue fail" warnings under normal animation load.

1. Deploy and run a multi-channel animation script that exercises serial + servo + I2C + GPIO simultaneously.
2. Play the script back-to-back 10 times.

**Expected:** Zero `Send serial queue fail`, `Send servo queue fail`, `Send i2c queue fail`, `Send gpio queue fail` warnings across the 10 runs on either node.

## 2. Animation dispatch refactor (Task 2)

**Goal:** Confirm dispatch cadence, panic-stop responsiveness, and script chaining behave equivalently to the pre-refactor timer-driven implementation.

1. **Cadence timing.** Run a script with known-interval commands (e.g. servo moves every 200 ms for 30 iterations). Timestamp each `ESP_LOGI "Maestro command val"` and compute actual inter-command deltas.
   - **Expected:** 200 ms ± 10 ms per step. No drift across the 30 iterations.
2. **Panic-stop mid-script.** Start a long-running script (30+ s). Issue panic-stop from the web UI mid-playback.
   - **Expected:** Dispatch stops within one delay cycle (~250 ms at worst). No queued servo/serial/I2C/GPIO commands dispatched after the panic-stop log line.
3. **Script chaining.** Queue script A (short). When it completes, queue script B. Repeat A then C with no gap.
   - **Expected:** Each script plays to completion. No missed first command on the second script.
4. **Idle to active.** Let the device sit idle (no script) for 2 minutes. Queue a script.
   - **Expected:** Script begins within 250 ms of the queueScript call (the idle wake interval).
5. **Animation Dispatch Stack HWM.** Monitor for `Animation Dispatch Stack HWM:` warnings throughout.
   - **Expected:** No warnings (high-water-mark stays above 500 bytes).

## 3. I2C leak fixes (Task 3)

**Goal:** Confirm no heap regression when I2C operations fail repeatedly.

1. Power the node with a PCA9685 board physically disconnected on the bus.
2. Let it run for 5 minutes.
3. Observe `RAM left` log from the maintenance timer at 10 s intervals.

**Expected:** `RAM left` value stays within ±200 bytes of its starting value across the run. No unbounded decline.

## 4. Button task stack (Task 4)

**Goal:** Confirm 4 KB stack is adequate for the reset-button path.

1. Flash the firmware. Leave the device powered with no further actions for 2 minutes.
2. Press the reset button briefly (< 3 s).
3. Press and hold for 4 s (medium press).
4. Press and hold for 11 s (long press).

**Expected:** No `button_listener_task Stack HWM:` warnings in any log. Each press produces its expected side effect (reboot, etc.).

## 5. Storage manager error hardening (Task 5)

**Goal:** Confirm path sanitization, mkdir checks, and format error codes behave as specified.

1. **Path traversal rejection.** Deploy a script named `../foo.scr`.
   - **Expected:** Deploy NAK or storage layer logs `isPathSafe: traversal component rejected`. No file written outside `/sdcard/scripts/`.
2. **Absolute path rejection.** Deploy a script named `/etc/passwd`.
   - **Expected:** Same as above, with `isPathSafe: absolute path rejected`.
3. **Overlong path.** Deploy a script with a name of 200 characters.
   - **Expected:** `isPathSafe: path too long` in log, deploy fails.
4. **Full-SD deploy.** Fill the SD card to near capacity (within 1 KB). Deploy a new script.
   - **Expected:** `fwrite` or `fopen` failure logged; storage layer returns error; caller surfaces a meaningful NAK to the web UI.
5. **Format with no card present.** Remove the SD card and issue a format command from the web UI.
   - **Expected:** `formatSdCard failed:` log line with distinct `ESP_ERR_NO_MEM` (work-buffer alloc) or `ESP_FAIL` (f_mkfs) error name.
6. **Normal deploy regression.** Deploy, run, delete a valid script.
   - **Expected:** All three operations succeed with no warnings.

## 6. `stringFormat` snprintf (Task 6)

**Goal:** Native unit tests still pass; no observable runtime regression.

1. Run `pio test -e test`.

**Expected:** All tests pass.

## 7. Error counters (Task 7)

**Goal:** Confirm counters increment on induced errors and show up in the maintenance log.

1. **RX overflow.** From the connected host (via the AstrOs interface UART), send a message longer than 2000 bytes without a newline terminator.
   - **Expected:** `AstrOs RX Buffer overflow` warning. Within 10 s, a maintenance log line of the form `err-counters rx-overflow=1 espnow-malloc-fail=0`.
2. **ESP-NOW malloc failure.** Difficult to induce deliberately; verify indirectly by observing the counter stays at 0 during normal operation.
   - **Expected:** `espnow-malloc-fail=0` in any counter log line during an un-stressed session.
3. **No-error idle.** Leave the device idle with no induced errors for 60 s.
   - **Expected:** No `err-counters` log lines emitted (both counters zero → log suppressed).

## 8. Regression sweep

Run these after all Phase C commits land, before merging:

1. Full animation playback across all five dispatch channels.
2. Master node polling of a padawan for 5 minutes, confirming no poll NAK escalations.
3. OLED display updates during an active animation (shows timeout / countdown as expected).
4. Fresh peer registration from the web UI.
5. Panic-stop → queueScript recovery cycle.

**Expected:** All behavior matches pre-Phase-C reference baseline. No new error logs, no heap decline over 10 minutes of mixed activity.
```

- [ ] **Step 2: Commit the QA plan**

```bash
git add .docs/qa/code-review-phase-c.md
git commit -m "$(cat <<'EOF'
docs: add QA test plan for code review Phase C

Covers all eight tasks: queue-depth smoke test, animation dispatch
cadence + panic-stop + chaining, I2C leak heap-stability, button
stack HWM, storage path traversal + mkdir + format error codes,
stringFormat native tests, error counters, and regression sweep.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Final verification

- [ ] **All build environments green**

```
pio run -e metro_s3
pio run -e lolin_d32_pro
pio test -e test
```

Expected: all three succeed with no new warnings.

- [ ] **No stale `animationTimer` references**

Grep `src/main.cpp` for `animationTimer`. Expected: zero matches.

- [ ] **No stale `sprintf` (bare) in util code**

Grep `lib/AstrOsUtility` for `std::sprintf`. Expected: zero matches.

- [ ] **Pre-commit hook runs clean on all commits**

The project uses `clang-format` in `.githooks/pre-commit`. Every commit in this plan should have passed without manual touchup. Spot-check by running `git log --oneline fix/code-review-phase-c ^develop` and confirming each commit made it through.

- [ ] **PR ready**

Branch `fix/code-review-phase-c` contains 10 commits (9 implementation + 1 QA plan) plus the two documentation commits from the brainstorming phase. Title the PR `fix: code-review Phase C — resilience & defensive hardening`; link to `.docs/plans/20260414-0954-code-review-phase-c.md` and `.docs/qa/code-review-phase-c.md` in the body.
