#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

HOST="${STACKCHAN_STT_HOST:-0.0.0.0}"
PORT="${STACKCHAN_STT_PORT:-8091}"
MODEL="${STACKCHAN_STT_MODEL:-small}"
LANGUAGE="${STACKCHAN_STT_LANGUAGE:-zh}"
DEVICE="${STACKCHAN_STT_DEVICE:-cpu}"
COMPUTE_TYPE="${STACKCHAN_STT_COMPUTE_TYPE:-int8}"
OUTPUT="${STACKCHAN_STT_OUTPUT:-recordings}"
VENV="${STACKCHAN_STT_VENV:-.venv-stt}"

if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi

if ! "$VENV/bin/python" -c 'import faster_whisper' >/dev/null 2>&1; then
  "$VENV/bin/python" -m pip install -U pip
  "$VENV/bin/python" -m pip install -r requirements.txt
fi

"$VENV/bin/python" download_stt_model.py "$MODEL" \
  --device "$DEVICE" \
  --compute-type "$COMPUTE_TYPE"

echo
echo "Xiaopai local STT service"
echo "  upload: http://$HOST:$PORT/upload"
echo "  health: http://127.0.0.1:$PORT/health"
echo "  model:  $MODEL"
echo "  lang:   $LANGUAGE"
echo
echo "Set CONFIG_STACKCHAN_RECORD_UPLOAD_URL to http://<this-computer-lan-ip>:$PORT/upload"
echo

exec "$VENV/bin/python" local_stt_server.py \
  --host "$HOST" \
  --port "$PORT" \
  --output "$OUTPUT" \
  --model "$MODEL" \
  --language "$LANGUAGE" \
  --device "$DEVICE" \
  --compute-type "$COMPUTE_TYPE"
