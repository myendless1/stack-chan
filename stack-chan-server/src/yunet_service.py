import os
import threading
import time


class YunetFaceService:
    def __init__(
        self,
        model_path: str,
        score_threshold: float = 0.45,
        nms_threshold: float = 0.3,
        top_k: int = 5000,
    ):
        self.model_path = model_path
        self.score_threshold = score_threshold
        self.nms_threshold = nms_threshold
        self.top_k = top_k
        self._detector = None
        self._cv2 = None
        self._np = None
        self._load_error = ""
        self._lock = threading.Lock()

    def status(self) -> dict:
        return {
            "backend": "yunet",
            "model_path": self.model_path,
            "model_exists": os.path.exists(self.model_path),
            "available": self.available,
            "error": self._load_error,
            "score_threshold": self.score_threshold,
            "nms_threshold": self.nms_threshold,
            "top_k": self.top_k,
        }

    @property
    def available(self) -> bool:
        return self._ensure_detector()

    def _ensure_detector(self) -> bool:
        if self._detector is not None:
            return True
        if self._load_error:
            return False
        if not os.path.exists(self.model_path):
            self._load_error = f"YuNet model not found: {self.model_path}"
            return False
        try:
            import cv2
            import numpy as np

            self._cv2 = cv2
            self._np = np
            self._detector = cv2.FaceDetectorYN.create(
                self.model_path,
                "",
                (320, 240),
                self.score_threshold,
                self.nms_threshold,
                self.top_k,
                cv2.dnn.DNN_BACKEND_OPENCV,
                cv2.dnn.DNN_TARGET_CPU,
            )
            return True
        except Exception as exc:
            self._load_error = f"YuNet unavailable: {exc}"
            return False

    def detect_rgb565(self, rgb565: bytes, width: int, height: int, visual_path: str = "") -> tuple[str, dict]:
        if not self._ensure_detector():
            return "", {"available": False, "backend": "yunet", "error": self._load_error, "faces": []}
        if len(rgb565) != width * height * 2:
            return "", {
                "available": False,
                "backend": "yunet",
                "error": f"rgb565 size mismatch: got {len(rgb565)}, expected {width * height * 2}",
                "faces": [],
            }
        frame = self._rgb565_be_to_bgr(rgb565, width, height)
        return self.detect_bgr(frame, visual_path)

    def detect_jpeg(self, jpeg: bytes, visual_path: str = "") -> tuple[str, dict]:
        if not self._ensure_detector():
            return "", {"available": False, "backend": "yunet", "error": self._load_error, "faces": []}
        arr = self._np.frombuffer(jpeg, dtype=self._np.uint8)
        frame = self._cv2.imdecode(arr, self._cv2.IMREAD_COLOR)
        if frame is None:
            return "", {"available": True, "backend": "yunet", "error": "failed to decode jpeg", "faces": []}
        return self.detect_bgr(frame, visual_path)

    def detect_bgr(self, frame, visual_path: str = "") -> tuple[str, dict]:
        if not self._ensure_detector():
            return "", {"available": False, "backend": "yunet", "error": self._load_error, "faces": []}
        height, width = frame.shape[:2]
        started = time.perf_counter()
        with self._lock:
            self._detector.setInputSize((width, height))
            _, raw_faces = self._detector.detect(frame)
        detect_ms = (time.perf_counter() - started) * 1000.0

        faces = []
        if raw_faces is not None:
            faces = [self._face_to_dict(face) for face in raw_faces]
            faces.sort(key=lambda item: item["area"], reverse=True)
        best_face = faces[0] if faces else None
        output_path = ""
        if visual_path:
            output_path = self._draw_faces(frame, faces, visual_path)
        return output_path, {
            "available": True,
            "backend": "yunet",
            "model_path": self.model_path,
            "width": width,
            "height": height,
            "detect_ms": detect_ms,
            "faces": faces,
            "best_face": best_face,
        }

    def _rgb565_be_to_bgr(self, rgb565: bytes, width: int, height: int):
        data = self._np.frombuffer(rgb565, dtype=self._np.uint8).reshape((height, width, 2))
        value = (data[:, :, 0].astype(self._np.uint16) << 8) | data[:, :, 1].astype(self._np.uint16)
        r = ((value >> 11) & 0x1F).astype(self._np.uint16) * 255 // 31
        g = ((value >> 5) & 0x3F).astype(self._np.uint16) * 255 // 63
        b = (value & 0x1F).astype(self._np.uint16) * 255 // 31
        return self._np.dstack((b, g, r)).astype(self._np.uint8)

    @staticmethod
    def _face_to_dict(face) -> dict:
        x = float(face[0])
        y = float(face[1])
        w = float(face[2])
        h = float(face[3])
        return {
            "left": x,
            "top": y,
            "right": x + w,
            "bottom": y + h,
            "width": w,
            "height": h,
            "center": {"x": x + w / 2.0, "y": y + h / 2.0},
            "area": w * h,
            "confidence": float(face[-1]),
        }

    def _draw_faces(self, frame, faces: list[dict], visual_path: str) -> str:
        annotated = frame.copy()
        for idx, face in enumerate(faces, start=1):
            left = round(face["left"])
            top = round(face["top"])
            right = round(face["right"])
            bottom = round(face["bottom"])
            center = face["center"]
            self._cv2.rectangle(annotated, (left, top), (right, bottom), (0, 220, 0), 2)
            self._cv2.circle(annotated, (round(center["x"]), round(center["y"])), 3, (0, 0, 255), -1)
            label = f"{idx}: {face['confidence']:.2f}"
            self._cv2.putText(
                annotated,
                label,
                (left, max(16, top - 6)),
                self._cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (0, 220, 0),
                1,
            )
        os.makedirs(os.path.dirname(visual_path), exist_ok=True)
        self._cv2.imwrite(visual_path, annotated)
        return visual_path
