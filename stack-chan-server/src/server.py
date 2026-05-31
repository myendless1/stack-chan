#!/usr/bin/env python3
import argparse
import datetime as _dt
import json
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


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

        sample_rate = detect_wav_sample_rate(body) or self.server.sample_rate
        audio_format = "wav" if detect_wav_sample_rate(body) else "pcm"
        print(f"ASR upload: bytes={len(body)} format={audio_format} sample_rate={sample_rate}")
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

        self._send_json({"type": "stt", "text": text, "task_id": result.get("task_id", "")})

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
                "X-NLS-Token": self.server.token,
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
            "token": self.server.token,
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
    httpd.token = required_env("ALIYUN_NLS_TOKEN")
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

    print("Stack-chan Aliyun voice bridge")
    print(f"  health: http://127.0.0.1:{args.port}/health")
    print(f"  ASR:    http://{args.host}:{args.port}/upload")
    print(f"  TTS:    http://{args.host}:{args.port}/stream-speak?text=...")
    print(f"  Image:  http://{args.host}:{args.port}/upload-image -> {args.capture_dir}")
    print(f"  voice:  {args.voice}, pcm_s16le {args.sample_rate}Hz mono")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
