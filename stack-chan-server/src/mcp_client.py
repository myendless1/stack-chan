from dataclasses import dataclass
import threading
import time


STACKCHAN_TOOL_FACE_SET = "self.stackchan.face.set_expression"
STACKCHAN_TOOL_FACE_ACTION = "self.stackchan.face.play_action"
STACKCHAN_TOOL_HEAD_MOVE = "self.stackchan.head.move"
STACKCHAN_TOOL_HEAD_SET_POSE = "self.stackchan.head.set_pose"
STACKCHAN_TOOL_HEAD_FIND_OWNER = "self.stackchan.head.find_owner"
STACKCHAN_TOOL_CAMERA_CAPTURE = "self.stackchan.camera.capture"
STACKCHAN_TOOL_VOLUME_SET = "self.stackchan.volume.set"
STACKCHAN_TOOL_VOLUME_ADJUST = "self.stackchan.volume.adjust"
STACKCHAN_TOOL_SEQUENCE_RUN = "self.stackchan.sequence.run"
STACKCHAN_TOOL_STOP = "self.stackchan.stop"


ACTION_NAMES = {
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
}


EXPRESSION_ALIASES = {
    "害羞": "shy",
    "思考": "thinking",
    "眨眼": "wink",
    "爱心": "heart_action",
    "愛心": "heart_action",
    "点头": "nod",
    "點頭": "nod",
    "开心": "happy_squint",
    "開心": "happy_squint",
}


@dataclass(frozen=True)
class McpToolCall:
    name: str
    arguments: dict


class McpRequestTracker:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._pending: dict[str, dict] = {}

    def add(self, request_id: str, *, device_id: str, tool_name: str, command_id: str = "") -> None:
        with self._lock:
            self._pending[str(request_id)] = {
                "device_id": device_id,
                "tool_name": tool_name,
                "command_id": command_id,
                "created_at": time.time(),
            }

    def pop(self, request_id: str) -> dict | None:
        with self._lock:
            return self._pending.pop(str(request_id), None)

    def snapshot(self) -> dict[str, dict]:
        with self._lock:
            return dict(self._pending)


def normalize_expression_name(expression: str) -> str:
    value = str(expression or "").strip()
    return EXPRESSION_ALIASES.get(value, value or "calm")


def command_to_mcp_calls(command: dict) -> list[McpToolCall]:
    command_type = str(command.get("type") or "")
    payload = command.get("payload")
    payload = payload if isinstance(payload, dict) else payload

    if command_type in ("face", "expression", "action"):
        expression = "calm"
        if isinstance(payload, dict):
            expression = normalize_expression_name(payload.get("expression") or payload.get("face") or payload.get("action"))
        if command_type == "action" or expression in ACTION_NAMES:
            return [McpToolCall(STACKCHAN_TOOL_FACE_ACTION, {"action": expression})]
        return [McpToolCall(STACKCHAN_TOOL_FACE_SET, {"expression": expression})]

    if command_type in ("motion", "move"):
        args = dict(payload) if isinstance(payload, dict) else {}
        if "pan" in args or "tilt" in args:
            return [
                McpToolCall(
                    STACKCHAN_TOOL_HEAD_SET_POSE,
                    {
                        "pan": float(args.get("pan", 0)),
                        "tilt": float(args.get("tilt", 45)),
                        "duration_ms": int(args.get("duration_ms", 500)),
                    },
                )
            ]
        return [
            McpToolCall(
                STACKCHAN_TOOL_HEAD_MOVE,
                {
                    "type": str(args.get("type") or args.get("direction") or "center"),
                    "degree": float(args.get("degree", args.get("degrees", 15))),
                    "duration_ms": int(args.get("duration_ms", 500)),
                },
            )
        ]

    if command_type in ("find_owner", "locate_owner"):
        args = dict(payload) if isinstance(payload, dict) else {}
        return [
            McpToolCall(
                STACKCHAN_TOOL_HEAD_FIND_OWNER,
                {
                    "rounds": int(args.get("rounds", 1)),
                    "gain_x": float(args.get("gain_x", 0.45)),
                    "gain_y": float(args.get("gain_y", 0.55)),
                    "stop_pixels": float(args.get("stop_pixels", 32)),
                },
            )
        ]

    if command_type in ("capture_image", "track_once", "camera"):
        args = dict(payload) if isinstance(payload, dict) else {}
        return [
            McpToolCall(
                STACKCHAN_TOOL_CAMERA_CAPTURE,
                {
                    "upload_url": args.get("upload_url", ""),
                    "visual_tracking": bool(args.get("visual_tracking", command_type == "track_once")),
                },
            )
        ]

    if command_type in ("volume", "sound"):
        args = dict(payload) if isinstance(payload, dict) else {}
        if args.get("mode") == "set" or "value" in args:
            return [McpToolCall(STACKCHAN_TOOL_VOLUME_SET, {"value": int(args.get("value", 80))})]
        return [
            McpToolCall(
                STACKCHAN_TOOL_VOLUME_ADJUST,
                {
                    "direction": str(args.get("direction") or "up"),
                    "step": int(args.get("step", 10)),
                },
            )
        ]

    if command_type == "sequence":
        steps = payload if isinstance(payload, list) else []
        return [McpToolCall(STACKCHAN_TOOL_SEQUENCE_RUN, {"steps": steps})]

    if command_type == "stop":
        return [McpToolCall(STACKCHAN_TOOL_STOP, {})]

    if command_type == "speak":
        return []

    return []
