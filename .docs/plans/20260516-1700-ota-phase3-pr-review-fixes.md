# OTA Phase 3 PR-Review Fixes

Light plan to resolve the findings from the comprehensive PR review of
`feature/ota-phase3-wire-up` (1 Critical, 8 Important, 12 Suggestions).

Findings are grouped into 5 commits so a regression bisects to a single
class of fix. Each commit is independently shippable and reviewable.

## Tasks

- [x] **Commit 1 — Critical: `handleEnd` parse-failure must not clobber a live transfer**
    - C1: `OtaReceiver.cpp:285-298` — gate cleanup on `active_` + transferId match before tearing down state
    - I8 partial: native test for `OTA_MSG_WATCHDOG_FIRE` layout (nullptr transferId, no union arm)

- [x] **Commit 2 — Error-path hardening**
    - I1: reject `payloadLen == 0` in `parseFwChunk` (PURE) before the dispatcher mallocs
    - I2: bound `astrosRxTask` per-byte append with LOGE + reset on overflow
    - I4: lower END `xQueueSend` timeout from 500 ms → 50 ms (match CHUNK)
    - I6: upgrade base64 decode failure logs LOGW → LOGE (protocol violations, not transient)
    - S1: replace `abort()` on unknown `ota_msg_kind_t` with LOGE + free + return
    - S5: defensively `free(msg.transferId)` in the WATCHDOG_FIRE arm of `process()`
    - S6: check `xQueueCreate` and `xTaskCreatePinnedToCore` return values in `init()` for OTA

- [x] **Commit 3 — Extract `chunksForSize()` to PURE + tests (TDD)**
    - I7: move the ceil-divide out of `AstrOsSerialMsgHandler.cpp:402` into `AstrOsSerialProtocol` (PURE)
    - 4 unit tests: exact multiple, +1 byte, totalSize < chunkSize, totalSize == 0

- [x] **Commit 4 — Comments + quick code wins**
    - S10: fix inaccurate / stale comments (transferIdStr_, beginMsgId_ removal, milestone refs, c.6c.1 → structural)
    - S2: switch `otaReceiverTask` poll from 10 ms `vTaskDelay` to blocking `xQueueReceive`
    - S3: check `esp_timer_start_once` return and LOGW on non-OK
    - S4: drop the double-LOGE in `handleDeployBegin` empty-results path
    - S8: introduce `kSha256HexLen` constant for the 64/65 magic in `OtaQueueMessage.h`

- [x] **Commit 5 — Type design tightening + QA + remaining tests**

- [x] **Commit 6 — Round-2 polish (PR-toolkit re-review findings)**
    - QA-1: fix 3 log-string mismatches in `ota-master-serial-receive.md`
    - S5: convert `SHA256_HEX_LEN` macro → scoped enum constant
    - S1: defensive `chunkSize == 0` guard in `chunksForSize` + native test
    - S3: 5 native tests for `freeOtaMsg` (one per `ota_msg_kind_t`)
    - S6: explicit `default:` arm in `freeOtaMsg` switch with rationale comment
    - S7: idempotency guard in `OtaReceiver::Init()` (LOGW + early return)
    - S4 + S8: extract `shouldTeardownOnEndResult()` to `AstrOsBulkTransport` (PURE),
      lead with `Status` for future-enumerator safety, 5 native tests pin every
      EndResult case, wire into `OtaReceiver::handleEnd`
    - S7: add `freeOtaMsg(queue_ota_msg_t&)` helper and use from `process()` arms + producer failure branches
    - S9: delete copy/move ctors on `OtaReceiver`; release timer in `~OtaReceiver()`
    - S11: expand `.docs/qa/ota-master-serial-receive.md` negative-case rigor to match `ota-receiver-watchdog.md`
    - S12: `PollAckMessageWithEmptyVariant` — assert against raw wire bytes (trailing US), not post-split size
    - I3: document wall-clock max-transfer guard as Phase 4 follow-up (do not implement here)

## Deferred (not in this plan)

- I3 implementation — wall-clock max-transfer deadline. Belongs with future deploy/flash work where the policy is decided alongside abort semantics.
- Pre-existing `pdTICKS_TO_MS(250)` misuse in `AstrOsSerialMsgHandler.cpp:140` — predates this branch.
