import json
import time
import uuid


PROTOCOL_VERSION = 1
DEFAULT_TRANSPORT = "websocket"
DEFAULT_AUDIO_PARAMS = {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60,
}


def json_dumps(message: dict) -> str:
    return json.dumps(message, ensure_ascii=False, separators=(",", ":"))


def parse_json_message(raw: str | bytes) -> dict:
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8", errors="replace")
    message = json.loads(raw)
    if not isinstance(message, dict):
        raise ValueError("xiaozhi message must be a JSON object")
    return message


def make_request_id(prefix: str = "srv") -> str:
    return f"{prefix}_{uuid.uuid4().hex[:12]}"


def build_hello(session_id: str, audio_params: dict | None = None) -> dict:
    return {
        "type": "hello",
        "version": PROTOCOL_VERSION,
        "transport": DEFAULT_TRANSPORT,
        "session_id": session_id,
        "audio_params": audio_params or DEFAULT_AUDIO_PARAMS,
    }


def build_stt(text: str, *, is_final: bool = False, session_id: str = "") -> dict:
    body = {
        "type": "stt",
        "text": text or "",
        "is_final": bool(is_final),
    }
    if session_id:
        body["session_id"] = session_id
    return body


def build_llm(text: str, *, emotion: str = "neutral", session_id: str = "") -> dict:
    body = {
        "type": "llm",
        "text": text or "",
        "emotion": emotion,
    }
    if session_id:
        body["session_id"] = session_id
    return body


def build_tts_state(state: str, *, text: str = "", session_id: str = "") -> dict:
    body = {
        "type": "tts",
        "state": state,
    }
    if text:
        body["text"] = text
    if session_id:
        body["session_id"] = session_id
    return body


def build_mcp_request(method: str, params: dict | None = None, *, request_id: str | int | None = None) -> dict:
    return {
        "type": "mcp",
        "payload": {
            "jsonrpc": "2.0",
            "id": request_id or make_request_id("mcp"),
            "method": method,
            "params": params or {},
        },
    }


def build_mcp_tools_call(name: str, arguments: dict | None = None, *, request_id: str | int | None = None) -> dict:
    return build_mcp_request(
        "tools/call",
        {"name": name, "arguments": arguments or {}},
        request_id=request_id,
    )


def extract_device_id_from_hello(message: dict, fallback: str = "default") -> str:
    candidates = [
        message.get("device_id"),
        message.get("mac_address"),
        message.get("mac"),
    ]
    payload = message.get("payload")
    if isinstance(payload, dict):
        candidates.extend(
            [
                payload.get("device_id"),
                payload.get("mac_address"),
                payload.get("mac"),
                payload.get("chip_id"),
            ]
        )
    for candidate in candidates:
        value = str(candidate or "").strip()
        if value:
            return value
    return fallback


def ota_config(ws_url: str, token: str, *, timezone_offset_minutes: int = 480) -> dict:
    return {
        "websocket": {
            "url": ws_url,
            "token": token,
            "version": PROTOCOL_VERSION,
        },
        "server_time": {
            "timestamp": int(time.time() * 1000),
            "timezone_offset": timezone_offset_minutes,
        },
    }
