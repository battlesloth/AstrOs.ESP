# QA Test Plan — Code Review Phase A: Memory Leaks + Buffer Safety

**Branch:** `fix/code-review-phase-a`
**Plan:** `.docs/plans/20260412-1250-code-review-phase-a.md`
**Related:** `.docs/code-review/code-review.md` (issues #1, #4, #6, #11, #12, #23, #28)

## Preconditions

- Firmware built from `fix/code-review-phase-a` with all Phase A commits applied
- One master node and at least one padawan node available
- Serial monitor connected at 115200 baud (`pio device monitor -e metro_s3`)
- Both nodes flashed and able to communicate via ESP-NOW

## Test 1: Polling timer heap soak (Task 2 — fingerprint leak)

**Target:** Master node. **Verifies:** 37-byte leak every 2s is gone.

1. Flash master node with Phase A firmware
2. Open serial monitor; note the initial "free heap" value from the maintenance timer (logs every 10s)
3. Let the master run idle for 30 minutes (~900 poll cycles)
4. Compare final free heap vs initial

**Expected:** Free heap stable within ~100-200 bytes of initial. Pre-fix baseline would have lost ~33 KB over 30 min.

## Test 2: Animation queue stress (Task 7 — unique_ptr ownership)

**Target:** Any node. **Verifies:** RAII cleanup works under load.

1. Queue 5 animation scripts in rapid succession via serial interface
2. Let all 5 run to completion
3. Queue another script, let it complete
4. Monitor serial for crashes, guru meditations, or heap corruption messages

**Expected:** All scripts execute normally. No crashes, no memory corruption. Behavior identical to pre-fix (unique_ptr is transparent at runtime).

## Test 3: Display command under queue-full (Task 3 — DisplayService leak)

**Target:** Any node with OLED. **Verifies:** Queue-failure path frees data.

1. Flash with Phase A firmware
2. Trigger a burst of 20+ rapid display updates (animation with 0ms delays or repeated manual triggers)
3. Watch for "Failed to send display command to hardware queue" log lines
4. Note free heap before and after the burst

**Expected:** If queue sends fail, the `free(i2cMsg.data)` cleanup runs. Heap returns to baseline after burst. Pre-fix: each failed send leaked a string buffer.

## Test 4: ESP-NOW peer with long name (Task 6 — name bounds check)

**Target:** Master node. **Verifies:** memcpy bounds check prevents overflow.

1. Using the config interface, register a peer with a name longer than 15 characters (e.g., `"VeryLongPeerName123"`)
2. Verify the peer appears in the list
3. Verify the stored name is truncated to 15 chars (`"VeryLongPeerNam"`)
4. Verify no crash, no corrupt state afterward

**Expected:** Peer registers with truncated name. No buffer overflow into adjacent `crypto_key` field. Names ≤ 15 chars stored unchanged.

## Test 5: GUID format visual check (Task 1 — snprintf)

**Target:** Any node. **Verifies:** snprintf produces same output as sprintf for well-formed data.

1. Trigger a GUID generation (send a command that creates a message with origination ID)
2. Observe GUID in serial logs

**Expected:** GUID in standard hex-dash format (e.g., `a1b2c3d4-e5f6-a7b8-c9d0-e1f2a3b4c5d6`). No truncation, no garbage. Smoke test — snprintf output identical to sprintf for in-range data.

## Edge cases / negative tests

- **NvsManager setKeyId (Task 5):** A peer index ≥ 100 triggers `ESP_LOGE` and early return. Current peer limit is well below 100, so verify by code inspection — no runtime test needed unless limit is raised.
- **AnimationController destructor (Task 4):** `AnimationCtrl` is a global singleton never destroyed at runtime. Verify correctness by code inspection — `vSemaphoreDelete` is called only on teardown, which doesn't happen in production.
- **unique_ptr nullptr path (Task 7):** The `if (cmd == nullptr)` early-return at `main.cpp:453` works with `unique_ptr` (it's contextually convertible to `bool` and compares to `nullptr`). Verify by forcing a null return path and confirming no crash or double-delete.

## Acceptance

Phase A is complete when all 5 tests pass on hardware AND:
- `pio run -e lolin_d32_pro` builds clean
- `pio run -e metro_s3` builds clean
- `pio test -e test` passes 46/46
- PR validation CI passes all 4 checks
