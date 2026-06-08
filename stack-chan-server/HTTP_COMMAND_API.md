# Stack-chan HTTP Command API

This server exposes local HTTP endpoints for Stack-chan voice upload, TTS playback, and command delivery.

Default base URL:

```text
http://<server-ip>:8091
```

For local tests on the server machine:

```text
http://127.0.0.1:8091
```

## Device ID

Most command endpoints need a `device_id`.

The firmware currently uses the ESP32 MAC address as its device ID. You can list recently connected devices:

```bash
curl 'http://127.0.0.1:8091/devices'
```

Example response:

```json
{
  "type": "devices",
  "devices": [
    {
      "device_id": "44:1b:f6:e4:83:8c",
      "last_seen_seconds_ago": 1.2,
      "pending_commands": 0,
      "last_ack": {
        "cmd_id": "cmd_123",
        "status": "done",
        "message": "",
        "ts": 1780761026.82
      }
    }
  ]
}
```

## Health

```http
GET /health
```

Test:

```bash
curl 'http://127.0.0.1:8091/health'
```

## Send Commands

All command shortcuts support `GET`, so they can be called from a browser.

### Speak

Queues a TTS command. Stack-chan will pause listening, stream TTS audio from `/stream-speak`, play it, then resume listening.

```http
GET /command/speak?device_id=<id>&text=<text>
```

```bash
curl -G 'http://127.0.0.1:8091/command/speak' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'text=早上好，我已经收到你的问题啦。'
```

### Face

Queues a simple expression display command.

```http
GET /command/face?device_id=<id>&expression=<expression>
```

```bash
curl -G 'http://127.0.0.1:8091/command/face' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'expression=happy'
```

Common expression values are free-form strings for now, such as:

```text
calm
happy
shy
thinking
happy_squint
happy_squint_soft
listening
stopped
```

The firmware also supports animated expression actions through the same face command:

```text
blink
nod
nodding
happy_dynamic
happy_squint_dynamic
```

Shortcut routes are available for browser/manual testing:

```http
GET /expressions
GET /expression/<name>?device_id=<id>
GET /action/<name>?device_id=<id>
```

```bash
curl -G 'http://127.0.0.1:8091/expression/shy' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c'

curl -G 'http://127.0.0.1:8091/expression/thinking' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c'

curl -G 'http://127.0.0.1:8091/action/blink' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c'

curl -G 'http://127.0.0.1:8091/action/nod' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c'

curl -G 'http://127.0.0.1:8091/action/happy_dynamic' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c'
```

The generic command endpoint also accepts expression/action aliases:

```bash
curl -G 'http://127.0.0.1:8091/command/expression' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'name=害羞'

curl -G 'http://127.0.0.1:8091/command/action' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'action=点头'
```

Chinese aliases are accepted in query params and shortcut paths:

```text
害羞 -> shy
思考 -> thinking
眨眼 -> blink
点头 -> nod
开心 -> happy_dynamic
```

### Motion

Moves the head servos. The simplified motion API uses only `type` and `degree`.

```http
GET /command/move?device_id=<id>&type=left|right|up|down|center&degree=<deg>
```

```bash
curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'type=left' \
  --data-urlencode 'degree=15'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'type=right' \
  --data-urlencode 'degree=15'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'type=up' \
  --data-urlencode 'degree=10'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'type=down' \
  --data-urlencode 'degree=10'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'type=center'
```

`/command/motion` also accepts the same simplified params:

```bash
curl -G 'http://127.0.0.1:8091/command/motion' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'type=left' \
  --data-urlencode 'degree=15' \
  --data-urlencode 'duration_ms=500'
```

The older absolute `pan` / `tilt` form is still supported for debugging.

Firmware limits:

```text
pan:  -75 to 75 degrees
tilt:   0 to 90 degrees
```

### Sequence

Queues a small built-in sequence from GET params.

```http
GET /command/sequence?device_id=<id>&expression=<expression>&text=<text>
```

```bash
curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'expression=thinking' \
  --data-urlencode 'text=让我想一下这个问题。'
```

You can also pass full JSON steps in `payload`:

```bash
curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c' \
  --data-urlencode 'payload=[{"type":"face","expression":"thinking"},{"type":"move","action":"left","degree":15,"duration_ms":400},{"type":"speak","text":"让我想一下这个问题。"}]'
```

### Stop

Stops current speaker playback and displays `stopped`.

```http
GET /command/stop?device_id=<id>
```

```bash
curl -G 'http://127.0.0.1:8091/command/stop' \
  --data-urlencode 'device_id=44:1b:f6:e4:83:8c'
```

## Full Command Schema

```http
POST /command
Content-Type: application/json
```

```json
{
  "device_id": "44:1b:f6:e4:83:8c",
  "type": "sequence",
  "priority": 0,
  "interrupt": false,
  "payload": [
    {"type": "face", "expression": "thinking"},
    {"type": "move", "action": "left", "degree": 15, "duration_ms": 400},
    {"type": "speak", "text": "让我想一下这个问题。"}
  ]
}
```

Server queued response:

```json
{
  "type": "queued",
  "device_id": "44:1b:f6:e4:83:8c",
  "command": {
    "cmd_id": "cmd_ef0dcb4b73b7",
    "type": "speak",
    "priority": 0,
    "interrupt": false,
    "payload": {"text": "早上好"},
    "created_at": 1780760998.46
  }
}
```

## Device Command Channel

The firmware calls this endpoint continuously with long polling:

```http
GET /device/next-command?device_id=<id>&timeout=25
```

If a command is queued:

```json
{
  "type": "command",
  "device_id": "44:1b:f6:e4:83:8c",
  "command": {
    "cmd_id": "cmd_ef0dcb4b73b7",
    "type": "speak",
    "priority": 0,
    "interrupt": false,
    "payload": {"text": "早上好"},
    "created_at": 1780760998.46
  }
}
```

If no command arrives before timeout:

```json
{"type": "noop", "device_id": "44:1b:f6:e4:83:8c"}
```

## ACK

The firmware ACKs command states:

```http
GET /device/ack?device_id=<id>&cmd_id=<cmd_id>&status=received
GET /device/ack?device_id=<id>&cmd_id=<cmd_id>&status=done
GET /device/ack?device_id=<id>&cmd_id=<cmd_id>&status=failed
```

Response:

```json
{
  "type": "ack",
  "device_id": "44:1b:f6:e4:83:8c",
  "ack": {
    "cmd_id": "cmd_ef0dcb4b73b7",
    "status": "done",
    "message": "",
    "ts": 1780761026.82
  }
}
```

## Audio Upload

The firmware uploads detected speech here:

```http
POST /upload-audio
Content-Type: audio/wav
X-Device-Id: <device_id>
```

`/upload-audio` is an alias of `/upload`.

Response:

```json
{
  "type": "stt",
  "text": "用户说的话",
  "task_id": "aliyun-task-id",
  "device_id": "44:1b:f6:e4:83:8c",
  "handled_as": "motion",
  "motion": {"type": "left", "degree": 15, "duration_ms": 500},
  "queued_command": "cmd_abc123"
}
```

When ASR text contains a motion phrase such as `左转15度`, `向右转二十度`, `抬头10度`, or `低头五度`, the server queues a `motion` command back to the same device and does not queue TTS repeat speech. Other non-empty ASR text still queues the demo repeat `sequence`.

## TTS Stream

The firmware uses this endpoint when executing `speak` commands:

```http
GET /stream-speak?text=<text>
```

Response format:

```text
Content-Type: application/octet-stream
X-Audio-Format: pcm_s16le
X-Sample-Rate: 16000
X-Channels: 1
```

Manual test:

```bash
curl -G 'http://127.0.0.1:8091/stream-speak' \
  --data-urlencode 'text=你好，Stack-chan。' \
  -o /tmp/stackchan.pcm
```

## Image Upload

The image endpoint still exists:

```http
POST /upload-image
Content-Type: image/rgb565
X-Image-Format: rgb565
X-Image-Width: 320
X-Image-Height: 240
X-Device-Id: <device_id>
```

Face detection visualization is optional. Without `requirements-face.txt`, images are still saved, but face boxes are disabled.
