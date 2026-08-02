#!/bin/sh
set -eu

NEW_DASH_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
NEW_DASH_VENV="$NEW_DASH_DIR/.venv"

if [ ! -x "$NEW_DASH_VENV/bin/python" ]; then
    python3 -m venv "$NEW_DASH_VENV"
fi

NEW_DASH_PYTHON="$NEW_DASH_VENV/bin/python"
"$NEW_DASH_PYTHON" -m pip install -e "$NEW_DASH_DIR"
exec "$NEW_DASH_PYTHON" -m new_dash "$@"
