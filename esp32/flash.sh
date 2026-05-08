#!/bin/bash
# Friend or Foe — Auto-detect and flash connected ESP32 boards
# Usage: ./flash.sh [port]   (auto-detects if no port given)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ESPTOOL="$HOME/.platformio/penv/bin/esptool.py"
PYTHON="$HOME/.platformio/penv/bin/python"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

request_soft_bootloader() {
    local port="$1"

    [ -e "$port" ] || return 1
    echo -e "${YELLOW}Requesting software bootloader on ${port}...${NC}"

    set +e
    "$PYTHON" - "$port" <<'PY'
import sys
import time

try:
    import serial
except Exception as exc:
    print(f"pyserial unavailable: {exc}")
    sys.exit(2)

port = sys.argv[1]
try:
    ser = serial.Serial(port, 115200, timeout=0.12, write_timeout=0.5)
except Exception as exc:
    print(f"open failed: {exc}")
    sys.exit(3)

def read_for(seconds):
    deadline = time.time() + seconds
    data = bytearray()
    while time.time() < deadline:
        try:
            chunk = ser.read(256)
        except Exception:
            break
        if chunk:
            data.extend(chunk)
    return bytes(data)

try:
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    ser.write(b"\nFOF_PING\n")
    ser.flush()
    read_for(0.3)
    ser.write(b"FOF_BOOTLOADER\n")
    ser.flush()
    data = read_for(1.2)
finally:
    try:
        ser.close()
    except Exception:
        pass

if b"FOF_BOOTLOADER:OK" in data:
    print("ack")
    sys.exit(0)

if data:
    print(data.decode("utf-8", "replace")[-200:])
else:
    print("no bootloader ack")
sys.exit(1)
PY
    local rc=$?
    set -e
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}✓ Firmware accepted bootloader command${NC}"
        sleep 2
        return 0
    fi
    echo -e "${YELLOW}No firmware bootloader ack; trying normal esptool reset path${NC}"
    return 1
}

flash_board() {
    local port="$1"
    local chip="$2"
    local env="$3"
    local dir="$4"
    local name="$5"

    echo -e "${BLUE}Flashing ${name}${NC} on ${port} (${chip})..."
    cd "$SCRIPT_DIR/$dir"
    request_soft_bootloader "$port" || true
    "$PIO" run -e "$env" -t upload --upload-port "$port" 2>&1 | tail -5
    echo -e "${GREEN}✓ ${name} flashed successfully${NC}"
    echo
}

detect_and_flash() {
    local port="$1"
    echo -e "${YELLOW}Detecting chip on ${port}...${NC}"

    # Determine baud rate for detection
    local baud=115200
    if [[ "$port" == *usbmodem* ]]; then
        baud=921600
    fi

    # Get chip type
    local chip_info
    chip_info=$("$ESPTOOL" --port "$port" --connect-attempts 2 chip-id 2>&1 | grep "Chip type:" || true)
    if [ -z "$chip_info" ]; then
        request_soft_bootloader "$port" || true
        chip_info=$("$ESPTOOL" --port "$port" --connect-attempts 2 chip-id 2>&1 | grep "Chip type:" || true)
    fi

    if echo "$chip_info" | grep -q "ESP32-S3"; then
        echo -e "${GREEN}Detected: ESP32-S3${NC}"
        echo -e "${YELLOW}Choose firmware:${NC}"
        echo "  1) Scanner S3 combo               (production)"
        echo "  2) Scanner S3 seed                (production)"
        echo "  3) Uplink S3                      (production)"
        echo "  4) Scanner S3 combo — FoF Badge   (XIAO ESP32-S3)"
        echo "  5) Uplink S3 — FoF Badge          (XIAO ESP32-S3 + ST7735 display)"
        read -rp "Choice [1-5, default 1]: " choice
        case "$choice" in
            2) flash_board "$port" "ESP32-S3" "scanner-s3-combo-seed"      "scanner" "S3 Seed Scanner" ;;
            3) flash_board "$port" "ESP32-S3" "uplink-s3"                  "uplink"  "S3 Uplink" ;;
            4) flash_board "$port" "ESP32-S3" "scanner-s3-combo-fof_badge" "scanner" "FoF Badge Scanner (XIAO)" ;;
            5) flash_board "$port" "ESP32-S3" "uplink-s3-fof_badge"        "uplink"  "FoF Badge Uplink (XIAO)" ;;
            *) flash_board "$port" "ESP32-S3" "scanner-s3-combo"           "scanner" "S3 Combo Scanner" ;;
        esac

    else
        echo -e "${RED}Unsupported chip for current releases: ${chip_info}${NC}"
        echo "Current firmware supports ESP32-S3 only (production scanner/seed/uplink, FoF Badge scanner/uplink)."
        echo "Raw output:"
        "$ESPTOOL" --port "$port" --connect-attempts 2 chip-id 2>&1 | head -10
        return 1
    fi
}

# Main
if [ -n "$1" ]; then
    # Flash specific port
    detect_and_flash "$1"
else
    # Find all connected ESP32 boards
    ports=()
    for p in /dev/cu.wchusbserial* /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.SLAB*; do
        [ -e "$p" ] && ports+=("$p")
    done

    if [ ${#ports[@]} -eq 0 ]; then
        echo -e "${RED}No ESP32 boards found on USB${NC}"
        exit 1
    fi

    echo -e "${BLUE}Found ${#ports[@]} board(s):${NC}"
    for p in "${ports[@]}"; do
        echo "  $p"
    done
    echo

    for p in "${ports[@]}"; do
        detect_and_flash "$p"
    done

    echo -e "${GREEN}All boards flashed!${NC}"
fi
