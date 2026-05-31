#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

HOST="${STACKCHAN_SERVER_HOST:-${STACKCHAN_ALIYUN_HOST:-0.0.0.0}}"
PORT="${STACKCHAN_SERVER_PORT:-${STACKCHAN_ALIYUN_PORT:-8091}}"
VENV="${STACKCHAN_SERVER_VENV:-.venv}"
LOG_FILE="${STACKCHAN_SERVER_LOG:-/tmp/stack-chan-server.log}"

if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi

if ! "$VENV/bin/python" - <<'PY' >/dev/null 2>&1
import face_recognition
from PIL import Image
PY
then
  "$VENV/bin/python" -m pip install -r requirements.txt
fi

if [ -z "${ALIYUN_NLS_TOKEN:-}" ] || [ -z "${ALIYUN_NLS_APPKEY:-}" ]; then
  echo "Missing Aliyun credentials." >&2
  echo "Run:" >&2
  echo "  export ALIYUN_NLS_TOKEN='...'" >&2
  echo "  export ALIYUN_NLS_APPKEY='...'" >&2
  exit 1
fi

echo
echo "Stack-chan Server"
echo "  ASR upload:   http://$HOST:$PORT/upload"
echo "  TTS stream:   http://$HOST:$PORT/stream-speak?text=..."
echo "  Image upload: http://$HOST:$PORT/upload-image"
echo "  health:       http://127.0.0.1:$PORT/health"
echo "  log:          $LOG_FILE"
echo
echo "Firmware config examples:"
echo "  CONFIG_STACKCHAN_RECORD_UPLOAD_URL = http://<this-computer-lan-ip>:$PORT/upload"
echo "  CONFIG_STACKCHAN_STREAM_TTS_URL    = http://<this-computer-lan-ip>:$PORT/stream-speak"
echo "  CONFIG_STACKCHAN_IMAGE_UPLOAD_URL  = http://<this-computer-lan-ip>:$PORT/upload-image"
echo

mkdir -p "$(dirname "$LOG_FILE")"
echo "---- stack-chan-server start $(date '+%Y-%m-%d %H:%M:%S') ----" >> "$LOG_FILE"

PYTHONUNBUFFERED=1 "$VENV/bin/python" src/server.py \
  --host "$HOST" \
  --port "$PORT" \
  2>&1 | tee -a "$LOG_FILE"
