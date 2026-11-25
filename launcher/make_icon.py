"""Velox Preview 0.1 用マルチ解像度 Windows アイコン (.ico) 生成器（標準ライブラリのみ）。"""
from __future__ import annotations

import math
from pathlib import Path
import struct
import sys
import zlib


def _png_chunk(ctype: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", crc)


def _encode_rgba_png(size: int, pixels: bytearray) -> bytes:
    stride = size * 4
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        raw.extend(pixels[y * stride : (y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + _png_chunk(b"IEND", b"")
    )


def _render_icon(size: int) -> bytes:
    px = bytearray(size * size * 4)
    r_corner = size * 0.22
    pad = size * 0.05
    for y in range(size):
        for x in range(size):
            # 角丸四角形の距離関数
            cx = max(pad + r_corner, min(size - pad - r_corner, x + 0.5))
            cy = max(pad + r_corner, min(size - pad - r_corner, y + 0.5))
            dist = math.hypot((x + 0.5) - cx, (y + 0.5) - cy)
            alpha = max(0.0, min(1.0, r_corner - dist + 0.7))
            if alpha <= 0.0:
                continue

            # ダークネイビー → ディープブルーの斜めグラデーション
            t = (x + y) / (2.0 * size)
            bg_r = int(15 + 14 * t)
            bg_g = int(23 + 32 * t)
            bg_b = int(42 + 64 * t)

            # 細いアクセント枠
            border = 1.0 if abs(dist - (r_corner - size * 0.03)) < max(1.0, size * 0.025) else 0.0
            if border:
                bg_r = int(bg_r * 0.5 + 59 * 0.5)
                bg_g = int(bg_g * 0.5 + 130 * 0.5)
                bg_b = int(bg_b * 0.5 + 246 * 0.5)

            # 中央のシャープな "V" マーク（正規化座標）
            nx = (x + 0.5) / size
            ny = (y + 0.5) / size
            in_v = False
            if 0.22 <= ny <= 0.80:
                # 左右の V 腕の中心 X
                prog = (ny - 0.22) / (0.80 - 0.22)
                left_x = 0.23 + prog * (0.50 - 0.23)
                right_x = 0.77 - prog * (0.77 - 0.50)
                half_w = 0.072 * (1.0 - 0.15 * prog)
                if abs(nx - left_x) <= half_w or abs(nx - right_x) <= half_w:
                    in_v = True

            if in_v:
                # 明るいシアン〜ブルーグラデーション
                r = int(56 + 40 * (1.0 - ny))
                g = int(189 + 45 * (1.0 - ny))
                b = 255
            else:
                r, g, b = bg_r, bg_g, bg_b

            i = (y * size + x) * 4
            px[i] = min(255, max(0, r))
            px[i + 1] = min(255, max(0, g))
            px[i + 2] = min(255, max(0, b))
            px[i + 3] = int(alpha * 255)
    return _encode_rgba_png(size, px)


def make_ico(out_path: Path) -> None:
    sizes = [16, 32, 48, 64, 256]
    imgs = [_render_icon(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = bytearray()
    for s, blob in zip(sizes, imgs):
        dim = 0 if s >= 256 else s
        entries.extend(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset))
        offset += len(blob)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + bytes(entries) + b"".join(imgs))


if __name__ == "__main__":
    dst = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "velox.ico"
    make_ico(dst)
    print(f"Generated icon: {dst} ({dst.stat().st_size} bytes)")
