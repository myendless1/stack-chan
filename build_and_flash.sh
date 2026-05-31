#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"

. /home/myendless/code/stack-chan/esp-idf/export.sh

idf.py set-target esp32s3
idf.py build

(
  cd build
  python -m esptool \
    --chip esp32s3 \
    -p "$PORT" \
    -b 460800 \
    --before default_reset \
    --after no_reset \
    write_flash "@flash_args"
)

printf '\nFlash complete. Press the board RESET button, then press Enter to start monitor.\n'
read -r

idf.py -p "$PORT" monitor
