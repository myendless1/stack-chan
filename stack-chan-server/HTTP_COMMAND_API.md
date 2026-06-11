# Xiaopai HTTP Command API

This server exposes local HTTP endpoints for Xiaopai voice upload, TTS playback, and command delivery.

Default base URL:

```text
http://<server-ip>:8091
```

For local tests on the server machine:

```text
http://127.0.0.1:8091
```

## Device ID

Command endpoints can omit `device_id`. When it is omitted, the server queues the command for the first currently online device, based on the order devices checked in through `/device/next-command`, `/device/ack`, or `/upload`. Devices are considered online for 90 seconds after their last check-in.

Pass `device_id` only when you need to target a specific device.

The firmware currently uses the ESP32 MAC address as its device ID. You can list recently connected devices:

```bash
curl 'http://127.0.0.1:8091/devices'
```

Example response:

```json
{
  "type": "devices",
  "default_device_id": "44:1b:f6:e4:83:8c",
  "online_ttl_seconds": 90,
  "devices": [
    {
      "device_id": "44:1b:f6:e4:83:8c",
      "last_seen_seconds_ago": 1.2,
      "online": true,
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

Queues a TTS command. Xiaopai will pause listening, stream TTS audio from `/stream-speak`, play it, then resume listening.

```http
GET /command/speak?text=<text>
```

```bash
curl -G 'http://127.0.0.1:8091/command/speak' \
  --data-urlencode 'text=早上好，我已经收到你的问题啦。'
```

### Face

Queues a simple expression display command.

```http
GET /command/face?expression=<expression>
```

```bash
curl -G 'http://127.0.0.1:8091/command/face' \
  --data-urlencode 'expression=happy_squint'
```

Common expression values are free-form strings for now, such as:

```text
calm
shy
thinking
heart
heart_small
wink_half
wink_closed
happy_squint
happy_squint_soft
listening
stopped
```

Voice shortcuts use `开心` for `happy_squint` and `眯眼笑` for `happy_squint_soft`.

The firmware also supports animated expression actions through the same face command:

```text
blink
wink
heart_action
hearting
nod
nodding
speak
speaking
happy_dynamic
happy_squint_dynamic
```

Shortcut routes are available for browser/manual testing:

```http
GET /expressions
GET /expression/<name>
GET /action/<name>
```

```bash
curl -G 'http://127.0.0.1:8091/expression/shy'

curl -G 'http://127.0.0.1:8091/expression/thinking'

curl -G 'http://127.0.0.1:8091/action/blink'

curl -G 'http://127.0.0.1:8091/action/wink'

curl -G 'http://127.0.0.1:8091/action/heart_action'

curl -G 'http://127.0.0.1:8091/action/nod'

curl -G 'http://127.0.0.1:8091/action/happy_dynamic'
```

The generic command endpoint also accepts expression/action aliases:

```bash
curl -G 'http://127.0.0.1:8091/command/expression' \
  --data-urlencode 'name=害羞'

curl -G 'http://127.0.0.1:8091/command/action' \
  --data-urlencode 'action=点头'
```

Chinese aliases are accepted in query params and shortcut paths:

```text
害羞 -> shy
思考 -> thinking
眨眼 -> wink
爱心 -> heart_action
点头 -> nod
开心 -> happy_squint
```

### Motion

Moves the head servos. The simplified motion API uses only `type` and `degree`.

```http
GET /command/move?type=left|right|up|down|center&degree=<deg>
```

```bash
curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=left' \
  --data-urlencode 'degree=15'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=right' \
  --data-urlencode 'degree=15'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=up' \
  --data-urlencode 'degree=10'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=down' \
  --data-urlencode 'degree=10'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=center'
```

`/command/motion` also accepts the same simplified params:

```bash
curl -G 'http://127.0.0.1:8091/command/motion' \
  --data-urlencode 'type=left' \
  --data-urlencode 'degree=15' \
  --data-urlencode 'duration_ms=500'
```

The older absolute `pan` / `tilt` form is still supported for debugging.

Firmware limits:

```text
pan:  -180 to 180 degrees
tilt:   0 to 90 degrees
```

### Sequence

Queues a small built-in sequence from GET params.

```http
GET /command/sequence?expression=<expression>&text=<text>
```

```bash
curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'expression=thinking' \
  --data-urlencode 'text=让我想一下这个问题。'
```

You can also pass full JSON steps in `payload`:

```bash
curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'payload=[{"type":"face","expression":"thinking"},{"type":"move","action":"left","degree":15,"duration_ms":400},{"type":"speak","text":"让我想一下这个问题。"}]'
```

### Stop

Stops current speaker playback and displays `stopped`.

```http
GET /command/stop
```

```bash
curl -G 'http://127.0.0.1:8091/command/stop'
```

## Full Command Schema

```http
POST /command
Content-Type: application/json
```

```json
{
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

## Device Events

Xiaopai can report non-speech events to the server:

```http
GET /device/event?device_id=<device_id>&type=head_touch&name=click
POST /device/event
Content-Type: application/json
```

```json
{
  "device_id": "44:1b:f6:e4:83:8c",
  "type": "head_touch",
  "name": "click"
}
```

When OpenClaw is configured, the server forwards the event to OpenClaw and does not parse the returned text. OpenClaw should queue Xiaopai behavior by calling the HTTP command endpoints such as `POST /command`, `GET /command/<type>`, `GET /expression/<name>`, or `GET /action/<name>`.

The response includes `openclaw_sent` and `queued_commands`. Events forwarded to OpenClaw do not queue commands by themselves; Xiaopai executes commands that OpenClaw sends through the command API and the existing `/device/next-command` long-poll loop.

## Audio Upload

The firmware uploads detected speech here:

```http
POST /upload-audio
Content-Type: audio/wav
X-Device-Id: <device_id>
```

`/upload-audio` is an alias of `/upload`.

If OpenClaw is configured with `OPENCLAW_BASE_URL` and `OPENCLAW_GATEWAY_TOKEN`, non-empty ASR text is sent to OpenClaw. The server does not read or parse OpenClaw response text, tags, or actions. OpenClaw should call the command API when it wants Xiaopai to speak, move, or change expression.

Response:

```json
{
  "type": "stt",
  "text": "用户说的话",
  "task_id": "aliyun-task-id",
  "device_id": "44:1b:f6:e4:83:8c",
  "handled_as": "openclaw_forwarded",
  "dialog_awake": true,
  "openclaw_enabled": true,
  "openclaw_sent": true,
  "queued_commands": []
}
```

Other non-empty ASR text still queues the demo repeat `sequence`.

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
  --data-urlencode 'text=你好，Xiaopai。' \
  -o /tmp/xiaopai.pcm
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
