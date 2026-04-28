# AstrOs.ESP — Comprehensive Code Review

**Date:** April 15, 2026
**Scope:** Full codebase — `src/`, `lib/`, `components/`, build configuration
**Platform:** ESP-IDF / PlatformIO, targeting ESP32 (lolin_d32_pro) and ESP32-S3 (metro_s3)

---

## Executive Summary

AstrOs.ESP is an ESP-IDF C/C++ firmware that orchestrates animatronics hardware via ESP-NOW mesh networking, serial communication, I2C peripherals (PCA9685 servo drivers, OLED display), and GPIO. The application creates **12 FreeRTOS tasks** pinned across two cores, **9 message queues**, and **3 ESP timers** to coordinate animation playback, peer discovery, and hardware control.

Compared to the April 11, 2026 review, several critical issues have been resolved:
- AnimationController now uses `std::unique_ptr<CommandTemplate>` (was raw pointers)
- AnimationController now uses `std::atomic` for shared flags (was unprotected bools)
- AnimationController semaphore properly deleted in destructor
- AnimationController uses bounded timeouts (`pdMS_TO_TICKS(5000)`) instead of `portMAX_DELAY`
- `maestroModules` map is now protected by `maestroModulesMutex`
- Global state variables (`discoveryMode`, `displayTimeout`, `isMasterNode`) are now `std::atomic`
- `pollingTimerCallback` fingerprint is now stack-allocated
- Queue consumers properly free malloc'd pointers
- No pthread/FreeRTOS synchronization mixing (standardized on FreeRTOS)

Remaining issues are documented below, organized by severity.

### Issue Count by Severity

| Severity | Count | Categories |
|----------|-------|------------|
| P0 — Critical | 5 | Memory leaks, race condition, buffer overflow, unsafe parsing |
| P1 — High | 6 | Resource leaks, semaphore leaks, deprecated API, ISR cleanup |
| P2 — Medium | 5 | Timeout inconsistency, RX overflow handling, unbounded fragmentation, stack concerns |
| P3 — Low | 3 | Minor buffer risks, silent error paths, mutex timeout mismatch |

---

## P0 — Critical

### 1. `astrosRxTask()` — two permanent malloc leaks with no NULL checks

**File:** `src/main.cpp` — `astrosRxTask()`

```cpp
uint8_t *data = (uint8_t *)malloc(RX_BUF_SIZE + 1);      // ~1025 bytes
uint8_t *commandBuffer = (uint8_t *)malloc(bufferLength);  // 2000 bytes
```

Neither allocation is checked for NULL. The task runs an infinite `while(1)` loop so the buffers are never freed — a permanent ~3KB heap loss per boot. While the task never exits (so technically the memory is "in use"), the missing NULL check is the real risk: if either malloc returns NULL (heap pressure), the task will immediately crash on dereference.

**Fix:** Add NULL checks. If allocation fails, log an error and `vTaskDelete(NULL)` to avoid a crash. These buffers are effectively permanent so the leak is tolerable, but the NULL check is mandatory.

---

### 2. Unsafe `std::stoi()` in `handleServoTest()`

**File:** `src/main.cpp` — `handleServoTest()`

```cpp
int idx = std::stoi(parts[1]);
int servo = std::stoi(parts[2]);
int ms = std::stoi(parts[3]);
```

`std::stoi()` throws `std::invalid_argument` or `std::out_of_range` on malformed input. On ESP-IDF with exceptions effectively disabled, an uncaught exception terminates the calling FreeRTOS task with no recovery.

This is reachable from serial input — a malformed servo test command from the interface will crash the task.

**Fix:** Replace with `strtol()` + `errno` checking, or use the `safeStoi()` wrapper already present in `lib/AstrOsUtility/src/AstrOsFileUtils.hpp`. The safe wrapper already exists in the codebase — just use it.

---

### 3. Unsafe `std::stoi()` in AnimationCommand parsing

**File:** `lib/AnimationController/src/AnimationCommand.cpp` — `parseCommandType()`

```cpp
this->commandType = static_cast<MODULE_TYPE>(std::stoi(script.at(0)));
this->duration = std::stoi(script.at(1));
this->module = std::stoi(script.at(2));
```

Same issue as #2. A malformed animation script (received over serial or ESP-NOW from an external source) will crash the animation task via uncaught exception.

The `parts.size()` pre-check prevents `at()` from throwing, but `stoi()` itself can still throw on non-numeric strings.

**Fix:** Replace with `strtol()` + `errno` or the existing `safeStoi()` utility.

---

### 4. ESP-NOW callbacks access `peers` without mutex

**File:** `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

`espnowSendCallback()` and `espnowRecvCallback()` are called from the Wi-Fi driver task on core 0. They enqueue messages that reference peers, but do not hold `peersMutex` while accessing the `peers` vector. All other code paths properly acquire the semaphore.

An ESP-NOW receive that triggers a `push_back()` on `peers` (via the handler chain) concurrent with an iteration in `pollPadawans()` can invalidate iterators and crash.

**Fix:** Ensure any peers vector access in callback paths is either protected by `peersMutex` or deferred to a task that holds it.

---

### 5. No `payloadSize` bounds check in `parsePacket()`

**File:** `lib/AstrOsEspNow/src/AstrOsEspNowMessageService.cpp`

When parsing incoming ESP-NOW packets, the header's `payloadSize` field is trusted without validation. A malicious or corrupted packet claiming `payloadSize = 500` with only 180 bytes of actual data causes an out-of-bounds read from the receive buffer.

**Fix:** Validate `payloadSize <= ASTROS_PACKET_PAYLOAD_SIZE` (180) before reading payload bytes.

---

## P1 — High

### 6. SPIFFS `esp_vfs_spiffs_unregister()` missing on error paths

**File:** `lib/AstrOsStorageManager/src/AstrOsStorageManager.cpp`

Five SPIFFS functions (`saveFileSpiffs`, `deleteFileSpiffs`, `fileExistsSpiffs`, `readFileSpiffs`, `listFilesSpiffs`) call `esp_vfs_spiffs_register()` at entry but skip `esp_vfs_spiffs_unregister()` on early error returns (e.g., `fopen` failure, `stat` failure). Each leaked registration holds a VFS mount slot.

**Fix:** Use a cleanup-on-exit pattern (goto cleanup, or RAII wrapper) to ensure `esp_vfs_spiffs_unregister()` is always called.

---

### 7. Semaphores never deleted in hardware modules

**Files:**
- `lib/I2cMaster/src/I2cMaster.cpp` — `i2cBusMutext` (note: typo in variable name)
- `lib/Modules/src/SerialModule.cpp` — `serialMutex`
- `lib/Modules/src/I2cModule.cpp` — `i2cMutex`
- `lib/Modules/src/MaestroModule.cpp` — private mutex

All four modules create FreeRTOS semaphores in their constructors or `Init()` but never call `vSemaphoreDelete()` in their destructors. Since these are effectively singletons that live for the program's lifetime, this is not a runtime leak — but it prevents safe re-instantiation and is a pattern defect.

**Fix:** Add `vSemaphoreDelete()` in each destructor, guarded by a NULL check.

---

### 8. MaestroModule `sendQueueMsg()` race condition

**File:** `lib/Modules/src/MaestroModule.cpp` — `sendQueueMsg()`

If `xSemaphoreTake()` fails (timeout), the code falls through and continues to allocate and `xQueueSend` without holding the mutex. This means queue message data can be corrupted if another task is simultaneously modifying the same state.

**Fix:** Return early (with error log) if the semaphore take fails. Do not proceed to queue operations without the lock.

---

### 9. `stringToMac()` returns `new uint8_t[6]` — caller must `delete[]`

**File:** `lib/AstrOsUtility/src/AstrOsStringUtils.hpp`

```cpp
uint8_t *mac = new uint8_t[6];
```

Returns a heap-allocated array. The caller is responsible for `delete[]`, but this contract is not documented and easy to miss. Every call site must be audited.

**Fix:** Change to accept a caller-provided `uint8_t mac[6]` buffer, or return `std::array<uint8_t, 6>` by value.

---

### 10. SoftwareSerial ISR handler not removed on deletion

**File:** `lib/SoftwareSerial/include/SoftwareSerial.h` — `sw_del()`

The `sw_del()` function frees RX/TX buffers but never calls `gpio_isr_handler_remove()` or `gpio_uninstall_isr_service()`. The ISR remains registered and will fire on the now-freed buffer memory, causing a use-after-free crash.

**Fix:** Remove the ISR handler before freeing buffers.

---

### 11. Deprecated I2C driver API

**File:** `lib/I2cMaster/src/I2cMaster.cpp`

Uses the legacy `i2c_cmd_link_create()` / `i2c_master_write_byte()` / `i2c_cmd_link_delete()` API, which is deprecated in ESP-IDF 5.x. Commented-out code at end of file shows an abandoned migration attempt.

**Impact:** Low urgency if staying on IDF 4.x, but will be a build-breaking issue on IDF 5.x. Pca9685 `SetFrequency()` also ignores `WriteByte()` return values.

**Fix:** Plan migration to `i2c_master_bus_handle_t` / `i2c_master_transmit()` API when upgrading framework.

---

## P2 — Medium

### 12. `maestroModulesMutex` timeout inconsistency

**File:** `src/main.cpp`

| Call site | Timeout |
|-----------|---------|
| `servoMoveTimerCallback()` | 100 ms |
| `servoQueueTask()` | 1000 ms |
| `loadMaestroConfigs()` | 1000 ms |
| `handleServoTest()` | 1000 ms |

The servo move timer uses a 100ms timeout while all other sites use 1000ms. If the map is being modified during module load (a slow NVS operation), the timer will repeatedly fail to acquire the mutex, silently skipping servo update cycles.

**Fix:** Standardize on a single timeout (e.g., 500ms), or accept the skipped cycles and document why the timer is intentionally shorter.

---

### 13. `astrosRxTask()` silently drops overflowed messages

**File:** `src/main.cpp` — `astrosRxTask()`

When received data exceeds the 2000-byte buffer, the overflow counter is incremented (good) but the entire message is silently discarded with no feedback to the sender.

**Fix:** Consider sending a NAK response to the interface or logging the discarded message length so the root cause is diagnosable.

---

### 14. Unbounded ESP-NOW packet fragmentation

**Files:** `lib/AstrOsEspNow/src/AstrOsEspNowMessageService.cpp`, `lib/AstrOsEspNow/src/PacketTracker.cpp`

There is no maximum message size limit on fragmentation. A multi-megabyte message will be fragmented into thousands of packets, each stored in an `std::unordered_map<msgId, vector<PacketData>>` until expiration (1 second). This allows heap exhaustion via a single large message.

**Fix:** Add a max message size constant (e.g., 8KB) and reject messages that would exceed it. Also cap the number of entries in PacketTracker.

---

### 15. `NvsManager.c` — `setKeyId()` assumes peer index < 100

**File:** `lib/AstrOsStorageManager/src/NvsManager.c`

```cpp
char nameConfig[] = "p-00-name";
setKeyId(nameConfig, i, 2);
```

If peer index exceeds 99, `setKeyId` writes a 3+ digit number into a 2-character slot, overflowing the stack buffer. Current deployments likely have fewer than 100 peers, but no runtime check enforces it.

**Fix:** Validate `i < 100` before the call, or use `snprintf` for the full key.

---

### 16. `MaestroModule` assumes null-terminated command data

**File:** `lib/Modules/src/MaestroModule.cpp`

```cpp
std::string(reinterpret_cast<char*>(cmd))
```

This constructs a `std::string` from a `uint8_t*` pointer, relying on null termination. If the queue message data is not null-terminated (e.g., due to a truncation bug elsewhere), this reads past the buffer.

**Fix:** Use the length-bounded constructor: `std::string(reinterpret_cast<char*>(cmd), len)`.

---

## P3 — Low

### 17. `guid.h` — `sprintf` without bounds checking

**File:** `lib/Uuid/guid.h`

Uses a 64-byte stack buffer with `sprintf`. Current format strings fit, but there's no `snprintf` protection against future changes.

**Fix:** Replace with `snprintf(buf, sizeof(buf), ...)`.

---

### 18. `espnowRecvCallback()` — malloc failure silently drops message

**File:** `src/main.cpp`

When `malloc` fails for the ESP-NOW receive buffer, the atomic error counter is incremented (good), but no error is queued or logged beyond the counter. In a heap-pressure scenario, the only signal is a counter that must be manually observed.

**Fix:** Acceptable as-is given the counter exists. Consider also logging an `ESP_LOGE` on first failure (rate-limited).

---

### 19. `sscanf` in `stringToMac()` — no field width limits

**File:** `lib/AstrOsUtility/src/AstrOsStringUtils.hpp`

Minor: `sscanf` format string `%02x` does not strictly limit input width. Unlikely to be exploitable given the input is a pre-split string, but `%2x` would be more defensive.

---

## Architecture & Task Layout Reference

### FreeRTOS Tasks (12 total)

| Task | Stack | Priority | Core | Purpose |
|------|-------|----------|------|---------|
| buttonListenerTask | 4096 | 5 | 1 | GPIO polling (reset button) |
| serviceQueueTask | 3072 | 6 | 1 | Service commands (FORMAT_SD, discovery mode) |
| animationQueueTask | 4096 | 7 | 1 | Animation script loading |
| animationDispatchTask | 4096 | 5 | 1 | Core dispatch: animation frames → hardware queues |
| interfaceResponseQueueTask | 4096 | 10 | 1 | Route interface responses to serial |
| serialCh1QueueTask | 4096 | 9 | 1 | Serial channel 1 TX |
| serialCh2QueueTask | 4096 | 9 | 1 | Serial channel 2 TX |
| servoQueueTask | 4096 | 10 | 1 | Route servo commands to MaestroModule |
| i2cQueueTask | 3072 | 8 | 1 | Route I2C commands |
| gpioQueueTask | 3072 | 8 | 1 | Route GPIO commands |
| astrosRxTask | 4096 | 9 | 0 | UART0 RX buffering |
| espnowQueueTask | 4096 | 10 | 0 | ESP-NOW message dispatch |

### Queues (9 total)

| Queue | Depth | Payload | Producers | Consumer |
|-------|-------|---------|-----------|----------|
| animationQueue | 5 | queue_ani_cmd_t | Script loader | animationQueueTask |
| serviceQueue | 5 | queue_svc_cmd_t | Button, interface response | serviceQueueTask |
| interfaceResponseQueue | 5 | astros_interface_response_t | EspNow, SerialMsgHandler | interfaceResponseQueueTask |
| serialCh1Queue | 10 | queue_serial_msg_t | animationDispatch, service | serialCh1QueueTask |
| serialCh2Queue | 10 | queue_serial_msg_t | animationDispatch, maestro | serialCh2QueueTask |
| servoQueue | 20 | queue_msg_t | animationDispatch | servoQueueTask |
| i2cQueue | 16 | queue_msg_t | animationDispatch, display | i2cQueueTask |
| gpioQueue | 10 | queue_msg_t | animationDispatch | gpioQueueTask |
| espnowQueue | 5 | queue_espnow_msg_t | pollingTimer, WiFi RX | espnowQueueTask |

### Timers (3 total)

| Timer | Period | Type | Purpose |
|-------|--------|------|---------|
| pollingTimer | 2 s | Periodic | Heartbeat, discovery, display timeout |
| maintenanceTimer | 10 s | Periodic | Log free heap + error counters |
| servoMoveTimer | 300 ms | Periodic | Servo position updates |

---

## Positive Observations

The codebase demonstrates several strong patterns:

- **Queue ownership handoff** is correct across all 9 queues — every consumer properly frees malloc'd pointers, and every producer frees on send failure
- **`std::atomic`** used for all cross-task shared state (`discoveryMode`, `displayTimeout`, `isMasterNode`, error counters) with appropriate memory ordering
- **`std::unique_ptr`** for `CommandTemplate` in AnimationController — eliminates prior raw-pointer ownership ambiguity
- **`std::shared_ptr`** for `MaestroModule` instances — safe snapshot pattern in timer callbacks
- **Bounded mutex timeouts** throughout AnimationController (5s) — no `portMAX_DELAY` deadlock risk
- **Stack high-water mark monitoring** in every task at 500-byte threshold
- **Core pinning** — I/O tasks on core 1, ESP-NOW receive on core 0
- **Error counters** for malloc failures and RX overflows — observable via maintenance timer
- **ESP-NOW packet fragmentation** correctly respects the 250-byte limit (20B header + 180B payload)
- **`safeStoi()` utility exists** in `AstrOsFileUtils.hpp` using `strtol` + `errno` — just needs to be used consistently
- **No try/catch** blocks anywhere in the codebase (correct for ESP-IDF)
- **Consistent FreeRTOS synchronization** — no pthread mixing

---

## Recommendations Summary

| # | Priority | Category | Issue | Recommended Fix |
|---|----------|----------|-------|-----------------|
| 1 | P0 | Memory | `astrosRxTask` malloc without NULL check | Add NULL checks; `vTaskDelete(NULL)` on failure |
| 2 | P0 | Safety | `std::stoi()` in `handleServoTest()` | Replace with existing `safeStoi()` utility |
| 3 | P0 | Safety | `std::stoi()` in `AnimationCommand` | Replace with existing `safeStoi()` utility |
| 4 | P0 | Thread | ESP-NOW callbacks access peers without mutex | Protect or defer to mutex-holding task |
| 5 | P0 | Buffer | `parsePacket()` trusts `payloadSize` | Validate ≤ `ASTROS_PACKET_PAYLOAD_SIZE` |
| 6 | P1 | Resource | SPIFFS unregister missing on error paths | Add cleanup-on-exit pattern |
| 7 | P1 | Resource | 4 modules never delete semaphores | `vSemaphoreDelete()` in destructors |
| 8 | P1 | Thread | `MaestroModule::sendQueueMsg()` race | Return early if semaphore take fails |
| 9 | P1 | Memory | `stringToMac()` returns `new[]` | Accept caller buffer or return `std::array` |
| 10 | P1 | Resource | SoftwareSerial ISR not removed on delete | `gpio_isr_handler_remove()` before free |
| 11 | P1 | API | Deprecated I2C driver | Plan migration to IDF 5.x API |
| 12 | P2 | Thread | Mutex timeout inconsistency (100ms vs 1s) | Standardize or document intent |
| 13 | P2 | Error | RX overflow silently drops messages | Log discarded message details |
| 14 | P2 | Security | Unbounded packet fragmentation | Add max message size limit |
| 15 | P2 | Buffer | NVS `setKeyId` assumes index < 100 | Validate or use `snprintf` |
| 16 | P2 | Buffer | MaestroModule assumes null-terminated data | Use length-bounded `std::string` constructor |
| 17 | P3 | Safety | `guid.h` uses `sprintf` | Replace with `snprintf` |
| 18 | P3 | Error | ESP-NOW malloc failure silent | Rate-limited `ESP_LOGE` on first failure |
| 19 | P3 | Safety | `sscanf` no field width limit | Use `%2x` instead of `%02x` |

---

## Changes Since April 11, 2026 Review

The following items from the previous review have been **resolved**:

| Previous # | Issue | Resolution |
|------------|-------|------------|
| P0-1 | `pollingTimerCallback` fingerprint leak | Now stack-allocated `char fingerprint[37]` |
| P0-2 | Interface queue consumer never frees char* | Consumer now frees all 4 pointers |
| P0-3 | Serial queue consumer never frees data | All queue consumers now free properly |
| P0-4 | DisplayService queue failure leak | Fixed |
| P0-5 | CommandTemplate raw pointer ownership | Now returns `std::unique_ptr<CommandTemplate>` |
| P0-6 | AnimationController semaphore never deleted | Destructor now calls `vSemaphoreDelete()` |
| P0-7 | Global state without synchronization | All globals now `std::atomic` |
| P0-8 | Unprotected maestroModules map | Now protected by `maestroModulesMutex` |
| P0-10 | AnimationController flags read outside mutex | Now `std::atomic<bool>` / `std::atomic<int>` |
| P0-11 | ESP-NOW peer name overflow | Now bounds-checked with `std::min(name.length(), 15)` |
| P1-15 | `portMAX_DELAY` deadlock risk | AnimationController now uses `pdMS_TO_TICKS(5000)` |
| P1-17 | Mixed FreeRTOS/pthread sync primitives | Standardized on FreeRTOS |
| P2-19 | Queue depth too small | Queues resized: servo=20, i2c=16, serial=10, gpio=10 |
| P2-20 | `button_listener_task` 2048 bytes | Increased to 4096 |
| P2-25 | Mixed malloc/new style | Standardized within modules |
| P3-27 | Non-atomic boolean flags | Now `std::atomic<bool>` |
