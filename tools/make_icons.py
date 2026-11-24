# Builds res/icon/island.ico from the SVG sources (run inside res/icon).
import struct, subprocess, io
from PIL import Image
entries = []
for size in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256):
    src = 'island-small.svg' if size <= 32 else 'island.svg'
    png = subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), src], capture_output=True, check=True).stdout
    Image.open(io.BytesIO(png)).verify()
    entries.append((size, png))
hdr = struct.pack('<HHH', 0, 1, len(entries)); off = 6 + 16 * len(entries); dirs = b''; data = b''
for size, png in entries:
    s = 0 if size == 256 else size
    dirs += struct.pack('<BBBBHHII', s, s, 0, 0, 1, 32, len(png), off + len(data)); data += png
open('island.ico', 'wb').write(hdr + dirs + data)
