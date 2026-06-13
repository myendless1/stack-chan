#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

HOST="${STACKCHAN_SERVER_HOST:-${STACKCHAN_ALIYUN_HOST:-0.0.0.0}}"
PORT="${STACKCHAN_SERVER_PORT:-${STACKCHAN_ALIYUN_PORT:-8091}}"
VENV="${STACKCHAN_SERVER_VENV:-.venv}"
LOG_FILE="${STACKCHAN_SERVER_LOG:-/tmp/stack-chan-server.log}"
PIP_INDEX_URL="${STACKCHAN_PIP_INDEX_URL:-https://pypi.tuna.tsinghua.edu.cn/simple}"
PIP_TRUSTED_HOST="${STACKCHAN_PIP_TRUSTED_HOST:-pypi.tuna.tsinghua.edu.cn}"
DEBUG="${STACKCHAN_DEBUG:-0}"
SERVER_ARGS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --debug)
      DEBUG=1
      shift
      ;;
    *)
      SERVER_ARGS+=("$1")
      shift
      ;;
  esac
done

if [ -f .env ]; then
  set -a
  . ./.env
  set +a
fi

ALIYUN_AK_ID="${ALIYUN_AK_ID:-}"
ALIYUN_AK_SECRET="${ALIYUN_AK_SECRET:-}"
ALIYUN_NLS_APPKEY="${ALIYUN_NLS_APPKEY:-}"

if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi

has_requirements() {
  grep -Ev '^[[:space:]]*(#|$)' "$1" >/dev/null 2>&1
}

pip_install() {
  env -u http_proxy -u https_proxy -u all_proxy -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
    "$VENV/bin/python" -m pip install \
      -i "$PIP_INDEX_URL" \
      --trusted-host "$PIP_TRUSTED_HOST" \
      -r "$1"
}

if has_requirements requirements.txt; then
  pip_install requirements.txt
fi

if has_requirements requirements-yunet.txt; then
  pip_install requirements-yunet.txt
fi

if ! "$VENV/bin/python" - <<'PY' >/dev/null 2>&1
import cv2
import numpy
PY
then
  echo "YuNet dependencies are not installed; image upload works, face boxes may be disabled." >&2
  echo "Install them later with: env -u http_proxy -u https_proxy -u all_proxy $VENV/bin/python -m pip install -i $PIP_INDEX_URL --trusted-host $PIP_TRUSTED_HOST -r requirements-yunet.txt" >&2
fi

if { [ -z "${ALIYUN_NLS_TOKEN:-}" ] && { [ -z "${ALIYUN_AK_ID:-}" ] || [ -z "${ALIYUN_AK_SECRET:-}" ]; }; } || [ -z "${ALIYUN_NLS_APPKEY:-}" ]; then
  echo "Missing Aliyun credentials." >&2
  echo "Set these in stack-chan-server/.env or export them:" >&2
  echo "  ALIYUN_AK_ID='...'" >&2
  echo "  ALIYUN_AK_SECRET='...'" >&2
  echo "  ALIYUN_NLS_APPKEY='...'" >&2
  echo "Alternatively set ALIYUN_NLS_TOKEN instead of ALIYUN_AK_ID/ALIYUN_AK_SECRET." >&2
  exit 1
fi

echo
echo "Xiaopai Server"
echo "  log:          $LOG_FILE"
if [ "$DEBUG" = "1" ] || [ "$DEBUG" = "true" ]; then
  DEBUG_ARGS=(--debug)
  echo "  debug:        enabled"
  echo "  ASR upload:   http://$HOST:$PORT/upload"
  echo "  TTS stream:   http://$HOST:$PORT/stream-speak?text=..."
  echo "  Image upload: http://$HOST:$PORT/upload-image"
  echo "  health:       http://127.0.0.1:$PORT/health"
  echo
  echo "Firmware config examples:"
  echo "  CONFIG_STACKCHAN_RECORD_UPLOAD_URL = http://<this-computer-lan-ip>:$PORT/upload"
  echo "  CONFIG_STACKCHAN_STREAM_TTS_URL    = http://<this-computer-lan-ip>:$PORT/stream-speak"
  echo "  CONFIG_STACKCHAN_IMAGE_UPLOAD_URL  = http://<this-computer-lan-ip>:$PORT/upload-image"
else
  DEBUG_ARGS=()
  echo "  debug:        disabled (run ./start.sh --debug for device IDs, task IDs, IPs, ports, and full API bodies)"
fi
echo

mkdir -p "$(dirname "$LOG_FILE")"
echo "---- stack-chan-server start $(date '+%Y-%m-%d %H:%M:%S') ----" >> "$LOG_FILE"

PYTHONUNBUFFERED=1 "$VENV/bin/python" src/server.py \
  --host "$HOST" \
  --port "$PORT" \
  "${DEBUG_ARGS[@]}" \
  "${SERVER_ARGS[@]}" \
  2>&1 | tee -a "$LOG_FILE" &

echo "Use \`pkill -f -9 server.py\` to stop the server."
