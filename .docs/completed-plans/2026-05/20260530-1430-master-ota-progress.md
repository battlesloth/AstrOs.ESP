# Master OTA self-flash progress (verify / flash / reboot)

## Problem

During a firmware deploy, padawan rows surface live progress in the web UI via
`FW_PROGRESS` messages (`SENDING → VERIFYING → FLASHING → REBOOTING`). The
**master self-flash** path emits **no** `FW_PROGRESS` — the master's own row sits
on the server-set `SENDING` state for the whole multi-second self-flash, then
jumps straight to `PENDING` (via `FW_DEPLOY_DONE`) and finally
`VERSION_CONFIRMED` (via the post-reboot heartbeat). The operator sees no
verify/flash/reboot activity for the master.

Goal: the master emits `VERIFYING`, `FLASHING`, and `REBOOTING` `FW_PROGRESS`
messages using its sentinel MAC `00:00:00:00:00:00`, in the same wire format as a
padawan, so the web server's controller-agnostic state machine renders the master
identically.

## Design (forwarder-only — no `OtaWriter` changes)

One `FW_PROGRESS` per stage at 100% (`bytesSent == totalBytes`), mirroring how the
forwarder already emits for padawans (one milestone per stage, not an
in-progress/complete pair). The stages map onto the forwarder's own work:

| Stage | Emitted where | Meaning |
|---|---|---|
| `VERIFYING` | `startMasterSelfFlash`, just before `computeFileSha256` | forwarder hashes + checks the staged image |
| `FLASHING`  | `startMasterSelfFlash`, right after the writer-queue `xQueueSend` succeeds | image handed to `OtaWriter`; covers its write + read-back verify + commit |
| `REBOOTING` | `handleLocalFlashResult` OK branch, before `insertMasterRow`/reboot | about to `esp_restart` |

`VERSION_CONFIRMED` is **not** emitted by the master — it reboots immediately. The
existing `FW_DEPLOY_DONE`=`PENDING` (sentinel row) → server `FINALIZING` →
post-reboot heartbeat → `VERSION_CONFIRMED` flow already handles the finish and is
left untouched.

## Verified against the web server (`AstrOs.Server/astros_api`) — no rework risk

- **Wire format** (`message_handler.ts:304-335`): `FW_PROGRESS` type 38, payload
  `transferId␟controllerId␟stage␟bytesSent␟totalBytes␟detail`. The forwarder's
  existing `sendFwProgress(...)` already produces exactly this.
- **`VERIFYING` is a legal first message for the master row.** The orchestrator
  pre-advances *every* row (including the sentinel-MAC master row) `Queued →
  UploadingToMaster → Sending` when the upload to the master completes, before
  `FW_DEPLOY_BEGIN` (`flash_orchestrator.ts:833-848`). `Sending → Verifying` is
  legal (`flash_job_state_machine.ts:31-33`). So the master needs **no** `SENDING`
  message.
- **Order is mandatory.** Illegal transitions throw and `handleDeployProgress`
  turns that into `failDeployPhase('protocol_violation')`
  (`flash_orchestrator.ts:1294-1300`). Must emit `VERIFYING → FLASHING →
  REBOOTING`; `Sending→Flashing` (skipping verify) would fail the deploy.
- **Failure path stays via `FW_DEPLOY_DONE`.** A writer `FAILED` after we emitted
  `FLASHING` lands `Flashing → Failed` through `handleDeployDone` (legal) — same as
  padawans; no extra `FW_PROGRESS` on failure.
- **Sentinel MAC** `00:00:00:00:00:00` is the master's controller id in the job's
  target list (`post_deploy_heartbeat.ts`), matched in `handleDeployProgress`.

## Tasks

- [x] In `startMasterSelfFlash`: set `currentControllerId_ = "00:00:00:00:00:00"`
      and `firmwareTotalSize_ = expectedSize` early; emit `VERIFYING` before
      `computeFileSha256`. (Removed the redundant later `currentControllerId_`
      assignment.)
- [x] In `startMasterSelfFlash`: emit `FLASHING` immediately after the writer-queue
      `xQueueSend` succeeds.
- [x] In `handleLocalFlashResult` OK branch: emit `REBOOTING` before
      `insertMasterRow` / reboot.
- [x] Compile both boards (`pio run -e metro_s3` and `-e lolin_d32_pro`) — both SUCCESS.
- [x] Add QA plan `.docs/qa/master-ota-progress.md`.

## Out of scope / notes

- No new native logic (the `getFwProgress` builder is already unit-tested); change
  is MIXED forwarder wiring only, covered by the QA plan.
- No `OtaWriter` edits — deliberately avoids the just-stabilized
  `ota_writer_task` stack (PR #47, bench-verify pending).
