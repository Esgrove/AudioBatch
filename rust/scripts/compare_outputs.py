#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0", "numpy>=2"]
# ///
"""
Compares the files the C++ and the Rust CLI leave behind after normalizing their own copies of the same inputs.

Both folders must hold the same file names after the sources were moved to the trash,
and the sources that failed to normalize must be left as they were.
Every AIFF output pair must have the same container form and COMM chunk, the same samples, and the same ID3v2 frames.
With --sources, the outputs of lossless sources must have identical samples,
and only the outputs of lossy sources, which the two decoders may decode differently, may differ by the tolerance.
Without it, every output may differ by the tolerance.
Frames are compared by their decoded fields, so a different text encoding of the same text is no difference,
and a frame that appears more often in one output than in the other is a difference too.
Exits with code 1 when anything differs.

Usage: uv run compare_outputs.py <C++ folder> <Rust folder> [--tolerance <dBFS>] [--sources <list file>]
"""

import argparse
import hashlib
import struct
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from mutagen.aiff import AIFF
from mutagen.id3 import Frame

AIFF_EXTENSIONS = {".aif", ".aiff"}
# Sources whose decoders may differ between the two applications, so their outputs are compared with the tolerance.
LOSSY_EXTENSIONS = {".mp3", ".m4a", ".aac", ".mp4", ".ogg", ".oga", ".opus"}
# Binary frame payloads longer than this are compared by their hash, which keeps the report short.
HASHED_PAYLOAD_BYTES = 64
ID3_HEADER_SIZE = 10
ID3_EXTENDED_HEADER_FLAG = 0x40
# ID3v2.4 stores frame and extended header sizes as synchsafe integers, while earlier versions use plain ones.
ID3_SYNCHSAFE_VERSION = 4


def read_chunks(path: Path) -> tuple[bytes, dict[bytes, bytes]]:
    """Returns the form type of an AIFF file and its chunks by ID."""
    data = path.read_bytes()
    chunks = {}
    position = 12
    while position + 8 <= len(data):
        chunk_id = data[position : position + 4]
        size = struct.unpack(">I", data[position + 4 : position + 8])[0]
        chunks[chunk_id] = data[position + 8 : position + 8 + size]
        position += 8 + size + (size & 1)
    return data[8:12], chunks


def decode_samples(chunks: dict[bytes, bytes]) -> tuple[np.ndarray, int]:
    """Returns the integer samples of the SSND chunk and their full scale value."""
    bits = struct.unpack(">h", chunks[b"COMM"][6:8])[0]
    width = (bits + 7) // 8
    raw = np.frombuffer(chunks[b"SSND"][8:], dtype=np.uint8)
    raw = raw[: len(raw) - len(raw) % width].reshape(-1, width).astype(np.int64)
    values = np.zeros(len(raw), dtype=np.int64)
    for byte in range(width):
        values = (values << 8) | raw[:, byte]
    sign = 1 << (width * 8 - 1)
    return np.where(values >= sign, values - 2 * sign, values), sign


def synchsafe(data: bytes) -> int:
    """Returns the value of a four byte ID3v2 synchsafe integer."""
    return (data[0] << 21) | (data[1] << 14) | (data[2] << 7) | data[3]


def raw_frame_ids(chunks: dict[bytes, bytes]) -> Counter:
    """Returns how often each frame ID appears in the ID3 chunk, which mutagen merges or drops when repeated."""
    tag = chunks.get(b"ID3 ") or chunks.get(b"id3 ")
    if tag is None or len(tag) < ID3_HEADER_SIZE or tag[:3] != b"ID3":
        return Counter()
    version = tag[3]
    end = min(len(tag), ID3_HEADER_SIZE + synchsafe(tag[6:10]))
    position = ID3_HEADER_SIZE
    if tag[5] & ID3_EXTENDED_HEADER_FLAG:
        extended = tag[position : position + 4]
        position += synchsafe(extended) if version >= ID3_SYNCHSAFE_VERSION else 4 + struct.unpack(">I", extended)[0]
    counts = Counter()
    while position + ID3_HEADER_SIZE <= end and tag[position] != 0:
        frame_id = tag[position : position + 4].decode("latin-1")
        size_bytes = tag[position + 4 : position + 8]
        size = synchsafe(size_bytes) if version >= ID3_SYNCHSAFE_VERSION else struct.unpack(">I", size_bytes)[0]
        counts[frame_id] += 1
        position += ID3_HEADER_SIZE + size
    return counts


def frame_fields(frame: Frame) -> tuple:
    """Returns the decoded fields of an ID3v2 frame without its text encoding, hashing binary payloads."""
    fields = []
    for spec in list(frame._framespec) + list(getattr(frame, "_optionalspec", [])):
        if spec.name == "encoding" or not hasattr(frame, spec.name):
            continue
        value = getattr(frame, spec.name)
        if isinstance(value, bytes) and len(value) > HASHED_PAYLOAD_BYTES:
            value = f"{len(value)} bytes sha1 {hashlib.sha1(value).hexdigest()[:12]}"
        elif isinstance(value, list):
            value = tuple(str(item) for item in value)
        fields.append((spec.name, str(value) if not isinstance(value, tuple) else value))
    return frame.FrameID, tuple(fields)


def read_frames(path: Path) -> Counter:
    """Returns the decoded frames of the ID3v2 tag of an AIFF file, counted."""
    tags = AIFF(path).tags
    return Counter(frame_fields(frame) for frame in tags.values()) if tags is not None else Counter()


def compare_samples(name: str, cpp: dict[bytes, bytes], rust: dict[bytes, bytes], tolerance: float | None) -> list[str]:
    """Compares the sample data of two outputs and returns the differences, exactly when there is no tolerance."""
    if cpp.get(b"SSND") == rust.get(b"SSND"):
        return []
    if cpp[b"SSND"][:8] != rust[b"SSND"][:8]:
        return [f"{name}: SSND offset and block size {cpp[b'SSND'][:8]!r} in C++, {rust[b'SSND'][:8]!r} in Rust"]
    cpp_samples, full_scale = decode_samples(cpp)
    rust_samples, _ = decode_samples(rust)
    if len(cpp_samples) != len(rust_samples):
        return [f"{name}: {len(cpp_samples)} samples in C++, {len(rust_samples)} in Rust"]
    return compare_sample_values(name, cpp_samples, rust_samples, full_scale, tolerance)


def compare_sample_values(
    name: str, cpp: np.ndarray, rust: np.ndarray, full_scale: int, tolerance: float | None
) -> list[str]:
    """Compares two equally long sample arrays and returns the differences, exactly when there is no tolerance."""
    steps = int(np.max(np.abs(cpp - rust), initial=0))
    if steps == 0:
        return [f"{name}: SSND chunks differ after the samples"]
    level = 20 * np.log10(steps / full_scale)
    differing = int(np.count_nonzero(cpp != rust))
    print(f"  {name}: {differing} samples differ by at most {steps} steps, {level:.1f} dBFS")
    if tolerance is None:
        return [f"{name}: {differing} samples differ by up to {steps} steps, but the source is lossless"]
    if level > tolerance:
        return [f"{name}: samples differ by up to {level:.1f} dBFS, above the {tolerance} dBFS tolerance"]
    return []


def compare_frames(name: str, cpp_path: Path, rust_path: Path, cpp: dict, rust: dict) -> list[str]:
    """Compares the ID3v2 frames of two outputs and returns the differences."""
    cpp_frames = read_frames(cpp_path)
    rust_frames = read_frames(rust_path)
    differences = [f"{name}: frame only in C++ output: {frame}" for frame in (cpp_frames - rust_frames).elements()]
    differences += [f"{name}: frame only in Rust output: {frame}" for frame in (rust_frames - cpp_frames).elements()]
    cpp_ids = raw_frame_ids(cpp)
    rust_ids = raw_frame_ids(rust)
    for frame_id in sorted(set(cpp_ids) | set(rust_ids)):
        if cpp_ids[frame_id] != rust_ids[frame_id]:
            differences.append(
                f"{name}: {frame_id} appears {cpp_ids[frame_id]} times in C++ output, {rust_ids[frame_id]} in Rust"
            )
    return differences


def compare_outputs(cpp_path: Path, rust_path: Path, tolerance: float | None) -> list[str]:
    """Compares the container, the samples, and the tags of two AIFF outputs."""
    name = cpp_path.name
    cpp_form, cpp_chunks = read_chunks(cpp_path)
    rust_form, rust_chunks = read_chunks(rust_path)
    if cpp_form != rust_form:
        return [f"{name}: form {cpp_form!r} in C++, {rust_form!r} in Rust"]
    if cpp_chunks.get(b"COMM") != rust_chunks.get(b"COMM"):
        return [f"{name}: COMM chunk {cpp_chunks.get(b'COMM')!r} in C++, {rust_chunks.get(b'COMM')!r} in Rust"]
    return compare_samples(name, cpp_chunks, rust_chunks, tolerance) + compare_frames(
        name, cpp_path, rust_path, cpp_chunks, rust_chunks
    )


def lossy_outputs(sources: list[Path]) -> set[Path]:
    """Returns the output paths, without their extension, written from at least one lossy source."""
    return {source.with_suffix("") for source in sources if source.suffix.lower() in LOSSY_EXTENSIONS}


def list_files(folder: Path) -> set[Path]:
    """Returns the paths of the files in a folder relative to it, without hidden files such as Finder's .DS_Store."""
    return {path.relative_to(folder) for path in folder.rglob("*") if path.is_file() and not path.name.startswith(".")}


def main() -> None:
    """Compares the two folders given on the command line and exits with code 1 when they differ."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cpp", type=Path, help="Folder the C++ CLI normalized")
    parser.add_argument("rust", type=Path, help="Folder the Rust CLI normalized")
    parser.add_argument(
        "-t", "--tolerance", type=float, default=-72.0, help="Largest allowed sample difference in dBFS"
    )
    parser.add_argument(
        "-s",
        "--sources",
        type=Path,
        help="File listing the source paths relative to the folders, one per line, written before normalizing",
    )
    arguments = parser.parse_args()

    lossy = None
    if arguments.sources is not None:
        lines = arguments.sources.read_text(encoding="utf-8").splitlines()
        lossy = lossy_outputs([Path(line) for line in lines if line.strip()])

    cpp_files = list_files(arguments.cpp)
    rust_files = list_files(arguments.rust)
    differences = [f"{path}: only in the C++ folder" for path in sorted(cpp_files - rust_files)]
    differences += [f"{path}: only in the Rust folder" for path in sorted(rust_files - cpp_files)]

    for path in sorted(path for path in cpp_files & rust_files if path.suffix.lower() not in AIFF_EXTENSIONS):
        if (arguments.cpp / path).read_bytes() != (arguments.rust / path).read_bytes():
            differences.append(f"{path}: the remaining source differs between the folders")

    outputs = sorted(path for path in cpp_files & rust_files if path.suffix.lower() in AIFF_EXTENSIONS)
    identical = 0
    for path in outputs:
        tolerance = arguments.tolerance if lossy is None or path.with_suffix("") in lossy else None
        file_differences = compare_outputs(arguments.cpp / path, arguments.rust / path, tolerance)
        if not file_differences and (arguments.cpp / path).read_bytes() == (arguments.rust / path).read_bytes():
            identical += 1
        differences += file_differences

    print(f"Compared {len(outputs)} output files, {identical} byte for byte identical")
    if differences:
        print(f"{len(differences)} output difference(s):")
        for difference in differences:
            print(f"  {difference}")
        sys.exit(1)
    print("No output differences")


if __name__ == "__main__":
    main()
