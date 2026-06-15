import os
import sys
import types
import unittest


SRC_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "src")
sys.path.insert(0, SRC_DIR)

from aliyun_streaming_asr import parse_asr_event
from aliyun_streaming_tts import AliyunStreamingTtsClient, split_sentences
from mcp_client import (
    STACKCHAN_TOOL_FACE_ACTION,
    STACKCHAN_TOOL_FACE_SET,
    STACKCHAN_TOOL_HEAD_MOVE,
    STACKCHAN_TOOL_VOLUME_SET,
    command_to_mcp_calls,
)
from xiaozhi_protocol import build_hello, build_mcp_tools_call, build_stt


class RealtimeMappingTest(unittest.TestCase):
    def test_face_alias_maps_to_action(self):
        calls = command_to_mcp_calls({"type": "face", "payload": {"expression": "爱心"}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_FACE_ACTION)
        self.assertEqual(calls[0].arguments, {"action": "heart_action"})

    def test_face_expression_maps_to_expression_tool(self):
        calls = command_to_mcp_calls({"type": "face", "payload": {"expression": "thinking"}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_FACE_SET)
        self.assertEqual(calls[0].arguments, {"expression": "thinking"})

    def test_motion_maps_to_head_move(self):
        calls = command_to_mcp_calls({"type": "motion", "payload": {"type": "left", "degree": 12, "duration_ms": 300}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_HEAD_MOVE)
        self.assertEqual(calls[0].arguments["type"], "left")
        self.assertEqual(calls[0].arguments["degree"], 12.0)

    def test_volume_set_maps_to_volume_tool(self):
        calls = command_to_mcp_calls({"type": "volume", "payload": {"mode": "set", "value": 80}})
        self.assertEqual(calls[0].name, STACKCHAN_TOOL_VOLUME_SET)
        self.assertEqual(calls[0].arguments, {"value": 80})

    def test_xiaozhi_protocol_shapes(self):
        hello = build_hello("sess_1")
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["audio_params"]["frame_duration"], 60)
        stt = build_stt("你好", is_final=True)
        self.assertTrue(stt["is_final"])
        mcp = build_mcp_tools_call("self.stackchan.stop", {})
        self.assertEqual(mcp["payload"]["method"], "tools/call")

    def test_aliyun_asr_event_parser(self):
        event = parse_asr_event(
            '{"header":{"name":"SentenceEnd","status":20000000,"task_id":"t1"},"payload":{"result":"你好小派"}}'
        )
        self.assertTrue(event["is_final"])
        self.assertEqual(event["text"], "你好小派")

    def test_sentence_split(self):
        self.assertEqual(split_sentences("你好。我们开始吧！"), ["你好。", "我们开始吧！"])

    def test_tts_iter_pcm_chunks_streams_binary_frames(self):
        class FakeWebSocket:
            def __init__(self):
                self.sent = []
                self.frames = [
                    '{"header":{"name":"SynthesisStarted","status":20000000}}',
                    b"pcm1",
                    b"pcm2",
                    '{"header":{"name":"SynthesisCompleted","status":20000000}}',
                ]

            def send(self, payload):
                self.sent.append(payload)

            def recv(self):
                return self.frames.pop(0)

            def close(self):
                pass

        fake_ws = FakeWebSocket()
        fake_module = types.SimpleNamespace(create_connection=lambda *args, **kwargs: fake_ws)
        original = sys.modules.get("websocket")
        sys.modules["websocket"] = fake_module
        try:
            client = AliyunStreamingTtsClient(appkey="app", token_getter=lambda: "token")
            self.assertEqual(list(client.iter_pcm_chunks("你好")), [b"pcm1", b"pcm2"])
            self.assertTrue(fake_ws.sent)
        finally:
            if original is None:
                sys.modules.pop("websocket", None)
            else:
                sys.modules["websocket"] = original


if __name__ == "__main__":
    unittest.main()
