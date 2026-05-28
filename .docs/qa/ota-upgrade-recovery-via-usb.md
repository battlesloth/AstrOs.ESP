# OTA bricked board — USB recovery procedure

If a Phase A bench case (or worse, a production OTA) leaves a board
unable to boot AND the auto-rollback safety net also fails, this is the
recovery procedure. Both supported boards expose USB; the procedure is
identical except for the chip flag.

## Preconditions

- Physical access to the bricked board.
- USB cable.
- A workstation with `esptool.py` installed (`pip install esptool` or via
  PlatformIO's bundled toolchain).
- A known-good firmware .bin for the board's environment. Easiest source:
  download the latest RC artifact from the AstrOs.ESP GitHub Releases
  page (`https://github.com/<owner>/AstrOs.ESP/releases`). Pick the
  matching environment (`firmware-lolin_d32_pro-*.bin` or
  `firmware-metro_s3-*.bin`).

## Procedure

### Step 1 — Identify the USB serial port

Plug the board in. On Linux:

```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

On macOS:

```bash
ls /dev/cu.usbserial-* /dev/cu.usbmodem-*
```

Note the device path (e.g. `/dev/ttyUSB0`).

### Step 2 — Hold the board in download mode

- `lolin_d32_pro`: press and hold the `BOOT` (or `IO0`) button while
  pressing/releasing `RESET`, then release `BOOT`. The board is now in
  bootloader mode.
- `metro_s3`: double-tap the `RESET` button — the board enters native USB
  bootloader mode (it appears as a USB mass-storage device).

### Step 3 — Erase flash

```bash
esptool.py --port /dev/ttyUSB0 --chip esp32 erase_flash         # lolin_d32_pro
esptool.py --port /dev/ttyUSB0 --chip esp32s3 erase_flash       # metro_s3
```

**Expected output**: `Chip erase completed successfully in <N>s. Hard
resetting via RTS pin...`

### Step 4 — Write the known-good firmware

```bash
esptool.py --port /dev/ttyUSB0 --chip esp32 write_flash \
    --flash_mode dio --flash_size detect 0x1000 firmware-lolin_d32_pro-<version>.bin
```

For `metro_s3` adjust `--chip` to `esp32s3`. The base address `0x1000` is
where the second-stage bootloader sits — this is the standard PlatformIO
partition layout for both boards.

**Expected output**: `Hash of data verified. Leaving... Hard resetting
via RTS pin...`

### Step 5 — Confirm recovery

Open a serial monitor:

```bash
pio device monitor -p /dev/ttyUSB0 -b 115200
```

**Expected first lines**:

```
I (nnn) AstrOs.ESP: AstrOs.ESP version <V> (sha: <SHA>)
```

If you see the version banner, the board is recovered. Re-pair it with
the master if applicable.

## Common failures

- **`esptool.py: command not found`** — install esptool: `pip install
  esptool`.
- **`A fatal error occurred: Failed to connect to ESP32: Timed out
  waiting for packet header`** — the board is not in download mode.
  Repeat Step 2 carefully.
- **Erase succeeds but write fails partway** — the .bin may be for the
  wrong environment (variant mismatch between `lolin_d32_pro` and
  `metro_s3`). Double-check the .bin filename against the board.
