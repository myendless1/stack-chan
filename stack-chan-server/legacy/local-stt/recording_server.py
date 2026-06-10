#!/usr/bin/env python3
import argparse
import datetime as dt
import os
import shutil
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class RecordingHandler(BaseHTTPRequestHandler):
    server_version = "XiaopaiRecordingServer/1.0"

    def do_GET(self):
        if self.path not in ("/", "/health"):
            self.send_error(404)
            return
        body = b"ok\nPOST WAV audio to /upload\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path.split("?", 1)[0] != "/upload":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            self.send_error(400, "missing body")
            return

        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        wav_path = self.server.output_dir / f"recording-{stamp}.wav"
        with wav_path.open("wb") as f:
            remaining = length
            while remaining:
                chunk = self.rfile.read(min(64 * 1024, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)

        mp3_path = None
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg:
            mp3_path = wav_path.with_suffix(".mp3")
            subprocess.run(
                [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(wav_path), str(mp3_path)],
                check=False,
            )

        print(f"saved {wav_path} ({length} bytes)")
        if mp3_path and mp3_path.exists():
            print(f"saved {mp3_path}")

        body = f"saved {wav_path.name}\n".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print(f"{self.client_address[0]} - {fmt % args}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8091)
    parser.add_argument("--output", default="recordings")
    args = parser.parse_args()

    output_dir = Path(args.output).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    httpd = ThreadingHTTPServer((args.host, args.port), RecordingHandler)
    httpd.output_dir = output_dir
    print(f"listening on http://{args.host}:{args.port}/upload")
    print(f"saving recordings to {output_dir}")
    if not shutil.which("ffmpeg"):
        print("ffmpeg not found; saving WAV only")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
