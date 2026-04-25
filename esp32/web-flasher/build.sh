#!/usr/bin/env bash
# Build current ESP32-S3 firmwares and copy binaries for the web flasher.
# Requires PlatformIO CLI (pio) to be installed.
#
# Usage: ./build.sh
#
# Output: firmware/scanner/*.bin, firmware/scanner-seed/*.bin, firmware/uplink-s3/*.bin

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="$(dirname "$SCRIPT_DIR")"
FW_DIR="$SCRIPT_DIR/firmware"

echo "=== Building Scanner firmware (ESP32-S3) ==="
cd "$ESP32_DIR/scanner"
pio run -e scanner-s3-combo

echo ""
echo "=== Building Seed Scanner firmware (ESP32-S3) ==="
pio run -e scanner-s3-combo-seed

echo ""
echo "=== Building Uplink firmware (ESP32-S3) ==="
cd "$ESP32_DIR/uplink"
pio run -e uplink-s3

echo ""
echo "=== Copying binaries to web-flasher/firmware/ ==="

# Scanner binaries
mkdir -p "$FW_DIR/scanner"
cp "$ESP32_DIR/scanner/.pio/build/scanner-s3-combo/bootloader.bin"      "$FW_DIR/scanner/"
cp "$ESP32_DIR/scanner/.pio/build/scanner-s3-combo/partitions.bin"       "$FW_DIR/scanner/partition-table.bin"
cp "$ESP32_DIR/scanner/.pio/build/scanner-s3-combo/firmware.bin"         "$FW_DIR/scanner/"

# Seed scanner binaries
mkdir -p "$FW_DIR/scanner-seed"
cp "$ESP32_DIR/scanner/.pio/build/scanner-s3-combo-seed/bootloader.bin" "$FW_DIR/scanner-seed/"
cp "$ESP32_DIR/scanner/.pio/build/scanner-s3-combo-seed/partitions.bin"  "$FW_DIR/scanner-seed/partition-table.bin"
cp "$ESP32_DIR/scanner/.pio/build/scanner-s3-combo-seed/firmware.bin"    "$FW_DIR/scanner-seed/"

# Uplink binaries
mkdir -p "$FW_DIR/uplink-s3"
cp "$ESP32_DIR/uplink/.pio/build/uplink-s3/bootloader.bin"      "$FW_DIR/uplink-s3/"
cp "$ESP32_DIR/uplink/.pio/build/uplink-s3/partitions.bin"       "$FW_DIR/uplink-s3/partition-table.bin"
cp "$ESP32_DIR/uplink/.pio/build/uplink-s3/firmware.bin"         "$FW_DIR/uplink-s3/"

echo ""
echo "=== Done ==="
echo "Scanner: $(ls -lh "$FW_DIR/scanner/firmware.bin" | awk '{print $5}')"
echo "Seed:    $(ls -lh "$FW_DIR/scanner-seed/firmware.bin" | awk '{print $5}')"
echo "Uplink:  $(ls -lh "$FW_DIR/uplink-s3/firmware.bin" | awk '{print $5}')"
echo ""
echo "Serve locally with: cd $SCRIPT_DIR && python3 -m http.server 8080"
