"""
Adds a synthetic rainbow Serato Overview tag to an MP3 file that has no ID3v2 tag.

The overview has 240 slices in six bands of 40: red, yellow, green, cyan, blue, and magenta.
Every slice has two background cells at the top, twelve cells of its band colour,
then one reserved palette index and one more background cell, which the colouring must both skip.
The tag is ID3v2.4 with one GEOB frame, the way Serato stores the overview in MP3 files.
Only the standard library is used, so the fixtures need no Python packages.
"""

import struct
import sys
from pathlib import Path

SLICE_COUNT = 240

# Palette indices of the band colours in Serato's 6x6x6 colour cube.
BAND_COLOURS = [180, 210, 30, 35, 5, 185]

# Palette index from the unverified range 216 to 255.
RESERVED_INDEX = 250


def overview_payload() -> bytes:
    """Returns the tag payload: the 01 05 header followed by the slices."""
    slices_per_band = SLICE_COUNT // len(BAND_COLOURS)
    payload = bytearray([0x01, 0x05])
    for index in range(SLICE_COUNT):
        colour = BAND_COLOURS[index // slices_per_band]
        payload += bytes([0, 1] + [colour] * 12 + [RESERVED_INDEX, 0])
    return bytes(payload)


def synchsafe(value: int) -> bytes:
    """Returns a 28 bit value as the four synchsafe bytes ID3v2.4 uses for sizes."""
    return bytes((value >> shift) & 0x7F for shift in (21, 14, 7, 0))


def geob_frame(description: str, data: bytes) -> bytes:
    """Returns an ID3v2.4 GEOB frame with a Latin-1 description and an empty file name."""
    body = b"\x00" + b"application/octet-stream\x00" + b"\x00" + description.encode("latin-1") + b"\x00" + data
    return b"GEOB" + synchsafe(len(body)) + b"\x00\x00" + body


def main() -> None:
    path = Path(sys.argv[1])
    audio = path.read_bytes()
    if audio.startswith(b"ID3"):
        sys.exit(f"{path} already has an ID3v2 tag")
    frames = geob_frame("Serato Overview", overview_payload())
    header = b"ID3" + struct.pack("BBB", 4, 0, 0) + synchsafe(len(frames))
    path.write_bytes(header + frames + audio)


if __name__ == "__main__":
    main()
