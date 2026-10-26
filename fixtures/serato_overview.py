"""
Adds a synthetic rainbow Serato Overview tag to an MP3, AIFF, or FLAC file, stored the way Serato stores it.

The overview has 240 slices in six bands of 40: red, yellow, green, cyan, blue, and magenta.
Every slice has two background cells at the top, twelve cells of its band colour,
then one reserved palette index and one more background cell, which the colouring must both skip.
MP3 files get an ID3v2.4 tag with one GEOB frame in front of the audio,
and AIFF files get the same tag in an "ID3 " chunk.
FLAC files get a SERATO_OVERVIEW Vorbis comment instead,
holding base64 of the GEOB wrapper and the payload in lines of 72 characters.
The input must not have an ID3v2 tag or a Serato comment yet.
Only the standard library is used, so the fixtures need no Python packages.
"""

import base64
import struct
import sys
from pathlib import Path

SLICE_COUNT = 240

# Palette indices of the band colours in Serato's 6x6x6 colour cube.
BAND_COLOURS = [180, 210, 30, 35, 5, 185]

# Palette index from the unverified range 216 to 255.
RESERVED_INDEX = 250

DESCRIPTION = "Serato Overview"

MIME_TYPE = "application/octet-stream"

# Length of the base64 lines in a native FLAC value.
BASE64_LINE_LENGTH = 72

# FLAC metadata block types.
FLAC_STREAMINFO = 0
FLAC_VORBIS_COMMENT = 4


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


def id3_tag() -> bytes:
    """Returns an ID3v2.4 tag with one GEOB frame holding the overview, with a Latin-1 description."""
    body = b"\x00" + MIME_TYPE.encode() + b"\x00\x00" + DESCRIPTION.encode("latin-1") + b"\x00" + overview_payload()
    frame = b"GEOB" + synchsafe(len(body)) + b"\x00\x00" + body
    return b"ID3" + struct.pack("BBB", 4, 0, 0) + synchsafe(len(frame)) + frame


def tag_mp3(audio: bytes) -> bytes:
    """Puts the tag in front of the MPEG frames."""
    if audio.startswith(b"ID3"):
        sys.exit("The MP3 file already has an ID3v2 tag")
    return id3_tag() + audio


def tag_aiff(audio: bytes) -> bytes:
    """Appends an "ID3 " chunk to the FORM container and updates its size."""
    if audio[:4] != b"FORM" or audio[8:12] not in (b"AIFF", b"AIFC"):
        sys.exit("The file is not an AIFF file")
    if b"ID3 " in audio:
        sys.exit("The AIFF file already has an ID3 chunk")
    tag = id3_tag()
    chunk = b"ID3 " + struct.pack(">I", len(tag)) + tag + (b"\x00" if len(tag) % 2 else b"")
    body = audio[8:] + chunk
    return b"FORM" + struct.pack(">I", len(body)) + body


def native_value() -> str:
    """Returns the SERATO_OVERVIEW comment value: base64 of the wrapped payload, split into lines."""
    wrapper = MIME_TYPE.encode() + b"\x00\x00" + DESCRIPTION.encode() + b"\x00" + overview_payload()
    encoded = base64.b64encode(wrapper).decode("ascii")
    return "\n".join(
        encoded[start : start + BASE64_LINE_LENGTH] for start in range(0, len(encoded), BASE64_LINE_LENGTH)
    )


def vorbis_comment_block(vendor: bytes, comments: list[bytes]) -> bytes:
    """Returns the body of a Vorbis comment metadata block."""
    body = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(comments))
    for comment in comments:
        body += struct.pack("<I", len(comment)) + comment
    return body


def parse_vorbis_comments(body: bytes) -> tuple[bytes, list[bytes]]:
    """Returns the vendor string and the comments of a Vorbis comment metadata block."""
    (vendor_length,) = struct.unpack_from("<I", body, 0)
    offset = 4 + vendor_length
    vendor = body[4:offset]
    (count,) = struct.unpack_from("<I", body, offset)
    offset += 4
    comments = []
    for _ in range(count):
        (length,) = struct.unpack_from("<I", body, offset)
        comments.append(body[offset + 4 : offset + 4 + length])
        offset += 4 + length
    return vendor, comments


def tag_flac(audio: bytes) -> bytes:
    """Adds the SERATO_OVERVIEW comment to the Vorbis comment block, creating the block when there is none."""
    if audio[:4] != b"fLaC":
        sys.exit("The file is not a FLAC file")
    blocks: list[tuple[int, bytes]] = []
    offset = 4
    while True:
        header = audio[offset]
        block_type = header & 0x7F
        length = int.from_bytes(audio[offset + 1 : offset + 4], "big")
        blocks.append((block_type, audio[offset + 4 : offset + 4 + length]))
        offset += 4 + length
        if header & 0x80:
            break
    comment = f"SERATO_OVERVIEW={native_value()}".encode()
    for index, (block_type, body) in enumerate(blocks):
        if block_type == FLAC_VORBIS_COMMENT:
            vendor, comments = parse_vorbis_comments(body)
            if any(existing.upper().startswith(b"SERATO_OVERVIEW=") for existing in comments):
                sys.exit("The FLAC file already has a Serato Overview")
            blocks[index] = (block_type, vorbis_comment_block(vendor, comments + [comment]))
            break
    else:
        streaminfo = next(index for index, (block_type, _) in enumerate(blocks) if block_type == FLAC_STREAMINFO)
        blocks.insert(streaminfo + 1, (FLAC_VORBIS_COMMENT, vorbis_comment_block(b"", [comment])))
    output = bytearray(b"fLaC")
    for index, (block_type, body) in enumerate(blocks):
        last = 0x80 if index == len(blocks) - 1 else 0
        output += bytes([last | block_type]) + len(body).to_bytes(3, "big") + body
    return bytes(output + audio[offset:])


def main() -> None:
    path = Path(sys.argv[1])
    taggers = {".mp3": tag_mp3, ".aif": tag_aiff, ".aiff": tag_aiff, ".flac": tag_flac}
    tagger = taggers.get(path.suffix.lower())
    if tagger is None:
        sys.exit(f"Unsupported file type: {path}")
    path.write_bytes(tagger(path.read_bytes()))


if __name__ == "__main__":
    main()
