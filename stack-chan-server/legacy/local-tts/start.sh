#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

SERVER_DIR="$(cd ../.. && pwd)"

HOST="${STACKCHAN_TTS_HOST:-0.0.0.0}"
PORT="${STACKCHAN_TTS_PORT:-8091}"
MODEL="${STACKCHAN_TTS_MODEL:-$SERVER_DIR/models/zh_CN-huayan-x_low.onnx}"
CONFIG="${STACKCHAN_TTS_CONFIG:-$SERVER_DIR/models/zh_CN-huayan-x_low.onnx.json}"
VENV="${STACKCHAN_TTS_VENV:-.venv-tts}"
LENGTH_SCALE="${STACKCHAN_TTS_LENGTH_SCALE:-1.0}"
MAX_CHARS="${STACKCHAN_TTS_MAX_CHARS:-240}"

if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi

if ! "$VENV/bin/python" -c 'import piper' >/dev/null 2>&1; then
  "$VENV/bin/python" -m pip install -U pip
  "$VENV/bin/python" -m pip install -r requirements.txt
fi

if [ ! -f "$MODEL" ]; then
  echo "Missing Piper model: $MODEL" >&2
  echo "Download a .onnx voice model into models/, or set STACKCHAN_TTS_MODEL." >&2
  exit 1
fi

if [ ! -f "$CONFIG" ]; then
  echo "Missing Piper config: $CONFIG" >&2
  echo "Download the matching .onnx.json config, or set STACKCHAN_TTS_CONFIG." >&2
  exit 1
fi

echo
echo "Xiaopai local TTS service"
echo "  speak:  http://$HOST:$PORT/speak?text=..."
echo "  health: http://127.0.0.1:$PORT/health"
echo "  model:  $MODEL"
echo
echo "Set CONFIG_STACKCHAN_TTS_URL to http://<this-computer-lan-ip>:$PORT/speak"
echo

exec "$VENV/bin/python" local_tts_server.py \
  --host "$HOST" \
  --port "$PORT" \
  --piper-binary "$VENV/bin/piper" \
  --model "$MODEL" \
  --config "$CONFIG" \
  --length-scale "$LENGTH_SCALE" \
  --max-chars "$MAX_CHARS"
