# AstrOs.ESP — Comprehensive Code Review

**Date:** April 11, 2026  
**Scope:** Full codebase — `src/`, `lib/`, `components/`, build configuration  
**Platform:** ESP-IDF / PlatformIO, targeting ESP32 (lolin_d32_pro) and ESP32-S3 (metro_s3)

---

## Executive Summary

AstrOs.ESP is an ESP-IDF C/C++ firmware that orchestrates animatronics hardware via ESP-NOW mesh networking, serial communication, I2C peripherals (PCA9685 servo drivers, OLED display), and GPIO. The application creates **11 FreeRTOS tasks** pinned across two cores, **9 message queues**, and **4 ESP timers** to coordinate animation playback, peer discovery, and hardware control.

The codebase demonstrates solid architectural decisions — core pinning, queue-based task communication, stack watermark monitoring, and clear message-type enums. However, the review identified **10 critical (P0) issues**, primarily around **memory leaks in queue consumer paths**, **race conditions on shared state**, and **buffer overflow risks**. Several of these are on hot paths (2-second polling timer, every ESP-NOW receive) and will cause progressive heap exhaustion on a memory-constrained ESP32.

### Issue Count by Severity

| Severity | Count | Categories |
|----------|-------|------------|
| P0 — Critical | 10 | Memory leaks, race conditions, buffer overflows |
| P1 — High | 6 | Resource leaks, uncaught exceptions, deadlock risk |
| P2 — Medium | 8 | Stack sizes, queue depth, timer callback complexity, filesystem |
| P3 — Low | 3 | Non-atomic flags, GUID buffer, silent error swallowing |

---

## P0 — Critical Issues

### Memory Leaks

#### 1. `pollingTimerCallback()` — fingerprint never freed

**File:** `src/main.cpp` — `pollingTimerCallback()`

```cpp
char *fingerprint = (char *)malloc(37);
AstrOs_Storage.getControllerFingerprint(fingerprint);
AstrOs_SerialMsgHandler.sendPollAckNak(...);
// fingerprint is NEVER freed
```

This runs every **2 seconds** on the master node. At 37 bytes per leak, that's ~1 KB/min of permanent heap loss.

**Fix:** Add `free(fingerprint);` after `sendPollAckNak()` returns. Better yet, use a stack-allocated `char fingerprint[37];`.

---

#### 2. Interface queue consumer never frees `char*` fields

**Files:** `lib/AstrOsSerialMsgHandler/src/AstrOsSerialMsgHandler.cpp`, `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

The `astros_interface_response_t` struct contains four `malloc`'d pointers:

```cpp
typedef struct {
    AstrOsInterfaceResponseType type;
    char *originationMsgId;  // malloc'd
    char *peerMac;           // malloc'd
    char *peerName;          // malloc'd
    char *message;           // malloc'd
} astros_interface_response_t;
```

Both `sendToInterfaceQueue()` implementations correctly free these on `xQueueSend` failure, but the **consumer task that calls `xQueueReceive` never frees them**. Every successfully queued interface message leaks all four allocations.

**Fix:** In the `interfaceResponseQueueTask`, free all four `char*` fields after processing each dequeued message.

---

#### 3. Serial queue consumer never frees `data`

**File:** `src/main.cpp` — serial queue tasks, `animationTimerCallback()`

```cpp
queue_serial_msg_t serialMsg;
serialMsg.data = (uint8_t *)malloc(formatted.size() + 1);
// ... memcpy ...
xQueueSend(serialCh1Queue, &serialMsg, pdMS_TO_TICKS(2000));
```

The `data` pointer is freed on send failure, but the receiving task never frees `serialMsg.data` after consuming the message. Same pattern applies to servo, I2C, and GPIO queue messages.

**Fix:** Add `free(msg.data);` (or equivalent) in every queue consumer task after the message payload has been processed.

---

#### 4. DisplayService queue failure — `i2cMsg.data` leak

**File:** `lib/AstrOsDisplay/src/DisplayCommand.cpp`

```cpp
i2cMsg.data = (uint8_t *)malloc(strValue.length() + 1);
// ... if xQueueSend fails, this memory is leaked
```

**Fix:** Free `i2cMsg.data` in the `xQueueSend` failure path.

---

#### 5. `CommandTemplate` raw pointer ownership

**File:** `lib/AnimationController/src/AnimationController.cpp` — `getNextCommandPtr()`

```cpp
CommandTemplate *AnimationController::getNextCommandPtr() {
    // ... inside semaphore lock ...
    cmd = scriptEvents.back().GetCommandTemplatePtr();  // allocates with `new`
    this->scriptEvents.pop_back();
    xSemaphoreGive(this->animationMutex);
    return cmd;  // raw pointer — caller must delete
}
```

The caller receives a raw `new`-allocated pointer with no documentation or enforcement of deletion responsibility. If any call site forgets to `delete`, this leaks on every animation command dispatch.

**Fix:** Return `std::unique_ptr<CommandTemplate>` to enforce ownership transfer. If C++ overhead is a concern, at minimum add a comment contract and audit all call sites for proper `delete`.

---

#### 6. AnimationController semaphore never deleted

**File:** `lib/AnimationController/src/AnimationController.cpp`

```cpp
AnimationController::AnimationController() {
    this->animationMutex = xSemaphoreCreateMutex();  // created
}
AnimationController::~AnimationController() {}  // empty — never calls vSemaphoreDelete()
```

While the global instance likely lives for the program's lifetime, this is a resource leak by pattern and prevents safe re-instantiation.

**Fix:** Add `vSemaphoreDelete(this->animationMutex);` in the destructor.

---

### Race Conditions

#### 7. Global state without synchronization

**File:** `src/main.cpp`

```cpp
static int displayTimeout = 0;       // written by timer callback, read by tasks
static bool discoveryMode = false;    // toggled by multiple tasks
static bool isMasterNode = false;     // read across tasks
static std::string rank = "Padawan"; // read/written from multiple contexts
```

These are accessed from timer callbacks (running in the timer task) and multiple FreeRTOS tasks on both cores with **no atomic, volatile, or mutex protection**. On the dual-core ESP32, this is undefined behavior — the compiler may cache values in registers, and memory visibility across cores is not guaranteed.

**Fix:** Use `std::atomic<bool>` / `std::atomic<int>` for primitive flags, or protect with a mutex. For `rank` (a `std::string`), a mutex is required.

---

#### 8. Unprotected `maestroModules` map

**File:** `src/main.cpp`

```cpp
std::map<int, MaestroModule *> maestroModules;
```

Accessed from:
- `loadMaestroConfigs()` — startup, core 1
- `servoMoveTimerCallback()` — timer task context
- `servoQueueTask` — core 1

No synchronization exists. Concurrent read/write to `std::map` is undefined behavior.

**Fix:** Protect all accesses with a FreeRTOS mutex or spinlock.

---

#### 9. Unprotected `peers` vector in EspNowService

**File:** `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

The `peers` vector is:
- Modified via `push_back()` during registration
- Iterated in `getPeers()`, `findPeer()`, `pollPadawans()`
- Fields mutated in `pollPadawans()` (`pollAckThisCycle`)

All without any mutex protection. A `push_back()` during iteration can invalidate iterators and crash.

**Fix:** Add a dedicated mutex for the `peers` vector. Acquire before any read or write.

---

#### 10. AnimationController flags read outside mutex

**File:** `lib/AnimationController/src/AnimationController.cpp`

```cpp
bool scriptLoaded;       // written inside mutex, read in scriptIsLoaded() WITHOUT mutex
bool queueing;           // written inside mutex, read in while-loop WITHOUT mutex  
int delayTillNextEvent;  // written inside mutex, read in msTillNextServoCommand() WITHOUT mutex
```

**Fix:** Either protect reads with the same mutex, or declare these `std::atomic`.

---

### Buffer Overflows

#### 11. ESP-NOW peer name overflow

**File:** `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

```cpp
char name[16];  // fixed buffer in espnow_peer_t
// ...
memcpy(newPeer.name, name.c_str(), name.length() + 1);  // no bounds check
```

If `name.length() >= 16`, this writes past the end of the `name` field, corrupting adjacent struct members (likely `crypto_key`).

**Fix:** Bounds-check before copy: `size_t len = std::min(name.length(), (size_t)15); memcpy(newPeer.name, name.c_str(), len); newPeer.name[len] = '\0';`

---

#### 12. NVS key generation overflow

**File:** `lib/AstrOsStorageManager/src/NvsManager.c`

```cpp
char nameConfig[] = "p-00-name";
setKeyId(nameConfig, i, 2);  // writes index digits at position 2
```

If peer index `i` exceeds 99, `setKeyId` writes a 3+ digit number into a 2-character slot, overflowing the stack buffer.

**Fix:** Validate `i < 100` before calling, or use `snprintf` with the full key buffer.

---

## P1 — High Severity Issues

### 13. I2C cmd handle leak on error paths

**File:** `lib/I2cMaster/src/I2cMaster.cpp` — `ReadWord()`

```cpp
i2c_cmd_handle_t cmd = i2c_cmd_link_create();
// ... operations ...
if (err != ESP_OK) {
    xSemaphoreGive(i2cBusMutext);
    return false;  // cmd handle leaked — i2c_cmd_link_delete(cmd) never called
}
```

**Fix:** Call `i2c_cmd_link_delete(cmd)` before every early return.

---

### 14. Uncaught `std::stoi()` / `std::vector::at()` exceptions

**Files:** `lib/AnimationController/src/AnimationCommand.cpp`, `lib/Modules/src/ServoModule.cpp`, `lib/AstrOsUtility/src/AstrOsFileUtils.hpp`

`std::stoi()` throws `std::invalid_argument` or `std::out_of_range` on malformed input. On ESP-IDF, an uncaught exception terminates the **entire FreeRTOS task**, which is unrecoverable without a reboot.

```cpp
// AnimationCommand.cpp
int position = std::stoi(parts[0]);  // throws on non-numeric input
```

**Fix:** Replace `std::stoi()` with `strtol()` and check `errno`/`endptr`, or use a safe wrapper function that returns a default on failure. Avoid `try`/`catch` on ESP32 — exception handling is expensive and often disabled in ESP-IDF builds.

---

### 15. `portMAX_DELAY` deadlock risk

**File:** `lib/AnimationController/src/AnimationController.cpp` — `panicStop()`, `queueScript()`

```cpp
while (!cleared) {
    if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE) {
        // ...
        cleared = true;
    }
}
```

If the mutex holder is blocked or crashed, this waits **forever**. On an embedded system with no watchdog recovery for individual tasks, this is a permanent hang.

**Fix:** Use a finite timeout (e.g., `pdMS_TO_TICKS(5000)`) and log an error / take recovery action on timeout.

---

### 16. Spin-wait patterns in EspNowService

**File:** `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

```cpp
// getMasterMac(), getMac(), getName(), etc.
while (true) {
    if (xSemaphoreTake(valueMutex, portMAX_DELAY) == pdTRUE) {
        // ...
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // polling loop
}
```

The `vTaskDelay` after a `portMAX_DELAY` take is unreachable (the take either succeeds or blocks forever). This pattern suggests the original intent was a finite timeout with retry but was implemented incorrectly.

**Fix:** Remove the while-loop. A single `xSemaphoreTake(mutex, portMAX_DELAY)` suffices. Or use a finite timeout with proper error handling.

---

### 17. Mixed synchronization primitives

**Files:** `lib/I2cMaster/src/I2cMaster.cpp` (FreeRTOS semaphore), `lib/Modules/src/I2cModule.cpp` (POSIX `pthread_mutex`)

Using both FreeRTOS and POSIX synchronization for the same I2C bus creates potential for lock ordering issues and makes the concurrency model harder to reason about.

**Fix:** Standardize on FreeRTOS semaphores throughout, since the rest of the codebase already uses them.

---

### 18. Deprecated I2C API

**File:** `lib/I2cMaster/src/I2cMaster.cpp`

The code uses `i2c_cmd_link_create()` / `i2c_master_write_byte()` / `i2c_master_cmd_begin()`, which are deprecated in ESP-IDF 5.x in favor of the new `i2c_master` driver.

**Fix:** Migrate to the new `i2c_master_bus_add_device()` / `i2c_master_transmit()` API when upgrading ESP-IDF. Low urgency if staying on IDF 4.x.

---

## P2 — Medium Severity Issues

### 19. Queue depth too small for bursty producers

**File:** `src/main.cpp`

All 9 queues use `QUEUE_LENGTH = 5`. The `animationTimerCallback()` can enqueue many commands in rapid succession (one per animation channel — servo, I2C, GPIO, serial). With 4+ channel types, a single animation frame can fill or overflow a queue of depth 5.

**Fix:** Increase queue depth for high-throughput queues (animation, servo) to 10–20, or implement backpressure signaling.

---

### 20. `button_listener_task` stack too small

**File:** `src/main.cpp`

```cpp
xTaskCreatePinnedToCore(button_listener_task, "BTN", 2048, NULL, 5, NULL, 1);
```

2048 bytes is tight for a task that does GPIO reads, logging (`ESP_LOGW`), and queue sends. The existing high-water mark check at 500 bytes suggests this is known to be close.

**Fix:** Increase to 3072 or 4096 bytes.

---

### 21. Complex work in timer callbacks

**File:** `src/main.cpp` — `animationTimerCallback()`

ESP timer callbacks run in the dedicated `esp_timer` task, which has a fixed stack size (typically 4096 bytes). The animation timer callback performs multiple `malloc`, `memcpy`, string formatting, and `xQueueSend` operations — risking stack overflow in the timer task.

**Fix:** Have the timer callback send a single "tick" message to a dedicated task, which then performs the heavy lifting.

---

### 22. Missing file handle validation

**File:** `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp`

```cpp
FILE *fd = fopen(path, "r");
// ... on some error paths ...
fclose(fd);  // fd may be NULL if fopen failed
```

**Fix:** Check `fd != NULL` before calling `fclose()`.

---

### 23. No filesystem path validation

**File:** `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp`

File paths are constructed using external input (e.g., script names from serial/ESP-NOW messages) without sanitization. A crafted filename containing `../` could escape the intended directory.

**Fix:** Validate that resolved paths stay within the expected mount point. Reject paths containing `..`.

---

### 24. `mkdir` return values ignored

**File:** `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp`

Directory creation calls don't check return values. If `mkdir` fails (e.g., filesystem full), subsequent file operations fail silently with confusing errors.

**Fix:** Check `mkdir` return and log/propagate errors.

---

### 25. Mixed `malloc`/`new` allocation style

**Files:** Throughout `src/main.cpp` and `lib/` directories

Queue message data uses `malloc`/`free` (C-style), while `MaestroModule` and `CommandTemplate` use `new`/`delete` (C++). This inconsistency increases cognitive load and makes it harder to audit for leaks.

**Fix:** Standardize on one style. For ESP-IDF, `malloc`/`free` is idiomatic for buffer data, `new`/`delete` for C++ objects. Consider documenting the convention.

---

### 26. SD card format `workbuf` allocation

**File:** `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp`

The `formatSdCard` function allocates a work buffer. If allocation fails, it returns `false` but logs `ESP_ERR_NO_MEM` — the caller has no way to distinguish "format failed" from "out of memory."

**Fix:** Return distinct error codes or use `esp_err_t` return type.

---

## P3 — Low Severity Issues

### 27. Non-atomic boolean flags across threads

**File:** `lib/AnimationController/include/AnimationController.hpp`

```cpp
bool queueing;
bool scriptLoaded;
```

These should be `std::atomic<bool>` to guarantee visibility across cores. Without `atomic`, the compiler may optimize reads into register-cached values that never see cross-core writes.

---

### 28. GUID `sprintf` buffer

**File:** `lib/Uuid/guid.h`

Uses a 64-byte stack buffer with `sprintf`. While current format strings fit within 64 bytes, there's no `snprintf` protection.

**Fix:** Replace `sprintf` with `snprintf(buf, sizeof(buf), ...)`.

---

### 29. Silent error swallowing

**Files:** `src/main.cpp` — `astrosRxTask()`, `espnowRecvCallback()`

- `astrosRxTask()`: Buffer overflow resets `bufferIndex = 0`, silently discarding a partial message with only a warning log.
- `espnowRecvCallback()`: `malloc` failure returns immediately with no error propagated to the system.

**Fix:** Consider incrementing an error counter or setting a flag that the monitoring/maintenance task can report.

---

## Positive Observations

The codebase gets several things right that are worth preserving:

- **`ESP_ERROR_CHECK()`** used on critical initialization paths (I2C, UART, timer creation)
- **Stack high-water mark monitoring** in every FreeRTOS task with warning at 500 bytes remaining
- **Core pinning** — tasks explicitly assigned to cores, avoiding unnecessary cross-core context switches
- **Queue send timeouts** with failure logging (not fire-and-forget)
- **Callback-based timer architecture** — non-blocking design
- **Clear message type enums** for type-safe queue dispatch
- **Move semantics** used properly in `AnimationCommand` constructors
- **ESP-NOW packet fragmentation** correctly respects the 250-byte limit (20-byte header + 180-byte payload)
- **I2C bus mutex** — hardware bus access properly serialized with semaphore (1000ms timeout)
- **Maintenance timer** logs free heap every 10 seconds — helpful for detecting leaks at runtime

---

## Recommendations Summary

| # | Priority | Category | Issue | Recommended Fix |
|---|----------|----------|-------|-----------------|
| 1 | P0 | Memory | `pollingTimerCallback` fingerprint leak | `free(fingerprint)` after use, or use stack buffer |
| 2 | P0 | Memory | Interface queue consumer never frees `char*` fields | Free all 4 pointers after dequeue processing |
| 3 | P0 | Memory | Serial/servo/I2C/GPIO queue consumers never free `data` | Free `data` pointer after dequeue processing |
| 4 | P0 | Memory | DisplayService queue failure leak | Free `i2cMsg.data` on `xQueueSend` failure |
| 5 | P0 | Memory | `CommandTemplate` raw pointer ownership | Return `std::unique_ptr` or enforce caller `delete` |
| 6 | P0 | Memory | AnimationController semaphore never deleted | `vSemaphoreDelete()` in destructor |
| 7 | P0 | Thread | Global state race conditions | `std::atomic` for primitives, mutex for `std::string` |
| 8 | P0 | Thread | Unprotected `maestroModules` map | Add mutex around all accesses |
| 9 | P0 | Thread | Unprotected `peers` vector | Add mutex around all accesses |
| 10 | P0 | Thread | AnimationController flags read outside mutex | `std::atomic` or read under mutex |
| 11 | P0 | Buffer | ESP-NOW peer name overflow | Bounds-check before `memcpy`, truncate to 15 + null |
| 12 | P0 | Buffer | NVS key generation overflow | Validate index < 100, use `snprintf` |
| 13 | P1 | Resource | I2C cmd handle leak on error paths | `i2c_cmd_link_delete(cmd)` before early returns |
| 14 | P1 | Safety | Uncaught `std::stoi` exceptions | Replace with `strtol` + error check |
| 15 | P1 | Thread | `portMAX_DELAY` deadlock risk | Use finite timeout with error recovery |
| 16 | P1 | Thread | Spin-wait patterns in EspNowService | Simplify to single blocking take |
| 17 | P1 | Thread | Mixed FreeRTOS/pthread sync primitives | Standardize on FreeRTOS semaphores |
| 18 | P1 | API | Deprecated I2C driver API | Migrate to new `i2c_master` API (IDF 5.x) |
| 19 | P2 | Config | Queue depth too small (5) | Increase to 10–20 for bursty queues |
| 20 | P2 | Stack | `button_listener_task` 2048 bytes | Increase to 3072+ |
| 21 | P2 | Timer | Complex work in timer callbacks | Defer to task via single queue message |
| 22 | P2 | FS | Missing file handle validation | Check `fopen` return before `fclose` |
| 23 | P2 | FS | No path sanitization | Reject paths containing `..` |
| 24 | P2 | FS | `mkdir` return values ignored | Check and propagate errors |
| 25 | P2 | Style | Mixed `malloc`/`new` | Document and standardize convention |
| 26 | P2 | Error | SD format error reporting | Return distinct error codes |
| 27 | P3 | Thread | Non-atomic boolean flags | Use `std::atomic<bool>` |
| 28 | P3 | Safety | GUID `sprintf` buffer | Use `snprintf` |
| 29 | P3 | Error | Silent error swallowing | Add error counters / flags |
