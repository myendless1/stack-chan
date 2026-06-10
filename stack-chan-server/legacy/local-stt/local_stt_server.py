#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import tempfile
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


class FasterWhisperBackend:
    def __init__(
        self,
        model: str,
        device: str,
        compute_type: str,
        language: str | None,
        no_speech_threshold: float,
        min_avg_logprob: float,
    ):
        self.model_name = model
        self.device = device
        self.compute_type = compute_type
        self.language = language
        self.no_speech_threshold = no_speech_threshold
        self.min_avg_logprob = min_avg_logprob
        self._model = None

    def _load(self):
        if self._model is None:
            from faster_whisper import WhisperModel

            print(
                f"loading faster-whisper model={self.model_name} "
                f"device={self.device} compute_type={self.compute_type}"
            )
            self._model = WhisperModel(
                self.model_name,
                device=self.device,
                compute_type=self.compute_type,
            )
        return self._model

    def transcribe(self, audio_path: Path) -> str:
        model = self._load()
        segments, info = model.transcribe(
            str(audio_path),
            language=self.language,
            vad_filter=True,
            beam_size=5,
            condition_on_previous_text=False,
            no_speech_threshold=self.no_speech_threshold,
            log_prob_threshold=self.min_avg_logprob,
            hallucination_silence_threshold=1.0,
        )
        kept = []
        dropped = []
        for segment in segments:
            avg_logprob = getattr(segment, "avg_logprob", 0.0)
            no_speech_prob = getattr(segment, "no_speech_prob", 0.0)
            if avg_logprob < self.min_avg_logprob or no_speech_prob > self.no_speech_threshold:
                dropped.append(segment)
                continue
            kept.append(segment)
        text = "".join(segment.text for segment in kept).strip()
        print(
            f"transcribed language={info.language} "
            f"probability={info.language_probability:.2f} "
            f"segments={len(kept)} dropped={len(dropped)} text={text!r}"
        )
        return text


class WhisperCppBackend:
    def __init__(self, binary: str, model_path: str, language: str | None):
        self.binary = binary
        self.model_path = model_path
        self.language = language

    def transcribe(self, audio_path: Path) -> str:
        with tempfile.TemporaryDirectory(prefix="xiaopai-whispercpp-") as tmp:
            out_base = Path(tmp) / "result"
            cmd = [
                self.binary,
                "-m",
                self.model_path,
                "-f",
                str(audio_path),
                "-otxt",
                "-of",
                str(out_base),
                "-nt",
            ]
            if self.language:
                cmd += ["-l", self.language]
            subprocess.run(cmd, check=True)
            return out_base.with_suffix(".txt").read_text(encoding="utf-8").strip()


class LocalSTTHandler(BaseHTTPRequestHandler):
    server_version = "XiaopaiLocalSTT/1.0"

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/health"):
            body = {
                "ok": True,
                "service": "xiaopai-local-stt",
                "backend": self.server.backend_name,
                "model": self.server.model_name,
                "upload": "/upload",
                "response_shape": {"session_id": "...", "type": "stt", "text": "..."},
            }
            self._send_json(body)
            return
        self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        path = urlparse(self.path).path
        if path != "/upload":
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            self.send_error(HTTPStatus.BAD_REQUEST, "missing audio body")
            return

        session_id = self._session_id()
        suffix = self._suffix_from_content_type()
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        audio_path = self.server.output_dir / f"recording-{stamp}{suffix}"
        self._save_request_body(audio_path, length)

        mp3_path = self._save_mp3_copy(audio_path)
        print(f"saved {audio_path} ({length} bytes)")
        if mp3_path:
            print(f"saved {mp3_path}")

        try:
            text = self.server.backend.transcribe(audio_path)
        except Exception as exc:
            print(f"transcription failed: {exc}")
            self._send_json(
                {
                    "session_id": session_id,
                    "type": "error",
                    "error": "stt_failed",
                    "message": str(exc),
                },
                status=HTTPStatus.INTERNAL_SERVER_ERROR,
            )
            return

        self._send_json(
            {
                "session_id": session_id,
                "type": "stt",
                "text": text,
            }
        )

    def _session_id(self) -> str:
        query = parse_qs(urlparse(self.path).query)
        values = query.get("session_id") or query.get("session")
        if values and values[0]:
            return values[0]
        header = self.headers.get("X-Session-Id")
        return header or uuid.uuid4().hex

    def _suffix_from_content_type(self) -> str:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        mapping = {
            "audio/wav": ".wav",
            "audio/wave": ".wav",
            "audio/x-wav": ".wav",
            "audio/mpeg": ".mp3",
            "audio/mp3": ".mp3",
            "audio/ogg": ".ogg",
            "audio/opus": ".opus",
            "application/octet-stream": ".wav",
        }
        return mapping.get(content_type, ".wav")

    def _save_request_body(self, path: Path, length: int):
        with path.open("wb") as f:
            remaining = length
            while remaining:
                chunk = self.rfile.read(min(64 * 1024, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)

    def _save_mp3_copy(self, audio_path: Path) -> Path | None:
        if not self.server.save_mp3 or audio_path.suffix.lower() == ".mp3":
            return None
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            return None
        mp3_path = audio_path.with_suffix(".mp3")
        subprocess.run(
            [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(audio_path), str(mp3_path)],
            check=False,
        )
        return mp3_path if mp3_path.exists() else None

    def _send_json(self, body: dict, status: HTTPStatus = HTTPStatus.OK):
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} - {fmt % args}")


def build_backend(args):
    if args.backend == "faster-whisper":
        language = None if args.language == "auto" else args.language
        return FasterWhisperBackend(
            args.model,
            args.device,
            args.compute_type,
            language,
            args.no_speech_threshold,
            args.min_avg_logprob,
        )
    if args.backend == "whisper-cpp":
        if not args.whisper_cpp_model:
            raise SystemExit("--whisper-cpp-model is required for --backend whisper-cpp")
        return WhisperCppBackend(args.whisper_cpp_binary, args.whisper_cpp_model, args.language)
    raise SystemExit(f"unknown backend: {args.backend}")


def main():
    parser = argparse.ArgumentParser(description="Local open-source STT API for Xiaopai.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8091)
    parser.add_argument("--output", default="recordings")
    parser.add_argument("--backend", choices=("faster-whisper", "whisper-cpp"), default="faster-whisper")
    parser.add_argument("--model", default=os.environ.get("STACKCHAN_STT_MODEL", "small"))
    parser.add_argument("--language", default=os.environ.get("STACKCHAN_STT_LANGUAGE", "zh"))
    parser.add_argument("--device", default=os.environ.get("STACKCHAN_STT_DEVICE", "cpu"))
    parser.add_argument("--compute-type", default=os.environ.get("STACKCHAN_STT_COMPUTE_TYPE", "int8"))
    parser.add_argument("--no-speech-threshold", type=float, default=0.8)
    parser.add_argument("--min-avg-logprob", type=float, default=-1.0)
    parser.add_argument("--no-mp3-copy", action="store_true")
    parser.add_argument("--whisper-cpp-binary", default=os.environ.get("WHISPER_CPP_BINARY", "whisper-cli"))
    parser.add_argument("--whisper-cpp-model", default=os.environ.get("WHISPER_CPP_MODEL"))
    args = parser.parse_args()

    output_dir = Path(args.output).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    backend = build_backend(args)
    httpd = ThreadingHTTPServer((args.host, args.port), LocalSTTHandler)
    httpd.output_dir = output_dir
    httpd.backend = backend
    httpd.backend_name = args.backend
    httpd.model_name = args.model if args.backend == "faster-whisper" else args.whisper_cpp_model
    httpd.save_mp3 = not args.no_mp3_copy

    print(f"listening on http://{args.host}:{args.port}/upload")
    print(f"health check: http://{args.host}:{args.port}/health")
    print(f"saving recordings to {output_dir}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
