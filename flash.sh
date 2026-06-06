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

  python -m esptool \
    --chip esp32s3 \
    -p "$PORT" \
    -b 460800 \
    --before no_reset \
    --after hard_reset \
    run \
    || printf '\nWarning: post-flash run/reset fallback failed. If the board stays in bootloader, press RESET once.\n' >&2
)

printf '\nFlash complete. Board reset/run attempted automatically.\n'

if [ "$MODE" = "monitor" ]; then
  idf.py -p "$PORT" monitor
fi
