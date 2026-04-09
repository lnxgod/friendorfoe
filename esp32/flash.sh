#!/bin/bash
# Friend or Foe — Auto-detect and flash connected ESP32 boards
# Usage: ./flash.sh [port]   (auto-detects if no port given)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ESPTOOL="$HOME/.platformio/penv/bin/esptool.py"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

flash_board() {
    local port="$1"
    local chip="$2"
    local env="$3"
    local dir="$4"
    local name="$5"

    echo -e "${BLUE}Flashing ${name}${NC} on ${port} (${chip})..."
    cd "$SCRIPT_DIR/$dir"
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
    chip_info=$("$ESPTOOL" --port "$port" chip-id 2>&1 | grep "Chip type:" || true)

    if echo "$chip_info" | grep -q "ESP32-S3"; then
        echo -e "${GREEN}Detected: ESP32-S3${NC}"
        # S3 = BLE+WiFi combo scanner
        flash_board "$port" "ESP32-S3" "scanner-s3-combo" "scanner" "S3 Combo Scanner"

    elif echo "$chip_info" | grep -q "ESP32-C3"; then
        echo -e "${GREEN}Detected: ESP32-C3${NC}"
        # C3 = uplink
        flash_board "$port" "ESP32-C3" "uplink" "uplink" "Uplink (C3)"

    elif echo "$chip_info" | grep -q "ESP32-D0\|ESP32 "; then
        echo -e "${GREEN}Detected: Plain ESP32${NC}"
        # Plain ESP32 could be uplink-esp32 or scanner-esp32
        # Check serial output to determine which firmware is running
        local ident
        ident=$(timeout 5 cat "$port" 2>/dev/null | head -50 | grep -oE '(uplink|scanner)' | head -1 || true)

        if [ -z "$ident" ]; then
            echo -e "${YELLOW}Can't auto-detect firmware type from serial. Choose:${NC}"
            echo "  1) Uplink (ESP32 OLED)"
            echo "  2) WiFi Scanner"
            read -rp "Choice [1/2]: " choice
            case "$choice" in
                2) ident="scanner" ;;
                *) ident="uplink" ;;
            esac
        fi

        if [ "$ident" = "uplink" ]; then
            flash_board "$port" "ESP32" "uplink-esp32" "uplink" "Uplink (ESP32)"
        else
            flash_board "$port" "ESP32" "scanner-esp32" "scanner" "WiFi Scanner (ESP32)"
        fi

    elif echo "$chip_info" | grep -q "ESP32-C5"; then
        echo -e "${GREEN}Detected: ESP32-C5${NC}"
        flash_board "$port" "ESP32-C5" "scanner-c5" "scanner" "WiFi Scanner (C5)"

    else
        echo -e "${RED}Unknown chip: ${chip_info}${NC}"
        echo "Raw output:"
        "$ESPTOOL" --port "$port" chip-id 2>&1 | head -10
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
