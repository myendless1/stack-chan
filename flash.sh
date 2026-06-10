#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
MODE="${2:-}"

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/env.sh"

(
  cd build
  python -m esptool \
    --chip esp32s3 \
    -p "$PORT" \
    -b 460800 \
    --before default_reset \
    --after hard_reset \
    write_flash "@flash_args"
)

printf '\nFlash complete. Board hard reset attempted automatically.\n'

if [ "$MODE" = "monitor" ]; then
  idf.py -p "$PORT" monitor
fi
