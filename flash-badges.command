#!/bin/zsh
set -euo pipefail

REPO_DIR="${0:A:h}"
PIO_PYTHON="$HOME/.platformio/penv/bin/python"

if [[ ! -x "$PIO_PYTHON" ]]; then
  print -u2 "PlatformIO Python not found at $PIO_PYTHON"
  print -u2 "Install PlatformIO Core first, then double-click this file again."
  read -r "?Press ENTER to close. "
  exit 1
fi

cd "$REPO_DIR"
exec "$PIO_PYTHON" scripts/fof_badge_factory.py --offline "$@"
