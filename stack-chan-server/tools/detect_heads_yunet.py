#!/usr/bin/env python3
import argparse
import csv
import json
import os
import time
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageOps

try:
    from pillow_heif import register_heif_opener

    register_heif_opener()
except Exception:
    pass


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".heic", ".heif"}


def load_image_bgr(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        image = ImageOps.exif_transpose(image).convert("RGB")
        rgb = np.asarray(image)
    return cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)


def resize_for_detection(image: np.ndarray, max_side: int) -> tuple[np.ndarray, float]:
    height, width = image.shape[:2]
    if max_side <= 0 or max(height, width) <= max_side:
        return image, 1.0
    scale = max_side / float(max(height, width))
    resized = cv2.resize(image, (round(width * scale), round(height * scale)), interpolation=cv2.INTER_AREA)
    return resized, scale


def resize_to_canvas(image: np.ndarray, landscape_size: tuple[int, int], portrait_size: tuple[int, int]) -> tuple[np.ndarray, float, int, int]:
    height, width = image.shape[:2]
    target_width, target_height = landscape_size if width >= height else portrait_size
    scale = min(target_width / float(width), target_height / float(height))
    resized_width = max(1, round(width * scale))
    resized_height = max(1, round(height * scale))
    resized = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_AREA)
    canvas = np.zeros((target_height, target_width, 3), dtype=np.uint8)
    pad_x = (target_width - resized_width) // 2
    pad_y = (target_height - resized_height) // 2
    canvas[pad_y : pad_y + resized_height, pad_x : pad_x + resized_width] = resized
    return canvas, scale, pad_x, pad_y


def face_to_dict(face: np.ndarray, scale: float, pad_x: int = 0, pad_y: int = 0) -> dict:
    detect_x = float(face[0])
    detect_y = float(face[1])
    detect_w = float(face[2])
    detect_h = float(face[3])
    x = (detect_x - pad_x) / scale
    y = (detect_y - pad_y) / scale
    w = detect_w / scale
    h = detect_h / scale
    confidence = float(face[-1])
    return {
        "left": x,
        "top": y,
        "right": x + w,
        "bottom": y + h,
        "width": w,
        "height": h,
        "center": {"x": x + w / 2.0, "y": y + h / 2.0},
        "area": w * h,
        "confidence": confidence,
        "detect": {
            "left": detect_x,
            "top": detect_y,
            "right": detect_x + detect_w,
            "bottom": detect_y + detect_h,
            "width": detect_w,
            "height": detect_h,
            "center": {"x": detect_x + detect_w / 2.0, "y": detect_y + detect_h / 2.0},
            "area": detect_w * detect_h,
        },
    }


def draw_faces(image: np.ndarray, faces: list[dict], output_path: Path, coordinate_key: str | None = None) -> None:
    annotated = image.copy()
    for idx, face in enumerate(faces, start=1):
        box = face[coordinate_key] if coordinate_key else face
        left = round(box["left"])
        top = round(box["top"])
        right = round(box["right"])
        bottom = round(box["bottom"])
        center_x = round(box["center"]["x"])
        center_y = round(box["center"]["y"])
        cv2.rectangle(annotated, (left, top), (right, bottom), (0, 220, 0), 3)
        cv2.circle(annotated, (center_x, center_y), 5, (0, 0, 255), -1)
        label = f"{idx}: {face['confidence']:.2f}"
        cv2.putText(annotated, label, (left, max(20, top - 8)), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 220, 0), 2)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(output_path), annotated)


def iter_images(input_dir: Path) -> list[Path]:
    return sorted(path for path in input_dir.rglob("*") if path.is_file() and path.suffix.lower() in IMAGE_SUFFIXES)


def parse_size(value: str) -> tuple[int, int]:
    try:
        width_text, height_text = value.lower().split("x", 1)
        width = int(width_text)
        height = int(height_text)
    except Exception as exc:
        raise SystemExit(f"invalid size {value!r}; expected WIDTHxHEIGHT") from exc
    if width <= 0 or height <= 0:
        raise SystemExit(f"invalid size {value!r}; dimensions must be positive")
    return width, height


def main() -> int:
    parser = argparse.ArgumentParser(description="Batch face/head detection with OpenCV YuNet.")
    parser.add_argument("--input", default="heads", help="Directory containing test images.")
    parser.add_argument("--output", default="stack-chan-server/yunet-head-results", help="Directory for annotated images and reports.")
    parser.add_argument(
        "--model",
        default="stack-chan-server/models/face_detection_yunet_2023mar.onnx",
        help="Path to YuNet ONNX model.",
    )
    parser.add_argument("--max-side", type=int, default=320, help="Resize each image so its longest side matches this value before detection.")
    parser.add_argument(
        "--fixed-canvas",
        action="store_true",
        help="Resize and letterbox every image to 320x256 landscape or 256x320 portrait before detection.",
    )
    parser.add_argument("--landscape-size", default="320x256", help="Detection canvas for landscape images when --fixed-canvas is set.")
    parser.add_argument("--portrait-size", default="256x320", help="Detection canvas for portrait images when --fixed-canvas is set.")
    parser.add_argument("--score-threshold", type=float, default=0.45)
    parser.add_argument("--nms-threshold", type=float, default=0.3)
    parser.add_argument("--top-k", type=int, default=5000)
    args = parser.parse_args()

    input_dir = Path(args.input)
    output_dir = Path(args.output)
    model_path = Path(args.model)
    if not input_dir.exists():
        raise SystemExit(f"input directory does not exist: {input_dir}")
    if not model_path.exists():
        raise SystemExit(f"model does not exist: {model_path}")
    landscape_size = parse_size(args.landscape_size)
    portrait_size = parse_size(args.portrait_size)

    detector = cv2.FaceDetectorYN.create(
        str(model_path),
        "",
        (320, 320),
        args.score_threshold,
        args.nms_threshold,
        args.top_k,
        cv2.dnn.DNN_BACKEND_OPENCV,
        cv2.dnn.DNN_TARGET_CPU,
    )

    images = iter_images(input_dir)
    rows = []
    results = []
    total_detect_ms = 0.0
    total_load_ms = 0.0
    failures = []

    for path in images:
        started = time.perf_counter()
        try:
            image = load_image_bgr(path)
        except Exception as exc:
            failures.append({"file": str(path), "error": str(exc)})
            continue
        load_ms = (time.perf_counter() - started) * 1000.0
        total_load_ms += load_ms

        if args.fixed_canvas:
            detect_image, scale, pad_x, pad_y = resize_to_canvas(image, landscape_size, portrait_size)
        else:
            detect_image, scale = resize_for_detection(image, args.max_side)
            pad_x = 0
            pad_y = 0
        height, width = detect_image.shape[:2]
        detector.setInputSize((width, height))

        detect_started = time.perf_counter()
        _, raw_faces = detector.detect(detect_image)
        detect_ms = (time.perf_counter() - detect_started) * 1000.0
        total_detect_ms += detect_ms

        faces = []
        if raw_faces is not None:
            faces = [face_to_dict(face, scale, pad_x, pad_y) for face in raw_faces]
            faces.sort(key=lambda item: item["area"], reverse=True)

        rel = path.relative_to(input_dir)
        annotated_path = output_dir / "annotated" / rel.with_suffix(".jpg")
        draw_faces(detect_image, faces, annotated_path, "detect")

        best = faces[0] if faces else None
        result = {
            "file": str(path),
            "annotated": str(annotated_path),
            "original_size": {"width": image.shape[1], "height": image.shape[0]},
            "detect_size": {"width": width, "height": height},
            "scale": scale,
            "pad": {"x": pad_x, "y": pad_y},
            "load_ms": load_ms,
            "detect_ms": detect_ms,
            "face_count": len(faces),
            "best_face": best,
            "faces": faces,
        }
        results.append(result)
        rows.append(
            {
                "file": str(path),
                "annotated": str(annotated_path),
                "face_count": len(faces),
                "best_confidence": "" if best is None else f"{best['confidence']:.4f}",
                "best_center_x": "" if best is None else f"{best['center']['x']:.1f}",
                "best_center_y": "" if best is None else f"{best['center']['y']:.1f}",
                "best_width": "" if best is None else f"{best['width']:.1f}",
                "best_height": "" if best is None else f"{best['height']:.1f}",
                "load_ms": f"{load_ms:.2f}",
                "detect_ms": f"{detect_ms:.2f}",
                "detect_width": width,
                "detect_height": height,
            }
        )

    output_dir.mkdir(parents=True, exist_ok=True)
    summary = {
        "input": str(input_dir),
        "model": str(model_path),
        "max_side": args.max_side,
        "fixed_canvas": args.fixed_canvas,
        "landscape_size": {"width": landscape_size[0], "height": landscape_size[1]},
        "portrait_size": {"width": portrait_size[0], "height": portrait_size[1]},
        "score_threshold": args.score_threshold,
        "nms_threshold": args.nms_threshold,
        "image_count": len(images),
        "processed_count": len(results),
        "failed_count": len(failures),
        "images_with_faces": sum(1 for item in results if item["face_count"] > 0),
        "total_faces": sum(item["face_count"] for item in results),
        "avg_load_ms": total_load_ms / len(results) if results else 0,
        "avg_detect_ms": total_detect_ms / len(results) if results else 0,
        "failures": failures,
    }
    with open(output_dir / "results.json", "w", encoding="utf-8") as fp:
        json.dump({"summary": summary, "results": results}, fp, ensure_ascii=False, indent=2)
    with open(output_dir / "results.csv", "w", encoding="utf-8", newline="") as fp:
        fieldnames = [
            "file",
            "annotated",
            "face_count",
            "best_confidence",
            "best_center_x",
            "best_center_y",
            "best_width",
            "best_height",
            "load_ms",
            "detect_ms",
            "detect_width",
            "detect_height",
        ]
        writer = csv.DictWriter(fp, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
