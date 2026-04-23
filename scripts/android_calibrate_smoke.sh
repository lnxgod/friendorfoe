#!/usr/bin/env bash
set -euo pipefail

ADB_BIN="${ADB_BIN:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}/platform-tools/adb}"
PACKAGE="${PACKAGE:-com.friendorfoe}"
ACTIVITY="${ACTIVITY:-com.friendorfoe.presentation.MainActivity}"
BACKEND_HOST="${BACKEND_HOST:-fof-server.local}"
BACKEND_PORT="${BACKEND_PORT:-8000}"
GEO_LON="${GEO_LON:--122.431297}"
GEO_LAT="${GEO_LAT:-37.773972}"
TMP_XML="$(mktemp)"
TMP_LOG="$(mktemp)"

cleanup() {
  rm -f "$TMP_XML" "$TMP_LOG"
}
trap cleanup EXIT

require() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required command: $1" >&2
    exit 1
  }
}

require python3

adb_cmd() {
  "$ADB_BIN" "$@"
}

dump_ui() {
  adb_cmd shell uiautomator dump /sdcard/calibrate-smoke.xml >/dev/null
  adb_cmd shell cat /sdcard/calibrate-smoke.xml > "$TMP_XML"
}

ui_has() {
  local needle="$1"
  dump_ui
  python3 - "$needle" "$TMP_XML" <<'PY'
import sys
needle = sys.argv[1]
path = sys.argv[2]
text = open(path, "r", encoding="utf-8").read()
sys.exit(0 if needle in text else 1)
PY
}

tap_node() {
  local needle="$1"
  dump_ui
  local coords
  coords="$(python3 - "$needle" "$TMP_XML" <<'PY'
import re
import sys
import xml.etree.ElementTree as ET

needle = sys.argv[1]
path = sys.argv[2]

tree = ET.parse(path)
root = tree.getroot()

def match(node):
    for key in ("text", "content-desc"):
        value = node.attrib.get(key, "")
        if value == needle:
            return True
    return False

for node in root.iter("node"):
    if not match(node):
        continue
    bounds = node.attrib.get("bounds", "")
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds)
    if not m:
        continue
    x1, y1, x2, y2 = map(int, m.groups())
    print((x1 + x2) // 2, (y1 + y2) // 2)
    sys.exit(0)

sys.exit(1)
PY
)"
  local x="${coords%% *}"
  local y="${coords##* }"
  adb_cmd shell input tap "$x" "$y"
}

wait_for_ui_text() {
  local needle="$1"
  local timeout_s="${2:-20}"
  local start
  start="$(date +%s)"
  while true; do
    if ui_has "$needle"; then
      return 0
    fi
    if (( "$(date +%s)" - start >= timeout_s )); then
      echo "timed out waiting for UI text: $needle" >&2
      return 1
    fi
    sleep 1
  done
}

wait_for_log() {
  local pattern="$1"
  local timeout_s="${2:-20}"
  local start
  start="$(date +%s)"
  while true; do
    adb_cmd logcat -d > "$TMP_LOG"
    if grep -Eq "$pattern" "$TMP_LOG"; then
      return 0
    fi
    if (( "$(date +%s)" - start >= timeout_s )); then
      echo "timed out waiting for log pattern: $pattern" >&2
      return 1
    fi
    sleep 1
  done
}

reveal_ui_text() {
  local needle="$1"
  local attempts="${2:-4}"
  local direction="${3:-up}"
  local swipe_from_y="2100"
  local swipe_to_y="900"
  if [[ "$direction" == "down" ]]; then
    swipe_from_y="900"
    swipe_to_y="2100"
  fi
  local i
  for ((i = 0; i < attempts; i++)); do
    if ui_has "$needle"; then
      return 0
    fi
    adb_cmd shell input swipe 540 "$swipe_from_y" 540 "$swipe_to_y" 300
    sleep 1
  done
  ui_has "$needle"
}

echo "==> launching $PACKAGE"
adb_cmd logcat -c
adb_cmd shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
adb_cmd shell am start -n "$PACKAGE/$ACTIVITY" >/dev/null
sleep 2

if ui_has "Get Started"; then
  echo "==> dismissing welcome screen"
  tap_node "Get Started"
  sleep 2
fi

echo "==> opening Calibrate"
tap_node "Calibrate"
wait_for_log "phase=health method=GET .* success"
wait_for_log "phase=walk_sensors method=GET .* success code=200"
wait_for_ui_text "Backend reachable"

echo "==> preflight healthy"

reveal_ui_text "Start walk"
wait_for_ui_text "Start walk"

echo "==> simulating GPS"
adb_cmd emu geo fix "$GEO_LON" "$GEO_LAT" >/dev/null

echo "==> starting walk"
tap_node "Start walk"
wait_for_log "phase=walk_start method=POST .* success code=200"
wait_for_log "phase=walk_sample method=POST .* success code=200" 30
reveal_ui_text "Stop walk + apply fit" 4 down
wait_for_ui_text "Stop walk + apply fit" 20

echo "Calibration smoke passed."
echo "Backend: http://$BACKEND_HOST:$BACKEND_PORT/"
echo "GPS: lat=$GEO_LAT lon=$GEO_LON"
