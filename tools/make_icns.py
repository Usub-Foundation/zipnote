"""Packs PNG renders of the app icon into a macOS .icns file (no iconutil needed, works on any OS).

Usage: python3 tools/make_icns.py <dir with icon_<size>.png> resources/icons/zipnotes.icns
The PNGs (16..1024 px) can be rendered from resources/icons/zipnotes.svg with any SVG rasterizer.
"""
import struct
import sys
from pathlib import Path

# icns element types that hold PNG data, by pixel size; @2x variants reuse the larger renders
TYPES = [
    ("icp4", 16), ("icp5", 32), ("icp6", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512), ("ic10", 1024),
    ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512),
]

src, dst = Path(sys.argv[1]), Path(sys.argv[2])
chunks = []
for kind, size in TYPES:
    data = (src / f"icon_{size}.png").read_bytes()
    chunks.append(kind.encode("ascii") + struct.pack(">I", len(data) + 8) + data)
body = b"".join(chunks)
dst.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
print(f"{dst}: {len(body) + 8} bytes, {len(chunks)} images")
