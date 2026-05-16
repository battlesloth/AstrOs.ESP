#!/usr/bin/env bash
# build_ota_test.sh — Build two metro_s3 firmware binaries with different versions
# for OTA flash testing.
#
# Usage:
#   ./scripts/build_ota_test.sh [version_a] [version_b]
#
# Defaults to current VERSION as v_a and a patch-bumped version as v_b.
# Outputs are placed in .ota-test/ at the project root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PIO="${PIO:-$(command -v pio 2>/dev/null || echo "${HOME}/.platformio/penv/bin/pio")}"
VERSION_FILE="${PROJECT_DIR}/VERSION"
FIRMWARE_SRC="${PROJECT_DIR}/.pio/build/metro_s3/firmware.bin"
OUT_DIR="${PROJECT_DIR}/.ota-test"

# ── Read original version ──────────────────────────────────────────────────
ORIGINAL_VERSION="$(cat "${VERSION_FILE}")"

# ── Resolve version_a / version_b from args or defaults ───────────────────
VERSION_A="${1:-${ORIGINAL_VERSION}}"

if [[ -n "${2:-}" ]]; then
    VERSION_B="$2"
else
    # Bump the patch number of VERSION_A to derive a default VERSION_B.
    IFS='.' read -r major minor patch <<< "${VERSION_A%%-*}"
    VERSION_B="${major}.${minor}.$((patch + 1))"
fi

echo "================================================================"
echo "  AstrOs OTA test build"
echo "  Version A : ${VERSION_A}"
echo "  Version B : ${VERSION_B}"
echo "  Output    : .ota-test/"
echo "================================================================"

mkdir -p "${OUT_DIR}"

# Restore VERSION on exit (success or failure).
restore_version() {
    echo "${ORIGINAL_VERSION}" > "${VERSION_FILE}"
    echo "[build_ota_test] Restored VERSION to ${ORIGINAL_VERSION}"
}
trap restore_version EXIT

# ── Build A ───────────────────────────────────────────────────────────────
echo
echo ">>> Building VERSION_A: ${VERSION_A}"
echo "${VERSION_A}" > "${VERSION_FILE}"
(cd "${PROJECT_DIR}" && "${PIO}" run -e metro_s3)
cp "${FIRMWARE_SRC}" "${OUT_DIR}/astros-esp-${VERSION_A}-metro_s3-app.bin"
echo "    Saved: .ota-test/astros-esp-${VERSION_A}-metro_s3-app.bin"

# ── Build B ───────────────────────────────────────────────────────────────
echo
echo ">>> Building VERSION_B: ${VERSION_B}"
echo "${VERSION_B}" > "${VERSION_FILE}"
(cd "${PROJECT_DIR}" && "${PIO}" run -e metro_s3)
cp "${FIRMWARE_SRC}" "${OUT_DIR}/astros-esp-${VERSION_B}-metro_s3-app.bin"
echo "    Saved: .ota-test/astros-esp-${VERSION_B}-metro_s3-app.bin"

echo
echo "================================================================"
echo "  Done.  Both binaries are in .ota-test/"
ls -lh "${OUT_DIR}"/*.bin
echo "================================================================"
