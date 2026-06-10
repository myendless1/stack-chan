#!/usr/bin/env python3
import math
import struct
import zlib
from pathlib import Path


W = 320
H = 240
S = 3
OUT = Path(__file__).resolve().parents[1] / "main"


def lerp(a, b, t):
    return int(round(a + (b - a) * t))


def mix(c1, c2, t):
    return tuple(lerp(c1[i], c2[i], t) for i in range(3))


class Canvas:
    def __init__(self):
        self.w = W * S
        self.h = H * S
        self.pixels = [(0, 0, 0)] * (self.w * self.h)

    def blend(self, x, y, color, alpha=1.0):
        if x < 0 or y < 0 or x >= self.w or y >= self.h or alpha <= 0:
            return
        idx = y * self.w + x
        bg = self.pixels[idx]
        self.pixels[idx] = tuple(lerp(bg[i], color[i], alpha) for i in range(3))

    def fill_ellipse(self, cx, cy, rx, ry, fill, stroke=None, stroke_width=0, gradient=None):
        cx *= S
        cy *= S
        rx *= S
        ry *= S
        sw = stroke_width * S
        xmin = max(0, int(cx - rx - sw - 2))
        xmax = min(self.w - 1, int(cx + rx + sw + 2))
        ymin = max(0, int(cy - ry - sw - 2))
        ymax = min(self.h - 1, int(cy + ry + sw + 2))
        outer_rx = rx + sw / 2
        outer_ry = ry + sw / 2
        inner_rx = max(0.1, rx - sw / 2)
        inner_ry = max(0.1, ry - sw / 2)
        for y in range(ymin, ymax + 1):
            for x in range(xmin, xmax + 1):
                dx = (x + 0.5 - cx)
                dy = (y + 0.5 - cy)
                d_outer = dx * dx / (outer_rx * outer_rx) + dy * dy / (outer_ry * outer_ry)
                if d_outer > 1:
                    continue
                d_inner = dx * dx / (inner_rx * inner_rx) + dy * dy / (inner_ry * inner_ry)
                if stroke and d_inner > 1:
                    self.blend(x, y, stroke)
                elif fill:
                    color = gradient(y / S) if gradient else fill
                    self.blend(x, y, color)

    def stroke_polyline(self, points, color, width):
        radius = width * S / 2
        rr = radius * radius
        for a, b in zip(points, points[1:]):
            x1, y1 = a[0] * S, a[1] * S
            x2, y2 = b[0] * S, b[1] * S
            xmin = max(0, int(min(x1, x2) - radius - 2))
            xmax = min(self.w - 1, int(max(x1, x2) + radius + 2))
            ymin = max(0, int(min(y1, y2) - radius - 2))
            ymax = min(self.h - 1, int(max(y1, y2) + radius + 2))
            vx, vy = x2 - x1, y2 - y1
            length2 = vx * vx + vy * vy or 1
            for y in range(ymin, ymax + 1):
                for x in range(xmin, xmax + 1):
                    t = ((x + 0.5 - x1) * vx + (y + 0.5 - y1) * vy) / length2
                    t = min(1, max(0, t))
                    px = x1 + vx * t
                    py = y1 + vy * t
                    if (x + 0.5 - px) ** 2 + (y + 0.5 - py) ** 2 <= rr:
                        self.blend(x, y, color)

    def stroke_cubic(self, p0, p1, p2, p3, color, width):
        points = []
        for i in range(50):
            t = i / 49
            mt = 1 - t
            x = mt ** 3 * p0[0] + 3 * mt * mt * t * p1[0] + 3 * mt * t * t * p2[0] + t ** 3 * p3[0]
            y = mt ** 3 * p0[1] + 3 * mt * mt * t * p1[1] + 3 * mt * t * t * p2[1] + t ** 3 * p3[1]
            points.append((x, y))
        self.stroke_polyline(points, color, width)

    def fill_path(self, points, fill):
        pts = [(x * S, y * S) for x, y in points]
        ymin = max(0, int(min(y for _, y in pts)))
        ymax = min(self.h - 1, int(max(y for _, y in pts)))
        for y in range(ymin, ymax + 1):
            intersections = []
            for (x1, y1), (x2, y2) in zip(pts, pts[1:] + pts[:1]):
                if (y1 <= y + 0.5 < y2) or (y2 <= y + 0.5 < y1):
                    t = (y + 0.5 - y1) / (y2 - y1)
                    intersections.append(x1 + t * (x2 - x1))
            intersections.sort()
            for x1, x2 in zip(intersections[0::2], intersections[1::2]):
                for x in range(max(0, int(x1)), min(self.w - 1, int(x2)) + 1):
                    self.blend(x, y, fill)

    def downsample(self):
        rows = []
        for y in range(H):
            row = bytearray()
            for x in range(W):
                total = [0, 0, 0]
                for yy in range(S):
                    for xx in range(S):
                        p = self.pixels[(y * S + yy) * self.w + x * S + xx]
                        total[0] += p[0]
                        total[1] += p[1]
                        total[2] += p[2]
                row.extend(int(v / (S * S)) for v in total)
            rows.append(bytes(row))
        return rows

    def save(self, path):
        raw = b"".join(b"\x00" + row for row in self.downsample())
        def chunk(name, data):
            return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data) & 0xffffffff)
        png = (
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b"")
        )
        path.write_bytes(png)


def eye_gradient(y):
    if y < 86:
        return (5, 5, 5)
    if y < 132:
        return (5, 5, 5)
    return mix((5, 5, 5), (8, 115, 255), min(1, (y - 132) / 32))


def mouth_gradient(y):
    return mix((115, 186, 255), (8, 115, 255), min(1, max(0, (y - 174) / 33)))


def draw_common(c):
    for p in [((67, 63), (77, 54), (92, 52), (103, 56)), ((217, 56), (229, 51), (244, 54), (255, 64))]:
        c.stroke_cubic(*p, (12, 59, 128), 12)
        c.stroke_cubic(*p, (102, 173, 255), 8)
    for cx in (92, 228):
        c.fill_ellipse(cx, 123, 39, 39, (255, 255, 255))
        c.fill_ellipse(cx, 123, 37, 37, (255, 255, 255), (23, 61, 120), 2)
        c.fill_ellipse(cx, 127, 29, 31, (5, 5, 5), gradient=eye_gradient)
        c.fill_ellipse(cx + 10, 115, 7, 7, (255, 255, 255))
    for a, b in [((38, 190), (48, 176)), ((58, 190), (68, 176)), ((252, 190), (262, 176)), ((272, 190), (282, 176))]:
        c.stroke_polyline([a, b], (255, 196, 212), 8)


def draw_closed_eye(c, cx, cy=123, smile=True, width=14):
    c.fill_ellipse(cx, cy, 39, 39, (0, 0, 0))
    if smile:
        left = (cx - 30, cy + 16)
        mid = (cx, cy - 10)
        right = (cx + 30, cy + 16)
        p1 = (cx - 24, cy - 2)
        p2 = (cx - 10, cy - 12)
        p3 = (cx, cy - 10)
        p4 = (cx + 10, cy - 12)
        p5 = (cx + 24, cy - 2)
        c.stroke_cubic(left, p1, p2, mid, (23, 61, 120), width)
        c.stroke_cubic(mid, p4, p5, right, (23, 61, 120), width)
        c.stroke_cubic(left, p1, p2, mid, (255, 255, 255), width - 4)
        c.stroke_cubic(mid, p4, p5, right, (255, 255, 255), width - 4)
    else:
        c.stroke_cubic((cx - 28, cy), (cx - 10, cy + 8), (cx + 10, cy + 8), (cx + 28, cy), (23, 61, 120), width)
        c.stroke_cubic((cx - 28, cy), (cx - 10, cy + 8), (cx + 10, cy + 8), (cx + 28, cy), (102, 173, 255), width - 4)


def draw_half_eye(c, cx, cy=123):
    c.fill_ellipse(cx, cy, 39, 39, (0, 0, 0))
    c.fill_ellipse(cx, cy + 6, 30, 11, (5, 5, 5), gradient=eye_gradient)
    c.stroke_cubic((cx - 30, cy - 2), (cx - 12, cy + 9), (cx + 12, cy + 9), (cx + 30, cy - 2), (23, 61, 120), 10)
    c.stroke_cubic((cx - 30, cy - 2), (cx - 12, cy + 9), (cx + 12, cy + 9), (cx + 30, cy - 2), (255, 255, 255), 6)


def draw_mouth(c, outer, inner):
    c.fill_path(outer, (10, 61, 135))
    c.fill_path(inner, mouth_gradient(190))


def heart_points(cx, cy, scale):
    points = []
    for i in range(96):
        t = 2 * math.pi * i / 96
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        points.append((cx + x * scale, cy - y * scale))
    return points


def draw_heart(c, cx, cy, scale):
    outline = heart_points(cx, cy, scale * 1.08)
    body = heart_points(cx, cy, scale)
    shine = heart_points(cx - 7 * scale, cy - 5 * scale, scale * 0.18)
    c.fill_path(outline, (132, 28, 52))
    c.fill_path(body, (238, 66, 102))
    c.fill_path(shine, (255, 147, 170))


def draw_kiss_mouth(c):
    c.stroke_cubic((152, 175), (172, 164), (181, 176), (162, 186), (10, 61, 135), 11)
    c.stroke_cubic((162, 186), (181, 196), (170, 209), (151, 199), (10, 61, 135), 11)
    c.stroke_cubic((152, 175), (172, 164), (181, 176), (162, 186), (102, 173, 255), 7)
    c.stroke_cubic((162, 186), (181, 196), (170, 209), (151, 199), (102, 173, 255), 7)


def face(kind):
    c = Canvas()
    if kind == "happy_squint":
        draw_common(c)
        draw_closed_eye(c, 92, smile=True)
        draw_closed_eye(c, 228, smile=True)
        draw_mouth(c, [(136, 176), (140, 203), (150, 214), (160, 216), (170, 214), (180, 203), (184, 176)], [(140, 174), (144, 199), (152, 210), (160, 212), (168, 210), (176, 199), (180, 174)])
    elif kind == "happy_squint_soft":
        draw_common(c)
        draw_half_eye(c, 92)
        draw_half_eye(c, 228)
        draw_mouth(c, [(139, 178), (142, 202), (151, 211), (160, 211), (169, 211), (178, 202), (181, 178)], [(142, 176), (145, 198), (152, 207), (160, 207), (168, 207), (175, 198), (178, 176)])
    elif kind == "blink_half":
        draw_common(c)
        draw_half_eye(c, 92)
        draw_half_eye(c, 228)
        c.stroke_polyline([(145, 187), (175, 187)], (10, 61, 135), 12)
        c.stroke_polyline([(145, 187), (175, 187)], (102, 173, 255), 8)
    elif kind == "blink_closed":
        draw_common(c)
        draw_closed_eye(c, 92, smile=False, width=12)
        draw_closed_eye(c, 228, smile=False, width=12)
        c.stroke_polyline([(145, 187), (175, 187)], (10, 61, 135), 12)
        c.stroke_polyline([(145, 187), (175, 187)], (102, 173, 255), 8)
    elif kind == "shy":
        draw_common(c)
        draw_half_eye(c, 92)
        draw_half_eye(c, 228)
        for a, b in [((32, 197), (46, 177)), ((54, 199), (70, 177)), ((250, 199), (266, 177)), ((274, 197), (288, 177))]:
            c.stroke_polyline([a, b], (255, 126, 168), 10)
        draw_mouth(c, [(146, 183), (150, 195), (160, 199), (170, 195), (174, 183)], [(149, 181), (153, 191), (160, 195), (167, 191), (171, 181)])
    elif kind == "thinking":
        draw_common(c)
        c.stroke_cubic((62, 58), (75, 45), (94, 43), (110, 54), (102, 173, 255), 8)
        c.stroke_cubic((210, 69), (222, 62), (240, 63), (257, 75), (102, 173, 255), 8)
        c.stroke_cubic((144, 189), (153, 183), (167, 183), (176, 189), (10, 61, 135), 10)
        c.stroke_cubic((144, 189), (153, 183), (167, 183), (176, 189), (102, 173, 255), 6)
        c.fill_ellipse(198, 178, 5, 5, (102, 173, 255))
        c.fill_ellipse(209, 166, 4, 4, (102, 173, 255))
    elif kind == "wink_half":
        draw_common(c)
        draw_half_eye(c, 228)
        c.stroke_polyline([(145, 187), (175, 187)], (10, 61, 135), 12)
        c.stroke_polyline([(145, 187), (175, 187)], (102, 173, 255), 8)
    elif kind == "wink_closed":
        draw_common(c)
        draw_closed_eye(c, 228, smile=True, width=14)
        c.stroke_cubic((144, 187), (152, 194), (168, 194), (176, 187), (10, 61, 135), 10)
        c.stroke_cubic((144, 187), (152, 194), (168, 194), (176, 187), (102, 173, 255), 6)
    elif kind == "heart_small":
        draw_common(c)
        draw_closed_eye(c, 228, smile=True, width=14)
        draw_kiss_mouth(c)
        draw_heart(c, 207, 181, 1.05)
    elif kind == "heart":
        draw_common(c)
        draw_closed_eye(c, 228, smile=True, width=14)
        draw_kiss_mouth(c)
        draw_heart(c, 226, 180, 1.55)
    elif kind == "nod_soft":
        draw_common(c)
        draw_half_eye(c, 92)
        draw_half_eye(c, 228)
        c.stroke_cubic((144, 187), (152, 194), (168, 194), (176, 187), (10, 61, 135), 10)
        c.stroke_cubic((144, 187), (152, 194), (168, 194), (176, 187), (102, 173, 255), 6)
    elif kind == "nod_down":
        draw_common(c)
        draw_closed_eye(c, 92, smile=False, width=12)
        draw_closed_eye(c, 228, smile=False, width=12)
        c.stroke_cubic((144, 190), (152, 198), (168, 198), (176, 190), (10, 61, 135), 10)
        c.stroke_cubic((144, 190), (152, 198), (168, 198), (176, 190), (102, 173, 255), 6)
    else:
        draw_common(c)
        if kind == "calm":
            c.stroke_polyline([(145, 187), (175, 187)], (10, 61, 135), 12)
            c.stroke_polyline([(145, 187), (175, 187)], (102, 173, 255), 8)
        elif kind == "speak1":
            draw_mouth(c, [(139, 178), (141, 194), (150, 201), (160, 201), (170, 201), (179, 194), (181, 178)], [(142, 176), (144, 190), (151, 197), (160, 197), (169, 197), (176, 190), (178, 176)])
        elif kind == "speak2":
            draw_mouth(c, [(135, 176), (137, 200), (148, 211), (160, 211), (172, 211), (183, 200), (185, 176)], [(138, 174), (140, 196), (149, 207), (160, 207), (171, 207), (180, 196), (182, 174)])
    return c


for name in (
    "calm",
    "speak1",
    "speak2",
    "shy",
    "thinking",
    "blink_half",
    "blink_closed",
    "wink_half",
    "wink_closed",
    "heart_small",
    "heart",
    "nod_soft",
    "nod_down",
    "happy_squint",
    "happy_squint_soft",
):
    face(name).save(OUT / f"{name}_face.png")
    print(f"rendered {OUT / f'{name}_face.png'}")
