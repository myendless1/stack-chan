#!/usr/bin/env python3
import argparse
import base64
import datetime as _dt
import hashlib
import hmac
import json
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Empty, Queue


ASR_URLS = {
    "shanghai": "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr",
    "beijing": "https://nls-gateway-cn-beijing.aliyuncs.com/stream/v1/asr",
    "shenzhen": "https://nls-gateway-cn-shenzhen.aliyuncs.com/stream/v1/asr",
}

TTS_URLS = {
    "shanghai": "https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/tts",
    "beijing": "https://nls-gateway-cn-beijing.aliyuncs.com/stream/v1/tts",
    "shenzhen": "https://nls-gateway-cn-shenzhen.aliyuncs.com/stream/v1/tts",
}

TOKEN_META_ENDPOINT = "https://nls-meta.cn-shanghai.aliyuncs.com/"
TOKEN_REGION_ID = "cn-shanghai"
TOKEN_API_VERSION = "2019-02-28"
TOKEN_REFRESH_MARGIN_SECONDS = 300
DEVICE_ONLINE_TTL_SECONDS = 90

AVAILABLE_EXPRESSIONS = (
    "calm",
    "shy",
    "thinking",
    "speak1",
    "speak2",
    "blink_half",
    "blink_closed",
    "wink_half",
    "wink_closed",
    "heart_small",
    "heart",
    "nod_soft",
    "nod_down",
    "happy_squint",
    "happy_squint_soft",
)

AVAILABLE_ACTIONS = (
    "blink",
    "wink",
    "heart_action",
    "hearting",
    "nod",
    "nodding",
    "speak",
    "speaking",
    "happy_dynamic",
    "happy_squint_dynamic",
)

HEAD_TOUCH_EVENT_TEXT = {
    "press": "按压",
    "click": "你好，我是小派同学",
    "swipe_forward": "你好，我是小派同学",
    "swipe_backward": "你好，我是小派同学",
}

EXPRESSION_ALIASES = {
    "default": "calm",
    "listening": "calm",
    "stopped": "calm",
    "think": "thinking",
    "thinking": "thinking",
    "heart": "heart_action",
    "love": "heart_action",
    "wink": "wink",
    "blink": "blink",
    "shy": "shy",
    "happy": "happy_squint",
    "calm": "calm",
    "开心": "happy_squint",
    "害羞": "shy",
    "爱心": "heart_action",
    "思考": "thinking",
    "眨眼": "wink",
    "点头": "nod",
}

MOTION_DIRECTION_ALIASES = {
    "左": "left",
    "左边": "left",
    "左转": "left",
    "向左": "left",
    "往左": "left",
    "朝左": "left",
    "转左": "left",
    "右": "right",
    "右边": "right",
    "右转": "right",
    "向右": "right",
    "往右": "right",
    "朝右": "right",
    "转右": "right",
    "上": "up",
    "上面": "up",
    "向上": "up",
    "往上": "up",
    "朝上": "up",
    "抬头": "up",
    "下": "down",
    "下面": "down",
    "向下": "down",
    "往下": "down",
    "朝下": "down",
    "低头": "down",
    "left": "left",
    "right": "right",
    "up": "up",
    "down": "down",
}

MOTION_CENTER_PHRASES = (
    "请回正",
    "回正",
    "回中",
    "回中间",
    "回到中间",
    "回到正中",
    "回到正中间",
    "回到初始位置",
    "回到初始",
    "回初始位置",
    "回初始",
    "恢复初始位置",
    "恢复初始",
    "归位",
    "复位",
    "重置位置",
    "回家",
    "center",
    "home",
)

VOICE_FACE_COMMAND_TRIGGERS = (
    "切换到",
    "切到",
    "换成",
    "切换",
    "显示",
    "设置为",
    "设为",
    "变成",
    "做",
    "表情",
    "动作",
    "expression",
    "face",
    "action",
)

VOICE_FACE_ALIASES = (
    ("heart_action", ("爱心", "吐爱心", "亲亲爱心", "heart action", "hearting", "love")),
    ("wink", ("眨眼", "眨一下眼", "单眼眨眼", "wink")),
    ("thinking", ("思考", "思考表情", "想一想", "想一下", "thinking", "think")),
    ("happy_squint_soft", ("眯眼笑", "眯眼微笑", "happy squint soft", "happy_squint_soft", "柔和眯眼笑", "柔和眯眼开心")),
    ("happy_squint", ("开心表情", "开心", "高兴表情", "高兴", "快乐表情", "快乐", "happy squint", "happy_squint", "happy")),
    ("speak", ("说话动作", "说话表情", "说话脸", "讲话动作", "讲话表情", "讲话脸", "speak", "speaking")),
    ("calm", ("平静表情", "平静", "冷静表情", "冷静", "calm")),
    ("shy", ("害羞表情", "害羞", "羞涩表情", "羞涩", "shy")),
)

JOKE_TEXT_XIAOMING_SLOW_SCHOOL = (
    "老师问小明：“你为什么总是迟到？”\n"
    "小明说：“因为路上有个牌子写着‘学校前方，请慢行’。”\n"
    "老师气笑了：“那你也不能慢成这样吧？”\n"
    "小明委屈地说：“我已经很努力了，今天还超速了两步。”"
)

VOICE_SPEAK_COMMANDS = (
    {
        "name": "joke_xiaoming_slow_school",
        "aliases": ("讲个笑话", "说个笑话", "来个笑话", "讲笑话"),
        "text": JOKE_TEXT_XIAOMING_SLOW_SCHOOL,
    },
)

CHINESE_DIGITS = {
    "零": 0,
    "〇": 0,
    "一": 1,
    "二": 2,
    "两": 2,
    "三": 3,
    "四": 4,
    "五": 5,
    "六": 6,
    "七": 7,
    "八": 8,
    "九": 9,
}


def split_sentences(text: str, max_chars: int):
    text = re.sub(r"\s+", " ", text.strip())
    if not text:
        return

    buf = []
    for ch in text:
        buf.append(ch)
        sentence_end = ch in "。！？!?；;\n"
        if sentence_end or len(buf) >= max_chars:
            part = "".join(buf).strip()
            if part:
                yield part
            buf.clear()

    part = "".join(buf).strip()
    if part:
        yield part


def detect_wav_sample_rate(data: bytes) -> int | None:
    if len(data) < 28 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    return struct.unpack_from("<I", data, 24)[0]


def read_binary_file(path: str) -> bytes:
    with open(path, "rb") as fp:
        return fp.read()


def pcm_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    data_size = len(pcm)
    byte_rate = sample_rate * 2
    return b"".join(
        (
            b"RIFF",
            struct.pack("<I", 36 + data_size),
            b"WAVE",
            b"fmt ",
            struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, byte_rate, 2, 16),
            b"data",
            struct.pack("<I", data_size),
            pcm,
        )
    )


def parse_chinese_integer(text: str) -> int | None:
    text = text.strip()
    if not text:
        return None
    if all(ch in CHINESE_DIGITS for ch in text):
        value = 0
        for ch in text:
            value = value * 10 + CHINESE_DIGITS[ch]
        return value

    total = 0
    current = 0
    for ch in text:
        if ch in CHINESE_DIGITS:
            current = CHINESE_DIGITS[ch]
        elif ch == "十":
            total += (current or 1) * 10
            current = 0
        elif ch == "百":
            total += (current or 1) * 100
            current = 0
        else:
            return None
    return total + current


def parse_spoken_number(text: str) -> float | None:
    text = text.strip().translate(str.maketrans("０１２３４５６７８９．", "0123456789."))
    if not text:
        return None
    if re.fullmatch(r"\d+(?:\.\d+)?", text):
        return float(text)
    value = parse_chinese_integer(text)
    return float(value) if value is not None else None


def parse_voice_motion_command(text: str) -> dict | None:
    normalized = re.sub(r"[\s,，。.!！?？]+", "", text.strip().lower())
    if not normalized:
        return None

    if any(phrase in normalized for phrase in MOTION_CENTER_PHRASES):
        return {
            "type": "center",
            "duration_ms": 600,
            "source_text": text,
        }

    number_pattern = r"([0-9０-９]+(?:[.．][0-9０-９]+)?|[零〇一二两三四五六七八九十百]+)"
    direction_pattern = (
        r"(左转|右转|转左|转右|向左|向右|往左|往右|朝左|朝右|左边|右边|"
        r"抬头|低头|向上|向下|往上|往下|朝上|朝下|上面|下面|左|右|上|下|"
        r"left|right|up|down)"
    )
    action_pattern = r"(?:转|转动|移动|动|摆|看|运动)?"
    patterns = (
        re.compile(direction_pattern + action_pattern + number_pattern + r"(?:度|degrees?|°)"),
        re.compile(number_pattern + r"(?:度|degrees?|°)" + action_pattern + direction_pattern),
    )

    for pattern in patterns:
        match = pattern.search(normalized)
        if not match:
            continue
        first, second = match.group(1), match.group(2)
        if first in MOTION_DIRECTION_ALIASES:
            direction_text, number_text = first, second
        else:
            number_text, direction_text = first, second
        degree = parse_spoken_number(number_text)
        direction = MOTION_DIRECTION_ALIASES.get(direction_text)
        if direction and degree is not None and degree > 0:
            return {
                "type": direction,
                "degree": degree,
                "duration_ms": 500,
                "source_text": text,
            }
    return None


def normalize_voice_command_text(text: str) -> str:
    return re.sub(r"[\s,_\-，。.!！?？/（）()]+", "", text.strip().lower())


def parse_voice_face_command(text: str) -> dict | None:
    normalized = normalize_voice_command_text(text)
    if not normalized:
        return None

    has_trigger = any(trigger in normalized for trigger in VOICE_FACE_COMMAND_TRIGGERS)
    for expression, aliases in VOICE_FACE_ALIASES:
        for alias in aliases:
            alias_normalized = normalize_voice_command_text(alias)
            if not alias_normalized:
                continue
            if normalized == alias_normalized or (has_trigger and alias_normalized in normalized):
                return {
                    "expression": expression,
                    "source_text": text,
                }
    return None


def parse_voice_speak_command(text: str) -> dict | None:
    normalized = normalize_voice_command_text(text)
    if not normalized:
        return None

    for command in VOICE_SPEAK_COMMANDS:
        for alias in command["aliases"]:
            alias_normalized = normalize_voice_command_text(alias)
            if normalized == alias_normalized or alias_normalized in normalized:
                return {
                    "name": command["name"],
                    "text": command["text"],
                    "source_text": text,
                }
    return None


class AliyunVoiceServer(ThreadingHTTPServer):
    token: str
    token_expire_time: int
    access_key_id: str
    access_key_secret: str
    appkey: str
    asr_url: str
    tts_url: str
    voice: str
    sample_rate: int
    volume: int
    speech_rate: int
    pitch_rate: int
    max_sentence_chars: int
    chunk_size: int
    tts_prefetch_workers: int
    tts_request_timeout: int
    tts_retries: int
    capture_dir: str
    static_dir: str
    device_queues: dict[str, Queue]
    last_ack: dict[str, dict]
    last_seen: dict[str, float]
    device_order: list[str]

    def get_token(self) -> str:
        if self.access_key_id and self.access_key_secret:
            now = int(time.time())
            if not self.token or now >= self.token_expire_time - TOKEN_REFRESH_MARGIN_SECONDS:
                self.token, self.token_expire_time = create_aliyun_nls_token(
                    self.access_key_id, self.access_key_secret
                )
                print(f"Aliyun NLS token refreshed, expires_at={self.token_expire_time}", flush=True)
        return self.token


class Handler(BaseHTTPRequestHandler):
    server_version = "XiaopaiAliyunVoice/1.0"

    def do_GET(self):
        path, query = self._path_query()
        if path in ("/", "/health"):
            self._send_json(
                {
                    "ok": True,
                    "service": "xiaopai-aliyun-voice",
                    "asr": "/upload",
                    "tts": "/stream-speak?text=...",
                    "image": "/upload-image",
                    "tts_format": "pcm_s16le",
                    "sample_rate": self.server.sample_rate,
                    "channels": 1,
                    "voice": self.server.voice,
                    "expressions": list(AVAILABLE_EXPRESSIONS),
                    "actions": list(AVAILABLE_ACTIONS),
                    "head_touch_events": HEAD_TOUCH_EVENT_TEXT,
                }
            )
            return
        if path == "/expressions":
            self._send_json(
                {
                    "type": "expressions",
                    "expressions": list(AVAILABLE_EXPRESSIONS),
                    "actions": list(AVAILABLE_ACTIONS),
                    "aliases": EXPRESSION_ALIASES,
                    "examples": {
                        "expression": "/expression/shy?device_id=...",
                        "action": "/action/blink?device_id=...",
                    },
                }
            )
            return
        if path.startswith("/expression/"):
            expression = urllib.parse.unquote(path.rsplit("/", 1)[-1])
            self._handle_face_shortcut(query, expression)
            return
        if path.startswith("/action/"):
            action = urllib.parse.unquote(path.rsplit("/", 1)[-1])
            self._handle_face_shortcut(query, action, action_only=True)
            return
        if path == "/devices":
            self._handle_devices()
            return
        if path == "/command":
            self._handle_command(query)
            return
        if path.startswith("/command/"):
            command_type = path.rsplit("/", 1)[-1]
            self._handle_command(query, command_type=command_type)
            return
        if path == "/device/next-command":
            self._handle_next_command(query)
            return
        if path == "/device/ack":
            self._handle_ack(query)
            return
        if path == "/head-touch-events":
            self._send_json(
                {
                    "type": "head_touch_events",
                    "events": [
                        {
                            "name": name,
                            "text": text,
                            "audio": f"/event-audio/{name}.pcm",
                            "wav": f"/event-audio/{name}.wav",
                        }
                        for name, text in HEAD_TOUCH_EVENT_TEXT.items()
                    ],
                    "format": "pcm_s16le",
                    "sample_rate": self.server.sample_rate,
                    "channels": 1,
                }
            )
            return
        if path.startswith("/event-audio/"):
            self._handle_event_audio(path.rsplit("/", 1)[-1])
            return
        if path == "/stream-speak":
            self._handle_stream_speak(query.get("text", [""])[0])
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        path, query = self._path_query()
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        if path == "/upload":
            self._handle_upload(body)
            return
        if path == "/upload-audio":
            self._handle_upload(body)
            return
        if path == "/command":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_command(query, posted=payload)
            return
        if path == "/device/ack":
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_ack(query, posted=payload)
            return
        if path == "/upload-image":
            self._handle_upload_image(body)
            return
        if path == "/stream-speak":
            text = query.get("text", [""])[0]
            content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
            if not text and content_type == "application/json" and body:
                payload = json.loads(body.decode("utf-8"))
                text = payload.get("text") or payload.get("input") or ""
            elif not text and body:
                text = body.decode("utf-8")
            self._handle_stream_speak(text)
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def _path_query(self):
        parsed = urllib.parse.urlparse(self.path)
        return parsed.path, urllib.parse.parse_qs(parsed.query)

    def _handle_upload(self, body: bytes):
        if not body:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing audio body")
            return

        path, query = self._path_query()
        device_id = self._device_id(query)
        self._mark_device_seen(device_id)
        sample_rate = detect_wav_sample_rate(body) or self.server.sample_rate
        audio_format = "wav" if detect_wav_sample_rate(body) else "pcm"
        print(f"ASR upload: device={device_id} bytes={len(body)} format={audio_format} sample_rate={sample_rate}")
        try:
            result = self._aliyun_asr(body, audio_format, sample_rate)
        except Exception as exc:
            print(f"ASR failed: {exc}", file=sys.stderr)
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        text = result.get("result", "")
        status = result.get("status")
        message = result.get("message", "")
        print(f"ASR result: status={status} text={text!r} message={message!r}")
        if status != 20000000:
            self._send_json({"type": "error", "message": message or f"Aliyun ASR status {status}"}, HTTPStatus.BAD_GATEWAY)
            return

        response = {"type": "stt", "text": text, "task_id": result.get("task_id", ""), "device_id": device_id}
        if text:
            speak_payload = parse_voice_speak_command(text)
            if speak_payload:
                source_text = speak_payload.pop("source_text", text)
                command_name = speak_payload.pop("name", "")
                command = make_command("speak", speak_payload, priority=1, interrupt=True)
                response["handled_as"] = "speak"
                response["speak"] = {"name": command_name}
                response["source_text"] = source_text
            elif motion_payload := parse_voice_motion_command(text):
                source_text = motion_payload.pop("source_text", text)
                command = make_command("motion", motion_payload, priority=1, interrupt=True)
                response["handled_as"] = "motion"
                response["motion"] = motion_payload
                response["source_text"] = source_text
            elif face_payload := parse_voice_face_command(text):
                source_text = face_payload.pop("source_text", text)
                command = make_command("face", face_payload, priority=1, interrupt=True)
                response["handled_as"] = "face"
                response["face"] = face_payload
                response["source_text"] = source_text
            else:
                reply = f"我听到你说：{text}"
                command = make_command(
                    "sequence",
                    [
                        {"type": "face", "expression": "thinking"},
                        {"type": "speak", "text": reply},
                        {"type": "face", "expression": "happy_squint"},
                    ],
                )
                response["handled_as"] = "repeat"
            self._enqueue_command(device_id, command)
            response["queued_command"] = command["cmd_id"]
        self._send_json(response)

    def _handle_devices(self):
        now = time.time()
        devices = []
        known_device_ids = list(self.server.device_order)
        for device_id in self.server.last_seen:
            if device_id not in self.server.device_order:
                known_device_ids.append(device_id)
        for device_id in known_device_ids:
            seen = self.server.last_seen.get(device_id)
            if seen is None:
                continue
            queue = self._queue_for(device_id)
            devices.append(
                {
                    "device_id": device_id,
                    "last_seen_seconds_ago": round(now - seen, 1),
                    "online": now - seen <= DEVICE_ONLINE_TTL_SECONDS,
                    "pending_commands": queue.qsize(),
                    "last_ack": self.server.last_ack.get(device_id),
                }
            )
        self._send_json(
            {
                "type": "devices",
                "default_device_id": first_connected_device_id(
                    self.server.last_seen, self.server.device_order
                ),
                "online_ttl_seconds": DEVICE_ONLINE_TTL_SECONDS,
                "devices": devices,
            }
        )

    def _handle_command(self, query: dict, command_type: str = "", posted: dict | None = None):
        posted = posted or {}
        requested_device_id = first_value(query, "device_id") or posted.get("device_id") or ""
        device_id = self._resolve_command_device_id(requested_device_id)
        command_type = command_type or first_value(query, "type") or posted.get("type") or "speak"
        priority = int(first_value(query, "priority") or posted.get("priority") or 0)
        interrupt = parse_bool(first_value(query, "interrupt") or posted.get("interrupt") or "false")

        if "payload" in posted and isinstance(posted["payload"], (dict, list)):
            payload = posted["payload"]
        else:
            payload = command_payload_from_query(command_type, query)

        if command_type in ("expression", "action"):
            command_wire_type = "face"
        else:
            command_wire_type = "motion" if command_type == "move" else command_type
        if command_wire_type == "face" and isinstance(payload, dict):
            payload["expression"] = normalize_expression_name(payload.get("expression") or payload.get("face") or "calm")
        elif command_wire_type == "sequence" and isinstance(payload, list):
            for step in payload:
                if isinstance(step, dict) and step.get("type") == "face":
                    step["expression"] = normalize_expression_name(step.get("expression") or step.get("face") or "calm")
        command = make_command(command_wire_type, payload, priority=priority, interrupt=interrupt)
        self._enqueue_command(device_id, command)
        self._send_json({"type": "queued", "device_id": device_id, "command": command})

    def _handle_face_shortcut(self, query: dict, expression: str, action_only: bool = False):
        expression = normalize_expression_name(expression)
        if action_only and expression not in AVAILABLE_ACTIONS:
            self._send_json(
                {
                    "type": "error",
                    "message": f"unknown action: {expression}",
                    "actions": list(AVAILABLE_ACTIONS),
                },
                HTTPStatus.BAD_REQUEST,
            )
            return
        if expression not in AVAILABLE_EXPRESSIONS and expression not in AVAILABLE_ACTIONS:
            self._send_json(
                {
                    "type": "error",
                    "message": f"unknown expression or action: {expression}",
                    "expressions": list(AVAILABLE_EXPRESSIONS),
                    "actions": list(AVAILABLE_ACTIONS),
                },
                HTTPStatus.BAD_REQUEST,
            )
            return

        requested_device_id = first_value(query, "device_id")
        device_id = self._resolve_command_device_id(requested_device_id)
        priority = int(first_value(query, "priority") or 0)
        interrupt = parse_bool(first_value(query, "interrupt") or "false")
        command = make_command("face", {"expression": expression}, priority=priority, interrupt=interrupt)
        self._enqueue_command(device_id, command)
        self._send_json(
            {
                "type": "queued",
                "device_id": device_id,
                "expression": expression,
                "kind": "action" if expression in AVAILABLE_ACTIONS else "expression",
                "command": command,
            }
        )

    def _handle_next_command(self, query: dict):
        device_id = self._device_id(query)
        timeout = float(first_value(query, "timeout") or "25")
        timeout = max(0.0, min(timeout, 55.0))
        self._mark_device_seen(device_id)
        queue = self._queue_for(device_id)
        try:
            command = queue.get(timeout=timeout)
            self._send_json({"type": "command", "device_id": device_id, "command": command})
        except Empty:
            self._send_json({"type": "noop", "device_id": device_id})

    def _handle_ack(self, query: dict, posted: dict | None = None):
        posted = posted or {}
        device_id = self._device_id(query) if query else posted.get("device_id", "default")
        ack = {
            "cmd_id": first_value(query, "cmd_id") or posted.get("cmd_id", ""),
            "status": first_value(query, "status") or posted.get("status", "received"),
            "message": first_value(query, "message") or posted.get("message", ""),
            "ts": time.time(),
        }
        self.server.last_ack[device_id] = ack
        self._mark_device_seen(device_id)
        self._send_json({"type": "ack", "device_id": device_id, "ack": ack})

    def _device_id(self, query: dict) -> str:
        device_id = first_value(query, "device_id") or self.headers.get("X-Device-Id", "") or "default"
        return safe_device_id(device_id)

    def _resolve_command_device_id(self, requested_device_id: str) -> str:
        device_id = safe_device_id(requested_device_id)
        if is_placeholder_device_id(device_id):
            first_connected = first_connected_device_id(self.server.last_seen, self.server.device_order)
            if first_connected:
                return first_connected
        return device_id

    def _queue_for(self, device_id: str) -> Queue:
        queue = self.server.device_queues.get(device_id)
        if queue is None:
            queue = Queue()
            self.server.device_queues[device_id] = queue
        return queue

    def _enqueue_command(self, device_id: str, command: dict) -> None:
        device_id = safe_device_id(device_id)
        self._queue_for(device_id).put(command)
        detail = ""
        if command.get("type") == "face" and isinstance(command.get("payload"), dict):
            detail = f" expression={command['payload'].get('expression', '')}"
        print(f"Command queued: device={device_id} cmd_id={command['cmd_id']} type={command['type']}{detail}", flush=True)

    def _mark_device_seen(self, device_id: str) -> None:
        device_id = safe_device_id(device_id)
        if device_id not in self.server.device_order:
            self.server.device_order.append(device_id)
        self.server.last_seen[device_id] = time.time()

    def _handle_event_audio(self, filename: str):
        audio_ext = "wav" if filename.endswith(".wav") else "pcm"
        name = filename.rsplit(".", 1)[0] if "." in filename else filename
        if name not in HEAD_TOUCH_EVENT_TEXT:
            self._send_json(
                {
                    "type": "error",
                    "message": f"unknown head touch event: {name}",
                    "events": list(HEAD_TOUCH_EVENT_TEXT),
                },
                HTTPStatus.NOT_FOUND,
            )
            return

        cache_dir = os.path.join(self.server.static_dir, "event-audio")
        os.makedirs(cache_dir, exist_ok=True)
        pcm_path = os.path.join(cache_dir, f"{name}.pcm")
        wav_path = os.path.join(cache_dir, f"{name}.wav")
        if not os.path.exists(pcm_path) or os.path.getsize(pcm_path) == 0:
            text = HEAD_TOUCH_EVENT_TEXT[name]
            print(f"Event audio cache miss: {name} -> {text!r}", flush=True)
            try:
                audio = self._aliyun_tts_pcm_with_retries(text)
            except Exception as exc:
                print(f"Event audio TTS failed: {exc}", file=sys.stderr, flush=True)
                self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
                return
            tmp_path = f"{pcm_path}.tmp"
            with open(tmp_path, "wb") as fp:
                fp.write(audio)
            os.replace(tmp_path, pcm_path)
            print(f"Event audio cached: {pcm_path} bytes={len(audio)}", flush=True)
        if not os.path.exists(wav_path) or os.path.getsize(wav_path) == 0:
            pcm = read_binary_file(pcm_path)
            tmp_path = f"{wav_path}.tmp"
            with open(tmp_path, "wb") as fp:
                fp.write(pcm_to_wav(pcm, self.server.sample_rate))
            os.replace(tmp_path, wav_path)

        path = wav_path if audio_ext == "wav" else pcm_path

        try:
            stat = os.stat(path)
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "audio/wav" if audio_ext == "wav" else "application/octet-stream")
            self.send_header("Content-Length", str(stat.st_size))
            self.send_header("X-Audio-Format", "wav" if audio_ext == "wav" else "pcm_s16le")
            self.send_header("X-Sample-Rate", str(self.server.sample_rate))
            self.send_header("X-Channels", "1")
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
            self.end_headers()
            with open(path, "rb") as fp:
                while True:
                    chunk = fp.read(64 * 1024)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            print(f"Event audio client disconnected: {name}", flush=True)

    def _handle_upload_image(self, body: bytes):
        if not body:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing image body")
            return

        content_type = self.headers.get("Content-Type", "application/octet-stream")
        width = int(self.headers.get("X-Image-Width", "0") or "0")
        height = int(self.headers.get("X-Image-Height", "0") or "0")
        image_format = self.headers.get("X-Image-Format", "").strip().lower()
        device_id = self.headers.get("X-Device-Id", "unknown").replace(":", "")
        safe_device = re.sub(r"[^A-Za-z0-9_.-]+", "_", device_id)[:40] or "unknown"

        os.makedirs(self.server.capture_dir, exist_ok=True)
        stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        base = os.path.join(self.server.capture_dir, f"xiaopai-{safe_device}-{stamp}")

        raw_ext = "jpg" if content_type.startswith("image/jpeg") else (image_format or "bin")
        raw_path = f"{base}.{raw_ext}"
        with open(raw_path, "wb") as fp:
            fp.write(body)

        bmp_path = ""
        png_path = ""
        face_visual_path = ""
        face_result = {"available": False, "faces": []}
        if image_format == "rgb565" and width > 0 and height > 0:
            expected = width * height * 2
            if len(body) != expected:
                self._send_json(
                    {
                        "type": "error",
                        "message": f"rgb565 size mismatch: got {len(body)}, expected {expected}",
                        "raw_path": raw_path,
                    },
                    HTTPStatus.BAD_REQUEST,
                )
                return
            bmp_path = f"{base}.bmp"
            with open(bmp_path, "wb") as fp:
                fp.write(rgb565_to_bmp(body, width, height))
            png_path = f"{base}.png"
            with open(png_path, "wb") as fp:
                fp.write(rgb565_to_png(body, width, height))
            face_visual_path, face_result = detect_and_visualize_faces(png_path, f"{base}.faces.png")
        elif content_type.startswith("image/jpeg"):
            face_visual_path, face_result = detect_and_visualize_faces(raw_path, f"{base}.faces.png")

        print(
            f"Image upload: bytes={len(body)} type={content_type} format={image_format} "
            f"size={width}x{height} raw={raw_path} bmp={bmp_path} png={png_path} "
            f"faces={len(face_result.get('faces', []))} face_visual={face_visual_path}"
        )
        self._send_json(
            {
                "type": "image",
                "bytes": len(body),
                "format": image_format or content_type,
                "width": width,
                "height": height,
                "raw_path": raw_path,
                "bmp_path": bmp_path,
                "png_path": png_path,
                "face_visual_path": face_visual_path,
                "face_detection": face_result,
            }
        )

    def _aliyun_asr(self, audio: bytes, audio_format: str, sample_rate: int):
        params = {
            "appkey": self.server.appkey,
            "format": audio_format,
            "sample_rate": str(sample_rate),
            "enable_punctuation_prediction": "true",
            "enable_inverse_text_normalization": "true",
            "enable_voice_detection": "true",
        }
        url = self.server.asr_url + "?" + urllib.parse.urlencode(params)
        req = urllib.request.Request(
            url,
            data=audio,
            method="POST",
            headers={
                "X-NLS-Token": self.server.get_token(),
                "Content-Type": "application/octet-stream",
            },
        )
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def _handle_stream_speak(self, text: str):
        text = text.strip()
        if not text:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing text")
            return

        parts = list(split_sentences(text, self.server.max_sentence_chars))
        if not parts:
            self.send_error(HTTPStatus.BAD_REQUEST, "empty text")
            return

        stream_started = time.perf_counter()
        try:
            print(f"TTS prepare first sentence: {parts[0]!r}", flush=True)
            first_audio = self._aliyun_tts_pcm_with_retries(parts[0])
        except Exception as exc:
            print(f"TTS failed before stream started: {exc}", file=sys.stderr, flush=True)
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        first_ready_ms = (time.perf_counter() - stream_started) * 1000
        print(f"TTS stream: {len(parts)} sentence(s), first_ready_ms={first_ready_ms:.0f}, text={text!r}", flush=True)
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("X-Audio-Format", "pcm_s16le")
        self.send_header("X-Sample-Rate", str(self.server.sample_rate))
        self.send_header("X-Channels", "1")
        if len(parts) == 1:
            self.send_header("Content-Length", str(len(first_audio)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        try:
            print(f"TTS audio bytes: {len(first_audio)} for {parts[0]!r}", flush=True)
            if first_audio:
                self.wfile.write(first_audio)
                self.wfile.flush()
            sent_bytes = len(first_audio)

            remaining = parts[1:]
            if not remaining:
                total_ms = (time.perf_counter() - stream_started) * 1000
                print(f"TTS stream done: bytes={sent_bytes} total_ms={total_ms:.0f}", flush=True)
                return

            workers = max(1, min(self.server.tts_prefetch_workers, len(remaining)))
            with ThreadPoolExecutor(max_workers=workers) as pool:
                futures = [pool.submit(self._aliyun_tts_pcm_with_retries, part) for part in remaining]
                future_timeout = self.server.tts_request_timeout * (self.server.tts_retries + 1) + 5
                for part, future in zip(remaining, futures):
                    wait_started = time.perf_counter()
                    print(f"TTS sentence ready wait: {part!r}", flush=True)
                    audio = future.result(timeout=future_timeout)
                    wait_ms = (time.perf_counter() - wait_started) * 1000
                    print(f"TTS audio bytes: {len(audio)} wait_ms={wait_ms:.0f} for {part!r}", flush=True)
                    if audio:
                        self.wfile.write(audio)
                        self.wfile.flush()
                        sent_bytes += len(audio)
            total_ms = (time.perf_counter() - stream_started) * 1000
            print(f"TTS stream done: bytes={sent_bytes} total_ms={total_ms:.0f}", flush=True)
        except (BrokenPipeError, ConnectionResetError):
            print("TTS client disconnected")
        except TimeoutError:
            print("TTS failed after stream started: sentence timed out", file=sys.stderr, flush=True)
        except Exception as exc:
            print(f"TTS failed after stream started: {exc}", file=sys.stderr, flush=True)

    def _aliyun_tts_pcm_with_retries(self, text: str) -> bytes:
        last_error: Exception | None = None
        for attempt in range(1, self.server.tts_retries + 2):
            try:
                return self._aliyun_tts_pcm(text)
            except Exception as exc:
                last_error = exc
                print(f"TTS attempt {attempt} failed for {text!r}: {exc}", file=sys.stderr, flush=True)
        raise RuntimeError(f"Aliyun TTS failed after {self.server.tts_retries + 1} attempt(s): {last_error}")

    def _aliyun_tts_pcm(self, text: str) -> bytes:
        started = time.perf_counter()
        params = {
            "appkey": self.server.appkey,
            "token": self.server.get_token(),
            "text": text,
            "format": "pcm",
            "sample_rate": self.server.sample_rate,
            "voice": self.server.voice,
            "volume": self.server.volume,
            "speech_rate": self.server.speech_rate,
            "pitch_rate": self.server.pitch_rate,
        }
        url = self.server.tts_url + "?" + urllib.parse.urlencode(params)
        req = urllib.request.Request(url, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=self.server.tts_request_timeout) as resp:
                content_type = resp.headers.get("Content-Type", "")
                if "json" in content_type:
                    raise RuntimeError(resp.read().decode("utf-8", errors="replace"))
                audio = resp.read()
                elapsed_ms = (time.perf_counter() - started) * 1000
                print(f"Aliyun TTS ok: chars={len(text)} bytes={len(audio)} elapsed_ms={elapsed_ms:.0f}", flush=True)
                return audio
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Aliyun TTS HTTP {exc.code}: {detail}") from exc

    def _send_json(self, body: dict, status: HTTPStatus = HTTPStatus.OK):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} - {fmt % args}")


def required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Missing {name}. Export it before starting this service.")
    return value


def optional_env(*names: str) -> str:
    for name in names:
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return ""


def load_dotenv(path: str) -> None:
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8") as fp:
        for raw_line in fp:
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip("'\"")
            if key and key not in os.environ:
                os.environ[key] = value


def percent_encode(value: str) -> str:
    return urllib.parse.quote(value, safe="-_.~")


def create_aliyun_nls_token(access_key_id: str, access_key_secret: str) -> tuple[str, int]:
    timestamp = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    params = {
        "AccessKeyId": access_key_id,
        "Action": "CreateToken",
        "Format": "JSON",
        "RegionId": TOKEN_REGION_ID,
        "SignatureMethod": "HMAC-SHA1",
        "SignatureNonce": str(uuid.uuid4()),
        "SignatureVersion": "1.0",
        "Timestamp": timestamp,
        "Version": TOKEN_API_VERSION,
    }
    canonical_query = "&".join(
        f"{percent_encode(key)}={percent_encode(params[key])}" for key in sorted(params)
    )
    string_to_sign = "GET&%2F&" + percent_encode(canonical_query)
    digest = hmac.new(
        (access_key_secret + "&").encode("utf-8"),
        string_to_sign.encode("utf-8"),
        hashlib.sha1,
    ).digest()
    signature = base64.b64encode(digest).decode("ascii")
    query = "Signature=" + percent_encode(signature) + "&" + canonical_query
    url = TOKEN_META_ENDPOINT + "?" + query
    try:
        with urllib.request.urlopen(url, timeout=15) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Aliyun CreateToken HTTP {exc.code}: {detail}") from exc

    token = payload.get("Token", {})
    token_id = token.get("Id", "")
    expire_time = int(token.get("ExpireTime", 0) or 0)
    if not token_id or not expire_time:
        raise RuntimeError(f"Aliyun CreateToken returned no token: {payload}")
    return token_id, expire_time


def first_value(query: dict, key: str) -> str:
    value = query.get(key, [""])
    if isinstance(value, list):
        return value[0] if value else ""
    return str(value)


def parse_bool(value: str) -> bool:
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def safe_device_id(device_id: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.:-]+", "_", str(device_id).strip())[:64]
    return safe or "default"


def is_placeholder_device_id(device_id: str) -> bool:
    value = str(device_id).strip().upper()
    return value in ("", "DEFAULT", "AA:BB:CC:DD:EE:FF", "AABBCCDDEEFF")


def normalize_expression_name(expression: str) -> str:
    value = str(expression or "").strip()
    if not value:
        return "calm"
    return EXPRESSION_ALIASES.get(value, value)


def first_connected_device_id(
    last_seen: dict[str, float],
    device_order: list[str],
    now: float | None = None,
) -> str:
    if not last_seen:
        return "default"
    now = time.time() if now is None else now
    for device_id in device_order:
        seen = last_seen.get(device_id)
        if seen is not None and now - seen <= DEVICE_ONLINE_TTL_SECONDS:
            return device_id
    return "default"


def make_command(command_type: str, payload, priority: int = 0, interrupt: bool = False) -> dict:
    return {
        "cmd_id": f"cmd_{uuid.uuid4().hex[:12]}",
        "type": command_type,
        "priority": priority,
        "interrupt": interrupt,
        "payload": payload,
        "created_at": time.time(),
    }


def command_payload_from_query(command_type: str, query: dict):
    if command_type in ("face", "expression", "action"):
        expression = first_value(query, "expression") or first_value(query, "face") or "calm"
        if command_type in ("expression", "action"):
            expression = first_value(query, "name") or first_value(query, "action") or expression
        return {"expression": normalize_expression_name(expression)}
    if command_type == "speak":
        return {"text": first_value(query, "text") or "你好呀"}
    if command_type == "play_audio":
        return {"url": first_value(query, "url")}
    if command_type in ("motion", "move"):
        motion_type = first_value(query, "type") or first_value(query, "action") or first_value(query, "direction")
        if motion_type:
            return {
                "type": motion_type,
                "degree": float(first_value(query, "degree") or first_value(query, "degrees") or "15"),
                "duration_ms": int(first_value(query, "duration_ms") or "500"),
            }
        return {
            "pan": float(first_value(query, "pan") or "0"),
            "tilt": float(first_value(query, "tilt") or "45"),
            "duration_ms": int(first_value(query, "duration_ms") or "500"),
        }
    if command_type == "stop":
        return {}
    if command_type == "sequence":
        raw = first_value(query, "payload") or first_value(query, "steps")
        if raw:
            try:
                payload = json.loads(raw)
                if isinstance(payload, list):
                    return payload
            except json.JSONDecodeError:
                pass
        text = first_value(query, "text")
        expression = normalize_expression_name(first_value(query, "expression") or "calm")
        steps = [{"type": "face", "expression": expression}]
        if text:
            steps.append({"type": "speak", "text": text})
        return steps
    return {key: values[0] for key, values in query.items() if values}


def rgb565_to_bmp(rgb565: bytes, width: int, height: int) -> bytes:
    row_stride = width * 3
    padding = (4 - (row_stride % 4)) % 4
    pixel_bytes = (row_stride + padding) * height
    file_size = 14 + 40 + pixel_bytes

    out = bytearray()
    out += b"BM"
    out += struct.pack("<IHHI", file_size, 0, 0, 54)
    out += struct.pack("<IIIHHIIIIII", 40, width, height, 1, 24, 0, pixel_bytes, 2835, 2835, 0, 0)

    for y in range(height - 1, -1, -1):
        row_start = y * width * 2
        for x in range(width):
            hi = rgb565[row_start + x * 2]
            lo = rgb565[row_start + x * 2 + 1]
            value = (hi << 8) | lo
            r = ((value >> 11) & 0x1F) * 255 // 31
            g = ((value >> 5) & 0x3F) * 255 // 63
            b = (value & 0x1F) * 255 // 31
            out += bytes((b, g, r))
        out += b"\x00" * padding
    return bytes(out)


def rgb565_to_rgb_rows(rgb565: bytes, width: int, height: int) -> list[bytes]:
    rows = []
    for y in range(height):
        row_start = y * width * 2
        row = bytearray(width * 3)
        out = 0
        for x in range(width):
            hi = rgb565[row_start + x * 2]
            lo = rgb565[row_start + x * 2 + 1]
            value = (hi << 8) | lo
            row[out] = ((value >> 11) & 0x1F) * 255 // 31
            row[out + 1] = ((value >> 5) & 0x3F) * 255 // 63
            row[out + 2] = (value & 0x1F) * 255 // 31
            out += 3
        rows.append(bytes(row))
    return rows


def png_chunk(chunk_type: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + chunk_type + data + struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)


def rgb565_to_png(rgb565: bytes, width: int, height: int) -> bytes:
    raw = b"".join(b"\x00" + row for row in rgb565_to_rgb_rows(rgb565, width, height))
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    out += png_chunk(b"IDAT", zlib.compress(raw, level=6))
    out += png_chunk(b"IEND", b"")
    return bytes(out)


def detect_and_visualize_faces(image_path: str, output_path: str) -> tuple[str, dict]:
    try:
        import face_recognition
        from PIL import Image, ImageDraw, ImageFont
    except Exception as exc:
        return "", {"available": False, "error": f"face_recognition/Pillow unavailable: {exc}", "faces": []}

    try:
        image = face_recognition.load_image_file(image_path)
        locations = face_recognition.face_locations(image, number_of_times_to_upsample=1, model="hog")
        landmarks = face_recognition.face_landmarks(image, locations)

        pil_image = Image.open(image_path).convert("RGB")
        draw = ImageDraw.Draw(pil_image)
        try:
            font = ImageFont.truetype("DejaVuSans.ttf", 14)
        except Exception:
            font = ImageFont.load_default()

        faces = []
        for idx, (top, right, bottom, left) in enumerate(locations, start=1):
            faces.append(
                {
                    "top": top,
                    "right": right,
                    "bottom": bottom,
                    "left": left,
                    "center": {"x": (left + right) / 2, "y": (top + bottom) / 2},
                    "area": (right - left) * (bottom - top),
                }
            )
            draw.rectangle(((left, top), (right, bottom)), outline=(0, 255, 0), width=3)
            label = f"face {idx}"
            text_box = draw.textbbox((left, top), label, font=font)
            label_h = text_box[3] - text_box[1] + 4
            draw.rectangle(((left, max(0, top - label_h)), (left + text_box[2] - text_box[0] + 8, top)), fill=(0, 160, 0))
            draw.text((left + 4, max(0, top - label_h + 2)), label, fill=(255, 255, 255), font=font)

        for face_landmarks in landmarks:
            for points in face_landmarks.values():
                if len(points) > 1:
                    draw.line(points, fill=(255, 220, 0), width=2)

        pil_image.save(output_path, "PNG")
        best_face = max(faces, key=lambda face: face["area"], default=None)
        return output_path, {"available": True, "faces": faces, "best_face": best_face, "landmarks": landmarks}
    except Exception as exc:
        return "", {"available": True, "error": str(exc), "faces": []}


def main():
    load_dotenv(os.path.join(os.path.dirname(os.path.dirname(__file__)), ".env"))

    parser = argparse.ArgumentParser(description="Local Xiaopai bridge for Aliyun ASR and PCM streaming TTS.")
    parser.add_argument("--host", default=os.environ.get("STACKCHAN_ALIYUN_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_PORT", "8091")))
    parser.add_argument("--region", choices=sorted(ASR_URLS), default=os.environ.get("STACKCHAN_ALIYUN_REGION", "shanghai"))
    parser.add_argument("--tts-url", default=os.environ.get("STACKCHAN_ALIYUN_TTS_URL", ""))
    parser.add_argument("--voice", default=os.environ.get("STACKCHAN_ALIYUN_VOICE", "xiaoyun"))
    parser.add_argument("--sample-rate", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_SAMPLE_RATE", "16000")))
    parser.add_argument("--volume", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_VOLUME", "80")))
    parser.add_argument("--speech-rate", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_SPEECH_RATE", "0")))
    parser.add_argument("--pitch-rate", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_PITCH_RATE", "0")))
    parser.add_argument("--max-sentence-chars", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_MAX_SENTENCE_CHARS", "120")))
    parser.add_argument("--chunk-size", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_CHUNK_SIZE", "4096")))
    parser.add_argument("--tts-prefetch-workers", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_TTS_PREFETCH_WORKERS", "2")))
    parser.add_argument("--tts-request-timeout", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_TTS_REQUEST_TIMEOUT", "12")))
    parser.add_argument("--tts-retries", type=int, default=int(os.environ.get("STACKCHAN_ALIYUN_TTS_RETRIES", "2")))
    parser.add_argument("--capture-dir", default=os.environ.get("STACKCHAN_CAPTURE_DIR", "captures"))
    parser.add_argument("--static-dir", default=os.environ.get("STACKCHAN_STATIC_DIR", "static"))
    args = parser.parse_args()

    httpd = AliyunVoiceServer((args.host, args.port), Handler)
    httpd.access_key_id = optional_env("ALIYUN_AK_ID", "ALIYUN_ACCESS_KEY_ID")
    httpd.access_key_secret = optional_env("ALIYUN_AK_SECRET", "ALIYUN_ACCESS_KEY_SECRET")
    httpd.token = optional_env("ALIYUN_NLS_TOKEN")
    httpd.token_expire_time = int(optional_env("ALIYUN_NLS_TOKEN_EXPIRE_TIME") or "0")
    if not httpd.token and (not httpd.access_key_id or not httpd.access_key_secret):
        raise SystemExit(
            "Missing Aliyun credentials. Set ALIYUN_NLS_TOKEN, or set ALIYUN_AK_ID and ALIYUN_AK_SECRET."
        )
    if not httpd.token:
        httpd.token, httpd.token_expire_time = create_aliyun_nls_token(
            httpd.access_key_id, httpd.access_key_secret
        )
    httpd.appkey = required_env("ALIYUN_NLS_APPKEY")
    httpd.asr_url = ASR_URLS[args.region]
    httpd.tts_url = args.tts_url or TTS_URLS[args.region]
    httpd.voice = args.voice
    httpd.sample_rate = args.sample_rate
    httpd.volume = args.volume
    httpd.speech_rate = args.speech_rate
    httpd.pitch_rate = args.pitch_rate
    httpd.max_sentence_chars = args.max_sentence_chars
    httpd.chunk_size = args.chunk_size
    httpd.tts_prefetch_workers = args.tts_prefetch_workers
    httpd.tts_request_timeout = args.tts_request_timeout
    httpd.tts_retries = args.tts_retries
    httpd.capture_dir = args.capture_dir
    httpd.static_dir = args.static_dir
    httpd.device_queues = {}
    httpd.last_ack = {}
    httpd.last_seen = {}
    httpd.device_order = []

    print("Xiaopai Aliyun voice bridge")
    print(f"  health: http://127.0.0.1:{args.port}/health")
    print(f"  ASR:    http://{args.host}:{args.port}/upload")
    print(f"  TTS:    http://{args.host}:{args.port}/stream-speak?text=...")
    print(f"  Events: http://{args.host}:{args.port}/event-audio/press.pcm -> {args.static_dir}/event-audio")
    print(f"  Image:  http://{args.host}:{args.port}/upload-image -> {args.capture_dir}")
    print(f"  Command push via HTTP long poll:")
    print(f"          device: GET http://{args.host}:{args.port}/device/next-command?device_id=...")
    print(f"          send:   GET http://{args.host}:{args.port}/command/speak?device_id=...&text=...")
    print(f"  voice:  {args.voice}, pcm_s16le {args.sample_rate}Hz mono")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
