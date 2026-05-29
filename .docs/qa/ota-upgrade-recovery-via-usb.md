# OTA bricked board — USB recovery procedure

If a Phase A bench case (or worse, a production OTA) leaves a board
unable to boot AND the auto-rollback safety net also fails, this is the
recovery procedure. Both supported boards expose USB; the procedure
uses PlatformIO so chip-specific bootloader offsets are handled
automatically.

## Preconditions

- Physical access to the bricked board.
- USB cable.
- A workstation set up to build this repo with PlatformIO (the same
  setup used for normal development — see `README.md`).
- The git tag of a known-good firmware version. Find it on the AstrOs.ESP
  GitHub Releases page or in `git tag`.

## Procedure

### Step 1 — Check out the known-good version

```bash
cd <path-to-AstrOs.ESP>
git fetch --tags
git checkout <vX.Y.Z>      # e.g., v1.0.0 — the version you want to recover to
```

### Step 2 — Identify the USB serial port

Plug the board in. On Linux:

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

On macOS:

```bash
ls /dev/cu.usbserial-* /dev/cu.usbmodem-*
```

Note the device path (e.g. `/dev/ttyUSB0` or `/dev/cu.usbserial-XXXX`).

### Step 3 — Put the board in download mode

- **`lolin_d32_pro` (classic ESP32)**: hold the `BOOT` (or `IO0`) button
  while pressing-and-releasing `RESET`, then release `BOOT`. The chip is
  now in the UART download bootloader and PlatformIO can flash it.
- **`metro_s3` (Adafruit Metro ESP32-S3)**: the ESP32-S3 has a built-in
  USB serial/JTAG controller; under normal operation PlatformIO's
  `esptool` invocation triggers download mode over USB automatically.
  If automatic entry fails (e.g., the running app is in a tight crash
  loop on boot), hold the `BOOT` button while pressing-and-releasing
  `RESET`, then release `BOOT` — same sequence as the classic ESP32.
  **Do NOT use double-tap-RESET** — that activates the UF2 bootloader,
  which esptool cannot talk to.

### Step 4 — Flash via PlatformIO

```bash
~/.platformio/penv/bin/pio run -e lolin_d32_pro -t upload --upload-port /dev/ttyUSB0
# or
~/.platformio/penv/bin/pio run -e metro_s3      -t upload --upload-port /dev/ttyUSB0
```

PlatformIO will build the firmware for the target version, invoke
`esptool` with the correct chip flag (`esp32` or `esp32s3`) and the
correct bootloader/partition/app offsets, and reset the board.

**Expected output** ends with:

```
========================= [SUCCESS] Took N seconds =========================
```

### Step 5 — Confirm recovery

Open a serial monitor:

```bash
~/.platformio/penv/bin/pio device monitor -e <env> -p /dev/ttyUSB0
```

**Expected first lines** after the board boots:

```
I (nnn) AstrOs.ESP: AstrOs.ESP version <V> (sha: <SHA>)
```

The version banner confirms the board is recovered. Re-pair it with the
master if applicable.

## Common failures

- **`pio: command not found`** — install PlatformIO Core (see
  `README.md`) or use `~/.platformio/penv/bin/pio` directly.
- **`A fatal error occurred: Failed to connect to ESP32: Timed out
  waiting for packet header`** — the board is not in download mode.
  Repeat Step 3 carefully. For `metro_s3`, prefer the BOOT+RESET
  sequence over relying on automatic entry.
- **Upload starts but partial-writes before failing** — check the cable
  and the USB-port quality. Some USB-C extension cables degrade the
  signal enough to break esptool's framing.
