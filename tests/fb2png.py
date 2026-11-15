#!/usr/bin/env python3
"""Convert a raw RGB565 framebuffer dump to PNG: fb2png.py in.fb W H out.png"""
import struct
import sys
import zlib

src, w, h, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
raw = open(src, "rb").read()
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        (p,) = struct.unpack_from("<H", raw, (y * w + x) * 2)
        r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
        row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    rows.append(bytes(row))


def chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
