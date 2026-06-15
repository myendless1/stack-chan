import asyncio
from collections.abc import Callable
from dataclasses import dataclass
import json
from queue import Empty, Queue
import threading
import time
from urllib.parse import parse_qs, urlparse

from aliyun_streaming_asr import AliyunStreamingAsrSession, build_stop_transcription, parse_asr_event
from aliyun_streaming_tts import AliyunStreamingTtsClient, split_sentences
from mcp_client import McpRequestTracker, command_to_mcp_calls
from openclaw_agent import OpenClawAgent
from opus_codec import OpusCodec, OpusUnavailableError
from xiaozhi_protocol import (
    build_hello,
    build_llm,
    build_mcp_tools_call,
    build_stt,
    build_tts_state,
    extract_device_id_from_hello,
    json_dumps,
    make_request_id,
    parse_json_message,
)


@dataclass
class RealtimeConfig:
    host: str = "0.0.0.0"
    port: int = 8092
    path: str = "/xiaozhi/ws"
    token: str = ""
    region: str = "shanghai"
    appkey: str = ""
    token_getter: Callable[[], str] | None = None
    aliyun_asr_ws_url: str = ""
    aliyun_tts_ws_url: str = ""
    voice: str = "xiaoyun"
    sample_rate: int = 16000
    volume: int = 80
    speech_rate: int = 0
    pitch_rate: int = 0
    max_sentence_chars: int = 120
    openclaw_base_url: str = ""
    openclaw_token: str = ""
    openclaw_model: str = "openclaw/default"
    openclaw_backend_model: str = ""
    openclaw_timeout: int = 45
    openclaw_session_prefix: str = "xiaopai"
    openclaw_max_completion_tokens: int = 512
    debug: bool = False


class RealtimeDeviceSession:
    def __init__(self, *, device_id: str, websocket, session_id: str) -> None:
        self.device_id = device_id
        self.websocket = websocket
        self.session_id = session_id
        self.connected_at = time.time()
        self.last_seen = self.connected_at
        self.last_stt = ""
        self.tts_task: asyncio.Task | None = None
        self.asr_bridge: RealtimeAsrBridge | None = None
        self.latency_started_at = time.perf_counter()
        self.latency_marks: dict[str, float] = {}

    def snapshot(self) -> dict:
        return {
            "device_id": self.device_id,
            "session_id": self.session_id,
            "connected_at": self.connected_at,
            "last_seen_seconds_ago": round(time.time() - self.last_seen, 1),
            "online": True,
            "last_stt": self.last_stt,
            "asr_active": bool(self.asr_bridge and self.asr_bridge.active),
            "latency_ms": self.latency_snapshot(),
        }

    def reset_latency(self) -> None:
        self.latency_started_at = time.perf_counter()
        self.latency_marks = {"voice_start": self.latency_started_at}

    def mark_latency(self, name: str) -> float:
        now = time.perf_counter()
        self.latency_marks[name] = now
        return (now - self.latency_started_at) * 1000

    def latency_snapshot(self) -> dict[str, int]:
        return {
            name: int((marked_at - self.latency_started_at) * 1000)
            for name, marked_at in self.latency_marks.items()
        }


class RealtimeAsrBridge:
    def __init__(self, manager: "RealtimeManager", session: RealtimeDeviceSession) -> None:
        self.manager = manager
        self.device_id = session.device_id
        self.session_id = session.session_id
        self._queue: Queue[bytes | None] = Queue(maxsize=80)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"asr-{session.device_id}", daemon=True)
        self._ws = None
        self._task_id = ""

    @property
    def active(self) -> bool:
        return self._thread.is_alive() and not self._stop.is_set()

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            self._queue.put_nowait(None)
        except Exception:
            pass
        ws = self._ws
        if ws is not None:
            try:
                if self._task_id:
                    ws.send(json.dumps(build_stop_transcription(self.manager.config.appkey, self._task_id), ensure_ascii=False))
            except Exception:
                pass
            try:
                ws.close()
            except Exception:
                pass

    def push_opus(self, opus_frame: bytes) -> None:
        try:
            pcm = self.manager.opus.decode(opus_frame)
        except OpusUnavailableError as exc:
            self.manager.logger(f"Realtime ASR unavailable: {exc}")
            self.stop()
            return
        try:
            self._queue.put_nowait(pcm)
        except Exception:
            self.manager.logger(f"Realtime ASR audio queue full: {self.device_id}")

    def _run(self) -> None:
        if not self.manager.config.appkey or self.manager.config.token_getter is None:
            self.manager.logger("Realtime ASR skipped: missing Aliyun appkey/token getter")
            return
        try:
            asr = AliyunStreamingAsrSession(
                appkey=self.manager.config.appkey,
                token_getter=self.manager.config.token_getter,
                region=self.manager.config.region,
                ws_url=self.manager.config.aliyun_asr_ws_url,
                sample_rate=self.manager.config.sample_rate,
            )
            ws, task_id = asr.connect()
            self._ws = ws
            self._task_id = task_id
            try:
                ws.settimeout(0.02)
            except Exception:
                pass
            while not self._stop.is_set():
                try:
                    pcm = self._queue.get(timeout=0.02)
                    if pcm is None:
                        break
                    ws.send_binary(pcm)
                except Empty:
                    pass
                except Exception as exc:
                    self.manager.logger(f"Realtime ASR send failed: {exc}")
                    break
                self._drain_events(ws)
            self._drain_events(ws)
        except Exception as exc:
            self.manager.logger(f"Realtime ASR bridge stopped: {exc}")
        finally:
            try:
                if self._ws is not None:
                    self._ws.close()
            except Exception:
                pass

    def _drain_events(self, ws) -> None:
        while not self._stop.is_set():
            try:
                raw = ws.recv()
            except Exception as exc:
                if exc.__class__.__name__ in ("WebSocketTimeoutException", "TimeoutError"):
                    return
                raise
            if isinstance(raw, bytes):
                continue
            event = parse_asr_event(raw)
            text = str(event.get("text") or "")
            if not text:
                if event.get("name") == "TaskFailed":
                    raise RuntimeError(f"Aliyun ASR failed: {event.get('raw')}")
                continue
            self.manager.submit_asr_text(
                self.device_id,
                self.session_id,
                text,
                is_final=bool(event.get("is_final")),
            )


class RealtimeManager:
    def __init__(self, config: RealtimeConfig, *, logger=print) -> None:
        self.config = config
        self.logger = logger
        self._thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._server = None
        self._started = threading.Event()
        self._startup_error: BaseException | None = None
        self._sessions: dict[str, RealtimeDeviceSession] = {}
        self._sessions_lock = threading.Lock()
        self._mcp_tracker = McpRequestTracker()
        self._opus = OpusCodec(sample_rate=config.sample_rate)
        self._openclaw = OpenClawAgent(
            base_url=config.openclaw_base_url,
            token=config.openclaw_token,
            model=config.openclaw_model,
            backend_model=config.openclaw_backend_model,
            timeout=config.openclaw_timeout,
            session_prefix=config.openclaw_session_prefix,
            max_completion_tokens=config.openclaw_max_completion_tokens,
        )
        self._pcm_bytes_per_frame = self._opus.samples_per_frame * self._opus.channels * 2

    @property
    def opus(self) -> OpusCodec:
        return self._opus

    @property
    def enabled(self) -> bool:
        return self._thread is not None and self._thread.is_alive() and self._startup_error is None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run_loop, name="xiaozhi-realtime", daemon=True)
        self._thread.start()
        self._started.wait(timeout=5)
        if self._startup_error is not None:
            raise RuntimeError(f"xiaozhi realtime server failed to start: {self._startup_error}") from self._startup_error

    def _run_loop(self) -> None:
        loop = asyncio.new_event_loop()
        self._loop = loop
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._start_server())
            self._started.set()
            loop.run_forever()
        except BaseException as exc:
            self._startup_error = exc
            self._started.set()
            self.logger(f"Xiaozhi realtime loop stopped: {exc}")
        finally:
            if not loop.is_closed():
                loop.run_until_complete(self._close_sessions())
            loop.close()

    async def _start_server(self) -> None:
        try:
            import websockets  # type: ignore
        except Exception as exc:
            self._started.set()
            raise RuntimeError("websockets is required for xiaozhi realtime server") from exc
        self._server = await websockets.serve(self._dispatch, self.config.host, self.config.port)
        self.logger(f"Xiaozhi realtime WebSocket ready: ws://{self.config.host}:{self.config.port}{self.config.path}")

    async def _close_sessions(self) -> None:
        with self._sessions_lock:
            sessions = list(self._sessions.values())
            self._sessions.clear()
        for session in sessions:
            try:
                await session.websocket.close()
            except Exception:
                pass

    def stop(self) -> None:
        if self._loop is None:
            return
        self._loop.call_soon_threadsafe(self._loop.stop)

    def devices_snapshot(self) -> list[dict]:
        with self._sessions_lock:
            return [session.snapshot() for session in self._sessions.values()]

    def first_device_id(self) -> str:
        with self._sessions_lock:
            return next(iter(self._sessions), "default")

    def has_device(self, device_id: str) -> bool:
        with self._sessions_lock:
            if device_id in self._sessions:
                return True
            return bool(self._sessions and device_id in ("", "default"))

    def enqueue_command(self, device_id: str, command: dict) -> bool:
        if self._loop is None:
            return False
        future = asyncio.run_coroutine_threadsafe(self._send_command(device_id, command), self._loop)
        try:
            return bool(future.result(timeout=1.5))
        except Exception as exc:
            self.logger(f"Realtime command dispatch failed: {exc}")
            return False

    async def _send_command(self, device_id: str, command: dict) -> bool:
        session = self._select_session(device_id)
        if session is None:
            return False
        if command.get("interrupt"):
            await self._abort_session_tts(session)
        if command.get("type") == "speak":
            payload = command.get("payload") if isinstance(command.get("payload"), dict) else {}
            await self._speak(session, str(payload.get("text") or ""))
            return True
        sent = False
        for call in command_to_mcp_calls(command):
            request_id = make_request_id("mcp")
            self._mcp_tracker.add(request_id, device_id=session.device_id, tool_name=call.name, command_id=command.get("cmd_id", ""))
            await session.websocket.send(json_dumps(build_mcp_tools_call(call.name, call.arguments, request_id=request_id)))
            sent = True
        return sent

    def _select_session(self, device_id: str) -> RealtimeDeviceSession | None:
        with self._sessions_lock:
            if device_id in self._sessions:
                return self._sessions[device_id]
            if device_id in ("", "default") and self._sessions:
                return next(iter(self._sessions.values()))
            return None

    async def _dispatch(self, websocket, path=None) -> None:
        if path is None:
            request = getattr(websocket, "request", None)
            path = getattr(websocket, "path", "") or getattr(request, "path", "")
        parsed = urlparse(path or "")
        if parsed.path != self.config.path:
            await websocket.close(code=1008, reason="invalid path")
            return
        if not self._authorized(websocket, parsed.query):
            await websocket.close(code=1008, reason="unauthorized")
            return
        session: RealtimeDeviceSession | None = None
        try:
            async for frame in websocket:
                if isinstance(frame, bytes):
                    if session is not None:
                        session.last_seen = time.time()
                        if session.asr_bridge is not None:
                            session.asr_bridge.push_opus(frame)
                    continue
                message = parse_json_message(frame)
                if session is None:
                    device_id = extract_device_id_from_hello(message)
                    session = RealtimeDeviceSession(
                        device_id=device_id,
                        websocket=websocket,
                        session_id=make_request_id("sess"),
                    )
                    self._register_session(session)
                    await websocket.send(json_dumps(build_hello(session.session_id)))
                    continue
                session.last_seen = time.time()
                await self._handle_json(session, message)
        except Exception as exc:
            self.logger(f"Xiaozhi realtime session ended: {exc}")
        finally:
            if session is not None:
                self._stop_asr(session)
                await self._abort_session_tts(session)
                self._unregister_session(session.device_id, session.session_id)

    def _authorized(self, websocket, query: str) -> bool:
        if not self.config.token:
            return True
        token = ""
        headers = getattr(websocket, "request_headers", {}) or {}
        if not headers:
            request = getattr(websocket, "request", None)
            headers = getattr(request, "headers", {}) or {}
        auth = headers.get("Authorization") or headers.get("authorization") or ""
        if auth.lower().startswith("bearer "):
            token = auth.split(" ", 1)[1].strip()
        if not token:
            token = (parse_qs(query).get("token") or [""])[0]
        return token == self.config.token

    def _register_session(self, session: RealtimeDeviceSession) -> None:
        with self._sessions_lock:
            self._sessions[session.device_id] = session
        self.logger(f"Xiaozhi device connected: {session.device_id}")

    def _unregister_session(self, device_id: str, session_id: str) -> None:
        with self._sessions_lock:
            current = self._sessions.get(device_id)
            if current and current.session_id == session_id:
                self._sessions.pop(device_id, None)
        self.logger(f"Xiaozhi device disconnected: {device_id}")

    async def _handle_json(self, session: RealtimeDeviceSession, message: dict) -> None:
        message_type = message.get("type")
        if message_type == "abort":
            await self._abort_session_tts(session)
            return
        if message_type == "listen":
            state = str(message.get("state") or "")
            if state in ("start", "detect"):
                await self._abort_session_tts(session)
                self._start_asr(session)
            elif state == "stop":
                self._stop_asr(session)
            return
        if message_type == "mcp":
            payload = message.get("payload") if isinstance(message.get("payload"), dict) else {}
            if "id" in payload:
                self._mcp_tracker.pop(str(payload["id"]))
            return
        if message_type == "stt":
            text = str(message.get("text") or "")
            if text:
                final = bool(message.get("is_final", True))
                session.last_stt = text
                if final:
                    await self._handle_final_text(session, text)

    def submit_asr_text(self, device_id: str, session_id: str, text: str, *, is_final: bool) -> None:
        if self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(
            self._handle_asr_text(device_id, session_id, text, is_final=is_final),
            self._loop,
        )

    async def _handle_asr_text(self, device_id: str, session_id: str, text: str, *, is_final: bool) -> None:
        session = self._select_session(device_id)
        if session is None or session.session_id != session_id:
            return
        session.last_stt = text
        if is_final:
            self._mark(session, "asr_final")
        elif "asr_first_partial" not in session.latency_marks:
            self._mark(session, "asr_first_partial")
        await session.websocket.send(json_dumps(build_stt(text, is_final=is_final, session_id=session.session_id)))
        if is_final:
            self._stop_asr(session)
            await self._handle_final_text(session, text)

    def _start_asr(self, session: RealtimeDeviceSession) -> None:
        if session.asr_bridge and session.asr_bridge.active:
            return
        session.reset_latency()
        self._mark(session, "asr_start")
        session.asr_bridge = RealtimeAsrBridge(self, session)
        session.asr_bridge.start()

    def _stop_asr(self, session: RealtimeDeviceSession) -> None:
        bridge = session.asr_bridge
        session.asr_bridge = None
        if bridge is not None:
            bridge.stop()

    async def _handle_final_text(self, session: RealtimeDeviceSession, text: str) -> None:
        if not self._openclaw.enabled:
            await self._speak(session, "我没听清，可以再说一遍吗")
            return
        loop = asyncio.get_running_loop()
        try:
            self._mark(session, "openclaw_start")
            reply = await loop.run_in_executor(None, self._openclaw.chat, session.device_id, text)
            self._mark(session, "openclaw_done")
        except Exception as exc:
            self.logger(f"OpenClaw realtime chat failed: {exc}")
            reply = ""
        await self._speak(session, reply or "我没听清，可以再说一遍吗")

    async def _speak(self, session: RealtimeDeviceSession, text: str) -> None:
        text = str(text or "").strip()
        if not text:
            return
        await self._abort_session_tts(session)
        self._mark(session, "device_tts_start")
        session.tts_task = asyncio.create_task(self._run_tts(session, text))

    async def _abort_session_tts(self, session: RealtimeDeviceSession) -> None:
        task = session.tts_task
        if task is not None and not task.done():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            except Exception:
                pass
        session.tts_task = None

    async def _run_tts(self, session: RealtimeDeviceSession, text: str) -> None:
        await session.websocket.send(json_dumps(build_llm(text, session_id=session.session_id)))
        await session.websocket.send(json_dumps(build_tts_state("start", session_id=session.session_id)))
        try:
            for sentence in split_sentences(text, self.config.max_sentence_chars):
                await session.websocket.send(json_dumps(build_tts_state("sentence_start", text=sentence, session_id=session.session_id)))
                await self._send_sentence_audio(session, sentence)
        finally:
            await session.websocket.send(json_dumps(build_tts_state("stop", session_id=session.session_id)))

    async def _send_sentence_audio(self, session: RealtimeDeviceSession, sentence: str) -> None:
        if not self.config.appkey or self.config.token_getter is None:
            return
        tts = AliyunStreamingTtsClient(
            appkey=self.config.appkey,
            token_getter=self.config.token_getter,
            region=self.config.region,
            ws_url=self.config.aliyun_tts_ws_url,
            voice=self.config.voice,
            sample_rate=self.config.sample_rate,
            volume=self.config.volume,
            speech_rate=self.config.speech_rate,
            pitch_rate=self.config.pitch_rate,
        )
        loop = asyncio.get_running_loop()
        queue: Queue[bytes | BaseException | None] = Queue(maxsize=16)

        def produce_pcm() -> None:
            try:
                for pcm_chunk in tts.iter_pcm_chunks(sentence):
                    queue.put(pcm_chunk)
            except BaseException as exc:
                queue.put(exc)
            finally:
                queue.put(None)

        threading.Thread(target=produce_pcm, name=f"tts-{session.device_id}", daemon=True).start()

        pcm_buffer = bytearray()
        try:
            while True:
                item = await loop.run_in_executor(None, queue.get)
                if item is None:
                    break
                if isinstance(item, BaseException):
                    raise item
                if item and "tts_first_pcm" not in session.latency_marks:
                    self._mark(session, "tts_first_pcm")
                pcm_buffer.extend(item)
                while len(pcm_buffer) >= self._pcm_bytes_per_frame:
                    pcm_frame = bytes(pcm_buffer[: self._pcm_bytes_per_frame])
                    del pcm_buffer[: self._pcm_bytes_per_frame]
                    await session.websocket.send(self._opus.encode(pcm_frame))
                    if "tts_first_opus_sent" not in session.latency_marks:
                        self._mark(session, "tts_first_opus_sent")
                    await asyncio.sleep(0)
            if pcm_buffer:
                pcm_frame = bytes(pcm_buffer).ljust(self._pcm_bytes_per_frame, b"\x00")
                await session.websocket.send(self._opus.encode(pcm_frame))
                if "tts_first_opus_sent" not in session.latency_marks:
                    self._mark(session, "tts_first_opus_sent")
        except OpusUnavailableError as exc:
            self.logger(f"Realtime TTS audio unavailable: {exc}")
        except Exception as exc:
            self.logger(f"Realtime streaming TTS failed: {exc}")

    def _mark(self, session: RealtimeDeviceSession, name: str) -> None:
        elapsed_ms = session.mark_latency(name)
        if self.config.debug:
            self.logger(f"latency device={session.device_id} stage={name} elapsed_ms={elapsed_ms:.0f}")
