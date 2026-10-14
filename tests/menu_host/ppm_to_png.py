#!/usr/bin/env python3
"""Convert the host LVGL framebuffer (P6 RGB) to a portable PNG, no Pillow."""
import struct
import sys
import zlib
from pathlib import Path


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def main(src, dst):
    raw = Path(src).read_bytes()
    header = b"P6\n320 240\n255\n"
    if not raw.startswith(header) or len(raw) != len(header) + 320 * 240 * 3:
        raise ValueError("unexpected framebuffer format")
    pixels = raw[len(header):]
    rows = b"".join(b"\0" + pixels[i:i + 320 * 3] for i in range(0, len(pixels), 320 * 3))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 240, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    Path(dst).write_bytes(png)
    print(f"PNG 320x240 RGB: {dst} ({len(png)} bytes)")


if __name__ == "__main__":
    main(*sys.argv[1:])
