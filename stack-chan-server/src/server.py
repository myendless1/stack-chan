#!/usr/bin/env python3
import argparse
import base64
import datetime as _dt
import hashlib
import hmac
import json
import os
import random
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Empty

from yunet_service import YunetFaceService


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
DIALOG_AWAKE_SECONDS = 60
LOG_TEXT_MAX_CHARS = 2000
DIALOG_WAKE_WORDS = (
    "小派同学",
    "小派同學",
    "小派",
    "小胖",
    "小盼",
    "小潘",
    "小排",
    "小白",
    "小坏",
    "小壞",
    "小蔡",
    "小外",
    "xiaopai",
)
DIALOG_SLEEP_WORDS = (
    "退下",
    "退一下",
    "退一下吧",
    "退一退",
    "拜拜",
    "再见",
    "再會",
    "再会",
    "不用了",
    "先这样",
    "先這樣",
)
DIALOG_WAKE_ONLY_FILLERS = ("你好", "您好", "在吗", "在嗎", "醒醒", "hello", "hi", "嗨", "哈喽", "哈囉")

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

COMMAND_QUEUE_MAX_SIZE = 24
COMMAND_DEFAULT_PRIORITIES = {
    "stop": 100,
    "volume": 90,
    "sound": 90,
    "find_owner": 85,
    "locate_owner": 85,
    "capture_image": 70,
    "track_once": 70,
    "camera": 70,
    "face": 65,
    "expression": 65,
    "action": 65,
    "motion": 45,
    "move": 45,
    "sequence": 30,
    "speak": 10,
    "play_audio": 10,
}
COMMAND_DEFAULT_TTL_SECONDS = {
    "face": 8.0,
    "expression": 8.0,
    "action": 8.0,
    "motion": 5.0,
    "move": 5.0,
    "speak": 30.0,
    "sequence": 45.0,
}
COMMAND_COALESCE_BY_TYPE = {"face", "expression", "action", "motion", "move", "speak"}
COMMAND_DISCARDABLE_TYPES = {"face", "expression", "action", "motion", "move", "speak", "sequence"}

WAKE_REPLY_EVENTS = (
    ("wake_reply", "我在。"),
    ("wake_reply_help", "有什么要帮忙的"),
    ("wake_reply_hello", "你好呀"),
    ("wake_reply_here", "我在呢"),
    ("wake_reply_xiaopai_here", "小派在呢"),
)

HEAD_TOUCH_EVENT_TEXT = {name: text for name, text in WAKE_REPLY_EVENTS}
HEAD_TOUCH_EVENT_TEXT.update(
    {
        "press": "按压",
        "click": "你好，我是小派同学",
        "swipe_forward": "你好，我是小派同学",
        "swipe_backward": "你好，我是小派同学",
    }
)

XIAOPAI_OPENCLAW_SYSTEM_PROMPT = """你会收到“小派同学”（用户终端小机器人）的语音识别文本或设备事件。

这个服务端只负责把文本/事件转发给你，不会解析你的回复来控制机器人。
如果需要让机器人说话、移动或切换表情，请主动调用小派服务端提供的 HTTP 控制 API。
"""


def log_timestamp() -> str:
    return _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def log_print(message: str, *, file=None) -> None:
    print(f"[{log_timestamp()}] {message}", file=file or sys.stdout, flush=True)

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


def has_dialog_wake_word(text: str) -> bool:
    normalized = normalize_voice_command_text(text)
    return any(normalize_voice_command_text(word) in normalized for word in DIALOG_WAKE_WORDS)


def has_dialog_sleep_word(text: str) -> bool:
    normalized = normalize_voice_command_text(text)
    return any(normalize_voice_command_text(word) in normalized for word in DIALOG_SLEEP_WORDS)


def is_wake_only_text(text: str) -> bool:
    normalized = normalize_voice_command_text(text)
    for word in DIALOG_WAKE_WORDS:
        normalized = normalized.replace(normalize_voice_command_text(word), "")
    for filler in DIALOG_WAKE_ONLY_FILLERS:
        normalized = normalized.replace(normalize_voice_command_text(filler), "")
    return not normalized


def parse_voice_volume_command(text: str) -> dict | None:
    normalized = normalize_voice_command_text(text)
    if "声音" not in normalized:
        return None
    down_words = ("小", "轻", "低", "降低", "调低", "关小", "小点", "小一点")
    up_words = ("大", "响", "高", "提高", "调高", "放大", "大点", "大一点")
    if any(word in normalized for word in down_words):
        if "最" in normalized:
            return {"mode": "set", "value": 10, "source_text": text}
        return {"direction": "down", "step": 10, "source_text": text}
    if any(word in normalized for word in up_words):
        if "最" in normalized:
            return {"mode": "set", "value": 100, "source_text": text}
        return {"direction": "up", "step": 10, "source_text": text}
    return None


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


def command_default_priority(command_type: str) -> int:
    return COMMAND_DEFAULT_PRIORITIES.get(str(command_type or ""), 20)


def command_default_ttl(command_type: str) -> float:
    return COMMAND_DEFAULT_TTL_SECONDS.get(str(command_type or ""), 60.0)


def command_is_discardable(command: dict) -> bool:
    if "discardable" in command:
        return bool(command.get("discardable"))
    return str(command.get("type") or "") in COMMAND_DISCARDABLE_TYPES


def command_coalesce_key(command: dict) -> str:
    key = str(command.get("coalesce_key") or "").strip()
    if key:
        return key
    command_type = str(command.get("type") or "")
    if command_type in COMMAND_COALESCE_BY_TYPE:
        return command_type
    return ""


class DeviceCommandQueue:
    def __init__(self, max_size: int = COMMAND_QUEUE_MAX_SIZE):
        self.max_size = max(1, int(max_size))
        self._items = []
        self._seq = 0
        self._cv = threading.Condition()

    def qsize(self) -> int:
        with self._cv:
            self._drop_expired_locked(time.time())
            return len(self._items)

    def put(self, command: dict) -> dict:
        now = time.time()
        command_type = str(command.get("type") or "")
        priority = max(int(command.get("priority") or 0), command_default_priority(command_type))
        ttl = float(command.get("ttl_seconds") or command_default_ttl(command_type))
        expires_at = now + ttl if ttl > 0 else 0.0
        item = {
            "command": command,
            "priority": priority,
            "seq": self._seq,
            "expires_at": expires_at,
            "coalesce_key": command_coalesce_key(command),
            "discardable": command_is_discardable(command),
        }

        with self._cv:
            self._seq += 1
            stats = {"queued": False, "expired": self._drop_expired_locked(now), "preempted": 0, "coalesced": 0, "dropped": 0}

            if command.get("interrupt"):
                kept = []
                for existing in self._items:
                    if existing["priority"] <= priority or existing["discardable"]:
                        stats["preempted"] += 1
                    else:
                        kept.append(existing)
                self._items = kept

            if item["coalesce_key"]:
                kept = []
                for existing in self._items:
                    if existing["coalesce_key"] == item["coalesce_key"] and existing["priority"] <= priority:
                        stats["coalesced"] += 1
                    else:
                        kept.append(existing)
                self._items = kept

            while len(self._items) >= self.max_size:
                drop_index = self._find_drop_index_locked(priority)
                if drop_index is None:
                    stats["dropped"] += 1
                    return stats
                self._items.pop(drop_index)
                stats["dropped"] += 1

            self._items.append(item)
            stats["queued"] = True
            self._cv.notify()
            return stats

    def get(self, timeout: float | None = None) -> dict:
        deadline = None if timeout is None else time.monotonic() + timeout
        with self._cv:
            while True:
                self._drop_expired_locked(time.time())
                if self._items:
                    index = max(range(len(self._items)), key=lambda i: (self._items[i]["priority"], -self._items[i]["seq"]))
                    return self._items.pop(index)["command"]
                if timeout == 0:
                    raise Empty
                if deadline is None:
                    self._cv.wait()
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise Empty
                self._cv.wait(remaining)

    def get_nowait(self) -> dict:
        return self.get(timeout=0)

    def _drop_expired_locked(self, now: float) -> int:
        before = len(self._items)
        self._items = [
            item for item in self._items if item["expires_at"] <= 0 or item["expires_at"] > now
        ]
        return before - len(self._items)

    def _find_drop_index_locked(self, incoming_priority: int) -> int | None:
        discardable = [
            (item["priority"], item["seq"], index)
            for index, item in enumerate(self._items)
            if item["discardable"] and item["priority"] <= incoming_priority
        ]
        if discardable:
            return min(discardable)[2]
        lower_or_equal = [
            (item["priority"], item["seq"], index)
            for index, item in enumerate(self._items)
            if item["priority"] <= incoming_priority
        ]
        if lower_or_equal:
            return min(lower_or_equal)[2]
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
    tts_tail_silence_ms: int
    capture_save_mode: str
    command_queue_max_size: int
    capture_dir: str
    static_dir: str
    openclaw_base_url: str
    openclaw_token: str
    openclaw_model: str
    openclaw_backend_model: str
    openclaw_timeout: int
    openclaw_max_completion_tokens: int
    openclaw_session_prefix: str
    openclaw_executor: ThreadPoolExecutor
    debug_log: bool
    device_lock: threading.Lock
    face_detector_backend: str
    face_detector: YunetFaceService | None
    visual_tracking_enabled: bool
    visual_tracking_deadzone_px: float
    visual_tracking_gain_x: float
    visual_tracking_gain_y: float
    visual_tracking_max_degree: float
    visual_tracking_min_degree: float
    visual_tracking_duration_ms: int
    visual_tracking_min_interval_ms: int
    visual_tracking_max_pending: int
    visual_tracking_invert_x: bool
    visual_tracking_invert_y: bool
    visual_tracking_last_command_at: dict[str, float]
    find_owner_gain_x: float
    find_owner_gain_y: float
    find_owner_stop_pixels: float
    device_queues: dict[str, DeviceCommandQueue]
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
                if getattr(self, "debug_log", False):
                    log_print(f"Aliyun NLS token refreshed, expires_at={self.token_expire_time}")
                else:
                    log_print("Aliyun NLS token refreshed")
        return self.token


class Handler(BaseHTTPRequestHandler):
    server_version = "XiaopaiAliyunVoice/1.0"

    def _debug_enabled(self) -> bool:
        return bool(getattr(self.server, "debug_log", False))

    def _log_info(self, message: str) -> None:
        log_print(message)

    def _log_debug(self, message: str) -> None:
        if self._debug_enabled():
            log_print(message)

    def _log_error(self, message: str) -> None:
        log_print(message, file=sys.stderr)

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
                    "face_detector": self.server.face_detector.status()
                    if self.server.face_detector is not None
                    else {"backend": self.server.face_detector_backend, "available": False},
                    "visual_tracking": {
                        "enabled": self.server.visual_tracking_enabled,
                        "deadzone_px": self.server.visual_tracking_deadzone_px,
                        "gain_x": self.server.visual_tracking_gain_x,
                        "gain_y": self.server.visual_tracking_gain_y,
                        "max_degree": self.server.visual_tracking_max_degree,
                        "min_degree": self.server.visual_tracking_min_degree,
                        "duration_ms": self.server.visual_tracking_duration_ms,
                        "min_interval_ms": self.server.visual_tracking_min_interval_ms,
                        "max_pending": self.server.visual_tracking_max_pending,
                        "invert_x": self.server.visual_tracking_invert_x,
                        "invert_y": self.server.visual_tracking_invert_y,
                    },
                    "find_owner": {
                        "gain_x": self.server.find_owner_gain_x,
                        "gain_y": self.server.find_owner_gain_y,
                        "stop_pixels": self.server.find_owner_stop_pixels,
                    },
                    "openclaw": {
                        "enabled": self._openclaw_enabled(),
                        "base_url": self.server.openclaw_base_url,
                        "model": self.server.openclaw_model,
                    },
                    "command_queue": {
                        "max_size": self.server.command_queue_max_size,
                        "default_priorities": COMMAND_DEFAULT_PRIORITIES,
                        "coalesced_types": sorted(COMMAND_COALESCE_BY_TYPE),
                        "discardable_types": sorted(COMMAND_DISCARDABLE_TYPES),
                    },
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
        if path in ("/device/event", "/event"):
            self._handle_device_event(query)
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
        if path in ("/device/event", "/event"):
            payload = json.loads(body.decode("utf-8")) if body else {}
            self._handle_device_event(query, posted=payload)
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
        self._log_info(f"ASR upload received: {audio_format}, {len(body)} bytes")
        self._log_debug(f"ASR upload detail: device={device_id} bytes={len(body)} format={audio_format} sample_rate={sample_rate}")
        try:
            result = self._aliyun_asr(body, audio_format, sample_rate)
        except Exception as exc:
            self._log_error(f"ASR failed: {exc}")
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        text = result.get("result", "")
        status = result.get("status")
        message = result.get("message", "")
        if text:
            self._log_info(f"ASR recognized: {text!r}")
        else:
            self._log_info("ASR recognized no speech")
        self._log_debug(
            "ASR result detail: "
            f"device={device_id} status={status} task_id={result.get('task_id', '')!r} "
            f"text={text!r} message={message!r}"
        )
        if status != 20000000:
            self._send_json({"type": "error", "message": message or f"Aliyun ASR status {status}"}, HTTPStatus.BAD_GATEWAY)
            return

        response = {"type": "stt", "text": text, "task_id": result.get("task_id", ""), "device_id": device_id}
        if text:
            volume_command = parse_voice_volume_command(text)
            if volume_command is not None:
                command = make_command("volume", volume_command, priority=1, interrupt=True)
                self._enqueue_command(device_id, command)
                response["handled_as"] = "volume_adjust"
                response["dialog_awake"] = self._dialog_awake(device_id)
                response["queued_command"] = command["cmd_id"]
                response["volume_direction"] = volume_command.get("direction") or volume_command.get("mode") or "set"
                self._send_json(response)
                return

            if has_dialog_sleep_word(text):
                self._sleep_dialog(device_id, reason=text)
                response["handled_as"] = "sleep"
                response["dialog_awake"] = False
                self._send_json(response)
                return

            woke_by_word = has_dialog_wake_word(text)
            if woke_by_word:
                self._wake_dialog(device_id, reason=text)
                response["woke_by"] = "wake_word"
                wake_reply_name, wake_reply_text = random.choice(WAKE_REPLY_EVENTS)
                wake_reply_command = make_command(
                    "speak",
                    {"text": wake_reply_text, "pause_listener": True, "cache_name": wake_reply_name},
                    priority=95,
                    interrupt=True,
                    ttl_seconds=8,
                    discardable=False,
                    coalesce_key="wake_reply",
                )
                find_owner_command = make_command(
                    "find_owner",
                    {
                        "rounds": 1,
                        "reply": "",
                        "preserve_speech": True,
                        "wait_for_speech": False,
                        "gain_x": self.server.find_owner_gain_x,
                        "gain_y": self.server.find_owner_gain_y,
                        "stop_pixels": self.server.find_owner_stop_pixels,
                    },
                    priority=85,
                    interrupt=True,
                )
                self._enqueue_command(device_id, wake_reply_command)
                self._enqueue_command(device_id, find_owner_command)
                response["queued_command"] = wake_reply_command["cmd_id"]
                response["queued_commands"] = [wake_reply_command["cmd_id"], find_owner_command["cmd_id"]]
                response["wake_reply"] = {"name": wake_reply_name, "text": wake_reply_text}
                if is_wake_only_text(text):
                    response["handled_as"] = "wake"
                    response["dialog_awake"] = True
                    self._send_json(response)
                    return
            elif not self._dialog_awake(device_id):
                response["handled_as"] = "sleeping"
                response["dialog_awake"] = False
                self._log_info("ASR ignored while sleeping")
                self._log_debug(f"ASR sleeping detail: device={device_id} text={text!r}")
                self._send_json(response)
                return
            else:
                self._wake_dialog(device_id, reason="dialog activity")

            response["dialog_awake"] = True
            openclaw_result = self._send_openclaw_event(
                device_id,
                "speech_recognition",
                {"text": text},
            )
            response.update(openclaw_result)
            response["handled_as"] = "openclaw_forwarded" if openclaw_result.get("openclaw_sent") else "openclaw_not_sent"
        else:
            response["handled_as"] = "empty"
            self._log_info("ASR empty; OpenClaw skipped")
            self._log_debug(f"ASR empty detail: device={device_id}")
        self._send_json(response)

    def _handle_device_event(self, query: dict, posted: dict | None = None):
        posted = posted or {}
        device_id = self._device_id(query) if query else posted.get("device_id", "default")
        device_id = safe_device_id(posted.get("device_id") or device_id)
        self._mark_device_seen(device_id)

        event_type = first_value(query, "type") or posted.get("type") or posted.get("event_type") or "event"
        name = first_value(query, "name") or posted.get("name") or posted.get("event") or ""
        text = first_value(query, "text") or posted.get("text") or ""
        details = posted.get("details") if isinstance(posted.get("details"), dict) else {}
        if not details:
            details = {key: values[0] for key, values in query.items() if values and key not in ("device_id",)}
        if name:
            details["name"] = name
        if text:
            details["text"] = text

        if str(event_type) in ("head_touch", "touch"):
            command = make_command("face", {"expression": "shy"}, priority=1, interrupt=True)
            self._enqueue_command(device_id, command)
            self._send_json(
                {
                    "type": "event",
                    "device_id": device_id,
                    "event_type": event_type,
                    "name": name,
                    "openclaw_enabled": self._openclaw_enabled(),
                    "openclaw_skipped": "local_head_touch_expression",
                    "openclaw_sent": False,
                    "queued_commands": [command["cmd_id"]],
                }
            )
            return

        if str(event_type) == "speech_recognition" and not str(details.get("text") or "").strip():
            self._send_json(
                {
                    "type": "event",
                    "device_id": device_id,
                    "event_type": event_type,
                    "name": name,
                    "openclaw_enabled": self._openclaw_enabled(),
                    "openclaw_skipped": "empty_speech_recognition",
                    "openclaw_sent": False,
                    "queued_commands": [],
                }
            )
            self._log_info("Speech event empty; OpenClaw skipped")
            self._log_debug(f"Speech event empty detail: device={device_id}")
            return

        result = self._send_openclaw_event(device_id, str(event_type), details)
        body = {
            "type": "event",
            "device_id": device_id,
            "event_type": event_type,
            "name": name,
            "queued_commands": [],
            **result,
        }
        self._send_json(body)

    def _handle_devices(self):
        now = time.time()
        devices = []
        with self.server.device_lock:
            known_device_ids = list(self.server.device_order)
            last_seen_snapshot = dict(self.server.last_seen)
            last_ack_snapshot = dict(self.server.last_ack)
        for device_id in last_seen_snapshot:
            if device_id not in known_device_ids:
                known_device_ids.append(device_id)
        for device_id in known_device_ids:
            seen = last_seen_snapshot.get(device_id)
            if seen is None:
                continue
            queue = self._queue_for(device_id)
            devices.append(
                {
                    "device_id": device_id,
                    "last_seen_seconds_ago": round(now - seen, 1),
                    "online": now - seen <= DEVICE_ONLINE_TTL_SECONDS,
                    "pending_commands": queue.qsize(),
                    "last_ack": last_ack_snapshot.get(device_id),
                }
            )
        self._send_json(
            {
                "type": "devices",
                "default_device_id": first_connected_device_id(
                    last_seen_snapshot, known_device_ids
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
        ttl_raw = first_value(query, "ttl_seconds") or posted.get("ttl_seconds")
        ttl_seconds = float(ttl_raw) if ttl_raw not in (None, "") else None
        discardable_raw = first_value(query, "discardable")
        discardable = None
        if discardable_raw:
            discardable = parse_bool(discardable_raw)
        elif "discardable" in posted:
            posted_discardable = posted.get("discardable")
            discardable = (
                parse_bool(posted_discardable) if isinstance(posted_discardable, str) else bool(posted_discardable)
            )
        coalesce_key = first_value(query, "coalesce_key") or str(posted.get("coalesce_key") or "")

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
        command = make_command(
            command_wire_type,
            payload,
            priority=priority,
            interrupt=interrupt,
            ttl_seconds=ttl_seconds,
            discardable=discardable,
            coalesce_key=coalesce_key,
        )
        queued = self._enqueue_command(device_id, command)
        self._send_json({"type": "queued" if queued else "dropped", "device_id": device_id, "command": command})

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
        queued = self._enqueue_command(device_id, command)
        self._send_json(
            {
                "type": "queued" if queued else "dropped",
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
        self._expire_dialog_if_needed(device_id)
        queue = self._queue_for(device_id)
        try:
            command = queue.get(timeout=timeout)
            self._send_json({"type": "command", "device_id": device_id, "command": command})
        except Empty:
            if self._expire_dialog_if_needed(device_id):
                try:
                    command = queue.get_nowait()
                    self._send_json({"type": "command", "device_id": device_id, "command": command})
                    return
                except Empty:
                    pass
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
        with self.server.device_lock:
            self.server.last_ack[device_id] = ack
        self._mark_device_seen(device_id)
        self._send_json({"type": "ack", "device_id": device_id, "ack": ack})

    def _device_id(self, query: dict) -> str:
        device_id = first_value(query, "device_id") or self.headers.get("X-Device-Id", "") or "default"
        return safe_device_id(device_id)

    def _resolve_command_device_id(self, requested_device_id: str) -> str:
        device_id = safe_device_id(requested_device_id)
        if is_placeholder_device_id(device_id):
            with self.server.device_lock:
                first_connected = first_connected_device_id(self.server.last_seen, self.server.device_order)
            if first_connected:
                return first_connected
        return device_id

    def _queue_for(self, device_id: str) -> DeviceCommandQueue:
        with self.server.device_lock:
            queue = self.server.device_queues.get(device_id)
            if queue is None:
                queue = DeviceCommandQueue(self.server.command_queue_max_size)
                self.server.device_queues[device_id] = queue
            return queue

    def _enqueue_command(self, device_id: str, command: dict) -> bool:
        device_id = safe_device_id(device_id)
        queue = self._queue_for(device_id)
        stats = queue.put(command)
        detail = ""
        if command.get("type") == "face" and isinstance(command.get("payload"), dict):
            detail = f" expression={command['payload'].get('expression', '')}"
        if stats.get("queued"):
            self._log_info(f"Command queued: {command['type']}{detail} priority={command.get('priority')}")
        else:
            self._log_info(f"Command dropped: {command['type']}{detail} priority={command.get('priority')}")
        self._log_debug(
            f"Command queue detail: device={device_id} cmd_id={command['cmd_id']} "
            f"type={command['type']}{detail} stats={stats}"
        )
        return bool(stats.get("queued"))

    def _mark_device_seen(self, device_id: str) -> None:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            if device_id not in self.server.device_order:
                self.server.device_order.append(device_id)
            self.server.last_seen[device_id] = time.time()

    def _dialog_awake(self, device_id: str) -> bool:
        device_id = safe_device_id(device_id)
        if self._expire_dialog_if_needed(device_id):
            return False
        with self.server.device_lock:
            return time.time() < self.server.dialog_awake_until.get(device_id, 0)

    def _expire_dialog_if_needed(self, device_id: str) -> bool:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            awake_until = self.server.dialog_awake_until.get(device_id, 0)
        if awake_until > 0 and time.time() >= awake_until:
            self._sleep_dialog(device_id, reason="timeout")
            return True
        return False

    def _wake_dialog(self, device_id: str, reason: str = "") -> None:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            self.server.dialog_awake_until[device_id] = time.time() + DIALOG_AWAKE_SECONDS
        self._log_info("Dialog awake")
        self._log_debug(f"Dialog awake detail: device={device_id} ttl={DIALOG_AWAKE_SECONDS}s reason={reason!r}")

    def _sleep_dialog(self, device_id: str, reason: str = "") -> None:
        device_id = safe_device_id(device_id)
        with self.server.device_lock:
            self.server.dialog_awake_until[device_id] = 0
        self._enqueue_command(device_id, make_command("face", {"expression": "calm"}, priority=1, interrupt=True))
        self._log_info("Dialog sleep")
        self._log_debug(f"Dialog sleep detail: device={device_id} reason={reason!r}")

    def _openclaw_enabled(self) -> bool:
        return bool(self.server.openclaw_base_url and self.server.openclaw_token)

    def _send_openclaw_event(self, device_id: str, event_type: str, details: dict) -> dict:
        if not self._openclaw_enabled():
            return {"openclaw_enabled": False, "openclaw_sent": False, "queued_commands": []}

        def run_event() -> None:
            try:
                self._call_openclaw(device_id, event_type, details)
            except Exception as exc:
                self._log_error(f"OpenClaw event failed: {exc}")
                self._log_debug(f"OpenClaw event failed detail: device={device_id} event={event_type} error={exc}")

        self.server.openclaw_executor.submit(run_event)
        self._log_info(f"OpenClaw event submitted: {event_type}")
        return {
            "openclaw_enabled": True,
            "openclaw_sent": True,
            "openclaw_async": True,
            "queued_commands": [],
        }

    def _call_openclaw(self, device_id: str, event_type: str, details: dict) -> None:
        event_text = build_openclaw_event_text(event_type, details)
        url = self.server.openclaw_base_url.rstrip("/") + "/chat/completions"
        payload = {
            "model": self.server.openclaw_model,
            "messages": [
                {"role": "system", "content": XIAOPAI_OPENCLAW_SYSTEM_PROMPT},
                {"role": "user", "content": event_text},
            ],
            "max_completion_tokens": self.server.openclaw_max_completion_tokens,
        }
        headers = {
            "Authorization": f"Bearer {self.server.openclaw_token}",
            "Content-Type": "application/json",
            "x-openclaw-session-key": f"{self.server.openclaw_session_prefix}-{safe_device_id(device_id)}",
        }
        if self.server.openclaw_backend_model:
            headers["x-openclaw-model"] = self.server.openclaw_backend_model

        req = urllib.request.Request(
            url,
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            method="POST",
            headers=headers,
        )
        try:
            with urllib.request.urlopen(req, timeout=self.server.openclaw_timeout) as resp:
                status = getattr(resp, "status", HTTPStatus.OK)
                response_text = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"OpenClaw HTTP {exc.code}: {detail}") from exc

        self._log_info(f"OpenClaw response: HTTP {status}")
        self._log_debug(
            "OpenClaw response detail: "
            f"device={device_id} event={event_type} status={status} "
            f"body={truncate_log_text(response_text)!r}"
        )

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
        meta_path = os.path.join(cache_dir, f"{name}.txt")
        text = HEAD_TOUCH_EVENT_TEXT[name]
        cached_text = ""
        if os.path.exists(meta_path):
            try:
                cached_text = read_binary_file(meta_path).decode("utf-8")
            except UnicodeDecodeError:
                cached_text = ""
        if cached_text != text:
            for stale_path in (pcm_path, wav_path):
                try:
                    os.remove(stale_path)
                except FileNotFoundError:
                    pass
            if cached_text:
                self._log_info(f"Event audio text changed: {name} {cached_text!r} -> {text!r}")
        if not os.path.exists(pcm_path) or os.path.getsize(pcm_path) == 0:
            self._log_info(f"Event audio cache miss: {name} -> {text!r}")
            try:
                audio = aliyun_tts_pcm_with_retries_for_server(self.server, text)
            except Exception as exc:
                self._log_error(f"Event audio TTS failed: {exc}")
                self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
                return
            tmp_path = f"{pcm_path}.tmp"
            with open(tmp_path, "wb") as fp:
                fp.write(audio)
            os.replace(tmp_path, pcm_path)
            tmp_meta_path = f"{meta_path}.tmp"
            with open(tmp_meta_path, "w", encoding="utf-8") as fp:
                fp.write(text)
            os.replace(tmp_meta_path, meta_path)
            self._log_info(f"Event audio cached: {pcm_path} bytes={len(audio)}")
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
            self._log_info(f"Event audio client disconnected: {name}")

    def _handle_upload_image(self, body: bytes):
        if not body:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing image body")
            return

        content_type = self.headers.get("Content-Type", "application/octet-stream")
        width = int(self.headers.get("X-Image-Width", "0") or "0")
        height = int(self.headers.get("X-Image-Height", "0") or "0")
        image_format = self.headers.get("X-Image-Format", "").strip().lower()
        device_id = self.headers.get("X-Device-Id", "unknown")
        device_id = safe_device_id(device_id)
        visual_tracking_requested = parse_bool(self.headers.get("X-Visual-Tracking", "true"))
        self._mark_device_seen(device_id)
        safe_device = re.sub(r"[^A-Za-z0-9_.-]+", "_", device_id)[:40] or "unknown"

        os.makedirs(self.server.capture_dir, exist_ok=True)
        stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        base = os.path.join(self.server.capture_dir, f"xiaopai-{safe_device}-{stamp}")
        save_mode = getattr(self.server, "capture_save_mode", "raw")
        save_raw = save_mode in ("raw", "debug") or self.server.face_detector_backend == "legacy"
        save_debug = save_mode == "debug" or self.server.face_detector_backend == "legacy"
        save_visual = save_mode == "debug"

        raw_ext = "jpg" if content_type.startswith("image/jpeg") else (image_format or "bin")
        raw_path = f"{base}.{raw_ext}" if save_raw else ""
        if raw_path:
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
            if save_debug:
                bmp_path = f"{base}.bmp"
                with open(bmp_path, "wb") as fp:
                    fp.write(rgb565_to_bmp(body, width, height))
                png_path = f"{base}.png"
                with open(png_path, "wb") as fp:
                    fp.write(rgb565_to_png(body, width, height))
            if self.server.face_detector is not None:
                face_visual_path, face_result = self.server.face_detector.detect_rgb565(
                    body,
                    width,
                    height,
                    f"{base}.faces.jpg" if save_visual else "",
                )
            elif self.server.face_detector_backend == "legacy":
                if not png_path:
                    png_path = f"{base}.png"
                    with open(png_path, "wb") as fp:
                        fp.write(rgb565_to_png(body, width, height))
                face_visual_path, face_result = detect_and_visualize_faces(png_path, f"{base}.faces.png")
        elif content_type.startswith("image/jpeg"):
            if self.server.face_detector is not None:
                face_visual_path, face_result = self.server.face_detector.detect_jpeg(
                    body,
                    f"{base}.faces.jpg" if save_visual else "",
                )
            elif self.server.face_detector_backend == "legacy":
                face_visual_path, face_result = detect_and_visualize_faces(raw_path, f"{base}.faces.png")

        if visual_tracking_requested:
            tracking_command = self._maybe_enqueue_visual_tracking(device_id, width, height, face_result)
        else:
            tracking_command = {"status": "suppressed"}
        self._log_info(
            "Image upload processed: "
            f"{width}x{height}, faces={len(face_result.get('faces', []))}, "
            f"tracking={tracking_command.get('status', 'none')}"
        )
        self._log_debug(
            f"Image upload detail: bytes={len(body)} type={content_type} format={image_format} "
            f"size={width}x{height} raw={raw_path} bmp={bmp_path} png={png_path} "
            f"face_visual={face_visual_path} tracking={compact_log_json(tracking_command)}"
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
                "visual_tracking": tracking_command,
            }
        )

    def _maybe_enqueue_visual_tracking(self, device_id: str, width: int, height: int, face_result: dict) -> dict:
        if not self.server.visual_tracking_enabled:
            return {"status": "disabled"}
        if not face_result.get("available"):
            return {"status": "no_detector", "message": face_result.get("error", "")}
        best_face = face_result.get("best_face")
        if not isinstance(best_face, dict):
            return {"status": "no_face"}

        detect_width = float(face_result.get("width") or width or 0)
        detect_height = float(face_result.get("height") or height or 0)
        if detect_width <= 0 or detect_height <= 0:
            return {"status": "bad_frame_size"}
        center = best_face.get("center") if isinstance(best_face.get("center"), dict) else {}
        face_x = float(center.get("x", detect_width / 2.0))
        face_y = float(center.get("y", detect_height / 2.0))
        error_x = face_x - detect_width / 2.0
        error_y = face_y - detect_height / 2.0
        if self.server.visual_tracking_invert_x:
            error_x = -error_x
        if self.server.visual_tracking_invert_y:
            error_y = -error_y
        deadzone = float(self.server.visual_tracking_deadzone_px)

        steps = []
        if abs(error_x) > deadzone:
            steps.append(
                {
                    "type": "motion",
                    "direction": "right" if error_x > 0 else "left",
                    "degree": self._visual_tracking_degree(
                        abs(error_x), detect_width / 2.0, self.server.visual_tracking_gain_x
                    ),
                    "duration_ms": self.server.visual_tracking_duration_ms,
                }
            )
        if abs(error_y) > deadzone:
            steps.append(
                {
                    "type": "motion",
                    "direction": "down" if error_y > 0 else "up",
                    "degree": self._visual_tracking_degree(
                        abs(error_y), detect_height / 2.0, self.server.visual_tracking_gain_y
                    ),
                    "duration_ms": self.server.visual_tracking_duration_ms,
                }
            )
        if not steps:
            return {
                "status": "centered",
                "target": {"x": face_x, "y": face_y},
                "error": {"x": error_x, "y": error_y},
            }

        queue = self._queue_for(device_id)
        if queue.qsize() >= self.server.visual_tracking_max_pending:
            return {
                "status": "skipped_queue_full",
                "pending_commands": queue.qsize(),
                "max_pending": self.server.visual_tracking_max_pending,
            }

        now = time.time()
        last_command_at = self.server.visual_tracking_last_command_at.get(device_id, 0.0)
        min_interval = self.server.visual_tracking_min_interval_ms / 1000.0
        if now - last_command_at < min_interval:
            return {
                "status": "skipped_rate_limited",
                "elapsed_ms": round((now - last_command_at) * 1000.0, 1),
                "min_interval_ms": self.server.visual_tracking_min_interval_ms,
            }

        if len(steps) == 1:
            payload = steps[0]
            payload = {key: value for key, value in payload.items() if key != "type"}
            command = make_command("motion", payload, priority=0, interrupt=False)
        else:
            command = make_command("sequence", steps, priority=0, interrupt=False)
        self._enqueue_command(device_id, command)
        self.server.visual_tracking_last_command_at[device_id] = now
        return {
            "status": "queued",
            "device_id": device_id,
            "cmd_id": command["cmd_id"],
            "command": command,
            "target": {"x": face_x, "y": face_y},
            "error": {"x": error_x, "y": error_y},
        }

    def _visual_tracking_degree(self, abs_error_px: float, half_dimension_px: float, gain: float) -> float:
        if half_dimension_px <= 0:
            return self.server.visual_tracking_min_degree
        ratio = min(1.0, abs_error_px / half_dimension_px)
        degree = ratio * self.server.visual_tracking_max_degree * gain
        degree = max(self.server.visual_tracking_min_degree, min(self.server.visual_tracking_max_degree, degree))
        return round(degree, 1)

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
            self._log_info(f"TTS prepare first sentence: {parts[0]!r}")
            first_audio = self._aliyun_tts_pcm_with_retries(parts[0])
        except Exception as exc:
            self._log_error(f"TTS failed before stream started: {exc}")
            self._send_json({"type": "error", "message": str(exc)}, HTTPStatus.BAD_GATEWAY)
            return

        first_ready_ms = (time.perf_counter() - stream_started) * 1000
        tail_silence = self._tts_tail_silence()
        self._log_info(f"TTS stream: {len(parts)} sentence(s), first_ready_ms={first_ready_ms:.0f}, text={text!r}")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("X-Audio-Format", "pcm_s16le")
        self.send_header("X-Sample-Rate", str(self.server.sample_rate))
        self.send_header("X-Channels", "1")
        if len(parts) == 1:
            self.send_header("Content-Length", str(len(first_audio) + len(tail_silence)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        try:
            self._log_info(f"TTS audio bytes: {len(first_audio)} for {parts[0]!r}")
            if first_audio:
                self.wfile.write(first_audio)
                self.wfile.flush()
            sent_bytes = len(first_audio)

            remaining = parts[1:]
            if not remaining:
                if tail_silence:
                    self.wfile.write(tail_silence)
                    self.wfile.flush()
                    sent_bytes += len(tail_silence)
                total_ms = (time.perf_counter() - stream_started) * 1000
                self._log_info(f"TTS stream done: bytes={sent_bytes} total_ms={total_ms:.0f}")
                return

            workers = max(1, min(self.server.tts_prefetch_workers, len(remaining)))
            with ThreadPoolExecutor(max_workers=workers) as pool:
                futures = [pool.submit(self._aliyun_tts_pcm_with_retries, part) for part in remaining]
                future_timeout = self.server.tts_request_timeout * (self.server.tts_retries + 1) + 5
                for part, future in zip(remaining, futures):
                    wait_started = time.perf_counter()
                    self._log_info(f"TTS sentence ready wait: {part!r}")
                    audio = future.result(timeout=future_timeout)
                    wait_ms = (time.perf_counter() - wait_started) * 1000
                    self._log_info(f"TTS audio bytes: {len(audio)} wait_ms={wait_ms:.0f} for {part!r}")
                    if audio:
                        self.wfile.write(audio)
                        self.wfile.flush()
                        sent_bytes += len(audio)
            if tail_silence:
                self.wfile.write(tail_silence)
                self.wfile.flush()
                sent_bytes += len(tail_silence)
            total_ms = (time.perf_counter() - stream_started) * 1000
            self._log_info(f"TTS stream done: bytes={sent_bytes} total_ms={total_ms:.0f}")
        except (BrokenPipeError, ConnectionResetError):
            self._log_info("TTS client disconnected")
        except TimeoutError:
            self._log_error("TTS failed after stream started: sentence timed out")
        except Exception as exc:
            self._log_error(f"TTS failed after stream started: {exc}")

    def _aliyun_tts_pcm_with_retries(self, text: str) -> bytes:
        last_error: Exception | None = None
        for attempt in range(1, self.server.tts_retries + 2):
            try:
                return self._aliyun_tts_pcm(text)
            except Exception as exc:
                last_error = exc
                self._log_error(f"TTS attempt {attempt} failed for {text!r}: {exc}")
        raise RuntimeError(f"Aliyun TTS failed after {self.server.tts_retries + 1} attempt(s): {last_error}")

    def _tts_tail_silence(self) -> bytes:
        ms = max(0, int(self.server.tts_tail_silence_ms))
        samples = self.server.sample_rate * ms // 1000
        return b"\x00\x00" * samples

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
                self._log_info(f"Aliyun TTS ok: chars={len(text)} bytes={len(audio)} elapsed_ms={elapsed_ms:.0f}")
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
        self._log_api_result(body, status)

    def _log_api_result(self, body: dict, status: HTTPStatus) -> None:
        path, _query = self._path_query()
        if not (
            path == "/command"
            or path.startswith("/command/")
            or path.startswith("/action/")
            or path.startswith("/expression/")
            or path in ("/device/event", "/event", "/device/ack")
        ):
            return
        if self._debug_enabled():
            self._log_debug(
                "API result detail: "
                f"method={self.command} path={self.path!r} status={int(status)} "
                f"body={compact_log_json(body)}"
            )
            return
        if path in ("/device/ack",):
            ack = body.get("ack") if isinstance(body.get("ack"), dict) else {}
            self._log_info(f"API ack: {ack.get('status', 'unknown')}")
            return
        if path == "/command" or path.startswith("/command/"):
            command = body.get("command") if isinstance(body.get("command"), dict) else {}
            self._log_info(f"API command response: {body.get('type', 'response')} {command.get('type', '')}".rstrip())
            return
        self._log_info(f"API response: {path} -> {int(status)}")

    def log_message(self, fmt, *args):
        if self._debug_enabled():
            self._log_debug(f"{self.client_address[0]} - {fmt % args}")
            return
        parsed = urllib.parse.urlparse(getattr(self, "path", ""))
        code = args[1] if len(args) > 1 else ""
        suffix = f" -> {code}" if code else ""
        self._log_info(f"HTTP {getattr(self, 'command', '')} {parsed.path}{suffix}")


def required_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Missing {name}. Export it before starting this service.")
    return value


def truncate_log_text(value: str, limit: int = LOG_TEXT_MAX_CHARS) -> str:
    text = str(value or "")
    if len(text) <= limit:
        return text
    return text[:limit] + f"...<truncated {len(text) - limit} chars>"


def compact_log_json(value) -> str:
    try:
        text = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    except (TypeError, ValueError):
        text = repr(value)
    return truncate_log_text(text)


def build_openclaw_event_text(event_type: str, details: dict) -> str:
    event_type = str(event_type or "event").strip() or "event"
    compact_details = {str(key): value for key, value in details.items() if value not in (None, "")}
    if event_type == "speech_recognition":
        text = str(compact_details.get("text") or "").strip()
        return (
            "小派同学收到用户语音交互识别结果。\n"
            f"事件类型：speech_recognition\n"
            f"用户文本：{text}"
        )
    if event_type in ("head_touch", "touch"):
        name = str(compact_details.get("name") or compact_details.get("event") or "").strip()
        return (
            "小派同学收到头部触摸事件。\n"
            f"事件类型：head_touch\n"
            f"触摸名称：{name}\n"
            f"事件详情：{json.dumps(compact_details, ensure_ascii=False)}"
        )
    return (
        "小派同学收到设备事件。\n"
        f"事件类型：{event_type}\n"
        f"事件详情：{json.dumps(compact_details, ensure_ascii=False)}"
    )


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


def aliyun_tts_pcm_for_server(server: AliyunVoiceServer, text: str) -> bytes:
    started = time.perf_counter()
    params = {
        "appkey": server.appkey,
        "token": server.get_token(),
        "text": text,
        "format": "pcm",
        "sample_rate": server.sample_rate,
        "voice": server.voice,
        "volume": server.volume,
        "speech_rate": server.speech_rate,
        "pitch_rate": server.pitch_rate,
    }
    url = server.tts_url + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=server.tts_request_timeout) as resp:
            content_type = resp.headers.get("Content-Type", "")
            if "json" in content_type:
                raise RuntimeError(resp.read().decode("utf-8", errors="replace"))
            audio = resp.read()
            elapsed_ms = (time.perf_counter() - started) * 1000
            log_print(f"Aliyun TTS ok: chars={len(text)} bytes={len(audio)} elapsed_ms={elapsed_ms:.0f}")
            return audio
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Aliyun TTS HTTP {exc.code}: {detail}") from exc


def aliyun_tts_pcm_with_retries_for_server(server: AliyunVoiceServer, text: str) -> bytes:
    last_error: Exception | None = None
    for attempt in range(1, server.tts_retries + 2):
        try:
            return aliyun_tts_pcm_for_server(server, text)
        except Exception as exc:
            last_error = exc
            log_print(f"TTS attempt {attempt} failed for {text!r}: {exc}", file=sys.stderr)
    raise RuntimeError(f"Aliyun TTS failed after {server.tts_retries + 1} attempt(s): {last_error}")


def ensure_event_audio_cache(server: AliyunVoiceServer, name: str) -> None:
    if name not in HEAD_TOUCH_EVENT_TEXT:
        raise ValueError(f"unknown event audio: {name}")
    cache_dir = os.path.join(server.static_dir, "event-audio")
    os.makedirs(cache_dir, exist_ok=True)
    pcm_path = os.path.join(cache_dir, f"{name}.pcm")
    wav_path = os.path.join(cache_dir, f"{name}.wav")
    meta_path = os.path.join(cache_dir, f"{name}.txt")
    text = HEAD_TOUCH_EVENT_TEXT[name]
    cached_text = ""
    if os.path.exists(meta_path):
        try:
            cached_text = read_binary_file(meta_path).decode("utf-8")
        except UnicodeDecodeError:
            cached_text = ""
    if cached_text != text:
        for stale_path in (pcm_path, wav_path):
            try:
                os.remove(stale_path)
            except FileNotFoundError:
                pass
    if not os.path.exists(pcm_path) or os.path.getsize(pcm_path) == 0:
        audio = aliyun_tts_pcm_with_retries_for_server(server, text)
        tmp_path = f"{pcm_path}.tmp"
        with open(tmp_path, "wb") as fp:
            fp.write(audio)
        os.replace(tmp_path, pcm_path)
        tmp_meta_path = f"{meta_path}.tmp"
        with open(tmp_meta_path, "w", encoding="utf-8") as fp:
            fp.write(text)
        os.replace(tmp_meta_path, meta_path)
        log_print(f"Event audio cached: {pcm_path} bytes={len(audio)}")
    if not os.path.exists(wav_path) or os.path.getsize(wav_path) == 0:
        pcm = read_binary_file(pcm_path)
        tmp_path = f"{wav_path}.tmp"
        with open(tmp_path, "wb") as fp:
            fp.write(pcm_to_wav(pcm, server.sample_rate))
        os.replace(tmp_path, wav_path)


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


def make_command(
    command_type: str,
    payload,
    priority: int = 0,
    interrupt: bool = False,
    ttl_seconds: float | None = None,
    discardable: bool | None = None,
    coalesce_key: str = "",
) -> dict:
    normalized_type = str(command_type or "")
    effective_priority = max(int(priority or 0), command_default_priority(normalized_type))
    if ttl_seconds is None:
        ttl_seconds = command_default_ttl(normalized_type)
    if discardable is None:
        discardable = normalized_type in COMMAND_DISCARDABLE_TYPES
    if not coalesce_key and normalized_type in COMMAND_COALESCE_BY_TYPE:
        coalesce_key = normalized_type
    return {
        "cmd_id": f"cmd_{uuid.uuid4().hex[:12]}",
        "type": normalized_type,
        "priority": effective_priority,
        "interrupt": bool(interrupt or normalized_type == "stop"),
        "ttl_seconds": ttl_seconds,
        "discardable": bool(discardable),
        "coalesce_key": coalesce_key,
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
    if command_type in ("volume", "sound"):
        direction = first_value(query, "direction") or first_value(query, "action") or first_value(query, "type") or "up"
        mode = first_value(query, "mode") or ""
        value = first_value(query, "value")
        if mode == "set" or value:
            return {
                "mode": "set",
                "value": int(value or "100"),
            }
        return {
            "direction": direction,
            "step": int(first_value(query, "step") or "10"),
        }
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
    if command_type in ("find_owner", "locate_owner"):
        return {
            "rounds": int(first_value(query, "rounds") or "1"),
            "reply": first_value(query, "reply") or "我在",
            "preserve_speech": parse_bool(first_value(query, "preserve_speech") or "false"),
            "wait_for_speech": parse_bool(first_value(query, "wait_for_speech") or "false"),
            "gain_x": float(first_value(query, "gain_x") or "1.0"),
            "gain_y": float(first_value(query, "gain_y") or "0.8"),
            "stop_pixels": float(first_value(query, "stop_pixels") or "32"),
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
    parser.add_argument("--debug", action="store_true", default=parse_bool(os.environ.get("STACKCHAN_DEBUG", "false")))
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
    parser.add_argument("--tts-tail-silence-ms", type=int, default=int(os.environ.get("STACKCHAN_TTS_TAIL_SILENCE_MS", "350")))
    parser.add_argument("--command-queue-max-size", type=int, default=int(os.environ.get("STACKCHAN_COMMAND_QUEUE_MAX_SIZE", str(COMMAND_QUEUE_MAX_SIZE))))
    parser.add_argument("--capture-dir", default=os.environ.get("STACKCHAN_CAPTURE_DIR", "captures"))
    parser.add_argument(
        "--capture-save-mode",
        choices=("none", "raw", "debug"),
        default=os.environ.get("STACKCHAN_CAPTURE_SAVE_MODE", "none"),
        help="Image upload persistence: none, raw, or debug (raw + converted images + face visualizations).",
    )
    parser.add_argument("--static-dir", default=os.environ.get("STACKCHAN_STATIC_DIR", "static"))
    parser.add_argument(
        "--face-detector",
        choices=("yunet", "legacy", "none"),
        default=os.environ.get("STACKCHAN_FACE_DETECTOR", "yunet"),
        help="Face detection backend for /upload-image.",
    )
    parser.add_argument(
        "--yunet-model",
        default=os.environ.get(
            "STACKCHAN_YUNET_MODEL",
            os.path.join(os.path.dirname(os.path.dirname(__file__)), "models", "face_detection_yunet_2023mar.onnx"),
        ),
    )
    parser.add_argument(
        "--yunet-score-threshold",
        type=float,
        default=float(os.environ.get("STACKCHAN_YUNET_SCORE_THRESHOLD", "0.45")),
    )
    parser.add_argument(
        "--yunet-nms-threshold",
        type=float,
        default=float(os.environ.get("STACKCHAN_YUNET_NMS_THRESHOLD", "0.3")),
    )
    parser.add_argument("--yunet-top-k", type=int, default=int(os.environ.get("STACKCHAN_YUNET_TOP_K", "5000")))
    parser.add_argument(
        "--visual-tracking-enabled",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Queue Xiaopai motion commands from /upload-image face detections.",
    )
    parser.add_argument(
        "--visual-tracking-deadzone-px",
        type=float,
        default=20.0,
    )
    parser.add_argument(
        "--visual-tracking-gain-x",
        type=float,
        default=1.0,
        help="Horizontal multiplier for pixel error to head-motion degree conversion.",
    )
    parser.add_argument(
        "--visual-tracking-gain-y",
        type=float,
        default=1.0,
        help="Vertical multiplier for pixel error to head-motion degree conversion.",
    )
    parser.add_argument(
        "--visual-tracking-max-degree",
        type=float,
        default=12.0,
    )
    parser.add_argument(
        "--visual-tracking-min-degree",
        type=float,
        default=2.0,
    )
    parser.add_argument(
        "--visual-tracking-duration-ms",
        type=int,
        default=280,
    )
    parser.add_argument(
        "--visual-tracking-min-interval-ms",
        type=int,
        default=350,
    )
    parser.add_argument(
        "--visual-tracking-max-pending",
        type=int,
        default=2,
    )
    parser.add_argument(
        "--visual-tracking-invert-x",
        action=argparse.BooleanOptionalAction,
        default=False,
    )
    parser.add_argument(
        "--visual-tracking-invert-y",
        action=argparse.BooleanOptionalAction,
        default=False,
    )
    parser.add_argument(
        "--find-owner-gain-x",
        type=float,
        default=1.0,
        help="Horizontal multiplier for wake-up owner finding.",
    )
    parser.add_argument(
        "--find-owner-gain-y",
        type=float,
        default=0.8,
        help="Vertical multiplier for wake-up owner finding.",
    )
    parser.add_argument(
        "--find-owner-stop-pixels",
        type=float,
        default=32.0,
        help="Stop moving when the detected face center is within this many pixels of frame center.",
    )
    parser.add_argument("--openclaw-base-url", default=optional_env("STACKCHAN_OPENCLAW_BASE_URL", "OPENCLAW_BASE_URL"))
    parser.add_argument("--openclaw-token", default=optional_env("STACKCHAN_OPENCLAW_GATEWAY_TOKEN", "OPENCLAW_GATEWAY_TOKEN"))
    parser.add_argument("--openclaw-model", default=os.environ.get("STACKCHAN_OPENCLAW_MODEL", "openclaw/default"))
    parser.add_argument("--openclaw-backend-model", default=os.environ.get("STACKCHAN_OPENCLAW_BACKEND_MODEL", ""))
    parser.add_argument("--openclaw-timeout", type=int, default=int(os.environ.get("STACKCHAN_OPENCLAW_TIMEOUT", "45")))
    parser.add_argument("--openclaw-workers", type=int, default=int(os.environ.get("STACKCHAN_OPENCLAW_WORKERS", "4")))
    parser.add_argument(
        "--openclaw-max-completion-tokens",
        type=int,
        default=int(os.environ.get("STACKCHAN_OPENCLAW_MAX_COMPLETION_TOKENS", "512")),
    )
    parser.add_argument("--openclaw-session-prefix", default=os.environ.get("STACKCHAN_OPENCLAW_SESSION_PREFIX", "xiaopai"))
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
    httpd.tts_tail_silence_ms = args.tts_tail_silence_ms
    httpd.command_queue_max_size = args.command_queue_max_size
    httpd.capture_save_mode = args.capture_save_mode
    httpd.debug_log = args.debug
    httpd.capture_dir = args.capture_dir
    httpd.static_dir = args.static_dir
    httpd.face_detector_backend = args.face_detector
    httpd.face_detector = None
    if args.face_detector == "yunet":
        httpd.face_detector = YunetFaceService(
            args.yunet_model,
            score_threshold=args.yunet_score_threshold,
            nms_threshold=args.yunet_nms_threshold,
            top_k=args.yunet_top_k,
        )
    httpd.visual_tracking_enabled = args.visual_tracking_enabled
    httpd.visual_tracking_deadzone_px = args.visual_tracking_deadzone_px
    httpd.visual_tracking_gain_x = args.visual_tracking_gain_x
    httpd.visual_tracking_gain_y = args.visual_tracking_gain_y
    httpd.visual_tracking_max_degree = args.visual_tracking_max_degree
    httpd.visual_tracking_min_degree = args.visual_tracking_min_degree
    httpd.visual_tracking_duration_ms = args.visual_tracking_duration_ms
    httpd.visual_tracking_min_interval_ms = args.visual_tracking_min_interval_ms
    httpd.visual_tracking_max_pending = args.visual_tracking_max_pending
    httpd.visual_tracking_invert_x = args.visual_tracking_invert_x
    httpd.visual_tracking_invert_y = args.visual_tracking_invert_y
    httpd.visual_tracking_last_command_at = {}
    httpd.find_owner_gain_x = args.find_owner_gain_x
    httpd.find_owner_gain_y = args.find_owner_gain_y
    httpd.find_owner_stop_pixels = args.find_owner_stop_pixels
    httpd.openclaw_base_url = args.openclaw_base_url
    httpd.openclaw_token = args.openclaw_token
    httpd.openclaw_model = args.openclaw_model
    httpd.openclaw_backend_model = args.openclaw_backend_model
    httpd.openclaw_timeout = args.openclaw_timeout
    httpd.openclaw_max_completion_tokens = args.openclaw_max_completion_tokens
    httpd.openclaw_session_prefix = args.openclaw_session_prefix
    httpd.openclaw_executor = ThreadPoolExecutor(max_workers=max(1, args.openclaw_workers), thread_name_prefix="openclaw")
    httpd.device_lock = threading.Lock()
    httpd.device_queues = {}
    httpd.last_ack = {}
    httpd.last_seen = {}
    httpd.device_order = []
    httpd.dialog_awake_until = {}

    for wake_reply_name, _ in WAKE_REPLY_EVENTS:
        try:
            ensure_event_audio_cache(httpd, wake_reply_name)
        except Exception as exc:
            log_print(f"Wake reply audio pre-cache failed for {wake_reply_name}: {exc}", file=sys.stderr)

    log_print("Xiaopai server ready")
    log_print(f"  face detector: {args.face_detector}")
    log_print(f"  capture save mode: {args.capture_save_mode}")
    log_print(f"  visual tracking: {'enabled' if args.visual_tracking_enabled else 'disabled'}")
    log_print(f"  command queue: max_size={args.command_queue_max_size}")
    log_print(f"  OpenClaw: {'enabled' if httpd.openclaw_base_url and httpd.openclaw_token else 'disabled'}")
    if args.debug:
        log_print("  debug: enabled")
        log_print(f"  health: http://127.0.0.1:{args.port}/health")
        log_print(f"  ASR:    http://{args.host}:{args.port}/upload")
        log_print(f"  TTS:    http://{args.host}:{args.port}/stream-speak?text=...")
        log_print(f"  Events: http://{args.host}:{args.port}/head-touch-events -> {args.static_dir}/event-audio")
        log_print(f"  Image:  http://{args.host}:{args.port}/upload-image -> {args.capture_dir}")
        log_print(f"  Face detector detail: {args.face_detector}{' ' + args.yunet_model if args.face_detector == 'yunet' else ''}")
        log_print(
            f"  Visual tracking detail: deadzone={args.visual_tracking_deadzone_px}px "
            f"gain_x={args.visual_tracking_gain_x} gain_y={args.visual_tracking_gain_y} "
            f"max_step={args.visual_tracking_max_degree}deg "
            f"invert_x={args.visual_tracking_invert_x} invert_y={args.visual_tracking_invert_y}"
        )
        log_print(
            f"  Find-owner detail: gain_x={args.find_owner_gain_x} gain_y={args.find_owner_gain_y} "
            f"stop={args.find_owner_stop_pixels}px"
        )
        log_print(f"  OpenClaw detail: {httpd.openclaw_base_url or ''}")
        log_print(f"  OpenClaw workers: {args.openclaw_workers}")
        log_print(f"  TTS tail silence: {args.tts_tail_silence_ms}ms")
        log_print(f"  Command push via HTTP long poll:")
        log_print(f"          device: GET http://{args.host}:{args.port}/device/next-command?device_id=...")
        log_print(f"          send:   GET http://{args.host}:{args.port}/command/speak?device_id=...&text=...")
        log_print(f"  voice:  {args.voice}, pcm_s16le {args.sample_rate}Hz mono")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
