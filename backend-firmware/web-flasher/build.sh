#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
BACKEND_FW_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)"
PIO_BIN="${PIO:-pio}"
FIRMWARE_DIR="$SCRIPT_DIR/firmware"
INDEX_PATH="$BACKEND_FW_DIR/release/backend-release-index.json"

cd "$BACKEND_FW_DIR/uplink"
"$PIO_BIN" run -e uplink-s3-backend
cd "$BACKEND_FW_DIR"
python tools/verify_backend_build.py uplink \
  --uplink-build-dir uplink/.pio/build/uplink-s3-backend \
  --uplink-partition-csv uplink/partitions_backend_uplink_8mb.csv \
  --uplink-sdkconfig uplink/.pio/build/uplink-s3-backend/config/sdkconfig.h \
  --output-dir web-flasher/firmware \
  --index release/backend-release-index.json

if [[ ! -s "$INDEX_PATH" ]]; then
  echo "backend release index was not produced" >&2
  exit 1
fi

FOF_BUILD_SCRIPT_CHILD=1 FOF_REQUIRE_BACKEND_RELEASE_INDEX=1 \
  python -m pytest tools/tests/test_backend_web_flasher.py -q

python - "$FIRMWARE_DIR" "$INDEX_PATH" <<'PY'
import json
from pathlib import Path
import sys

firmware_dir = Path(sys.argv[1])
index = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
for target in sorted(index["targets"]):
    for part in index["targets"][target]["parts"]:
        artifact = firmware_dir / part["path"]
        print(
            f"{artifact} size={part['size']} sha256={part['sha256']}"
        )
PY
