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
    device_queues: dict[str, Queue]
    last_ack: dict[str, dict]
    last_seen: dict[str, float]

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
    server_version = "StackChanAliyunVoice/1.0"

    def do_GET(self):
        path, query = self._path_query()
        if path in ("/", "/health"):
            self._send_json(
                {
                    "ok": True,
                    "service": "stackchan-aliyun-voice",
                    "asr": "/upload",
                    "tts": "/stream-speak?text=...",
                    "image": "/upload-image",
                    "tts_format": "pcm_s16le",
                    "sample_rate": self.server.sample_rate,
                    "channels": 1,
                    "voice": self.server.voice,
                }
            )
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
            reply = f"我听到你说：{text}"
            command = make_command(
                "sequence",
                [
                    {"type": "face", "expression": "thinking"},
                    {"type": "speak", "text": reply},
                    {"type": "face", "expression": "happy"},
                ],
            )
            self._enqueue_command(device_id, command)
            response["queued_command"] = command["cmd_id"]
        self._send_json(response)

    def _handle_devices(self):
        now = time.time()
        devices = []
        for device_id, seen in sorted(self.server.last_seen.items()):
            queue = self._queue_for(device_id)
            devices.append(
                {
                    "device_id": device_id,
                    "last_seen_seconds_ago": round(now - seen, 1),
                    "pending_commands": queue.qsize(),
                    "last_ack": self.server.last_ack.get(device_id),
                }
            )
        self._send_json({"type": "devices", "devices": devices})

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

        command_wire_type = "motion" if command_type == "move" else command_type
        command = make_command(command_wire_type, payload, priority=priority, interrupt=interrupt)
        self._enqueue_command(device_id, command)
        self._send_json({"type": "queued", "device_id": device_id, "command": command})

    def _handle_next_command(self, query: dict):
        device_id = self._device_id(query)
        timeout = float(first_value(query, "timeout") or "25")
        timeout = max(0.0, min(timeout, 55.0))
        self.server.last_seen[device_id] = time.time()
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
        self.server.last_seen[device_id] = time.time()
        self._send_json({"type": "ack", "device_id": device_id, "ack": ack})

    def _device_id(self, query: dict) -> str:
        device_id = first_value(query, "device_id") or self.headers.get("X-Device-Id", "") or "default"
        return safe_device_id(device_id)

    def _resolve_command_device_id(self, requested_device_id: str) -> str:
        device_id = safe_device_id(requested_device_id)
        if is_placeholder_device_id(device_id):
            latest = latest_seen_device_id(self.server.last_seen)
            if latest:
                return latest
        return device_id

    def _queue_for(self, device_id: str) -> Queue:
        queue = self.server.device_queues.get(device_id)
        if queue is None:
            queue = Queue()
            self.server.device_queues[device_id] = queue
        return queue

    def _enqueue_command(self, device_id: str, command: dict) -> None:
        device_id = safe_device_id(device_id)
        self.server.last_seen.setdefault(device_id, time.time())
        self._queue_for(device_id).put(command)
        print(f"Command queued: device={device_id} cmd_id={command['cmd_id']} type={command['type']}", flush=True)

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
        base = os.path.join(self.server.capture_dir, f"stackchan-{safe_device}-{stamp}")

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
        self.end_headers()

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


def latest_seen_device_id(last_seen: dict[str, float]) -> str:
    if not last_seen:
        return "default"
    return max(last_seen.items(), key=lambda item: item[1])[0]


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
    if command_type == "face":
        return {"expression": first_value(query, "expression") or first_value(query, "face") or "happy"}
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
        expression = first_value(query, "expression") or "happy"
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

    parser = argparse.ArgumentParser(description="Local Stack-chan bridge for Aliyun ASR and PCM streaming TTS.")
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
    httpd.device_queues = {}
    httpd.last_ack = {}
    httpd.last_seen = {}

    print("Stack-chan Aliyun voice bridge")
    print(f"  health: http://127.0.0.1:{args.port}/health")
    print(f"  ASR:    http://{args.host}:{args.port}/upload")
    print(f"  TTS:    http://{args.host}:{args.port}/stream-speak?text=...")
    print(f"  Image:  http://{args.host}:{args.port}/upload-image -> {args.capture_dir}")
    print(f"  Command push via HTTP long poll:")
    print(f"          device: GET http://{args.host}:{args.port}/device/next-command?device_id=...")
    print(f"          send:   GET http://{args.host}:{args.port}/command/speak?device_id=...&text=...")
    print(f"  voice:  {args.voice}, pcm_s16le {args.sample_rate}Hz mono")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
