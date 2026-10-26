"""Read one 8-bit pbemu framebuffer in its IPC namespace; emit a PGM."""
import ctypes
from pathlib import Path
import sys

fields = dict(token.split("=", 1) for token in Path(sys.argv[1]).read_text().split())
width, height, stride, offset, size = (
    int(fields[name], 0)
    for name in ("width", "height", "stride", "pixel_data_offset", "fb_size")
)
assert int(fields["depth"]) == 8, "Only grayscale 8-bit snapshots are supported"
assert 0 < width <= stride and height > 0 and 0 <= offset <= size - stride * height
libc = ctypes.CDLL(None, use_errno=True)
libc.shmget.argtypes = [ctypes.c_int, ctypes.c_size_t, ctypes.c_int]
libc.shmat.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
libc.shmat.restype = ctypes.c_void_p
libc.shmdt.argtypes = [ctypes.c_void_p]
segment = libc.shmget(int(fields["fb_key"], 0), size, 0)
assert segment >= 0, ctypes.get_errno()
address = libc.shmat(segment, None, 0o10000)  # SHM_RDONLY
assert address != ctypes.c_void_p(-1).value, ctypes.get_errno()
try:
    pixels = ctypes.string_at(address + offset, stride * height)
finally:
    libc.shmdt(address)
sys.stdout.buffer.write(f"P5\n{width} {height}\n255\n".encode())
for row in range(height):
    sys.stdout.buffer.write(pixels[row * stride:row * stride + width])
