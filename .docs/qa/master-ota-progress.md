# OTA — Master Self-Flash Progress QA

Verifies the master emits `FW_PROGRESS` (`VERIFYING` → `FLASHING` → `REBOOTING`)
for its own self-flash, under the sentinel MAC `00:00:00:00:00:00`, so the web
UI renders the master row through the same stages as a padawan.

Plan: `.docs/plans/20260530-1430-master-ota-progress.md`.
Change is forwarder-only (`lib/OtaForwarder/src/OtaForwarder.cpp`).

## Preconditions

- A master node (real hardware) connected to the AstrOs server / Pi over the
  interface UART (serial channel 1, 115200).
- The web app open on the firmware-deploy view, server logs tailable.
- A valid new firmware image stageable to the master (different version string
  than what's running, so the post-reboot heartbeat confirms a real change).
- At least one deploy target list that **includes the master** (sentinel MAC in
  the order). Test both: (a) master + ≥1 padawan, (b) master-only.

## Happy path — master + padawan deploy

### Steps
1. Select a padawan **and** the master in the deploy UI; start the deploy.
2. Watch the master row in the UI and the server log as the deploy proceeds
   past the padawan(s) to the master self-flash.

### Expected results
- Padawan row advances normally (`SENDING` %→ `VERIFYING` → `FLASHING` →
  `REBOOTING` → `VERSION_CONFIRMED`).
- Master row (sentinel `00:00:00:00:00:00`) — which the server pre-set to
  `SENDING` during upload — then advances:
  - `VERIFYING` appears as the master begins hashing the staged image.
  - `FLASHING` appears once the image is handed to the writer (stays for the
    multi-second write + read-back verify + commit window).
  - `REBOOTING` appears just before the master restarts.
- After reboot, the master's heartbeat resolves the row
  `FINALIZING → VERSION_CONFIRMED` with the new version string.
- **No** `protocol_violation` / "illegal flash-job transition" in the server log
  for the sentinel MAC.
- Each master `FW_PROGRESS` carries `bytesSent == totalBytes` == the firmware
  size (non-zero), and `controllerId == 00:00:00:00:00:00`.

## Happy path — master-only deploy

### Steps
1. Select **only** the master; start the deploy.

### Expected results
- Same master-row progression as above (`VERIFYING → FLASHING → REBOOTING`,
  then heartbeat → `VERSION_CONFIRMED`).
- `totalBytes` is the real firmware size (regression guard: `firmwareTotalSize_`
  is now set on the master path — previously only set during a padawan transfer,
  so a master-only deploy would otherwise have reported `0`).

## Negative path 1 — staged image fails verification

Force a bad/oversized/missing staged image (e.g., corrupt `staging.bin` or a
size mismatch) before the master self-flash step.

### Expected results
- `VERIFYING` may appear (emitted before hashing), then the master records
  `FAILED`; `FW_DEPLOY_DONE` carries the master row as `FAILED` with the reason
  (`firmware_sha_failed` / `no_firmware` / `firmware_stat_failed`).
- Server takes the row `Verifying → Failed` (or `Sending → Failed` if the failure
  precedes the `VERIFYING` emit). **No** `REBOOTING`, **no** reboot.
- No illegal-transition error in the server log.

## Negative path 2 — writer reports flash failure

Induce a writer-side flash failure (e.g., partition write / `esp_ota_end` error)
so `OtaWriter` posts `OTA_FWD_LOCAL_FLASH_RESULT` = FAILED.

### Expected results
- Master row shows `VERIFYING` then `FLASHING`, then `FW_DEPLOY_DONE` carries it
  as `FAILED`. Server takes `Flashing → Failed`. **No** `REBOOTING`, **no**
  reboot (bootloader still points at the running image).

## Negative path 3 — self-flash timeout

Make `OtaWriter` hang / never post a result (or queue-full the post).

### Expected results
- After the 60 s `masterSelfFlashTimer`, master records `FAILED`
  (`self_flash_timeout`) and emits `FW_DEPLOY_DONE`. Master row ends `FAILED`
  (`Flashing → Failed`). No reboot, no illegal transition.

## Wire-order check (optional, with a serial sniffer on ch1)

Confirm the on-wire `FW_PROGRESS` sequence for the sentinel MAC during a
successful self-flash is exactly:

```
FW_PROGRESS … 00:00:00:00:00:00 VERIFYING  <size> <size>
FW_PROGRESS … 00:00:00:00:00:00 FLASHING   <size> <size>
FW_PROGRESS … 00:00:00:00:00:00 REBOOTING  <size> <size>
FW_DEPLOY_DONE … 00:00:00:00:00:00 PENDING
```

`VERIFYING` must precede `FLASHING` must precede `REBOOTING` — out-of-order
emission fails the whole deploy server-side (`failDeployPhase('protocol_violation')`).
`REBOOTING` must precede `FW_DEPLOY_DONE`.
