#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import tempfile
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


class PiperCliBackend:
    def __init__(self, binary: str, model: Path, config: Path | None, length_scale: float):
        self.binary = binary
        self.model = model
        self.config = config
        self.length_scale = length_scale

    def synthesize(self, text: str) -> bytes:
        with tempfile.TemporaryDirectory(prefix="xiaopai-piper-") as tmp:
            out_path = Path(tmp) / "speech.wav"
            cmd = [
                self.binary,
                "--model",
                str(self.model),
                "--output_file",
                str(out_path),
                "--length_scale",
                str(self.length_scale),
            ]
            if self.config:
                cmd += ["--config", str(self.config)]

            subprocess.run(cmd, input=f"{text}\n", text=True, check=True)
            return out_path.read_bytes()


class LocalTTSHandler(BaseHTTPRequestHandler):
    server_version = "XiaopaiLocalTTS/1.0"

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/health"):
            self._send_json(
                {
                    "ok": True,
                    "service": "xiaopai-local-tts",
                    "backend": "piper-cli",
                    "model": str(self.server.backend.model),
                    "speak": "/speak?text=...",
                    "response": "audio/wav",
                }
            )
            return
        if path == "/speak":
            self._handle_speak()
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        path = urlparse(self.path).path
        if path not in ("/speak", "/v1/audio/speech"):
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        text = ""
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type == "application/json" and body:
            payload = json.loads(body.decode("utf-8"))
            text = payload.get("text") or payload.get("input") or ""
        elif body:
            text = body.decode("utf-8")
        if not text:
            text = self._query_text()
        self._synthesize_and_send(text)

    def _handle_speak(self):
        self._synthesize_and_send(self._query_text())

    def _query_text(self) -> str:
        query = parse_qs(urlparse(self.path).query)
        values = query.get("text") or query.get("input")
        return values[0] if values and values[0] else ""

    def _synthesize_and_send(self, text: str):
        text = text.strip()
        if not text:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing text")
            return
        if len(text) > self.server.max_chars:
            self.send_error(HTTPStatus.BAD_REQUEST, "text is too long")
            return

        print(f"synthesizing {len(text)} chars: {text!r}")
        try:
            wav = self.server.backend.synthesize(text)
        except subprocess.CalledProcessError as exc:
            print(f"piper failed: {exc}")
            self.send_error(HTTPStatus.INTERNAL_SERVER_ERROR, "piper failed")
            return
        except Exception as exc:
            print(f"synthesis failed: {exc}")
            self.send_error(HTTPStatus.INTERNAL_SERVER_ERROR, str(exc))
            return

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.end_headers()
        self.wfile.write(wav)

    def _send_json(self, body: dict, status: HTTPStatus = HTTPStatus.OK):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} - {fmt % args}")


def main():
    parser = argparse.ArgumentParser(description="Local Piper TTS WAV API for Xiaopai.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8091)
    parser.add_argument("--piper-binary", default="piper")
    parser.add_argument("--model", required=True, help="Path to a Piper .onnx voice model")
    parser.add_argument("--config", help="Optional path to the matching .onnx.json config")
    parser.add_argument("--length-scale", type=float, default=1.0)
    parser.add_argument("--max-chars", type=int, default=240)
    args = parser.parse_args()

    binary = shutil.which(args.piper_binary) or args.piper_binary
    model = Path(args.model).expanduser().resolve()
    config = Path(args.config).expanduser().resolve() if args.config else None
    if not model.exists():
        raise SystemExit(f"model not found: {model}")
    if config and not config.exists():
        raise SystemExit(f"config not found: {config}")

    httpd = ThreadingHTTPServer((args.host, args.port), LocalTTSHandler)
    httpd.backend = PiperCliBackend(binary, model, config, args.length_scale)
    httpd.max_chars = args.max_chars

    print(f"listening on http://{args.host}:{args.port}/speak?text=...")
    print(f"health check: http://{args.host}:{args.port}/health")
    print(f"using piper binary: {binary}")
    print(f"using model: {model}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
