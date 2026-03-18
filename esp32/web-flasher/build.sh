#!/usr/bin/env bash
# Build both ESP32 firmwares and copy binaries for the web flasher.
# Requires PlatformIO CLI (pio) to be installed.
#
# Usage: ./build.sh
#
# Output: firmware/scanner/*.bin and firmware/uplink/*.bin

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="$(dirname "$SCRIPT_DIR")"
FW_DIR="$SCRIPT_DIR/firmware"

echo "=== Building Scanner firmware (ESP32-S3) ==="
cd "$ESP32_DIR/scanner"
pio run

echo ""
echo "=== Building Uplink firmware (ESP32-C3) ==="
cd "$ESP32_DIR/uplink"
pio run

echo ""
echo "=== Copying binaries to web-flasher/firmware/ ==="

# Scanner binaries
mkdir -p "$FW_DIR/scanner"
cp "$ESP32_DIR/scanner/.pio/build/scanner/bootloader.bin"      "$FW_DIR/scanner/"
cp "$ESP32_DIR/scanner/.pio/build/scanner/partition-table.bin"  "$FW_DIR/scanner/"
cp "$ESP32_DIR/scanner/.pio/build/scanner/firmware.bin"         "$FW_DIR/scanner/"

# Uplink binaries
mkdir -p "$FW_DIR/uplink"
cp "$ESP32_DIR/uplink/.pio/build/uplink/bootloader.bin"      "$FW_DIR/uplink/"
cp "$ESP32_DIR/uplink/.pio/build/uplink/partition-table.bin"  "$FW_DIR/uplink/"
cp "$ESP32_DIR/uplink/.pio/build/uplink/firmware.bin"         "$FW_DIR/uplink/"

echo ""
echo "=== Done ==="
echo "Scanner: $(ls -lh "$FW_DIR/scanner/firmware.bin" | awk '{print $5}')"
echo "Uplink:  $(ls -lh "$FW_DIR/uplink/firmware.bin" | awk '{print $5}')"
echo ""
echo "Serve locally with: cd $SCRIPT_DIR && python3 -m http.server 8080"
