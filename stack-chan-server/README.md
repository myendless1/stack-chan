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
export ALIYUN_NLS_TOKEN='your-token'
export ALIYUN_NLS_APPKEY='your-appkey'
./start.sh
```

The first start installs dependencies from `requirements.txt`. Building `dlib` for `face_recognition` can take a few minutes.

Captured images are saved under `captures/` relative to this directory.

## Endpoints

`GET /health`

Returns service status and endpoint metadata.

`POST /upload`

Audio upload endpoint for speech recognition. The body can be WAV or raw PCM. WAV sample rate is detected from the header; raw PCM defaults to `STACKCHAN_ALIYUN_SAMPLE_RATE` or `16000`.

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
| `ALIYUN_NLS_TOKEN` | required | Aliyun NLS access token |
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
