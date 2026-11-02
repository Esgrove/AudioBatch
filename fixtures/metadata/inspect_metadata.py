#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Inspect real fixture metadata independently of either application's parsers."""

import argparse
import base64
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

AUDIO_SUFFIXES = {".aif", ".flac", ".mp3"}
ID3_V24 = 4
FLAC_VORBIS_COMMENT = 4
ANALYSIS_BYTES = 2
PALETTE_COLOURS = 216
NATIVE_FIELDS = {
    "SERATO_ANALYSIS",
    "SERATO_AUTOGAIN",
    "SERATO_BEATGRID",
    "SERATO_MARKERS_V2",
    "SERATO_MARKERS",
    "SERATO_OVERVIEW",
}


def synchsafe(data: bytes) -> int:
    """Decodes ID3's four seven-bit size bytes without using a tag-reading library."""
    return sum(byte << (7 * (3 - index)) for index, byte in enumerate(data))


def geob(body: bytes) -> tuple[str, bytes]:
    """Extracts the encoded description and raw payload of an ID3 GEOB frame."""
    encoding = body[0]
    _, rest = body[1:].split(b"\0", 1)
    terminator = b"\0\0" if encoding in (1, 2) else b"\0"
    codec = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}[encoding]
    fields = []
    for _ in range(2):
        end = next(
            index
            for index in range(0, len(rest), len(terminator))
            if rest[index : index + len(terminator)] == terminator
        )
        fields.append(rest[:end].decode(codec))
        rest = rest[end + len(terminator) :]
    return fields[1], rest


def id3(data: bytes) -> dict[str, bytes]:
    """Reads unflagged ID3v2.3 and ID3v2.4 GEOB frames directly from their container bytes."""
    if data[:3] != b"ID3" or data[3] not in (3, 4) or data[5] != 0:
        message = "Expected an unflagged ID3v2.3 or ID3v2.4 tag"
        raise ValueError(message)
    version = data[3]
    end = 10 + synchsafe(data[6:10])
    offset = 10
    values = {}
    while offset + 10 <= end and data[offset] != 0:
        kind = data[offset : offset + 4]
        size = data[offset + 4 : offset + 8]
        length = synchsafe(size) if version == ID3_V24 else int.from_bytes(size, "big")
        if data[offset + 8 : offset + 10] != b"\0\0":
            message = f"Unsupported ID3 frame flags: {kind!r}"
            raise ValueError(message)
        body = data[offset + 10 : offset + 10 + length]
        if len(body) != length:
            message = f"Truncated ID3 frame: {kind!r}"
            raise ValueError(message)
        if kind == b"GEOB":
            description, payload = geob(body)
            values[description] = payload
        offset += 10 + length
    return values


def native_payload(value: str) -> tuple[str, bytes]:
    """Unwraps native Serato base64, accounting for its zero sextet and structural padding."""
    cleaned = "".join(value.split())
    if len(cleaned) % 4 == 1:
        if not cleaned.endswith("A"):
            message = "Native base64 has a nonzero dangling sextet"
            raise ValueError(message)
        cleaned = cleaned[:-1]
    wrapped = base64.b64decode(cleaned + "=" * (-len(cleaned) % 4), validate=True)
    mime, filename, description, payload = wrapped.split(b"\0", 3)
    if mime != b"application/octet-stream" or filename:
        message = "Unexpected native GEOB wrapper"
        raise ValueError(message)
    name = description.decode()
    if name == "Serato Overview" and (len(payload) - 2) % 16 == 1 and payload[-1] == 0:
        payload = payload[:-1]
    if name == "Serato BeatGrid":
        expected = 7 + int.from_bytes(payload[2:6], "big") * 8
        if len(payload) < expected or any(payload[expected:]):
            message = "Truncated or nonzero-padded native BeatGrid"
            raise ValueError(message)
        payload = payload[:expected]
    if name == "Serato Analysis":
        if len(payload) < ANALYSIS_BYTES or any(payload[ANALYSIS_BYTES:]):
            message = "Unexpected native Analysis payload"
            raise ValueError(message)
        payload = payload[:2]
    return name, payload


def container_tags(path: Path, data: bytes) -> dict[str, bytes]:
    """Finds raw Serato GEOB payloads in MP3, AIFF chunks, or native FLAC comments."""
    if path.suffix.lower() == ".mp3":
        return id3(data)
    if path.suffix.lower() == ".aif":
        if data[:4] != b"FORM" or data[8:12] not in (b"AIFF", b"AIFC"):
            message = f"Not an AIFF file: {path}"
            raise ValueError(message)
        offset = 12
        while offset + 8 <= len(data):
            kind = data[offset : offset + 4]
            length = int.from_bytes(data[offset + 4 : offset + 8], "big")
            if kind == b"ID3 ":
                return id3(data[offset + 8 : offset + 8 + length])
            offset += 8 + length + length % 2
        message = f"Missing AIFF ID3 tag: {path}"
        raise ValueError(message)
    if data[:4] != b"fLaC":
        message = f"Not a FLAC file: {path}"
        raise ValueError(message)
    offset = 4
    values = {}
    while offset + 4 <= len(data):
        header = data[offset]
        length = int.from_bytes(data[offset + 1 : offset + 4], "big")
        body = data[offset + 4 : offset + 4 + length]
        if len(body) != length:
            message = f"Truncated FLAC metadata block: {path}"
            raise ValueError(message)
        if header & 0x7F == FLAC_VORBIS_COMMENT:
            vendor = int.from_bytes(body[:4], "little")
            cursor = 4 + vendor
            count = int.from_bytes(body[cursor : cursor + 4], "little")
            cursor += 4
            for _ in range(count):
                size = int.from_bytes(body[cursor : cursor + 4], "little")
                cursor += 4
                text = body[cursor : cursor + size].decode()
                cursor += size
                name, value = text.split("=", 1)
                if name.upper() in NATIVE_FIELDS:
                    description, payload = native_payload(value)
                    values[description] = payload
        offset += 4 + length
        if header & 0x80:
            return values
    message = f"Missing final FLAC metadata block: {path}"
    raise ValueError(message)


def marker_entries(data: bytes) -> list[tuple[str, bytes]]:
    """Decodes length-delimited Markers2 entries, retaining their unparsed bodies."""
    text = b"".join(data[2:].split(b"\0", 1)[0].split())
    if len(text) % 4 == 1:
        text += b"A"
    decoded = base64.b64decode(text + b"=" * (-len(text) % 4), validate=True)
    if data[:2] != b"\1\1" or decoded[:2] != b"\1\1":
        message = "Unexpected Markers2 version"
        raise ValueError(message)
    cursor = 2
    entries = []
    while cursor < len(decoded) and decoded[cursor] != 0:
        end = decoded.index(0, cursor)
        name = decoded[cursor:end].decode()
        length = int.from_bytes(decoded[end + 1 : end + 5], "big")
        body = decoded[end + 5 : end + 5 + length]
        if len(body) != length:
            message = f"Truncated Markers2 entry: {name}"
            raise ValueError(message)
        entries.append((name, body))
        cursor = end + 5 + length
    return entries


def overview_info(data: bytes) -> dict:
    """Checksums every palette byte and independently averages representative slice colours."""
    cells = data[2:]
    if data[:2] != b"\1\5" or not cells or len(cells) % 16:
        message = "Unexpected Overview header or partial raster"
        raise ValueError(message)
    count = len(cells) // 16
    checksum = 14695981039346656037
    for byte in cells:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    samples = []
    for index in (0, 1, count // 4, count // 2, 3 * count // 4, count - 1):
        row = list(cells[index * 16 : (index + 1) * 16])
        valid = [byte for byte in row if 1 < byte < PALETTE_COLOURS]
        rgb = None
        if valid:
            channels = [
                sum((byte // 36, byte % 36 // 6, byte % 6)[channel] * 51 for byte in valid) for channel in range(3)
            ]
            normalized = [binary32(value / (len(valid) * 255)) for value in channels]
            scaled = [binary32(value * 255) for value in normalized]
            rgb = sum(int(value + 0.5) << shift for value, shift in zip(scaled, (16, 8, 0), strict=True))
        samples.append({"index": index, "cells": row, "rgb": rgb})
    return {"slices": count, "fnv1a64": f"{checksum:016x}", "samples": samples}


def binary32(value: float) -> float:
    """Rounds through IEEE binary32 independently of either application's colour implementation."""
    return struct.unpack(">f", struct.pack(">f", value))[0]


def inspect(path: Path) -> dict:
    """Captures one file's raw Serato values, source identity, and ffprobe ordinary text tags."""
    raw = path.read_bytes()
    tags = container_tags(path, raw)
    ffprobe = shutil.which("ffprobe")
    if ffprobe is None:
        message = "ffprobe is required for ordinary text-tag inspection"
        raise FileNotFoundError(message)
    # The resolved executable receives the absolute filename as data, without a shell.
    probe = subprocess.run(  # noqa: S603
        [ffprobe, "-v", "error", "-show_entries", "format_tags", "-of", "json", str(path.resolve())],
        check=True,
        capture_output=True,
        encoding="utf-8",
    )
    ordinary = {key.upper(): value for key, value in json.loads(probe.stdout)["format"].get("tags", {}).items()}
    cues, loops, unknown = [], [], []
    track_color, bpm_locked = None, None
    for name, body in marker_entries(tags["Serato Markers2"]):
        if name == "BPMLOCK":
            bpm_locked = body[0] != 0
        elif name == "COLOR":
            track_color = int.from_bytes(body[1:4], "big")
        elif name == "CUE":
            cues.append(
                {
                    "index": body[1],
                    "position_ms": int.from_bytes(body[2:6], "big"),
                    "rgb": int.from_bytes(body[7:10], "big"),
                    "name": body[12:].split(b"\0", 1)[0].decode(),
                }
            )
        elif name == "LOOP":
            loops.append(
                {
                    "index": body[1],
                    "start_ms": int.from_bytes(body[2:6], "big", signed=True),
                    "end_ms": int.from_bytes(body[6:10], "big", signed=True),
                    "rgb": int.from_bytes(body[15:18], "big"),
                    "locked": body[19] != 0,
                    "name": body[20:].split(b"\0", 1)[0].decode(),
                }
            )
        else:
            unknown.append({"name": name, "hex": body.hex()})
    grid_data = tags.get("Serato BeatGrid")
    count = int.from_bytes(grid_data[2:6], "big") if grid_data is not None else 0
    grid = [] if grid_data is not None else None
    for index in range(count):
        row = grid_data[6 + 8 * index : 14 + 8 * index]
        grid.append(
            {
                "position_seconds": struct.unpack(">f", row[:4])[0],
                "beats_until_next": int.from_bytes(row[4:], "big") if index + 1 < count else 0,
                "bpm": struct.unpack(">f", row[4:])[0] if index + 1 == count else None,
            }
        )
    autotags = tags["Serato Autotags"]
    if autotags[:2] != b"\1\1":
        message = f"Unexpected Autotags version: {path}"
        raise ValueError(message)
    return {
        "file": path.name,
        "sha256": hashlib.sha256(raw).hexdigest(),
        "title": ordinary["TITLE"],
        "artist": ordinary["ARTIST"],
        "album": ordinary["ALBUM"],
        "genre": ordinary["GENRE"],
        "comment": ordinary["COMMENT"],
        "year": int(ordinary["DATE"]) if "DATE" in ordinary else None,
        "bpm": ordinary["TBPM"] if "TBPM" in ordinary else ordinary["BPM"],
        "key": ordinary["TKEY"] if "TKEY" in ordinary else ordinary["INITIALKEY"],
        "analysis": list(tags["Serato Analysis"]) if "Serato Analysis" in tags else None,
        "autotags": [float(text) for text in autotags[2:].split(b"\0") if text],
        "track_rgb": track_color,
        "bpm_locked": bpm_locked,
        "cues": cues,
        "loops": loops,
        "unknown_markers": unknown,
        "beatgrid": grid,
        "overview": overview_info(tags["Serato Overview"]),
    }


def main() -> int:
    """Prints or saves an independent capture, or verifies reviewed expectations without changing them."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--directory",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Fixture directory, defaulting to this script's directory",
    )
    parser.add_argument("--output", type=Path, help="Write inspection JSON here instead of stdout")
    parser.add_argument(
        "--check", action="store_true", help="Compare with expected.json without rewriting expectations"
    )
    args = parser.parse_args()
    paths = sorted(path for path in args.directory.iterdir() if path.suffix.lower() in AUDIO_SUFFIXES)
    if not paths:
        parser.error(f"No AIFF, FLAC, or MP3 fixtures found in {args.directory}")
    cases = [inspect(path) for path in paths]
    if args.check:
        expected = {
            case["file"]: case for case in json.loads((args.directory / "expected.json").read_text(encoding="utf-8"))
        }
        if set(expected) != {case["file"] for case in cases}:
            parser.error("Fixture inventory differs from expected.json")
        mismatches = []
        for capture in cases:
            case = capture.copy()
            unknown = case.pop("unknown_markers")
            if unknown:
                mismatches.append(f"{case['file']}: unexpected marker entries")
            for key, value in case.items():
                if value != expected[case["file"]].get(key):
                    mismatches.append(f"{case['file']}: {key}")
        if mismatches:
            print("Metadata expectations differ:\n" + "\n".join(mismatches), file=sys.stderr)
            return 1
        print(f"Independent metadata expectations match all {len(cases)} files", file=sys.stderr)
    if args.output or not args.check:
        text = json.dumps(cases, indent=2, ensure_ascii=True) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
