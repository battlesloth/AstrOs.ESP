# Code Review Phase B: Thread Safety + Synchronization

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate race conditions on shared firmware state across FreeRTOS tasks and timer callbacks by adding atomics, mutexes, finite timeouts, and standardized synchronization primitives.

**Architecture:** Seven task groups, ordered risk-ascending. Atomics for primitive flags read/written across cores. New FreeRTOS semaphores for container state (peers vector, maestroModules map). Existing `portMAX_DELAY` replaced with finite timeouts plus error logging. EspNowService spin-wait retry loops collapsed to single blocking takes. I2cModule standardized on FreeRTOS primitives to match the rest of the codebase.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS (SemaphoreHandle_t, xSemaphoreTake/Give), C++20 `<atomic>`, PlatformIO 6.x.

---

## Locked decisions

1. **Branch:** `fix/code-review-phase-b` from `develop`. Already created.
2. **Timeout policy:** 5000ms for slow/compound operations (animation mutex), 1000ms for fast get-value operations (peers, maestroModules, EspNow values), 100ms for timer-callback acquisitions (servoMoveTimerCallback) since blocking the esp_timer task is costly.
3. **Atomic type choices:** `std::atomic<bool>` for flags, `std::atomic<int>` for counters/timeouts. No memory order tuning — default `memory_order_seq_cst` is fine at our access frequencies.
4. **`rank` handled by derivation** rather than independent state: `currentRank()` returns a `const char *` derived from `isMasterNode.load()`. Eliminates the `isMasterNode`/`rank` consistency invariant entirely.
5. **I2cModule `pthread_mutex_t` replacement:** full swap to `SemaphoreHandle_t`. Not a coexistence scheme.
6. **No behavioral changes expected.** Every fix is either a type change (atomic), synchronization addition (mutex), or pattern simplification (spin-wait → single-take). Happy-path behavior stays identical.

## Repository facts

- `fix/code-review-phase-b` branched from `develop` at merge commit `c17d62d`
- Phase A changes are already merged: `AnimationController` has `vSemaphoreDelete` + NULL guards; `setKeyId` returns `bool` + `size_t id`; `guid.h` uses `snprintf` + `%02x`; etc.
- FreeRTOS semaphore primitives are canonical sync in `AstrOsEspNow`, `AstrOsStorageManager`, `I2cMaster`
- Existing `SemaphoreHandle_t valueMutex` and `SemaphoreHandle_t masterMacMutex` in `AstrOsEspNowService.cpp` protect non-peers state (file-scope statics)
- Build command: `~/.platformio/penv/bin/pio run -e metro_s3` (also `lolin_d32_pro`, `test`)
- Pre-commit hook at `.githooks/pre-commit` auto-formats via clang-format — Allman braces, 4-space indent

## File structure

**Modified files (no new files):**
- `lib/AnimationController/include/AnimationController.hpp` — add `#include <atomic>`, change 3 member types
- `lib/AnimationController/src/AnimationController.cpp` — replace 6 `portMAX_DELAY` spin-waits with finite-timeout single-takes
- `src/main.cpp` — global atomics, `currentRank()` helper, `maestroModulesMutex` declaration + creation, lock all `maestroModules` access sites
- `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp` — add `SemaphoreHandle_t peersMutex` private member
- `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp` — create `peersMutex` in `init()`, collapse 5 spin-wait methods, wrap all `peers` access sites
- `lib/Modules/src/I2cModule.cpp` — replace `pthread_mutex_t` with `SemaphoreHandle_t`, update all 3 lock sites
- `.docs/qa/code-review-phase-b.md` — manual QA test plan

---

## Task 1: AnimationController atomic flags

**Files:**
- Modify: `lib/AnimationController/include/AnimationController.hpp`
- Modify: `lib/AnimationController/src/AnimationController.cpp`

- [ ] **Step 1: Read the current header**

  Open `lib/AnimationController/include/AnimationController.hpp`. Confirm the three target members appear exactly as:
  ```cpp
  bool queueing;
  bool scriptLoaded;
  int delayTillNextEvent;
  ```

- [ ] **Step 2: Update the header — add `<atomic>` include and change member types**

  In `lib/AnimationController/include/AnimationController.hpp`:

  Change the includes block from:
  ```cpp
  #include <AnimationCommand.hpp>

  #include <array>
  #include <memory>
  #include <string>
  ```
  To:
  ```cpp
  #include <AnimationCommand.hpp>

  #include <array>
  #include <atomic>
  #include <memory>
  #include <string>
  ```

  Then change the three members in the `private:` section from:
  ```cpp
      bool queueing;
      int queueFront;
      int queueRear;
      int queueSize;
      int queueCapacity = QUEUE_CAPACITY;
      std::array<std::string, QUEUE_CAPACITY> scriptQueue;

      // script
      bool scriptLoaded;
      int delayTillNextEvent;
  ```
  To:
  ```cpp
      std::atomic<bool> queueing;
      int queueFront;
      int queueRear;
      int queueSize;
      int queueCapacity = QUEUE_CAPACITY;
      std::array<std::string, QUEUE_CAPACITY> scriptQueue;

      // script
      std::atomic<bool> scriptLoaded;
      std::atomic<int> delayTillNextEvent;
  ```

- [ ] **Step 3: Verify the constructor in the .cpp initialises queueing explicitly**

  Open `lib/AnimationController/src/AnimationController.cpp`. The constructor body already has:
  ```cpp
  this->queueing = false;
  ```
  This compiles unchanged for `std::atomic<bool>` (assignment from `bool` is defined). `scriptLoaded` and `delayTillNextEvent` are not explicitly initialised in the constructor body — `std::atomic`'s default constructor value-initialises them to `false`/`0`, which is correct. No .cpp changes needed for the constructor.

- [ ] **Step 4: Build to confirm no new warnings or errors**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ```
  Expected: `[SUCCESS]` with no new errors.

- [ ] **Step 5: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add lib/AnimationController/include/AnimationController.hpp
  git commit -m "fix: make AnimationController flag members atomic

  queueing, scriptLoaded, and delayTillNextEvent are read and written
  from multiple FreeRTOS tasks and timer callbacks without mutex
  protection. Converting to std::atomic eliminates the data races on
  these primitive fields without requiring lock expansion."
  ```

---

## Task 2: main.cpp global atomics + derive rank

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Read the current globals block**

  Open `src/main.cpp` lines 37–45. They currently read:
  ```cpp
  static int displayTimeout = 0;
  static int defaultDisplayTimeout = 10;
  static bool discoveryMode = false;
  static bool isMasterNode = false;
  static uart_port_t ASTRO_PORT = UART_NUM_0;
  static std::string rank = "Padawan";
  ```

- [ ] **Step 2: Add `<atomic>` include**

  In `src/main.cpp`, the includes block ends around line 33 with `#include <guid.h>`. Add `#include <atomic>` after `#include <map>` (line 13):

  Change:
  ```cpp
  #include <map>
  #include <nvs_flash.h>
  ```
  To:
  ```cpp
  #include <atomic>
  #include <map>
  #include <nvs_flash.h>
  ```

- [ ] **Step 3: Replace the globals block and add currentRank()**

  Replace the six-line globals block (lines 40–45):
  ```cpp
  static int displayTimeout = 0;
  static int defaultDisplayTimeout = 10;
  static bool discoveryMode = false;
  static bool isMasterNode = false;
  static uart_port_t ASTRO_PORT = UART_NUM_0;
  static std::string rank = "Padawan";
  ```
  With:
  ```cpp
  static std::atomic<int> displayTimeout{0};
  static std::atomic<int> defaultDisplayTimeout{10};
  static std::atomic<bool> discoveryMode{false};
  static std::atomic<bool> isMasterNode{false};
  static uart_port_t ASTRO_PORT = UART_NUM_0;

  static const char *currentRank()
  {
      return isMasterNode.load() ? "Master" : "Padawan";
  }
  ```

- [ ] **Step 4: Fix the `displayTimeout -= 2` read-modify-write**

  At approximately line 438, change:
  ```cpp
      if (!discoveryMode && displayTimeout > 0)
      {
          ESP_LOGD(TAG, "Display Timeout: %d", displayTimeout);
          displayTimeout -= 2;
          if (displayTimeout <= 0)
          {
              AstrOs_Display.displayClear();
          }
      }
  ```
  To:
  ```cpp
      if (!discoveryMode && displayTimeout > 0)
      {
          ESP_LOGD(TAG, "Display Timeout: %d", displayTimeout.load());
          displayTimeout.fetch_sub(2);
          if (displayTimeout.load() <= 0)
          {
              AstrOs_Display.displayClear();
          }
      }
  ```

- [ ] **Step 5: Replace `rank` uses and delete the two rank assignment lines**

  There are exactly three changes to make:

  **5a.** At approximately line 333, change:
  ```cpp
      AstrOs_Display.setDefault(rank, "", name);
  ```
  To:
  ```cpp
      AstrOs_Display.setDefault(currentRank(), "", name);
  ```

  **5b.** In `loadConfig()` at approximately lines 1357–1360, change:
  ```cpp
                      isMasterNode = true;
                      ASTRO_PORT = UART_NUM_1;
                      rank = "Master";
                      ESP_LOGI(TAG, "%s node using UART channel 1", rank.c_str());
  ```
  To:
  ```cpp
                      isMasterNode = true;
                      ASTRO_PORT = UART_NUM_1;
                      ESP_LOGI(TAG, "%s node using UART channel 1", currentRank());
  ```

  **5c.** At approximately lines 1365–1368, change:
  ```cpp
                      isMasterNode = false;
                      ASTRO_PORT = UART_NUM_0;
                      rank = "Padawan";
                      ESP_LOGI(TAG, "%s node using UART channel 0", rank.c_str());
  ```
  To:
  ```cpp
                      isMasterNode = false;
                      ASTRO_PORT = UART_NUM_0;
                      ESP_LOGI(TAG, "%s node using UART channel 0", currentRank());
  ```

- [ ] **Step 6: Search for any remaining `rank` references**

  ```bash
  grep -n '\brank\b' /home/jeff/Source/astros/AstrOs.ESP/src/main.cpp
  ```
  Expected: no matches. If any remain, fix them to use `currentRank()`.

- [ ] **Step 7: Build to confirm no new warnings or errors**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ```
  Expected: `[SUCCESS]` with no new errors.

- [ ] **Step 8: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add src/main.cpp
  git commit -m "fix: make main.cpp cross-core globals atomic, derive rank from isMasterNode

  displayTimeout, defaultDisplayTimeout, discoveryMode, and isMasterNode
  are accessed from both cores without synchronisation. Converting to
  std::atomic eliminates the data races. The rank string variable is
  removed entirely — currentRank() derives the value from isMasterNode
  at point of use, eliminating the isMasterNode/rank consistency
  invariant."
  ```

---

## Task 3: AnimationController portMAX_DELAY → finite timeouts

**Files:**
- Modify: `lib/AnimationController/src/AnimationController.cpp`

There are 6 `portMAX_DELAY` sites. Each spin-wait loop is replaced with a single `xSemaphoreTake` using a 5000ms timeout. On failure, log an error and return a safe default — no retry.

- [ ] **Step 1: Replace panicStop() (line ~51)**

  Change:
  ```cpp
  void AnimationController::panicStop()
  {
      ESP_LOGI(TAG, "Panicing!");
      auto cleared = false;

      while (!cleared)
      {
          if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
          {
              for (auto &script : this->scriptQueue)
              {
                  script = "";
              }
              this->queueFront = 0;
              this->queueRear = -1;
              this->queueSize = 0;

              this->scriptEvents.clear();
              this->scriptLoaded = false;
              cleared = true;
              xSemaphoreGive(this->animationMutex);
          }
          else
          {
              vTaskDelay(pdMS_TO_TICKS(10));
          }
      }
      // TODO
  }
  ```
  To:
  ```cpp
  void AnimationController::panicStop()
  {
      ESP_LOGI(TAG, "Panicing!");

      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "panicStop: failed to acquire animationMutex within 5s — state may be inconsistent");
          return;
      }

      for (auto &script : this->scriptQueue)
      {
          script = "";
      }
      this->queueFront = 0;
      this->queueRear = -1;
      this->queueSize = 0;

      this->scriptEvents.clear();
      this->scriptLoaded = false;
      xSemaphoreGive(this->animationMutex);
  }
  ```

- [ ] **Step 2: Replace queueScript() (line ~88)**

  Change:
  ```cpp
  bool AnimationController::queueScript(std::string scriptId)
  {
      this->queueing = true;

      ESP_LOGI(TAG, "Queueing %s", scriptId.c_str());

      if (queueIsFull())
      {
          ESP_LOGI(TAG, "Queue is full");
          return false;
      }

      while (this->queueing)
      {
          if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
          {
              this->queueRear = (queueRear + 1) % queueCapacity;

              this->scriptQueue[queueRear] = scriptId;

              this->queueSize++;
              this->queueing = false;
              xSemaphoreGive(this->animationMutex);
          }
          else
          {
              vTaskDelay(pdMS_TO_TICKS(10));
          }
      }

      return true;
  }
  ```
  To:
  ```cpp
  bool AnimationController::queueScript(std::string scriptId)
  {
      this->queueing = true;

      ESP_LOGI(TAG, "Queueing %s", scriptId.c_str());

      if (queueIsFull())
      {
          ESP_LOGI(TAG, "Queue is full");
          this->queueing = false;
          return false;
      }

      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "queueScript: failed to acquire animationMutex within 5s");
          this->queueing = false;
          return false;
      }

      this->queueRear = (queueRear + 1) % queueCapacity;
      this->scriptQueue[queueRear] = scriptId;
      this->queueSize++;
      this->queueing = false;
      xSemaphoreGive(this->animationMutex);

      return true;
  }
  ```

- [ ] **Step 3: Replace queueCommand() (line ~112)**

  Change:
  ```cpp
  bool AnimationController::queueCommand(std::string command)
  {
      auto queued = false;
      while (!queued)
      {
          if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
          {
              AnimationCommand cmd = AnimationCommand(command);
              this->scriptEvents.push_back(cmd);
              queued = true;
              xSemaphoreGive(this->animationMutex);
          }
          else
          {
              vTaskDelay(pdMS_TO_TICKS(10));
          }
      }
      return false;
  }
  ```
  To:
  ```cpp
  bool AnimationController::queueCommand(std::string command)
  {
      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "queueCommand: failed to acquire animationMutex within 5s");
          return false;
      }

      AnimationCommand cmd = AnimationCommand(command);
      this->scriptEvents.push_back(cmd);
      xSemaphoreGive(this->animationMutex);
      return true;
  }
  ```
  Note: the original returned `false` unconditionally on success — that appears to be a pre-existing bug. The replacement returns `true` on success and `false` on timeout. This is a correction, not a new change.

- [ ] **Step 4: Replace loadNextScript() (line ~151)**

  Change:
  ```cpp
      while (!retrieved)
      {
          if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
          {
              ESP_LOGI(TAG, "Loading script %s", this->scriptQueue[this->queueFront].c_str());

              std::string path = "scripts/" + this->scriptQueue[this->queueFront];

              script = AstrOs_Storage.readFile(path);

              this->queueFront = (this->queueFront + 1) % this->queueCapacity;
              this->queueSize--;

              retrieved = true;
              xSemaphoreGive(this->animationMutex);
          }
          else
          {
              vTaskDelay(pdMS_TO_TICKS(10));
          }
      }
  ```
  To:
  ```cpp
      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "loadNextScript: failed to acquire animationMutex within 5s");
          this->scriptLoaded = false;
          return;
      }

      ESP_LOGI(TAG, "Loading script %s", this->scriptQueue[this->queueFront].c_str());

      std::string path = "scripts/" + this->scriptQueue[this->queueFront];

      script = AstrOs_Storage.readFile(path);

      this->queueFront = (this->queueFront + 1) % this->queueCapacity;
      this->queueSize--;
      xSemaphoreGive(this->animationMutex);
  ```
  Also remove the now-unused `auto retrieved = false;` local variable declaration and the surrounding while loop scaffolding. The full replacement for `loadNextScript()` is:
  ```cpp
  void AnimationController::loadNextScript()
  {
      if (queueIsEmpty() || this->queueing)
      {
          this->scriptLoaded = false;
          return;
      }

      std::string script = "error";

      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "loadNextScript: failed to acquire animationMutex within 5s");
          this->scriptLoaded = false;
          return;
      }

      ESP_LOGI(TAG, "Loading script %s", this->scriptQueue[this->queueFront].c_str());

      std::string path = "scripts/" + this->scriptQueue[this->queueFront];

      script = AstrOs_Storage.readFile(path);

      this->queueFront = (this->queueFront + 1) % this->queueCapacity;
      this->queueSize--;
      xSemaphoreGive(this->animationMutex);

      if (script == "error")
      {
          ESP_LOGI(TAG, "Script not loaded");
          this->scriptLoaded = false;
      }
      else
      {
          AnimationController::parseScript(script);
          this->scriptLoaded = true;
      }
  }
  ```

- [ ] **Step 5: Replace getNextCommandPtr() (line ~240)**

  Change:
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
  To:
  ```cpp
  std::unique_ptr<CommandTemplate> AnimationController::getNextCommandPtr()
  {
      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "getNextCommandPtr: failed to acquire animationMutex within 5s");
          return nullptr;
      }

      std::unique_ptr<CommandTemplate> cmd;

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

          // 10 milliseconds minimum between events
          if (this->delayTillNextEvent < 10)
          {
              this->delayTillNextEvent = 10;
          }

          cmd = scriptEvents.back().GetCommandTemplatePtr();
          this->scriptEvents.pop_back();
      }

      xSemaphoreGive(this->animationMutex);
      return cmd;
  }
  ```

- [ ] **Step 6: Replace parseScript() (line ~292)**

  Change:
  ```cpp
  void AnimationController::parseScript(std::string script)
  {
      auto parsed = false;

      while (!parsed)
      {
          if (xSemaphoreTake(this->animationMutex, portMAX_DELAY) == pdTRUE)
          {
              this->scriptEvents.clear();

              auto parts = AstrOsStringUtils::splitString(script, ';');

              for (auto part : parts)
              {
                  if (part.empty())
                  {
                      continue;
                  }
                  AnimationCommand cmd = AnimationCommand(part);
                  this->scriptEvents.push_back(cmd);
              }

              std::reverse(this->scriptEvents.begin(), this->scriptEvents.end());

              ESP_LOGI(TAG, "Loaded: %s", script.c_str());
              ESP_LOGI(TAG, "Events loaded: %d", this->scriptEvents.size());
              parsed = true;
              xSemaphoreGive(this->animationMutex);
          }
          else
          {
              vTaskDelay(pdMS_TO_TICKS(10));
          }
      }
  }
  ```
  To:
  ```cpp
  void AnimationController::parseScript(std::string script)
  {
      if (xSemaphoreTake(this->animationMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "parseScript: failed to acquire animationMutex within 5s — script will not load");
          return;
      }

      this->scriptEvents.clear();

      auto parts = AstrOsStringUtils::splitString(script, ';');

      for (auto part : parts)
      {
          if (part.empty())
          {
              continue;
          }
          AnimationCommand cmd = AnimationCommand(part);
          this->scriptEvents.push_back(cmd);
      }

      std::reverse(this->scriptEvents.begin(), this->scriptEvents.end());

      ESP_LOGI(TAG, "Loaded: %s", script.c_str());
      ESP_LOGI(TAG, "Events loaded: %d", this->scriptEvents.size());
      xSemaphoreGive(this->animationMutex);
  }
  ```

- [ ] **Step 7: Verify no remaining portMAX_DELAY in AnimationController**

  ```bash
  grep -n 'portMAX_DELAY' /home/jeff/Source/astros/AstrOs.ESP/lib/AnimationController/src/AnimationController.cpp
  ```
  Expected: no matches.

- [ ] **Step 8: Build to confirm no new warnings or errors**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ```
  Expected: `[SUCCESS]`.

- [ ] **Step 9: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add lib/AnimationController/src/AnimationController.cpp
  git commit -m "fix: replace portMAX_DELAY spin-waits in AnimationController with 5s timeouts

  Six methods used infinite-wait retry loops on animationMutex. A
  deadlock or priority inversion would block those callers permanently
  with no diagnostic. Replacing with pdMS_TO_TICKS(5000) single-take
  means a stuck mutex logs an error and returns a safe default, keeping
  the system observable and recoverable."
  ```

---

## Task 4: EspNow spin-wait elimination

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

Five methods use the `while (!set) { take 100ms; else delay 10ms; }` pattern. Replace each with a single `xSemaphoreTake` using a 1000ms timeout. Note that `pdMS_TO_TICKS` is already in scope via `<freertos/FreeRTOS.h>` (included at line 11 of the .cpp).

- [x] **Step 1: Replace getMac()**

  Change:
  ```cpp
  std::string AstrOsEspNow::getMac()
  {
      if (isMasterNode)
      {
          return "00:00:00:00:00:00";
      }

      std::string macAddress;

      bool macSet = false;
      while (!macSet)
      {
          if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
          {
              macAddress = this->mac;
              macSet = true;
              xSemaphoreGive(valueMutex);
          }
          else
          {
              vTaskDelay(10 / portTICK_PERIOD_MS);
          }
      }

      return macAddress;
  }
  ```
  To:
  ```cpp
  std::string AstrOsEspNow::getMac()
  {
      if (isMasterNode)
      {
          return "00:00:00:00:00:00";
      }

      if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "getMac: failed to acquire valueMutex within 1s, returning empty");
          return "";
      }

      std::string macAddress = this->mac;
      xSemaphoreGive(valueMutex);
      return macAddress;
  }
  ```

- [x] **Step 2: Replace getName()**

  Change:
  ```cpp
  std::string AstrOsEspNow::getName()
  {
      std::string name;

      bool nameSet = false;
      while (!nameSet)
      {
          if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
          {
              name = this->name;
              nameSet = true;
              xSemaphoreGive(valueMutex);
          }
          else
          {
              vTaskDelay(10 / portTICK_PERIOD_MS);
          }
      }

      return name;
  }
  ```
  To:
  ```cpp
  std::string AstrOsEspNow::getName()
  {
      if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "getName: failed to acquire valueMutex within 1s, returning empty");
          return "";
      }

      std::string name = this->name;
      xSemaphoreGive(valueMutex);
      return name;
  }
  ```

- [x] **Step 3: Replace getFingerprint()**

  Change:
  ```cpp
  std::string AstrOsEspNow::getFingerprint()
  {
      std::string fingerprint;

      bool fingerprintSet = false;
      while (!fingerprintSet)
      {
          if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
          {
              fingerprint = this->fingerprint;
              fingerprintSet = true;
              xSemaphoreGive(valueMutex);
          }
          else
          {
              vTaskDelay(10 / portTICK_PERIOD_MS);
          }
      }

      return fingerprint;
  }
  ```
  To:
  ```cpp
  std::string AstrOsEspNow::getFingerprint()
  {
      if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "getFingerprint: failed to acquire valueMutex within 1s, returning empty");
          return "";
      }

      std::string fingerprint = this->fingerprint;
      xSemaphoreGive(valueMutex);
      return fingerprint;
  }
  ```

- [x] **Step 4: Replace updateFingerprint()**

  Change:
  ```cpp
  void AstrOsEspNow::updateFingerprint(std::string fingerprint)
  {
      bool fingerprintSet = false;
      while (!fingerprintSet)
      {
          if (xSemaphoreTake(valueMutex, 100 / portTICK_PERIOD_MS))
          {
              this->fingerprint = fingerprint;
              fingerprintSet = true;
              xSemaphoreGive(valueMutex);
          }
          else
          {
              vTaskDelay(10 / portTICK_PERIOD_MS);
          }
      }
  }
  ```
  To:
  ```cpp
  void AstrOsEspNow::updateFingerprint(std::string fingerprint)
  {
      if (xSemaphoreTake(valueMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "updateFingerprint: failed to acquire valueMutex within 1s — fingerprint not updated");
          return;
      }

      this->fingerprint = fingerprint;
      xSemaphoreGive(valueMutex);
  }
  ```

- [x] **Step 5: Replace updateMasterMac()**

  Change:
  ```cpp
  void AstrOsEspNow::updateMasterMac(uint8_t *macAddress)
  {
      bool macSet = false;
      while (!macSet)
      {
          if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
          {
              memcpy(this->masterMac, macAddress, ESP_NOW_ETH_ALEN);
              macSet = true;
              xSemaphoreGive(masterMacMutex);
          }
          else
          {
              vTaskDelay(10 / portTICK_PERIOD_MS);
          }
      }
  }
  ```
  To:
  ```cpp
  void AstrOsEspNow::updateMasterMac(uint8_t *macAddress)
  {
      if (xSemaphoreTake(masterMacMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "updateMasterMac: failed to acquire masterMacMutex within 1s — master MAC not updated");
          return;
      }

      memcpy(this->masterMac, macAddress, ESP_NOW_ETH_ALEN);
      xSemaphoreGive(masterMacMutex);
  }
  ```

- [x] **Step 6: Replace getMasterMac()**

  Change:
  ```cpp
  void AstrOsEspNow::getMasterMac(uint8_t *macAddress)
  {
      bool macSet = false;
      while (!macSet)
      {
          if (xSemaphoreTake(masterMacMutex, 100 / portTICK_PERIOD_MS))
          {
              memcpy(macAddress, this->masterMac, ESP_NOW_ETH_ALEN);
              macSet = true;
              xSemaphoreGive(masterMacMutex);
          }
          else
          {
              vTaskDelay(10 / portTICK_PERIOD_MS);
          }
      }
  }
  ```
  To:
  ```cpp
  void AstrOsEspNow::getMasterMac(uint8_t *macAddress)
  {
      if (xSemaphoreTake(masterMacMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "getMasterMac: failed to acquire masterMacMutex within 1s — output buffer unchanged");
          return;
      }

      memcpy(macAddress, this->masterMac, ESP_NOW_ETH_ALEN);
      xSemaphoreGive(masterMacMutex);
  }
  ```

- [x] **Step 7: Verify no remaining spin-wait patterns**

  ```bash
  grep -n 'portTICK_PERIOD_MS\|while (!.*Set)' /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
  ```
  Expected: no matches.

- [x] **Step 8: Build to confirm no new warnings or errors**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ```
  Expected: `[SUCCESS]`.

- [x] **Step 9: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
  git commit -m "fix: collapse EspNow spin-wait loops to single-take with 1s timeout

  Six methods used while(!set){take 100ms; else delay 10ms;} retry
  loops. A sustained mutex hold would spin indefinitely with wasted
  delay cycles. Replace with a single pdMS_TO_TICKS(1000) take that
  logs a warning and returns a safe default on timeout."
  ```

---

## Task 5: I2cModule pthread → FreeRTOS

**Files:**
- Modify: `lib/Modules/src/I2cModule.cpp`

The file currently uses `pthread_mutex_t` for bus serialisation. Every other sync primitive in the codebase uses FreeRTOS `SemaphoreHandle_t`. This task aligns I2cModule with that pattern.

- [ ] **Step 1: Read I2cModule.cpp**

  Open `lib/Modules/src/I2cModule.cpp`. Confirm:
  - Line 14: `static pthread_mutex_t i2cMutex;`
  - Line 28: `pthread_mutex_init(&i2cMutex, NULL) != 0`
  - Lines 59, 125, 135: `pthread_mutex_lock(&i2cMutex) == 0`
  - Lines 68, 129, 137: `pthread_mutex_unlock(&i2cMutex)`

- [ ] **Step 2: Replace the include and declaration**

  The file does not currently include the FreeRTOS semaphore header explicitly (it arrives transitively through `<AstrOsDisplay.hpp>` → ... but rely on the explicit header). Add explicit includes and change the mutex declaration.

  Change the top of the file from:
  ```cpp
  #include <AnimationCommands.hpp>
  #include <AstrOsDisplay.hpp>
  #include <AstrOsUtility.h>
  #include <I2cModule.hpp>

  #include <driver/i2c.h>
  #include <esp_log.h>
  #include <esp_system.h>
  #include <ssd1306.h>
  #include <sstream>
  #include <string>

  static const char *TAG = "I2cModule";
  static pthread_mutex_t i2cMutex;
  ```
  To:
  ```cpp
  #include <AnimationCommands.hpp>
  #include <AstrOsDisplay.hpp>
  #include <AstrOsUtility.h>
  #include <I2cModule.hpp>

  #include <driver/i2c.h>
  #include <esp_log.h>
  #include <esp_system.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>
  #include <ssd1306.h>
  #include <sstream>
  #include <string>

  static const char *TAG = "I2cModule";
  static SemaphoreHandle_t i2cMutex = NULL;
  ```

- [ ] **Step 3: Replace mutex initialisation in Init()**

  Change:
  ```cpp
      if (pthread_mutex_init(&i2cMutex, NULL) != 0)
      {
          ESP_LOGE(TAG, "Failed to initialize the I2C mutex");
      }
  ```
  To:
  ```cpp
      i2cMutex = xSemaphoreCreateMutex();
      if (i2cMutex == NULL)
      {
          ESP_LOGE(TAG, "Failed to create I2C mutex");
          return ESP_ERR_NO_MEM;
      }
  ```

- [ ] **Step 4: Replace mutex lock/unlock in write()**

  Change:
  ```cpp
  esp_err_t I2cModule::write(uint8_t addr, uint8_t *data, size_t size)
  {
      esp_err_t ret = ESP_OK;
      if (pthread_mutex_lock(&i2cMutex) == 0)
      {
          i2c_cmd_handle_t cmd = i2c_cmd_link_create();
          i2c_master_start(cmd);
          i2c_master_write_byte(cmd, (addr << 1) | WRITE_BIT, ACK_CHECK_EN);
          i2c_master_write(cmd, data, size, ACK_CHECK_EN);
          i2c_master_stop(cmd);
          ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
          i2c_cmd_link_delete(cmd);
          pthread_mutex_unlock(&i2cMutex);
      }
      return ret;
  }
  ```
  To:
  ```cpp
  esp_err_t I2cModule::write(uint8_t addr, uint8_t *data, size_t size)
  {
      esp_err_t ret = ESP_OK;
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
      {
          i2c_cmd_handle_t cmd = i2c_cmd_link_create();
          i2c_master_start(cmd);
          i2c_master_write_byte(cmd, (addr << 1) | WRITE_BIT, ACK_CHECK_EN);
          i2c_master_write(cmd, data, size, ACK_CHECK_EN);
          i2c_master_stop(cmd);
          ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_PERIOD_MS);
          i2c_cmd_link_delete(cmd);
          xSemaphoreGive(i2cMutex);
      }
      else
      {
          ESP_LOGW(TAG, "write: failed to acquire i2cMutex within 1s");
          ret = ESP_ERR_TIMEOUT;
      }
      return ret;
  }
  ```

- [ ] **Step 5: Replace mutex lock/unlock in writeSsd1306()**

  Change:
  ```cpp
      if (pthread_mutex_lock(&i2cMutex) == 0)
      {
          ssd1306_clear_line(&oled, line, false);
          ssd1306_display_text(&oled, line, c, len, false);
          pthread_mutex_unlock(&i2cMutex);
      }
  ```
  To:
  ```cpp
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
      {
          ssd1306_clear_line(&oled, line, false);
          ssd1306_display_text(&oled, line, c, len, false);
          xSemaphoreGive(i2cMutex);
      }
      else
      {
          ESP_LOGW(TAG, "writeSsd1306: failed to acquire i2cMutex within 1s");
      }
  ```

- [ ] **Step 6: Replace mutex lock/unlock in clearSsd1306()**

  Change:
  ```cpp
  void I2cModule::clearSsd1306(int line)
  {
      if (pthread_mutex_lock(&i2cMutex) == 0)
      {
          ssd1306_clear_line(&oled, line, false);
          pthread_mutex_unlock(&i2cMutex);
      }
  }
  ```
  To:
  ```cpp
  void I2cModule::clearSsd1306(int line)
  {
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
      {
          ssd1306_clear_line(&oled, line, false);
          xSemaphoreGive(i2cMutex);
      }
      else
      {
          ESP_LOGW(TAG, "clearSsd1306: failed to acquire i2cMutex within 1s");
      }
  }
  ```

- [ ] **Step 7: Verify no remaining pthread references in I2cModule.cpp**

  ```bash
  grep -n 'pthread' /home/jeff/Source/astros/AstrOs.ESP/lib/Modules/src/I2cModule.cpp
  ```
  Expected: no matches.

- [ ] **Step 8: Build to confirm no new warnings or errors**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ```
  Expected: `[SUCCESS]`.

- [ ] **Step 9: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add lib/Modules/src/I2cModule.cpp
  git commit -m "fix: replace pthread_mutex_t with FreeRTOS SemaphoreHandle_t in I2cModule

  I2cModule was the only module using POSIX thread primitives. Replacing
  with FreeRTOS semphr aligns it with the rest of the codebase, allows
  FreeRTOS priority inheritance to function correctly on the I2C bus
  lock, and adds explicit timeout/error logging on contention."
  ```

---

## Task 6: maestroModules map mutex

**Files:**
- Modify: `src/main.cpp`

`maestroModules` is a `std::map<int, MaestroModule *>` accessed from at least four different execution contexts without synchronisation:
- `servoMoveTimerCallback` — esp_timer task (core 1), iterating
- `loadMaestroConfigs` — called from `app_main` and potentially service queue, iterating + modifying
- `servoQueueTask` — task on core 1, find + at
- `handleServoTest` — called from `interfaceResponseQueueTask` on core 1, find + at

- [x] **Step 1: Add the mutex declaration after maestroModules**

  In `src/main.cpp`, find the line (approximately line 109):
  ```cpp
  static std::map<int, MaestroModule *> maestroModules;
  ```
  Change it to:
  ```cpp
  static std::map<int, MaestroModule *> maestroModules;
  static SemaphoreHandle_t maestroModulesMutex = NULL;
  ```

- [x] **Step 2: Create the mutex in init()**

  In `src/main.cpp`, in the `init()` function, after the queue creation block (after the `espnowQueue = xQueueCreate(...)` line at approximately line 235), add:
  ```cpp
      maestroModulesMutex = xSemaphoreCreateMutex();
      if (maestroModulesMutex == NULL)
      {
          ESP_LOGE(TAG, "Failed to create maestroModulesMutex");
      }
  ```

- [x] **Step 3: Wrap servoMoveTimerCallback (line ~596)**

  Change:
  ```cpp
  static void servoMoveTimerCallback(void *arg)
  {
      for (auto maestroMod : maestroModules)
      {
          maestroMod.second->CheckServos(300);
      }

      ESP_ERROR_CHECK(esp_timer_start_once(servoMoveTimer, 300 * 1000));
  }
  ```
  To:
  ```cpp
  static void servoMoveTimerCallback(void *arg)
  {
      if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
          for (auto maestroMod : maestroModules)
          {
              maestroMod.second->CheckServos(300);
          }
          xSemaphoreGive(maestroModulesMutex);
      }
      else
      {
          ESP_LOGW(TAG, "servoMoveTimerCallback: skipping servo check, maestroModulesMutex busy");
      }

      ESP_ERROR_CHECK(esp_timer_start_once(servoMoveTimer, 300 * 1000));
  }
  ```
  Note: 100ms timeout is used here because `servoMoveTimerCallback` runs on the esp_timer task. A long block would stall other timers. If the mutex is held (e.g., during `loadMaestroConfigs`), this cycle is safely skipped.

- [x] **Step 4: Wrap servoQueueTask maestroModules access (lines ~1164–1170)**

  Change:
  ```cpp
              if (maestroModules.find(msg.message_id) == maestroModules.end())
              {
                  ESP_LOGE(TAG, "Maestro module %d not found", msg.message_id);
              }
              else
              {
                  maestroModules.at(msg.message_id)->QueueCommand(msg.data);
              }
              free(msg.data);
  ```
  To:
  ```cpp
              if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
              {
                  if (maestroModules.find(msg.message_id) == maestroModules.end())
                  {
                      ESP_LOGE(TAG, "Maestro module %d not found", msg.message_id);
                  }
                  else
                  {
                      maestroModules.at(msg.message_id)->QueueCommand(msg.data);
                  }
                  xSemaphoreGive(maestroModulesMutex);
              }
              else
              {
                  ESP_LOGW(TAG, "servoQueueTask: failed to acquire maestroModulesMutex within 1s");
              }
              free(msg.data);
  ```

- [x] **Step 5: Wrap handleServoTest maestroModules access (lines ~1754–1760)**

  Change:
  ```cpp
          if (maestroModules.find(idx) == maestroModules.end())
          {
              ESP_LOGE(TAG, "Maestro module %d not found", idx);
          }
          else
          {
              maestroModules.at(idx)->SetServoPosition(servo, ms);
          }
  ```
  To:
  ```cpp
          if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
          {
              if (maestroModules.find(idx) == maestroModules.end())
              {
                  ESP_LOGE(TAG, "Maestro module %d not found", idx);
              }
              else
              {
                  maestroModules.at(idx)->SetServoPosition(servo, ms);
              }
              xSemaphoreGive(maestroModulesMutex);
          }
          else
          {
              ESP_LOGW(TAG, "handleServoTest: failed to acquire maestroModulesMutex within 1s");
          }
  ```

- [x] **Step 6: Wrap loadMaestroConfigs() — entire function body**

  `loadMaestroConfigs` iterates, inserts, updates, and erases from `maestroModules`. The whole function body must run under the mutex. The function is called from `app_main` (before tasks start — no contention there) and may be called again from service queue task at runtime (where contention is possible).

  The complete rewrite of `loadMaestroConfigs()`:
  ```cpp
  static void loadMaestroConfigs()
  {
      ESP_LOGI(TAG, "Loading Maestro configurations");

      if (xSemaphoreTake(maestroModulesMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "loadMaestroConfigs: failed to acquire maestroModulesMutex within 1s");
          return;
      }

      // get list of current maestro modules
      std::vector<int> currentModules;
      for (auto maestroMod : maestroModules)
      {
          currentModules.push_back(maestroMod.first);
      }

      ESP_LOGI(TAG, "Current Maestro module count: %d", currentModules.size());

      // load maestro configurations from storage
      auto maestroConfigs = AstrOs_Storage.loadMaestroConfigs();

      ESP_LOGI(TAG, "Loaded Maestro modules from file: %d", maestroConfigs.size());

      for (auto &cfg : maestroConfigs)
      {
          // if the module is already loaded, update it and remove from the list
          if (maestroModules.find(cfg.idx) != maestroModules.end())
          {
              // only allow modules to use uart 1 if this is not a master node
              if (cfg.uartChannel == 1 && isMasterNode)
              {
                  ESP_LOGE(TAG, "Master node cannot use UART channel 1 for Maestro module %d", cfg.idx);
                  continue; // skip invalid configurations
              }
              else if (cfg.uartChannel == 1)
              {
                  maestroModules[cfg.idx]->UpdateConfig(serialCh1Queue, cfg.baudrate);
              }
              else if (cfg.uartChannel == 2)
              {
                  maestroModules[cfg.idx]->UpdateConfig(serialCh2Queue, cfg.baudrate);
              }
              else
              {
                  ESP_LOGE(TAG, "Invalid UART channel %d for Maestro module %d", cfg.uartChannel, cfg.idx);
                  continue; // skip invalid configurations
              }
              currentModules.erase(std::remove(currentModules.begin(), currentModules.end(), cfg.idx),
                                   currentModules.end());
          }
          else
          {
              // if the module is not loaded, create a new one
              if (cfg.uartChannel == 1 && isMasterNode)
              {
                  ESP_LOGE(TAG, "Master node cannot use UART channel 1 for Maestro module %d", cfg.idx);
                  continue; // skip invalid configurations
              }
              if (cfg.uartChannel == 1)
              {
                  MaestroModule *maestroMod = new MaestroModule(serialCh1Queue, cfg.idx, cfg.baudrate);
                  maestroModules[cfg.idx] = maestroMod;
              }
              else if (cfg.uartChannel == 2)
              {
                  MaestroModule *maestroMod = new MaestroModule(serialCh2Queue, cfg.idx, cfg.baudrate);
                  maestroModules[cfg.idx] = maestroMod;
              }
              else
              {
                  ESP_LOGE(TAG, "Invalid UART channel %d for Maestro module %d", cfg.uartChannel, cfg.idx);
                  continue; // skip invalid configurations
              }
          }
      }

      // remove any modules that are no longer in the configuration
      for (int idx : currentModules)
      {
          ESP_LOGI(TAG, "Removing Maestro module: %d", idx);
          if (maestroModules.find(idx) != maestroModules.end())
          {
              delete maestroModules[idx];
              maestroModules.erase(idx);
          }
      }

      ESP_LOGI(TAG, "Maestro module count: %d", maestroModules.size());

      // load servo configurations from storage — these calls don't modify the map,
      // but LoadConfig may interact with the hardware; keep under lock for consistency
      for (auto maestroMod : maestroModules)
      {
          maestroMod.second->LoadConfig();
      }

      xSemaphoreGive(maestroModulesMutex);
  }
  ```

- [x] **Step 7: Verify no unprotected maestroModules accesses remain**

  ```bash
  grep -n 'maestroModules' /home/jeff/Source/astros/AstrOs.ESP/src/main.cpp
  ```
  Every access should now either be inside a `xSemaphoreTake(maestroModulesMutex, ...)` block or inside the `loadMaestroConfigs` function (which acquires the mutex at entry). The declaration line and mutex line are acceptable bare references.

- [x] **Step 8: Build to confirm no new warnings or errors**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ```
  Expected: `[SUCCESS]`.

- [x] **Step 9: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add src/main.cpp
  git commit -m "fix: protect maestroModules map with FreeRTOS mutex

  maestroModules is accessed from the esp_timer task (servoMoveTimerCallback),
  servoQueueTask, interfaceResponseQueueTask (handleServoTest), and
  app_main (loadMaestroConfigs) without synchronisation. Add
  maestroModulesMutex; use 100ms timeout in the timer callback to avoid
  stalling the esp_timer task, 1s timeout elsewhere."
  ```

---

## Task 7: peers vector mutex

**Files:**
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`
- Modify: `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`

`peers` is a `std::vector<espnow_peer_t>` accessed from the ESP-NOW receive callback (core 0) and the polling timer callback without any mutex protection. `cachePeer`, `getPeers`, `findPeer`, `isValidPollPeer`, `pollPadawans`, and `pollRepsonseTimeExpired` all touch `peers` directly.

The file-scope `masterMacMutex` and `valueMutex` are already declared as `SemaphoreHandle_t` file-scope statics. Add `peersMutex` as a private class member (parallel pattern to how the others are used).

- [ ] **Step 1: Add peersMutex to the header**

  In `lib/AstrOsEspNow/src/AstrOsEspNowService.hpp`, add `peersMutex` to the private member list. The existing private members start at line 36 (`std::string name;`). Add immediately after `std::vector<espnow_peer_t> peers;` (line 41):

  Change:
  ```cpp
      std::vector<espnow_peer_t> peers;
      QueueHandle_t serviceQueue;
  ```
  To:
  ```cpp
      std::vector<espnow_peer_t> peers;
      SemaphoreHandle_t peersMutex;
      QueueHandle_t serviceQueue;
  ```
  Note: `SemaphoreHandle_t` is available because the header already includes `<freertos/FreeRTOS.h>` and `<freertos/queue.h>`; add `<freertos/semphr.h>` to the header includes if it is not already present. Check the current includes block and add it after `<freertos/queue.h>`:
  ```cpp
  #include <freertos/FreeRTOS.h>
  #include <freertos/queue.h>
  #include <freertos/semphr.h>
  ```

- [ ] **Step 2: Initialise peersMutex in AstrOsEspNow::init()**

  In `lib/AstrOsEspNow/src/AstrOsEspNowService.cpp`, in `init()`, after the `valueMutex` creation block (approximately lines 92–98), add:
  ```cpp
      this->peersMutex = xSemaphoreCreateMutex();
      if (this->peersMutex == NULL)
      {
          ESP_LOGE(TAG, "Failed to initialize the peers mutex");
          return ESP_FAIL;
      }
  ```

- [ ] **Step 3: Wrap the init() peers.push_back at line ~121**

  The peer-list loading loop in `init()` runs before any tasks are spawned — no contention is possible at this point. However, for consistency (and in case `init` is ever called after tasks start), wrap the loop:

  Change:
  ```cpp
      // Load current peer list.
      for (int i = 0; i < config.peerCount; i++)
      {

          auto peer = config.peers[i];

          err = this->addPeer(peer.mac_addr);
          if (logError(TAG, __FUNCTION__, __LINE__, err))
          {
              return err;
          }

          peers.push_back(peer);
      }
  ```
  To:
  ```cpp
      // Load current peer list.
      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "init: failed to acquire peersMutex during peer list load");
          return ESP_FAIL;
      }

      for (int i = 0; i < config.peerCount; i++)
      {
          auto peer = config.peers[i];

          err = this->addPeer(peer.mac_addr);
          if (logError(TAG, __FUNCTION__, __LINE__, err))
          {
              xSemaphoreGive(this->peersMutex);
              return err;
          }

          this->peers.push_back(peer);
      }

      xSemaphoreGive(this->peersMutex);
  ```

- [ ] **Step 4: Wrap cachePeer() — entire body**

  Change:
  ```cpp
  bool AstrOsEspNow::cachePeer(uint8_t *macAddress, std::string name)
  {

      if (peers.size() > 10)
      {
          ESP_LOGE(TAG, "Peer cache is full");
          return false;
      }

      for (auto &peer : peers)
      {
          if (memcmp(peer.mac_addr, macAddress, ESP_NOW_ETH_ALEN) == 0)
          {
              ESP_LOGW(TAG, "Peer already cached");
              return true;
          }
      }

      // add to peer cache
      espnow_peer_t newPeer;

      // peer id is 0 indexed
      newPeer.id = peers.size();
      size_t nameLen = std::min(name.length(), (size_t)15);
      memcpy(newPeer.name, name.c_str(), nameLen);
      newPeer.name[nameLen] = '\0';
      memcpy(newPeer.mac_addr, macAddress, ESP_NOW_ETH_ALEN);
      memset(newPeer.crypto_key, 0, ESP_NOW_KEY_LEN);
      newPeer.is_paired = true;

      peers.push_back(newPeer);

      return this->cachePeerCallback(newPeer);
  }
  ```
  To:
  ```cpp
  bool AstrOsEspNow::cachePeer(uint8_t *macAddress, std::string name)
  {
      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGE(TAG, "cachePeer: failed to acquire peersMutex within 1s");
          return false;
      }

      if (this->peers.size() > 10)
      {
          ESP_LOGE(TAG, "Peer cache is full");
          xSemaphoreGive(this->peersMutex);
          return false;
      }

      for (auto &peer : this->peers)
      {
          if (memcmp(peer.mac_addr, macAddress, ESP_NOW_ETH_ALEN) == 0)
          {
              ESP_LOGW(TAG, "Peer already cached");
              xSemaphoreGive(this->peersMutex);
              return true;
          }
      }

      // add to peer cache
      espnow_peer_t newPeer;

      // peer id is 0 indexed
      newPeer.id = this->peers.size();
      size_t nameLen = std::min(name.length(), (size_t)15);
      memcpy(newPeer.name, name.c_str(), nameLen);
      newPeer.name[nameLen] = '\0';
      memcpy(newPeer.mac_addr, macAddress, ESP_NOW_ETH_ALEN);
      memset(newPeer.crypto_key, 0, ESP_NOW_KEY_LEN);
      newPeer.is_paired = true;

      this->peers.push_back(newPeer);

      xSemaphoreGive(this->peersMutex);

      return this->cachePeerCallback(newPeer);
  }
  ```
  Note: `cachePeerCallback` is called **after** the mutex is released — the callback calls into storage (NVS), which may itself block. Holding `peersMutex` during the callback would risk priority inversion with the storage layer.

- [ ] **Step 5: Wrap getPeers() — entire body**

  Change:
  ```cpp
  std::vector<espnow_peer_t> AstrOsEspNow::getPeers()
  {
      std::vector<espnow_peer_t> result;

      result.clear();

      espnow_peer_t master = {.id = 0, .name = "master"};

      memcpy(master.mac_addr, nullMac, ESP_NOW_ETH_ALEN);

      result.push_back(master);

      for (auto &peer : peers)
      {
          result.push_back(peer);
      }

      return result;
  }
  ```
  To:
  ```cpp
  std::vector<espnow_peer_t> AstrOsEspNow::getPeers()
  {
      std::vector<espnow_peer_t> result;

      espnow_peer_t master = {.id = 0, .name = "master"};
      memcpy(master.mac_addr, nullMac, ESP_NOW_ETH_ALEN);
      result.push_back(master);

      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "getPeers: failed to acquire peersMutex within 1s, returning master-only list");
          return result;
      }

      for (auto &peer : this->peers)
      {
          result.push_back(peer);
      }

      xSemaphoreGive(this->peersMutex);
      return result;
  }
  ```

- [ ] **Step 6: Wrap findPeer() — entire body**

  Change:
  ```cpp
  bool AstrOsEspNow::findPeer(std::string peerMac)
  {
      for (auto &p : peers)
      {
          auto pMac = AstrOsStringUtils::macToString(p.mac_addr);

          if (memcmp(pMac.c_str(), peerMac.c_str(), peerMac.size()) == 0)
          {
              return true;
          }
      }

      return false;
  }
  ```
  To:
  ```cpp
  bool AstrOsEspNow::findPeer(std::string peerMac)
  {
      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "findPeer: failed to acquire peersMutex within 1s, returning false");
          return false;
      }

      bool found = false;
      for (auto &p : this->peers)
      {
          auto pMac = AstrOsStringUtils::macToString(p.mac_addr);
          if (memcmp(pMac.c_str(), peerMac.c_str(), peerMac.size()) == 0)
          {
              found = true;
              break;
          }
      }

      xSemaphoreGive(this->peersMutex);
      return found;
  }
  ```

- [ ] **Step 7: Wrap isValidPollPeer() — entire body**

  Change:
  ```cpp
  bool AstrOsEspNow::isValidPollPeer(std::string peerMac)
  {
      for (auto &p : peers)
      {
          auto pMac = AstrOsStringUtils::macToString(p.mac_addr);

          if (memcmp(pMac.c_str(), peerMac.c_str(), ESP_NOW_ETH_ALEN) == 0)
          {
              p.pollAckThisCycle = true;
              return true;
          }
      }

      return false;
  }
  ```
  To:
  ```cpp
  bool AstrOsEspNow::isValidPollPeer(std::string peerMac)
  {
      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "isValidPollPeer: failed to acquire peersMutex within 1s, returning false");
          return false;
      }

      bool found = false;
      for (auto &p : this->peers)
      {
          auto pMac = AstrOsStringUtils::macToString(p.mac_addr);
          if (memcmp(pMac.c_str(), peerMac.c_str(), ESP_NOW_ETH_ALEN) == 0)
          {
              p.pollAckThisCycle = true;
              found = true;
              break;
          }
      }

      xSemaphoreGive(this->peersMutex);
      return found;
  }
  ```

- [ ] **Step 8: Wrap pollPadawans() — entire body**

  Change:
  ```cpp
  void AstrOsEspNow::pollPadawans()
  {
      for (auto &peer : peers)
      {
          if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
          {
              continue;
          }

          peer.pollAckThisCycle = false;

          astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::POLL, this->mac)[0];

          if (esp_now_send(peer.mac_addr, data.data, data.size) != ESP_OK)
          {
              ESP_LOGE(TAG, "Error sending poll message to " MACSTR, MAC2STR(peer.mac_addr));
          }

          free(data.data);
      }
  }
  ```
  To:
  ```cpp
  void AstrOsEspNow::pollPadawans()
  {
      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "pollPadawans: failed to acquire peersMutex within 1s, skipping poll cycle");
          return;
      }

      for (auto &peer : this->peers)
      {
          if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
          {
              continue;
          }

          peer.pollAckThisCycle = false;

          astros_espnow_data_t data = this->messageService.generateEspNowMsg(AstrOsPacketType::POLL, this->mac)[0];

          if (esp_now_send(peer.mac_addr, data.data, data.size) != ESP_OK)
          {
              ESP_LOGE(TAG, "Error sending poll message to " MACSTR, MAC2STR(peer.mac_addr));
          }

          free(data.data);
      }

      xSemaphoreGive(this->peersMutex);
  }
  ```

- [ ] **Step 9: Wrap pollRepsonseTimeExpired() — entire body**

  Change:
  ```cpp
  void AstrOsEspNow::pollRepsonseTimeExpired()
  {
      for (auto &peer : peers)
      {
          if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
          {
              continue;
          }

          if (!peer.pollAckThisCycle)
          {
              ESP_LOGD(TAG, "Poll response time expired for %s:" MACSTR, peer.name, MAC2STR(peer.mac_addr));

              auto macStr = AstrOsStringUtils::macToString(peer.mac_addr);

              this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SEND_POLL_NAK, "", macStr, peer.name, "");
          }
          else
          {
              peer.pollAckThisCycle = false;
          }
      }
  }
  ```
  To:
  ```cpp
  void AstrOsEspNow::pollRepsonseTimeExpired()
  {
      if (xSemaphoreTake(this->peersMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      {
          ESP_LOGW(TAG, "pollRepsonseTimeExpired: failed to acquire peersMutex within 1s, skipping expiry check");
          return;
      }

      for (auto &peer : this->peers)
      {
          if (memcmp(peer.mac_addr, broadcastMac, ESP_NOW_ETH_ALEN) == 0)
          {
              continue;
          }

          if (!peer.pollAckThisCycle)
          {
              ESP_LOGD(TAG, "Poll response time expired for %s:" MACSTR, peer.name, MAC2STR(peer.mac_addr));

              auto macStr = AstrOsStringUtils::macToString(peer.mac_addr);

              this->sendToInterfaceQueue(AstrOsInterfaceResponseType::SEND_POLL_NAK, "", macStr, peer.name, "");
          }
          else
          {
              peer.pollAckThisCycle = false;
          }
      }

      xSemaphoreGive(this->peersMutex);
  }
  ```

- [ ] **Step 10: Search for remaining bare `peers` accesses**

  ```bash
  grep -n '\bpeers\b' /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
  ```
  Every access should now be inside a `peersMutex`-protected block. The only acceptable bare access is `this->peers = {};` in `init()` at line 50 — this occurs before `peersMutex` is created and before any task runs, so it is safe.

- [ ] **Step 11: Build both envs and run native tests**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -20
  ~/.platformio/penv/bin/pio run -e lolin_d32_pro 2>&1 | tail -20
  ~/.platformio/penv/bin/pio test -e test 2>&1 | tail -30
  ```
  Expected: both firmware builds `[SUCCESS]`, all native tests pass (46/46).

- [ ] **Step 12: Commit**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add lib/AstrOsEspNow/src/AstrOsEspNowService.hpp
  git add lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
  git commit -m "fix: protect AstrOsEspNow peers vector with FreeRTOS mutex

  The peers vector is accessed from the ESP-NOW receive callback (core 0)
  and the polling timer callback simultaneously with no synchronisation.
  Add peersMutex (SemaphoreHandle_t private member) protecting all read
  and write accesses: cachePeer, getPeers, findPeer, isValidPollPeer,
  pollPadawans, pollRepsonseTimeExpired, and the init peer-load loop."
  ```

---

## Task 8: QA test plan

**Files:**
- Create: `.docs/qa/code-review-phase-b.md`

- [ ] **Step 1: Write the QA test plan**

  Create `.docs/qa/code-review-phase-b.md` with the following content:

  ```markdown
  # Code Review Phase B — Manual QA Test Plan

  ## Preconditions

  - Both boards (metro_s3 and lolin_d32_pro) flashed with Phase B firmware
  - One board configured as master, one as padawan
  - AstrOs host application connected to master via USB serial at 115200 baud
  - At least one animation script deployed to the SD card as `scripts/test.txt`
  - Serial monitor open on both boards during tests

  ## Test Cases

  ### TC-01: Boot and role assignment

  1. Power on both boards.
  2. Observe serial logs on master: verify log contains `Master node using UART channel 1`.
  3. Observe serial logs on padawan: verify log contains `Padawan node using UART channel 0`.
  4. Verify no log line contains `rank =` (the `rank` variable has been removed).

  **Expected:** Both boards boot without error, roles are correctly identified.

  ### TC-02: ESP-NOW peer registration

  1. With both boards running, observe master serial log.
  2. Wait up to 30 seconds for padawan to send a registration request.
  3. Verify master log shows `Peer already cached` or `Peer registered` — not a mutex timeout log.
  4. Verify padawan log shows `Registration ACK received`.

  **Expected:** Registration completes without any `failed to acquire peersMutex` warnings.

  ### TC-03: Polling under normal load

  1. After registration, wait for 3 polling cycles (6 seconds).
  2. Verify master log shows POLL sent entries and POLL_ACK received entries.
  3. Verify no `failed to acquire peersMutex` warnings appear.

  **Expected:** Polling completes every 2 s with clean logs.

  ### TC-04: Animation playback

  1. From the host application, queue the `test.txt` script on the master.
  2. Observe serial log for `Loading script`, `Events loaded:`, and command dispatch entries.
  3. Verify physical servo movement occurs (if servos connected) or that servo queue messages are logged.
  4. Let the animation complete. Verify log shows script unloaded cleanly.

  **Expected:** Animation plays without `failed to acquire animationMutex` errors.

  ### TC-05: panicStop during animation

  1. Start an animation (TC-04 procedure).
  2. While animation is running, send a panic stop command from the host application.
  3. Observe master log for `Panicing!` followed by clean state reset.
  4. Verify no `failed to acquire animationMutex` errors.
  5. Queue a new script and verify it plays successfully after the panic stop.

  **Expected:** Panic stop completes within one animation tick cycle, system recovers.

  ### TC-06: Concurrent animation + polling + servo

  1. Start an animation that exercises multiple servo commands.
  2. While animation runs, observe polling continues every 2 s.
  3. Verify `servoMoveTimerCallback` fires every 300 ms (log entries visible at DEBUG level).
  4. Run for at least 60 seconds without reboot or error.

  **Expected:** No mutex timeout warnings, no task watchdog triggers, system stable.

  ### TC-07: Config reload via service command

  1. From the host, send a SET_CONFIG command that includes a Maestro module configuration.
  2. Observe master log for `Loading Maestro configurations` and module count changes.
  3. Verify no `failed to acquire maestroModulesMutex` warnings.
  4. While the config reload is in progress (if timing allows), send a servo test command and verify it queues correctly after reload.

  **Expected:** Maestro config reloads cleanly; concurrent servo commands do not corrupt the map.

  ### TC-08: I2C OLED display during animation

  1. With `USE_I2C_OLED` defined (if board supports it), run an animation.
  2. Observe display updates during animation playback.
  3. Check serial log for any `failed to acquire i2cMutex` warnings.

  **Expected:** Display updates do not interfere with animation; no mutex timeouts.

  ### TC-09: EspNow getter contention (code inspection)

  This test cannot be reliably reproduced in hardware because the getter contention window is microseconds. Verify by code inspection:

  - Confirm `getMac`, `getName`, `getFingerprint`, `updateFingerprint`, `getMasterMac`, `updateMasterMac` each contain exactly one `xSemaphoreTake` call and one `xSemaphoreGive` call.
  - Confirm no `while (!set)` or `vTaskDelay` patterns remain in those functions.

  Run:
  ```bash
  grep -n 'while.*Set\|portTICK_PERIOD_MS' lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
  ```
  Expected: no matches.

  ### TC-10: lolin_d32_pro build and boot

  1. Flash the lolin_d32_pro board with the Phase B firmware.
  2. Observe serial log for clean boot: version, SHA, role assignment.
  3. Run TC-02 with lolin_d32_pro as padawan.

  **Expected:** Identical behavior to metro_s3.

  ## Acceptance Criteria

  - [ ] Both `metro_s3` and `lolin_d32_pro` firmware builds succeed with `[SUCCESS]`
  - [ ] Native test suite passes: `pio test -e test` reports 46/46 tests pass
  - [ ] Pre-commit hook passes with no clang-format diffs
  - [ ] TC-01 through TC-10 pass on at least one hardware pair
  - [ ] Zero `failed to acquire ...Mutex` warnings during TC-06 (60-second soak)
  - [ ] No ESP32 task watchdog triggers during any test case
  - [ ] No `rank` string variable references remain in `src/main.cpp`
  - [ ] No `portMAX_DELAY` references remain in `lib/AnimationController/`
  - [ ] No `pthread_mutex` references remain in `lib/Modules/src/I2cModule.cpp`

  ## Out of scope (Phase C)

  - `std::stoi` → `strtol` migration in `handleServoTest` and `handleRunSctipt`
  - Queue depth increases
  - Stack size bumps
  - `animationTimerCallback` refactor
  - Filesystem path sanitization
  ```

- [ ] **Step 2: Commit the QA plan**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP
  git add .docs/qa/code-review-phase-b.md
  git commit -m "docs: add manual QA test plan for code-review Phase B"
  ```

---

## Final verification

- [ ] **Run all three build environments**

  ```bash
  ~/.platformio/penv/bin/pio run -e metro_s3 2>&1 | tail -5
  ~/.platformio/penv/bin/pio run -e lolin_d32_pro 2>&1 | tail -5
  ~/.platformio/penv/bin/pio test -e test 2>&1 | tail -10
  ```
  Expected: all three show success, native tests 46/46.

- [ ] **Verify no portMAX_DELAY remains in AnimationController**

  ```bash
  grep -rn 'portMAX_DELAY' /home/jeff/Source/astros/AstrOs.ESP/lib/AnimationController/
  ```
  Expected: no matches.

- [ ] **Verify no pthread remains in I2cModule**

  ```bash
  grep -n 'pthread' /home/jeff/Source/astros/AstrOs.ESP/lib/Modules/src/I2cModule.cpp
  ```
  Expected: no matches.

- [ ] **Verify no spin-wait pattern remains in EspNowService**

  ```bash
  grep -n 'portTICK_PERIOD_MS\|while.*Set\b' /home/jeff/Source/astros/AstrOs.ESP/lib/AstrOsEspNow/src/AstrOsEspNowService.cpp
  ```
  Expected: no matches.

- [ ] **Verify rank variable is gone from main.cpp**

  ```bash
  grep -n '\brank\b' /home/jeff/Source/astros/AstrOs.ESP/src/main.cpp
  ```
  Expected: no matches.

- [ ] **Pre-commit hook clean pass**

  ```bash
  cd /home/jeff/Source/astros/AstrOs.ESP && git diff --name-only HEAD
  ```
  If any files appear modified after the hook ran on the last commit, the formatter found something. Stage and commit the formatted result.

---

## Out of scope (deferred to Phase C)

- `std::stoi` → `strtol` migration in `handleServoTest` and `handleRunSctipt`
- Queue depth increases
- Stack size bumps driven by high-water-mark warnings
- `animationTimerCallback` refactor (stack pressure + leak hazards)
- Filesystem validation and path sanitization
- Error counters for silent failures
