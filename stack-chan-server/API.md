# Xiaopai Control API

This server can queue commands for Xiaopai to speak, move its head, and change expressions. It is suitable for OpenClaw or another local agent to call over HTTP.

Default base URL:

```text
http://<server-ip>:8091
```

If `device_id` is omitted, the server queues the command for the first currently online Xiaopai device. A device is considered online after it polls `/device/next-command`.

There is no authentication on these local endpoints. Expose them only on a trusted LAN.

## Health

```http
GET /health
```

Use this to check whether the server is running.

## Device List

```http
GET /devices
```

Returns online devices, pending command counts, and last acknowledgements. OpenClaw can omit `device_id` for a single-device setup.

## Speak

Queue a TTS speech command.

```http
GET /command/speak?text=<text>&device_id=<optional>
```

Example:

```bash
curl -G 'http://127.0.0.1:8091/command/speak' \
  --data-urlencode 'text=你好，我是小派同学。'
```

## Expression And Animation

Queue a face expression or face animation.

```http
GET /command/face?expression=<name>&device_id=<optional>
GET /expression/<name>?device_id=<optional>
GET /action/<name>?device_id=<optional>
```

Expressions:

```text
calm, shy, thinking, speak1, speak2, blink_half, blink_closed,
wink_half, wink_closed, heart_small, heart, nod_soft, nod_down,
happy_squint, happy_squint_soft
```

Actions / animations:

```text
blink, wink, heart_action, hearting, nod, nodding,
speak, speaking, happy_dynamic, happy_squint_dynamic
```

Examples:

```bash
curl -G 'http://127.0.0.1:8091/command/face' \
  --data-urlencode 'expression=thinking'

curl 'http://127.0.0.1:8091/expression/shy'

curl 'http://127.0.0.1:8091/action/blink'
```

## Head Motion

Move Xiaopai's head.

```http
GET /command/move?type=left|right|up|down|center&degree=<deg>&duration_ms=<ms>
GET /command/motion?type=left|right|up|down|center&degree=<deg>&duration_ms=<ms>
```

`degree` defaults to `15`; `duration_ms` defaults to `500`. `center` returns the head to the home position and does not need `degree`.

Examples:

```bash
curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=left' \
  --data-urlencode 'degree=15'

curl -G 'http://127.0.0.1:8091/command/move' \
  --data-urlencode 'type=center'
```

## Visual Tracking

When Xiaopai uploads a camera frame to `POST /upload-image`, the server runs YuNet face detection locally. If the largest face is outside the configured deadzone, the server queues a `motion` command for the uploading device.

To ask an online Xiaopai device to take one photo and start this flow:

```http
GET /command/capture_image?device_id=<optional>
GET /command/track_once?device_id=<optional>
```

Example:

```bash
curl 'http://127.0.0.1:8091/command/capture_image'
```

The response includes:

```json
{
  "face_detection": {
    "backend": "yunet",
    "best_face": {
      "center": {"x": 180.0, "y": 110.0},
      "confidence": 0.92
    }
  },
  "visual_tracking": {
    "status": "queued",
    "cmd_id": "cmd_xxxxxxxxxxxx",
    "error": {"x": 20.0, "y": -18.0}
  }
}
```

Useful startup arguments:

```text
--visual-tracking-enabled / --no-visual-tracking-enabled
--visual-tracking-deadzone-px 20
--visual-tracking-gain-x 1.0
--visual-tracking-gain-y 1.0
--visual-tracking-max-degree 12
--visual-tracking-min-interval-ms 350
--visual-tracking-max-pending 2
--visual-tracking-invert-x / --no-visual-tracking-invert-x
--visual-tracking-invert-y / --no-visual-tracking-invert-y
--find-owner-gain-x 1.0
--find-owner-gain-y 0.8
--find-owner-stop-pixels 32
```

`--visual-tracking-gain-x` controls left/right sensitivity for normal `/upload-image` auto-tracking, and `--visual-tracking-gain-y` controls up/down sensitivity.

`find_owner` uploads frames with `X-Visual-Tracking: false`, so wake-up owner finding uses the `--find-owner-*` startup arguments that the server sends in the command payload. `--find-owner-gain-x` is the horizontal movement multiplier for the wake-up owner-finding path. Horizontal and vertical find-owner movement no longer has a per-step degree cap; only the final servo angle range is clamped by the firmware.

## Sequence

Queue multiple actions in order.

```http
GET /command/sequence?payload=<json-array>
```

The payload is a JSON array. Supported step types:

```json
[
  {"type": "face", "expression": "thinking"},
  {"type": "move", "action": "left", "degree": 15, "duration_ms": 500},
  {"type": "find_owner", "rounds": 1, "reply": "我在"},
  {"type": "volume", "direction": "up", "step": 10},
  {"type": "speak", "text": "我往左看一下。"},
  {"type": "face", "expression": "happy_squint"}
]
```

Voice recognition also maps phrases containing `声音` plus `大` or `小` to a volume command. The firmware stores
speaker volume as 10-100, maps it to the M5 speaker's 0-255 range, changes it by 10 each time, then replies
`已经将声音调到XXX`. If the phrase also contains `最`, `声音最大` sets 100 and `声音最小` sets 10.

Example:

```bash
curl -G 'http://127.0.0.1:8091/command/sequence' \
  --data-urlencode 'payload=[{"type":"face","expression":"thinking"},{"type":"move","action":"left","degree":15,"duration_ms":500},{"type":"speak","text":"我往左看一下。"},{"type":"face","expression":"happy_squint"}]'
```

## JSON Command Entry

For OpenClaw, the most convenient POST endpoint is:

```http
POST /command
Content-Type: application/json
```

Speak:

```json
{
  "type": "speak",
  "payload": {"text": "你好呀"},
  "interrupt": true
}
```

Face:

```json
{
  "type": "face",
  "payload": {"expression": "shy"},
  "interrupt": true
}
```

Motion:

```json
{
  "type": "move",
  "payload": {"type": "right", "degree": 20, "duration_ms": 500},
  "interrupt": true
}
```

Sequence:

```json
{
  "type": "sequence",
  "payload": [
    {"type": "face", "expression": "thinking"},
    {"type": "speak", "text": "我想一下。"},
    {"type": "face", "expression": "happy_squint"}
  ],
  "interrupt": true
}
```

Response shape:

```json
{
  "type": "queued",
  "device_id": "default-or-device-id",
  "command": {
    "cmd_id": "cmd_xxxxxxxxxxxx",
    "type": "speak",
    "priority": 10,
    "interrupt": true,
    "ttl_seconds": 30.0,
    "discardable": true,
    "coalesce_key": "speak",
    "payload": {"text": "你好呀"},
    "created_at": 1710000000.0
  }
}
```

Commands can include `priority`, `interrupt`, `ttl_seconds`, `discardable`, and `coalesce_key`.
Defaults prioritize `stop`, volume, camera/find-owner, and face updates above motion and speak.

## Stop

Stop current playback and show the stopped face state.

```http
GET /command/stop
```

## OpenClaw Control

ASR text and device events can be forwarded to OpenClaw when configured, but the server does not parse OpenClaw replies. OpenClaw should control Xiaopai by calling `POST /command`, `GET /command/<type>`, `GET /expression/<name>`, or `GET /action/<name>`.
