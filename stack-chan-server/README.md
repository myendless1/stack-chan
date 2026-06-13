# Xiaopai Server

Local bridge server for the Xiaopai firmware in this repository. It keeps cloud credentials on the computer and exposes simple HTTP endpoints for the ESP32 device.

## Features

- Aliyun NLS speech-to-text upload for recorded WAV/PCM audio.
- Aliyun NLS text-to-speech as streaming `pcm_s16le` audio.
- OpenClaw event bridge: speech recognition text and device events can be sent to OpenClaw; OpenClaw controls Xiaopai through the HTTP command API.
- Camera upload receiver for Xiaopai RGB565 frames.
- RGB565 conversion to PNG and BMP. The Xiaopai camera data is decoded as big-endian RGB565.
- Local CPU face/head detection using OpenCV YuNet. Detected boxes are returned in the `/upload-image` response and saved as `*.faces.jpg` visualizations.
- Legacy local open-source STT/TTS utilities are kept under `legacy/`.

## Setup

This project uses Python 3 and creates its own virtual environment at `.venv`.

```bash
cd stack-chan-server
cat > .env <<'EOF'
ALIYUN_AK_ID='your-access-key-id'
ALIYUN_AK_SECRET='your-access-key-secret'
ALIYUN_NLS_APPKEY='your-nls-appkey'
OPENCLAW_BASE_URL='http://127.0.0.1:18789/v1'
OPENCLAW_GATEWAY_TOKEN='your-openclaw-gateway-token'
EOF
./start.sh
```

The server can also use `ALIYUN_NLS_TOKEN` directly. With `ALIYUN_AK_ID` and `ALIYUN_AK_SECRET`, it creates and refreshes the NLS token automatically. The default ASR/TTS/command server uses only Python standard library modules.

YuNet face detection uses OpenCV and the small ONNX checkpoint in `models/`. `start.sh` installs the YuNet dependencies automatically. To install them manually:

```bash
cd stack-chan-server
.venv/bin/python -m pip install -r requirements-yunet.txt
```

`start.sh` uses Tsinghua PyPI by default and clears proxy variables for pip installs. Override with `STACKCHAN_PIP_INDEX_URL` and `STACKCHAN_PIP_TRUSTED_HOST` if needed.

Captured images are saved under `captures/` relative to this directory.

## Endpoints

See [HTTP_COMMAND_API.md](HTTP_COMMAND_API.md) for the full command API reference and browser-friendly GET examples.

`GET /health`

Returns service status and endpoint metadata.

`GET /devices`

Lists devices that have checked in through the HTTP command channel. `default_device_id` is the first currently online device used by command endpoints when `device_id` is omitted:

```bash
curl 'http://127.0.0.1:8091/devices'
```

`GET /device/next-command?device_id=...&timeout=25`

Device long-poll endpoint. Xiaopai keeps one blocking HTTP request open and receives a JSON command when the server has one queued. A timeout returns `{"type":"noop"}`.

`GET /device/ack?device_id=...&cmd_id=...&status=received|done|failed`

Device ACK endpoint.

`GET /device/event?device_id=...&type=head_touch&name=click`

Device event endpoint. Xiaopai uses this to report events to OpenClaw. The server forwards configured events but does not parse OpenClaw replies; OpenClaw should call the HTTP command endpoints to queue actions consumed by `/device/next-command`.

`GET /command/<type>`

Queues a command for the first currently online device. Pass `device_id` only when you need to target a specific device. These GET shortcuts are intended for manual testing from a browser or curl:

```bash
curl -G 'http://127.0.0.1:8091/command/face' \
  --data-urlencode 'expression=happy_squint'

curl -G 'http://127.0.0.1:8091/expression/shy'

curl -G 'http://127.0.0.1:8091/action/blink'

curl -G 'http://127.0.0.1:8091/action/heart_action'

curl -G 'http://127.0.0.1:8091/action/wink'

curl -G 'http://127.0.0.1:8091/action/nod'

curl -G 'http://127.0.0.1:8091/command/speak' \
  --data-urlencode 'text=早上好，我已经收到你的问题啦。'

curl -G 'http://127.0.0.1:8091/command/motion' \
  --data-urlencode 'pan=15' \
  --data-urlencode 'tilt=45' \
  --data-urlencode 'duration_ms=500'

curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'expression=thinking' \
  --data-urlencode 'text=让我想一下这个问题。'
```

`POST /command`

Queues the full command schema as JSON:

```json
{
  "type": "sequence",
  "payload": [
    {"type": "face", "expression": "thinking"},
    {"type": "motion", "pan": 15, "tilt": 45, "duration_ms": 400},
    {"type": "speak", "text": "让我想一下这个问题。"}
  ]
}
```

`POST /upload`

Audio upload endpoint for speech recognition. The body can be WAV or raw PCM. WAV sample rate is detected from the header; raw PCM defaults to `STACKCHAN_ALIYUN_SAMPLE_RATE` or `16000`.

When OpenClaw is configured, recognized text is sent to OpenClaw. The server does not read or parse OpenClaw response text, tags, or actions. OpenClaw should call the command API above when it wants Xiaopai to speak, move, or change expression.

`POST /upload-audio`

Alias for `/upload`. The firmware uses this route in background listening mode.

Response:

```json
{"type": "stt", "text": "...", "task_id": "...", "handled_as": "openclaw_forwarded", "openclaw_sent": true}
```

`GET /stream-speak?text=...`

`POST /stream-speak`

Aliyun TTS endpoint. Text is split by sentence, synthesized with retries, and returned as raw PCM:

- format: `pcm_s16le`
- sample rate: default `16000`
- channels: `1`

`GET /head-touch-events`

Lists the head touch event names and their cached audio URLs.

`GET /event-audio/<event>.pcm`

Returns cached static audio for head touch events. On the first request for an event, or whenever the configured event text changes, the server synthesizes the text with Aliyun TTS and saves it under `static/event-audio/`; later requests serve the cached file directly. Use `.pcm` for the firmware's raw `pcm_s16le` playback, and `.wav` for normal desktop/browser listening.

Supported events:

| Event | Spoken text |
| --- | --- |
| `press` | `按压` |
| `click` | `你好，我是小派同学` |
| `swipe_forward` | `你好，我是小派同学` |
| `swipe_backward` | `你好，我是小派同学` |

`POST /upload-image`

Image upload endpoint. Xiaopai sends raw RGB565 bytes with:

```text
Content-Type: image/rgb565
X-Image-Format: rgb565
X-Image-Width: 320
X-Image-Height: 240
```

The server saves:

- `*.rgb565`: raw upload
- `*.png`: converted image
- `*.bmp`: converted image
- `*.faces.jpg`: YuNet face/head detection visualization, when dependencies are available

Response includes `png_path`, `face_visual_path`, and `face_detection`. `face_detection.best_face.center` is the main field for visual tracking.

## Configuration

Environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `ALIYUN_AK_ID` | required unless token is set | Aliyun AccessKey ID, used to create and refresh NLS token |
| `ALIYUN_AK_SECRET` | required unless token is set | Aliyun AccessKey Secret, used to create and refresh NLS token |
| `ALIYUN_NLS_TOKEN` | optional | Aliyun NLS access token, required only when AccessKey is not configured |
| `ALIYUN_NLS_TOKEN_EXPIRE_TIME` | `0` | Optional token expire timestamp in seconds |
| `ALIYUN_NLS_APPKEY` | required | Aliyun NLS app key |
| `STACKCHAN_SERVER_HOST` | `0.0.0.0` | Bind host |
| `STACKCHAN_SERVER_PORT` | `8091` | Bind port |
| `STACKCHAN_SERVER_VENV` | `.venv` | Virtual environment path |
| `STACKCHAN_ALIYUN_REGION` | `shanghai` | Aliyun NLS region: `shanghai`, `beijing`, or `shenzhen` |
| `STACKCHAN_ALIYUN_TTS_URL` | empty | Override TTS URL |
| `STACKCHAN_ALIYUN_VOICE` | `xiaoyun` | Aliyun TTS voice |
| `STACKCHAN_ALIYUN_SAMPLE_RATE` | `16000` | ASR raw PCM and TTS sample rate |
| `STACKCHAN_ALIYUN_VOLUME` | `80` | TTS volume |
| `STACKCHAN_ALIYUN_SPEECH_RATE` | `0` | TTS speech rate |
| `STACKCHAN_ALIYUN_PITCH_RATE` | `0` | TTS pitch rate |
| `STACKCHAN_ALIYUN_MAX_SENTENCE_CHARS` | `120` | Sentence chunk size for TTS |
| `STACKCHAN_ALIYUN_TTS_PREFETCH_WORKERS` | `2` | Parallel TTS prefetch workers |
| `STACKCHAN_ALIYUN_TTS_REQUEST_TIMEOUT` | `12` | Per-request TTS timeout in seconds |
| `STACKCHAN_ALIYUN_TTS_RETRIES` | `2` | TTS retry count |
| `STACKCHAN_CAPTURE_DIR` | `captures` | Directory for image uploads |
| `STACKCHAN_STATIC_DIR` | `static` | Directory for cached static assets such as head-touch event audio |
| `STACKCHAN_FACE_DETECTOR` | `yunet` | Face detector backend for `/upload-image`: `yunet`, `legacy`, or `none` |
| `STACKCHAN_YUNET_MODEL` | `models/face_detection_yunet_2023mar.onnx` | YuNet ONNX checkpoint path |
| `STACKCHAN_YUNET_SCORE_THRESHOLD` | `0.45` | YuNet confidence threshold |
| `STACKCHAN_YUNET_NMS_THRESHOLD` | `0.3` | YuNet non-maximum suppression threshold |
| `STACKCHAN_YUNET_TOP_K` | `5000` | YuNet pre-NMS top-k candidate limit |
| `OPENCLAW_BASE_URL` / `STACKCHAN_OPENCLAW_BASE_URL` | empty | OpenClaw OpenAI-compatible base URL, for example `http://127.0.0.1:18789/v1` |
| `OPENCLAW_GATEWAY_TOKEN` / `STACKCHAN_OPENCLAW_GATEWAY_TOKEN` | empty | OpenClaw Gateway bearer token |
| `STACKCHAN_OPENCLAW_MODEL` | `openclaw/default` | OpenClaw agent target |
| `STACKCHAN_OPENCLAW_BACKEND_MODEL` | empty | Optional `x-openclaw-model` override |
| `STACKCHAN_OPENCLAW_TIMEOUT` | `45` | OpenClaw request timeout in seconds |
| `STACKCHAN_OPENCLAW_MAX_COMPLETION_TOKENS` | `512` | Max OpenClaw output tokens |
| `STACKCHAN_OPENCLAW_SESSION_PREFIX` | `xiaopai` | Prefix for per-device OpenClaw session keys |

## Firmware URLs

Set the Xiaopai firmware URLs to your computer's LAN IP:

```text
CONFIG_STACKCHAN_RECORD_UPLOAD_URL = http://<lan-ip>:8091/upload
CONFIG_STACKCHAN_STREAM_TTS_URL    = http://<lan-ip>:8091/stream-speak
CONFIG_STACKCHAN_IMAGE_UPLOAD_URL  = http://<lan-ip>:8091/upload-image
```

## Quick Tests

Health:

```bash
curl --noproxy 127.0.0.1,localhost http://127.0.0.1:8091/health
```

TTS:

```bash
curl --noproxy 127.0.0.1,localhost \
  'http://127.0.0.1:8091/stream-speak?text=你好，Xiaopai。' \
  -o /tmp/xiaopai.pcm
```

RGB565 upload:

```bash
curl --noproxy 127.0.0.1,localhost -X POST \
  http://127.0.0.1:8091/upload-image \
  -H 'Content-Type: image/rgb565' \
  -H 'X-Image-Format: rgb565' \
  -H 'X-Image-Width: 320' \
  -H 'X-Image-Height: 240' \
  --data-binary @example.rgb565
```

## Legacy Local STT/TTS

The older local-only services are still available for experiments:

```bash
./legacy/local-stt/start.sh
./legacy/local-tts/start.sh
```

They use separate virtual environments inside their own legacy folders. The local TTS script expects Piper voice files in `stack-chan-server/models/` by default.
