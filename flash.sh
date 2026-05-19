#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"

idf.py set-target esp32s3
idf.py build
idf.py -p "$PORT" flash monitor
