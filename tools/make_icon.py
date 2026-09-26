"""Generates src/launcher/RetroLaunch.ico: a sun (Project Sun) in 16, 24, 32,
48 and 256 pixel sizes, drawn with 4x4 supersampling.

    python tools/make_icon.py src/launcher/RetroLaunch.ico
"""

import math
import struct
import sys
import zlib


def render(size):
    """RGBA rows, top to bottom."""
    ss = 4
    cx = cy = size / 2.0
    disc = size * 0.26
    ray_inner, ray_outer, ray_width = size * 0.33, size * 0.48, 0.20  # width in radians
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss - cx
                    py = y + (sy + 0.5) / ss - cy
                    d = math.hypot(px, py)
                    color = None
                    if d <= disc:
                        t = d / disc  # yellow centre to orange edge
                        color = (255, int(214 - 70 * t), int(40 - 30 * t))
                    elif ray_inner <= d <= ray_outer:
                        angle = math.atan2(py, px) % (2 * math.pi / 12)
                        if abs(angle - math.pi / 12) <= ray_width * (1 - (d - ray_inner) / (ray_outer - ray_inner)) / 2 + 0.03:
                            color = (255, 160, 20)
                    if color:
                        r += color[0]
                        g += color[1]
                        b += color[2]
                        a += 255
            n = ss * ss
            if a:
                row.append((int(r / (a / 255)), int(g / (a / 255)), int(b / (a / 255)), int(a / n)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def bmp_image(size):
    rows = render(size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    mask_row = ((size + 31) // 32) * 4
    mask = bytes(mask_row * size)  # all zero: alpha decides
    return header + pixels + mask


def png_image(size):
    rows = render(size)
    raw = b"".join(b"\x00" + bytes(v for px in row for v in px) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    sizes = [16, 24, 32, 48, 256]
    images = [png_image(s) if s == 256 else bmp_image(s) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in zip(sizes, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(images)
    with open(sys.argv[1], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
