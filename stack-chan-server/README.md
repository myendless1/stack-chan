# Stack-chan Server

Local bridge server for the Stack-chan firmware in this repository. It keeps cloud credentials on the computer and exposes simple HTTP endpoints for the ESP32 device.

## Features

- Aliyun NLS speech-to-text upload for recorded WAV/PCM audio.
- Aliyun NLS text-to-speech as streaming `pcm_s16le` audio.
- Camera upload receiver for Stack-chan RGB565 frames.
- RGB565 conversion to PNG and BMP. The Stack-chan camera data is decoded as big-endian RGB565.
- Face detection visualization using `ageitgey/face_recognition`: detected face boxes and landmarks are saved as `*.faces.png`.
- Legacy local open-source STT/TTS utilities are kept under `legacy/`.

## Setup

This project uses Python 3 and creates its own virtual environment at `.venv`.

```bash
cd stack-chan-server
cat > .env <<'EOF'
ALIYUN_AK_ID='your-access-key-id'
ALIYUN_AK_SECRET='your-access-key-secret'
ALIYUN_NLS_APPKEY='your-nls-appkey'
EOF
./start.sh
```

The server can also use `ALIYUN_NLS_TOKEN` directly. With `ALIYUN_AK_ID` and `ALIYUN_AK_SECRET`, it creates and refreshes the NLS token automatically. The default ASR/TTS/command server uses only Python standard library modules.

Face detection visualization is optional because `face_recognition_models` is large. Install it only when needed:

```bash
cd stack-chan-server
.venv/bin/python -m pip install -r requirements-face.txt
```

`start.sh` uses Tsinghua PyPI by default and clears proxy variables for pip installs. Override with `STACKCHAN_PIP_INDEX_URL` and `STACKCHAN_PIP_TRUSTED_HOST` if needed.

Captured images are saved under `captures/` relative to this directory.

## Endpoints

See [HTTP_COMMAND_API.md](HTTP_COMMAND_API.md) for the full command API reference and browser-friendly GET examples.

`GET /health`

Returns service status and endpoint metadata.

`GET /devices`

Lists devices that have checked in through the HTTP command channel:

```bash
curl 'http://127.0.0.1:8091/devices'
```

`GET /device/next-command?device_id=...&timeout=25`

Device long-poll endpoint. Stack-chan keeps one blocking HTTP request open and receives a JSON command when the server has one queued. A timeout returns `{"type":"noop"}`.

`GET /device/ack?device_id=...&cmd_id=...&status=received|done|failed`

Device ACK endpoint.

`GET /command/<type>?device_id=...`

Queues a command for a device. These GET shortcuts are intended for manual testing from a browser or curl:

```bash
curl -G 'http://127.0.0.1:8091/command/face' \
  --data-urlencode 'device_id=stackchan-001' \
  --data-urlencode 'expression=happy'

curl -G 'http://127.0.0.1:8091/expression/shy' \
  --data-urlencode 'device_id=stackchan-001'

curl -G 'http://127.0.0.1:8091/action/blink' \
  --data-urlencode 'device_id=stackchan-001'

curl -G 'http://127.0.0.1:8091/action/nod' \
  --data-urlencode 'device_id=stackchan-001'

curl -G 'http://127.0.0.1:8091/command/speak' \
  --data-urlencode 'device_id=stackchan-001' \
  --data-urlencode 'text=早上好，我已经收到你的问题啦。'

curl -G 'http://127.0.0.1:8091/command/motion' \
  --data-urlencode 'device_id=stackchan-001' \
  --data-urlencode 'pan=15' \
  --data-urlencode 'tilt=45' \
  --data-urlencode 'duration_ms=500'

curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'device_id=stackchan-001' \
  --data-urlencode 'expression=thinking' \
  --data-urlencode 'text=让我想一下这个问题。'
```

`POST /command`

Queues the full command schema as JSON:

```json
{
  "device_id": "stackchan-001",
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

`POST /upload-audio`

Alias for `/upload`. The firmware uses this route in background listening mode.

Response:

```json
{"type": "stt", "text": "...", "task_id": "..."}
```

`GET /stream-speak?text=...`

`POST /stream-speak`

Aliyun TTS endpoint. Text is split by sentence, synthesized with retries, and returned as raw PCM:

- format: `pcm_s16le`
- sample rate: default `16000`
- channels: `1`

`POST /upload-image`

Image upload endpoint. Stack-chan sends raw RGB565 bytes with:

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
- `*.faces.png`: face detection visualization, when dependencies are available

Response includes `png_path`, `face_visual_path`, and `face_detection`.

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

## Firmware URLs

Set the Stack-chan firmware URLs to your computer's LAN IP:

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
  'http://127.0.0.1:8091/stream-speak?text=你好，Stack-chan。' \
  -o /tmp/stackchan.pcm
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
