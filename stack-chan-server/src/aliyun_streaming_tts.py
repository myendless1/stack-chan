import json
import re
import uuid


TTS_WS_URLS = {
    "shanghai": "wss://nls-gateway-cn-shanghai.aliyuncs.com/ws/v1",
    "beijing": "wss://nls-gateway-cn-beijing.aliyuncs.com/ws/v1",
    "shenzhen": "wss://nls-gateway-cn-shenzhen.aliyuncs.com/ws/v1",
}


def build_tts_ws_url(region: str, token: str, override_url: str = "") -> str:
    base = (override_url or TTS_WS_URLS.get(region, TTS_WS_URLS["shanghai"])).rstrip("?")
    separator = "&" if "?" in base else "?"
    if "token=" in base:
        return base
    return f"{base}{separator}token={token}"


def split_sentences(text: str, max_chars: int = 120) -> list[str]:
    text = re.sub(r"\s+", " ", str(text or "")).strip()
    if not text:
        return []
    parts = [part.strip() for part in re.split(r"(?<=[。！？!?；;,.，])", text) if part.strip()]
    sentences: list[str] = []
    for part in parts or [text]:
        while len(part) > max_chars:
            sentences.append(part[:max_chars])
            part = part[max_chars:]
        if part:
            sentences.append(part)
    return sentences


def build_start_synthesis(
    appkey: str,
    text: str,
    *,
    voice: str,
    sample_rate: int = 16000,
    volume: int = 80,
    speech_rate: int = 0,
    pitch_rate: int = 0,
    task_id: str = "",
) -> dict:
    return {
        "header": {
            "appkey": appkey,
            "namespace": "FlowingSpeechSynthesizer",
            "name": "StartSynthesis",
            "task_id": task_id or uuid.uuid4().hex,
            "message_id": uuid.uuid4().hex,
        },
        "payload": {
            "text": text,
            "voice": voice,
            "format": "pcm",
            "sample_rate": int(sample_rate),
            "volume": int(volume),
            "speech_rate": int(speech_rate),
            "pitch_rate": int(pitch_rate),
        },
    }


def parse_tts_event(raw: str | bytes) -> dict:
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8", errors="replace")
    data = json.loads(raw)
    header = data.get("header") if isinstance(data, dict) else {}
    payload = data.get("payload") if isinstance(data, dict) else {}
    header = header if isinstance(header, dict) else {}
    payload = payload if isinstance(payload, dict) else {}
    name = str(header.get("name") or data.get("name") or "")
    return {
        "name": name,
        "status": header.get("status") if "status" in header else data.get("status"),
        "task_id": header.get("task_id") or data.get("task_id") or "",
        "is_done": name in ("SynthesisCompleted", "TaskFailed"),
        "raw": data,
    }


class AliyunStreamingTtsClient:
    def __init__(
        self,
        *,
        appkey: str,
        token_getter,
        region: str = "shanghai",
        ws_url: str = "",
        voice: str = "xiaoyun",
        sample_rate: int = 16000,
        volume: int = 80,
        speech_rate: int = 0,
        pitch_rate: int = 0,
    ):
        self.appkey = appkey
        self.token_getter = token_getter
        self.region = region
        self.ws_url = ws_url
        self.voice = voice
        self.sample_rate = sample_rate
        self.volume = volume
        self.speech_rate = speech_rate
        self.pitch_rate = pitch_rate

    def synthesize_pcm(self, text: str) -> bytes:
        return b"".join(self.iter_pcm_chunks(text))

    def iter_pcm_chunks(self, text: str):
        try:
            import websocket  # type: ignore
        except Exception as exc:
            raise RuntimeError("websocket-client is required for Aliyun streaming TTS") from exc
        ws = websocket.create_connection(
            build_tts_ws_url(self.region, self.token_getter(), self.ws_url),
            timeout=10,
        )
        try:
            start = build_start_synthesis(
                self.appkey,
                text,
                voice=self.voice,
                sample_rate=self.sample_rate,
                volume=self.volume,
                speech_rate=self.speech_rate,
                pitch_rate=self.pitch_rate,
            )
            ws.send(json.dumps(start, ensure_ascii=False))
            while True:
                frame = ws.recv()
                if isinstance(frame, bytes):
                    if frame:
                        yield frame
                    continue
                event = parse_tts_event(frame)
                if event["name"] == "TaskFailed":
                    raise RuntimeError(f"Aliyun TTS failed: {event['raw']}")
                if event["is_done"]:
                    break
        finally:
            ws.close()
