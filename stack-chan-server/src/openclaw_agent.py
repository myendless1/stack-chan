import json
import urllib.error
import urllib.request
from http import HTTPStatus


SYSTEM_PROMPT = """你是 StackChan 小派的实时控制代理。你可以自然对话，也可以通过工具控制表情、动作、云台、相机和音量。输出普通文本时将被立即转成语音。"""


def extract_openclaw_text(response_text: str) -> str:
    data = json.loads(response_text)
    choices = data.get("choices") if isinstance(data, dict) else None
    if not choices:
        return ""
    first = choices[0] if isinstance(choices[0], dict) else {}
    message = first.get("message") if isinstance(first.get("message"), dict) else {}
    content = message.get("content")
    if isinstance(content, str):
        return content.strip()
    if isinstance(content, list):
        parts = []
        for item in content:
            if isinstance(item, dict) and item.get("type") == "text":
                parts.append(str(item.get("text") or ""))
        return "".join(parts).strip()
    return str(first.get("text") or "").strip()


class OpenClawAgent:
    def __init__(
        self,
        *,
        base_url: str,
        token: str,
        model: str,
        backend_model: str = "",
        timeout: int = 45,
        session_prefix: str = "xiaopai",
        max_completion_tokens: int = 512,
    ):
        self.base_url = base_url
        self.token = token
        self.model = model
        self.backend_model = backend_model
        self.timeout = timeout
        self.session_prefix = session_prefix
        self.max_completion_tokens = max_completion_tokens

    @property
    def enabled(self) -> bool:
        return bool(self.base_url and self.token)

    def chat(self, device_id: str, user_text: str) -> str:
        if not self.enabled:
            return ""
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_text},
            ],
            "max_completion_tokens": self.max_completion_tokens,
        }
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Content-Type": "application/json",
            "x-openclaw-session-key": f"{self.session_prefix}-{device_id}",
        }
        if self.backend_model:
            headers["x-openclaw-model"] = self.backend_model
        request = urllib.request.Request(
            self.base_url.rstrip("/") + "/chat/completions",
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            method="POST",
            headers=headers,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as resp:
                status = getattr(resp, "status", HTTPStatus.OK)
                response_text = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"OpenClaw HTTP {exc.code}: {detail}") from exc
        if int(status) >= 400:
            raise RuntimeError(f"OpenClaw HTTP {status}: {response_text}")
        return extract_openclaw_text(response_text)
