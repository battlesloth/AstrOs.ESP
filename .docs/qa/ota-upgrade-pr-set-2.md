# OTA Upgrade — PR set 2 QA test plan

Bench validation for the real flash-commit work shipped in PR set 2. Each
phase ships independently; this doc grows with the PR for each phase.

## Phase A — Padawan flash + master version-confirm gate

> **One-time precondition before testing A.3 (auto-rollback)**: enabling
> `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` requires a new bootloader image
> in flash. Existing field devices booted from the prior bootloader will
> still treat the rollback API calls as no-ops. Before bench-testing A.3,
> re-flash both boards via USB (`pio run -e <env> -t upload`) so the new
> bootloader with rollback enabled is in place. Subsequent OTAs benefit
> from the safety net.

### Preconditions

- One master board flashed with the Phase A firmware (post-merge).
- At least one padawan board flashed with the *previous* firmware (so we
  can observe a version change).
- Pi connected to master via USB serial at 115200 baud.
- AstrOs.Server running and able to upload a firmware .bin to the master.
- Build a "next" firmware on the Phase A branch with a bumped `VERSION`
  file so the .bin's `esp_app_desc_t.version` differs from what the
  padawan is currently running.

### Test case A.1 — Padawan happy path

1. From the server UI, upload the next .bin to the master.
2. Wait for the server's "staged" indicator.
3. Initiate a deploy targeting the single padawan.
4. **Expected serial-log progression on the padawan** (visible via
   `pio device monitor -e <env>`):
   - `OTA_BEGIN` received, partition opened, `OtaWriter: active`.
   - `OtaWriter: streaming` — chunks arriving, periodic stats.
   - `handleEnd: sleeping 2s before esp_ota_set_boot_partition`.
   - `handleEnd: boot partition flipped; reporting OK and rebooting`.
   - Padawan reboots.
   - On reboot: `AstrOs.ESP version <NEW> (sha: …)`.
   - First poll cycle (within ~2 s): `OTA rollback cancelled — running
     image is now valid`.
5. **Expected master-side FW_PROGRESS sequence** (visible in server UI
   for the padawan row):
   - `SENDING` (multiple, growing %) → `VERIFYING` → `FLASHING` →
     `REBOOTING` → `VERSION_CONFIRMED`.
6. **Expected DEPLOY_DONE result** for the padawan row:
   `status=OK, finalVersion=<NEW>, errorReason=""`.

### Test case A.2 — Padawan flash failure injection

1. Apply the temporary debug patch:
   ```
   // In OtaWriter::handleEnd, immediately before esp_ota_set_boot_partition:
   bootErr = ESP_FAIL; // FORCED FAILURE
   goto report_failed; // adapt to local control flow — skip the real call
   ```
2. Build and flash the padawan with the patched firmware.
3. From the server, trigger a deploy with a *different* .bin (so the
   padawan would otherwise want to flash).
4. **Expected**: padawan never calls `esp_ota_set_boot_partition`,
   reports `OtaFlashStatus::FAILED` with reason `"ESP_FAIL"`, does not
   reboot. Master records FAILED, deploy advances.
5. Revert the debug patch before continuing.

### Test case A.3 — Padawan boot crash + auto-rollback

1. Apply the temporary debug patch to the `app_main` entry point:
   ```
   void app_main() {
       abort(); // FORCED CRASH
       // ... rest unchanged
   }
   ```
2. Build the .bin with the patch and a bumped `VERSION` so it's
   identifiable.
3. Stage it on the master via the server.
4. Trigger a deploy to a clean padawan (running the previous good
   firmware).
5. **Expected**:
   - Padawan flashes, reports OK, reboots.
   - On reboot, new image immediately aborts.
   - Bootloader sees PENDING_VERIFY image crashed → reverts to old
     partition on next boot.
   - Padawan comes up running the *old* firmware.
   - Master's `versionConfirmTimer` (15 s) fires because the heartbeat
     keeps reporting the old version. Master records
     FAILED("version_unconfirmed").
   - Padawan is still functional, just on the old image.
6. Revert the debug patch.

### Test case A.4 — Version-confirm timeout (slow padawan)

1. Apply a temporary debug patch to the padawan to delay its first
   POLL_ACK send post-reboot:
   ```
   // In AstrOsEspNow::handlePoll, at the top:
   static bool delayedOnce = false;
   if (!delayedOnce) {
       delayedOnce = true;
       vTaskDelay(pdMS_TO_TICKS(20000)); // 20 s — beyond master's 15 s timer
   }
   ```
2. Build and flash. Trigger a deploy.
3. **Expected**:
   - Padawan flashes, reboots, comes up on new image.
   - First POLL_ACK doesn't go out for 20 s.
   - Master's 15 s `versionConfirmTimer` fires first → records
     FAILED("version_unconfirmed").
   - Later, padawan eventually sends its first POLL_ACK; new firmware
     calls `mark_app_valid_cancel_rollback` (verifies the late-arrival
     path is harmless).
4. Operator-facing UI shows the padawan as FAILED, but the padawan is
   running new firmware. This is the documented "operator-visible
   mismatch" failure mode — record it; not a bug.
5. Revert the debug patch.

### Edge case bench notes

- If a padawan's `esp_app_desc_t` parse fails at deploy start (unusual,
  would mean the staged .bin is corrupted), the master logs the parse
  error and falls through to the 15 s timeout. The padawan row appears
  as FAILED("version_unconfirmed"). The padawan never actually flashes
  in this case because the streaming SHA verify would catch the
  corruption first — but the parser is defense in depth.

### Recovery

If any test case bricks a padawan (image won't boot AND auto-rollback
also fails), follow `.docs/qa/ota-upgrade-recovery-via-usb.md`.
