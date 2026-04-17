#!/bin/bash
# Friend or Foe — Update Legacy ESP32 Nodes (OTA)
#
# Updates ESP32 OLED uplinks (NOT S3) and their attached S3 scanners.
# Flow:
#   1. OTA flash uplink firmware (self-update via /api/ota)
#   2. Wait for reboot + verify new firmware
#   3. Upload scanner firmware to uplink flash (POST /api/fw/upload)
#   4. Trigger reliable UART relay (POST /api/fw/relay)
#
# Usage: ./update_legacy_nodes.sh [NODE_IP_OR_HOSTNAME ...]
#   e.g.: ./update_legacy_nodes.sh 192.168.1.50 192.168.1.51
#         ./update_legacy_nodes.sh frontyard.local backyard.local

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UPLINK_FW="$SCRIPT_DIR/uplink/.pio/build/uplink-esp32/firmware.bin"
SCANNER_FW="$SCRIPT_DIR/scanner/.pio/build/scanner-s3-legacy/firmware.bin"
PORT=80

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

die()  { echo -e "${RED}FATAL: $*${NC}" >&2; exit 1; }
warn() { echo -e "${YELLOW}WARN: $*${NC}" >&2; }
info() { echo -e "${BLUE}$*${NC}"; }
ok()   { echo -e "${GREEN}$*${NC}"; }

# Verify firmware files exist
[ -f "$UPLINK_FW" ]  || die "Uplink firmware not found: $UPLINK_FW\n  Run: cd esp32/uplink && pio run -e uplink-esp32"
[ -f "$SCANNER_FW" ] || die "Scanner firmware not found: $SCANNER_FW\n  Run: cd esp32/scanner && pio run -e scanner-s3-legacy"

UPLINK_SIZE=$(stat -f%z "$UPLINK_FW" 2>/dev/null || stat -c%s "$UPLINK_FW" 2>/dev/null)
SCANNER_SIZE=$(stat -f%z "$SCANNER_FW" 2>/dev/null || stat -c%s "$SCANNER_FW" 2>/dev/null)
info "Uplink firmware:  $UPLINK_FW ($UPLINK_SIZE bytes)"
info "Scanner firmware: $SCANNER_FW ($SCANNER_SIZE bytes)"
echo

# ── Retry helper with exponential backoff ──────────────────────────────────

http_with_retry() {
    local url="$1"
    local method="${2:-GET}"
    local data_file="${3:-}"
    local max_retries=3
    local retry=0
    local backoff=5

    while [ $retry -lt $max_retries ]; do
        local curl_args=(-s -S --connect-timeout 10 --max-time 300 -X "$method")
        if [ -n "$data_file" ]; then
            curl_args+=(--data-binary "@$data_file" -H "Content-Type: application/octet-stream")
        fi

        local response
        local http_code
        response=$(curl "${curl_args[@]}" -w "\n%{http_code}" "$url" 2>&1) || true
        http_code=$(echo "$response" | tail -1)
        local body=$(echo "$response" | sed '$d')

        if [ "$http_code" = "200" ]; then
            echo "$body"
            return 0
        fi

        retry=$((retry + 1))
        if [ $retry -lt $max_retries ]; then
            warn "HTTP $http_code from $url — retry $retry/$max_retries in ${backoff}s"
            sleep $backoff
            backoff=$((backoff * 2))
        else
            warn "HTTP $http_code from $url — all $max_retries retries exhausted"
            echo "$body"
            return 1
        fi
    done
}

# ── Wait for node to come online after reboot ─────────────────────────────

wait_for_node() {
    local host="$1"
    local max_wait=60
    local waited=0
    local interval=3

    info "  Waiting for $host to come back online..."
    while [ $waited -lt $max_wait ]; do
        if curl -s --connect-timeout 3 --max-time 5 "http://$host:$PORT/api/status" >/dev/null 2>&1; then
            ok "  $host is back online after ${waited}s"
            return 0
        fi
        sleep $interval
        waited=$((waited + interval))
    done

    warn "$host did not come back after ${max_wait}s"
    return 1
}

# ── Verify node is an ESP32 (not S3) ──────────────────────────────────────

check_node_type() {
    local host="$1"
    local status
    status=$(curl -s --connect-timeout 5 --max-time 10 "http://$host:$PORT/api/status" 2>/dev/null) || {
        warn "Cannot reach $host:$PORT — skipping"
        return 1
    }

    # Check if the node identifies as ESP32 (not S3)
    if echo "$status" | grep -qi "esp32s3\|esp32-s3"; then
        warn "$host is an S3 node — SKIPPING (as requested)"
        return 1
    fi

    ok "  $host is a legacy ESP32 node"
    return 0
}

# ── Update a single node ──────────────────────────────────────────────────

update_node() {
    local host="$1"
    echo
    info "================================================"
    info "  Updating node: $host"
    info "================================================"

    # Step 0: Verify it's a legacy ESP32
    if ! check_node_type "$host"; then
        return 0  # Skip, don't fail
    fi

    # Step 1: OTA flash uplink firmware
    info "  Step 1/4: Flashing uplink firmware ($UPLINK_SIZE bytes)..."
    local ota_resp
    ota_resp=$(http_with_retry "http://$host:$PORT/api/ota" POST "$UPLINK_FW") || {
        warn "Uplink OTA failed for $host: $ota_resp"
        return 1
    }
    ok "  Uplink OTA response: $ota_resp"

    # Step 2: Wait for reboot
    info "  Step 2/4: Waiting for reboot..."
    sleep 5  # Give it time to start rebooting
    if ! wait_for_node "$host"; then
        warn "Node $host did not come back after uplink OTA"
        return 1
    fi

    # Verify the new firmware version
    sleep 2  # Let it fully initialize
    local ota_info
    ota_info=$(curl -s --connect-timeout 5 --max-time 10 "http://$host:$PORT/api/ota/info" 2>/dev/null) || true
    ok "  Running firmware: $ota_info"

    # Step 3: Upload scanner firmware to flash storage
    info "  Step 3/4: Uploading scanner firmware to flash ($SCANNER_SIZE bytes)..."
    local upload_resp
    upload_resp=$(http_with_retry "http://$host:$PORT/api/fw/upload?version=v0.56.0" POST "$SCANNER_FW") || {
        warn "Scanner upload failed for $host: $upload_resp"
        return 1
    }
    ok "  Upload response: $upload_resp"

    # Check it was stored correctly
    local fw_info
    fw_info=$(curl -s --connect-timeout 5 --max-time 10 "http://$host:$PORT/api/fw/info" 2>/dev/null) || true
    ok "  Stored firmware: $fw_info"

    # Step 4: Relay to scanner via UART (this takes a while — ~2000 chunks at 512B each)
    info "  Step 4/4: Relaying to scanner via UART (CRC32 + ACK, be patient)..."
    local relay_resp
    relay_resp=$(http_with_retry "http://$host:$PORT/api/fw/relay?uart=ble" POST) || {
        warn "Scanner relay failed for $host: $relay_resp"
        return 1
    }

    # Check if relay reported success
    if echo "$relay_resp" | grep -q '"ok":true'; then
        ok "  Relay SUCCESS: $relay_resp"
    else
        warn "Relay reported failure: $relay_resp"
        return 1
    fi

    ok "  Node $host fully updated!"
    return 0
}

# ── Main ──────────────────────────────────────────────────────────────────

if [ $# -eq 0 ]; then
    echo "Usage: $0 <NODE_IP_OR_HOSTNAME> [NODE2 ...]"
    echo ""
    echo "Examples:"
    echo "  $0 192.168.1.50"
    echo "  $0 frontyard.local backyard.local"
    echo ""
    echo "This script updates legacy ESP32 uplinks and their attached S3 scanners."
    echo "S3 uplink nodes are automatically skipped."
    exit 1
fi

TOTAL=$#
SUCCESS=0
FAILED=0
SKIPPED=0

for node in "$@"; do
    if update_node "$node"; then
        SUCCESS=$((SUCCESS + 1))
    else
        FAILED=$((FAILED + 1))
    fi
done

echo
info "================================================"
info "  Update Summary"
info "================================================"
echo -e "  Total:   $TOTAL"
echo -e "  ${GREEN}Success: $SUCCESS${NC}"
[ $FAILED -gt 0 ] && echo -e "  ${RED}Failed:  $FAILED${NC}" || echo -e "  Failed:  $FAILED"
echo
