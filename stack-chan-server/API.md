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
  {"type": "speak", "text": "我往左看一下。"},
  {"type": "face", "expression": "happy_squint"}
]
```

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
    "priority": 0,
    "interrupt": true,
    "payload": {"text": "你好呀"},
    "created_at": 1710000000.0
  }
}
```

## Stop

Stop current playback and show the stopped face state.

```http
GET /command/stop
```

## OpenClaw Tag Alternative

The server also supports an OpenClaw-style tag contract for internal ASR/event handling:

```text
<action>thinking</action><speak>我想一下。</speak><action>happy_squint</action>
<action>move:left:15</action><speak>我往左看一下。</speak>
```

For direct HTTP control, prefer `POST /command` or the `GET /command/<type>` endpoints above.
