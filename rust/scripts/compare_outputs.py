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
With --lossy, lossless outputs must have identical samples in peak mode,
and may differ by at most one integer PCM step in lufs mode.
Only the outputs of the lossy sources it lists, which the two decoders may decode differently,
may differ by the dBFS tolerance.
Without it, every output may differ by the tolerance.
--list-lossy prints that list for a folder, and must run before normalizing,
since normalizing moves the sources to the trash.
Physical frames are decoded in isolation, so same-identity duplicates retain their individual payloads,
while a different text encoding of the same text is no difference.
Pre-operation snapshots assert original bytes and filesystem metadata for expected failures and untouched files.
Successful outputs must retain the source frame count, ID3 metadata except stale Serato overviews, and artwork payloads.
Exits with code 1 when anything differs.

Usage: uv run compare_outputs.py <C++ folder> <Rust folder> [--tolerance <dBFS>] [--lossy <list file>]
       [--mode peak|lufs] [--snapshots <C++ original snapshot> <Rust original snapshot>]
       uv run compare_outputs.py --snapshot <folder> --expectations <JSON> > <snapshot JSON>
       uv run compare_outputs.py --list-lossy <folder> > <list file>
"""

import argparse
import base64
import hashlib
import io
import json
import struct
import sys
from collections import Counter
from pathlib import Path

import mutagen
import numpy as np
from mutagen.flac import Picture
from mutagen.id3 import ID3, Frame
from mutagen.mp4 import MP4

AIFF_EXTENSIONS = {".aif", ".aiff"}
# Sources whose decoders may differ between the two applications, so their outputs are compared with the tolerance.
# An MP4 file may hold lossless ALAC instead, which is told apart by its codec.
LOSSY_EXTENSIONS = {".mp3", ".m4a", ".aac", ".mp4", ".ogg", ".oga", ".opus"}
# Binary frame payloads longer than this are compared by their hash, which keeps the report short.
HASHED_PAYLOAD_BYTES = 64
ID3_HEADER_SIZE = 10
ID3_EXTENDED_HEADER_FLAG = 0x40
# ID3v2.4 stores frame and extended header sizes as synchsafe integers, while earlier versions use plain ones.
ID3_SYNCHSAFE_VERSION = 4
FORM_HEADER_SIZE = 12
CHUNK_HEADER_SIZE = 8
COMM_MINIMUM_SIZE = 18
ID3_SIZE_BYTES = 4


def read_chunks(path: Path) -> tuple[bytes, dict[bytes, bytes]]:
    """Returns complete AIFF chunks, rejecting truncation and duplicate structural chunks."""
    data = path.read_bytes()
    if len(data) < FORM_HEADER_SIZE or data[:4] != b"FORM" or struct.unpack(">I", data[4:8])[0] + 8 != len(data):
        raise ValueError(f"{path.name}: invalid FORM length")
    chunks = {}
    position = 12
    while position < len(data):
        if position + 8 > len(data):
            raise ValueError(f"{path.name}: truncated chunk header")
        chunk_id = data[position : position + 4]
        size = struct.unpack(">I", data[position + 4 : position + 8])[0]
        if position + 8 + size > len(data) or chunk_id in chunks:
            raise ValueError(f"{path.name}: truncated or duplicate {chunk_id!r} chunk")
        chunks[chunk_id] = data[position + 8 : position + 8 + size]
        position += 8 + size + (size & 1)
    return data[8:12], chunks


def decode_samples(chunks: dict[bytes, bytes]) -> tuple[np.ndarray, int]:
    """Returns the integer samples of the SSND chunk and their full scale value."""
    bits = struct.unpack(">h", chunks[b"COMM"][6:8])[0]
    width = (bits + 7) // 8
    offset = struct.unpack(">I", chunks[b"SSND"][:4])[0]
    raw = np.frombuffer(chunks[b"SSND"][8 + offset :], dtype=np.uint8)
    raw = raw[: len(raw) - len(raw) % width].reshape(-1, width).astype(np.int64)
    values = np.zeros(len(raw), dtype=np.int64)
    for byte in range(width):
        values = (values << 8) | raw[:, byte]
    sign = 1 << (width * 8 - 1)
    return np.where(values >= sign, values - 2 * sign, values), sign


def synchsafe(data: bytes) -> int:
    """Returns the value of a four byte ID3v2 synchsafe integer."""
    return (data[0] << 21) | (data[1] << 14) | (data[2] << 7) | data[3]


def raw_frames(tag: bytes) -> list[bytes]:
    """Returns separate serialized ID3v2.3 or ID3v2.4 frames without merging identical frame identities."""
    if len(tag) < ID3_HEADER_SIZE or tag[:3] != b"ID3" or tag[3] not in {3, 4}:
        raise ValueError("expected an ID3v2.3 or ID3v2.4 tag")
    version = tag[3]
    end = ID3_HEADER_SIZE + synchsafe(tag[6:10])
    if end > len(tag):
        raise ValueError("truncated ID3 tag")
    position = ID3_HEADER_SIZE
    if tag[5] & ID3_EXTENDED_HEADER_FLAG:
        extended = tag[position : position + 4]
        if len(extended) != ID3_SIZE_BYTES:
            raise ValueError("truncated ID3 extended header")
        position += synchsafe(extended) if version >= ID3_SYNCHSAFE_VERSION else 4 + struct.unpack(">I", extended)[0]
        if position > end:
            raise ValueError("ID3 extended header exceeds the tag")
    frames = []
    while position < end and tag[position] != 0:
        if position + ID3_HEADER_SIZE > end:
            raise ValueError("truncated ID3 frame header")
        size_bytes = tag[position + 4 : position + 8]
        size = synchsafe(size_bytes) if version >= ID3_SYNCHSAFE_VERSION else struct.unpack(">I", size_bytes)[0]
        next_position = position + ID3_HEADER_SIZE + size
        if next_position > end:
            raise ValueError("ID3 frame payload exceeds the tag")
        frames.append(tag[position:next_position])
        position = next_position
    return frames


def raw_frame_ids(chunks: dict[bytes, bytes]) -> Counter:
    """Returns physical frame ID counts, including frames that Mutagen normally merges."""
    tag = chunks.get(b"ID3 ") or chunks.get(b"id3 ")
    return Counter(frame[:4].decode("latin-1") for frame in raw_frames(tag)) if tag else Counter()


def encoded_synchsafe(value: int) -> bytes:
    """Encodes a tag length for isolated frame decoding."""
    return bytes((value >> shift) & 0x7F for shift in (21, 14, 7, 0))


def decoded_frames(tag: bytes, *, processed: bool = False) -> Counter:
    """Decodes each physical frame in isolation, retaining every duplicate's fields and opaque payload."""
    frames = Counter()
    legacy_date = processed and tag[3] < ID3_SYNCHSAFE_VERSION
    if legacy_date:
        # The v2.4 writer combines legacy year/date/time frames and drops the obsolete v2.3-only frames.
        converted = ID3(io.BytesIO(tag))
        frames.update(frame_fields(frame) for frame in converted.getall("TDRC"))
    for raw in raw_frames(tag):
        if legacy_date and raw[:4] in {b"TYER", b"TDAT", b"TIME", b"EQUA", b"RVAD", b"TRDA", b"TSIZ"}:
            continue
        # An isolated tag retains unsynchronization, but has neither an extended header nor a footer.
        header = tag[:5] + bytes([tag[5] & 0x80]) + encoded_synchsafe(len(raw))
        isolated = ID3(io.BytesIO(header + raw))
        for frame in isolated.values():
            if processed and (
                (frame.FrameID == "GEOB" and frame.desc == "Serato Overview")
                or (frame.FrameID == "TXXX" and frame.desc.upper() == "SERATO_OVERVIEW")
            ):
                continue
            frames[frame_fields(frame)] += 1
        for unknown in isolated.unknown_frames:
            frames[(unknown[:4].decode("latin-1"), (("opaque", unknown[10:].hex()),))] += 1
    return frames


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
    """Returns decoded physical ID3 frames, including duplicate same-identity frames and unknown payloads."""
    _, chunks = read_chunks(path)
    tag = chunks.get(b"ID3 ") or chunks.get(b"id3 ")
    return decoded_frames(tag) if tag else Counter()


def validate_sample_geometry(name: str, chunks: dict[bytes, bytes]) -> list[str]:
    """Checks declared COMM frame counts against actual SSND bytes, independently of the other implementation."""
    common = chunks.get(b"COMM", b"")
    sound = chunks.get(b"SSND", b"")
    if len(common) < COMM_MINIMUM_SIZE or len(sound) < CHUNK_HEADER_SIZE:
        return [f"{name}: missing or truncated COMM/SSND"]
    channels, frames, bits = struct.unpack(">HIH", common[:8])
    offset = struct.unpack(">I", sound[:4])[0]
    expected_bytes = frames * channels * ((bits + 7) // 8)
    actual_bytes = len(sound) - 8 - offset
    if channels == 0 or bits not in {16, 24, 32} or actual_bytes != expected_bytes:
        return [f"{name}: COMM declares {frames} frames ({expected_bytes} bytes), SSND contains {actual_bytes} bytes"]
    return []


def compare_samples(
    name: str,
    cpp: dict[bytes, bytes],
    rust: dict[bytes, bytes],
    tolerance: float | None,
    lossless_steps: int = 0,
) -> list[str]:
    """Compares sample data with a dBFS tolerance for lossy sources or an integer step limit for lossless sources."""
    if cpp.get(b"SSND") == rust.get(b"SSND"):
        return []
    if b"SSND" not in cpp or b"SSND" not in rust:
        return [f"{name}: SSND chunk only in the {'C++' if b'SSND' in cpp else 'Rust'} output"]
    if cpp[b"SSND"][:8] != rust[b"SSND"][:8]:
        return [f"{name}: SSND offset and block size {cpp[b'SSND'][:8]!r} in C++, {rust[b'SSND'][:8]!r} in Rust"]
    offset = struct.unpack(">I", cpp[b"SSND"][:4])[0]
    if cpp[b"SSND"][8 : 8 + offset] != rust[b"SSND"][8 : 8 + offset]:
        return [f"{name}: SSND alignment bytes differ"]
    cpp_samples, full_scale = decode_samples(cpp)
    rust_samples, _ = decode_samples(rust)
    if len(cpp_samples) != len(rust_samples):
        return [f"{name}: {len(cpp_samples)} samples in C++, {len(rust_samples)} in Rust"]
    return compare_sample_values(name, (cpp_samples, rust_samples), full_scale, tolerance, lossless_steps)


def compare_sample_values(
    name: str,
    samples: tuple[np.ndarray, np.ndarray],
    full_scale: int,
    tolerance: float | None,
    lossless_steps: int = 0,
) -> list[str]:
    """Checks the maximum sample difference against the lossy dBFS tolerance or lossless integer PCM step limit."""
    cpp, rust = samples
    steps = int(np.max(np.abs(cpp - rust), initial=0))
    if steps == 0:
        return [f"{name}: SSND chunks differ after the samples"]
    level = 20 * np.log10(steps / full_scale)
    differing = int(np.count_nonzero(cpp != rust))
    print(f"  {name}: {differing} samples differ by at most {steps} steps, {level:.1f} dBFS")
    if tolerance is None:
        if steps > lossless_steps:
            return [
                f"{name}: {differing} samples differ by up to {steps} steps, "
                f"above the {lossless_steps} step lossless limit"
            ]
        return []
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


def compare_outputs(cpp_path: Path, rust_path: Path, tolerance: float | None, mode: str = "peak") -> list[str]:
    """Compares the container, the samples, and the tags of two AIFF outputs."""
    name = cpp_path.name
    cpp_form, cpp_chunks = read_chunks(cpp_path)
    rust_form, rust_chunks = read_chunks(rust_path)
    if cpp_form != rust_form:
        return [f"{name}: form {cpp_form!r} in C++, {rust_form!r} in Rust"]
    if cpp_chunks.get(b"COMM") != rust_chunks.get(b"COMM"):
        return [f"{name}: COMM chunk {cpp_chunks.get(b'COMM')!r} in C++, {rust_chunks.get(b'COMM')!r} in Rust"]
    geometry = validate_sample_geometry(f"{name} C++", cpp_chunks) + validate_sample_geometry(
        f"{name} Rust", rust_chunks
    )
    if geometry:
        return geometry
    lossless_steps = 1 if mode == "lufs" else 0
    return compare_samples(name, cpp_chunks, rust_chunks, tolerance, lossless_steps) + compare_frames(
        name, cpp_path, rust_path, cpp_chunks, rust_chunks
    )


def is_lossy(path: Path) -> bool:
    """Returns true for a source with lossy audio, which an MP4 file with ALAC audio is not."""
    if path.suffix.lower() not in LOSSY_EXTENSIONS:
        return False
    try:
        audio = mutagen.File(path)
    except mutagen.MutagenError:
        return True
    return not (isinstance(audio, MP4) and audio.info.codec == "alac")


def lossy_outputs(sources: list[Path]) -> set[Path]:
    """Returns the output paths, without their extension, written from at least one of the lossy sources."""
    return {source.with_suffix("") for source in sources}


def list_files(folder: Path) -> set[Path]:
    """Returns the paths of the files in a folder relative to it, without hidden files such as Finder's .DS_Store."""
    return {path.relative_to(folder) for path in folder.rglob("*") if path.is_file() and not path.name.startswith(".")}


def source_id3(path: Path) -> bytes | None:
    """Reads the original physical ID3 tag from MP3, AIFF, or WAV without a merging tag-library view."""
    data = path.read_bytes()
    if data.startswith(b"ID3"):
        return data[: ID3_HEADER_SIZE + synchsafe(data[6:10])]
    if data[:4] not in {b"FORM", b"RIFF"}:
        return None
    position = 12
    endian = ">I" if data[:4] == b"FORM" else "<I"
    while position + 8 <= len(data):
        size = struct.unpack(endian, data[position + 4 : position + 8])[0]
        if data[position : position + 4].lower() == b"id3 ":
            return data[position + 8 : position + 8 + size]
        position += 8 + size + (size & 1)
    return None


def artwork_hashes(path: Path) -> list[str]:
    """Snapshots every source artwork payload for independent preservation checks across tag formats."""
    try:
        audio = mutagen.File(path)
    except mutagen.MutagenError:
        return []
    if audio is None:
        return []
    pictures = [picture.data for picture in getattr(audio, "pictures", [])]
    if isinstance(audio.tags, ID3):
        pictures += [frame.data for frame in audio.tags.getall("APIC")]
    elif isinstance(audio, MP4):
        pictures += list(audio.tags.get("covr", [])) if audio.tags else []
    elif audio.tags is not None:
        pictures += [Picture(base64.b64decode(value)).data for value in audio.tags.get("metadata_block_picture", [])]
    return [hashlib.sha256(picture).hexdigest() for picture in pictures]


def snapshot_folder(folder: Path, expected: dict) -> dict:
    """Snapshots source bytes, filesystem metadata, retained physical ID3 frames, artwork, and expected outcomes."""
    outcomes = {row["source"]: (output, row) for output, row in expected.items()}
    snapshot = {}
    for path in sorted(list_files(folder)):
        source = folder / path
        stat = source.stat()
        output, outcome = outcomes.get(str(path), (str(path), {"status": "untouched"}))
        tag = source_id3(source)
        frames = decoded_frames(tag, processed=True) if tag else Counter()
        snapshot[str(path)] = {
            "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns,
            "mode": stat.st_mode & 0o777,
            "frames": list(frames.elements()),
            "artwork": artwork_hashes(source),
            "output": output,
            "expected": outcome,
        }
    return snapshot


def frozen(value: object) -> object:
    """Restores tuple identities after the snapshot's JSON round trip."""
    return tuple(frozen(item) for item in value) if isinstance(value, list) else value


def check_preservation(folder: Path, snapshot: dict) -> list[str]:
    """Checks each implementation against its own original snapshot, not merely against the other implementation."""
    differences = []
    for relative, original in snapshot.items():
        source = folder / relative
        outcome = original["expected"]
        if outcome["status"] in {"failed", "untouched"}:
            if not source.is_file():
                differences.append(f"{relative}: original source was removed despite expected preservation")
                continue
            stat = source.stat()
            actual = (
                hashlib.sha256(source.read_bytes()).hexdigest(),
                stat.st_size,
                stat.st_mtime_ns,
                stat.st_mode & 0o777,
            )
            expected = (original["sha256"], original["size"], original["mtime_ns"], original["mode"])
            if actual != expected:
                differences.append(
                    f"{relative}: original bytes or filesystem metadata changed despite expected preservation"
                )
            continue
        output = folder / original["output"]
        if not output.is_file():
            differences.append(f"{relative}: expected normalized output is missing")
            continue
        if source != output and source.exists():
            differences.append(f"{relative}: converted source was not disposed of")
        _, chunks = read_chunks(output)
        differences += validate_sample_geometry(str(output), chunks)
        if "frameCount" in outcome:
            frames = struct.unpack(">I", chunks[b"COMM"][2:6])[0]
            if frames != outcome["frameCount"]:
                differences.append(f"{relative}: output has {frames} frames, source expected {outcome['frameCount']}")
        retained = Counter(frozen(frame) for frame in original["frames"])
        missing = retained - read_frames(output)
        for frame in missing.elements():
            differences.append(f"{relative}: original metadata frame not preserved: {frame}")
        missing_art = Counter(original["artwork"]) - Counter(artwork_hashes(output))
        if missing_art:
            differences.append(f"{relative}: {sum(missing_art.values())} original artwork payload(s) not preserved")
    return differences


def main() -> None:
    """Snapshots inputs or compares normalized folders against each other and their original sources."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cpp", type=Path, nargs="?", help="Folder the C++ CLI normalized")
    parser.add_argument("rust", type=Path, nargs="?", help="Folder the Rust CLI normalized")
    parser.add_argument(
        "-t", "--tolerance", type=float, default=-72.0, help="Largest allowed sample difference in dBFS"
    )
    parser.add_argument(
        "-l",
        "--lossy",
        type=Path,
        help="File listing the lossy source paths relative to the folders, one per line, written by --list-lossy",
    )
    parser.add_argument(
        "-L",
        "--list-lossy",
        type=Path,
        metavar="FOLDER",
        help="Print the paths of the lossy sources in the folder relative to it, and exit",
    )
    parser.add_argument(
        "--mode",
        choices=("peak", "lufs"),
        default="peak",
        help="Normalization mode, permits one integer PCM step for lossless outputs only in lufs mode",
    )
    parser.add_argument("--snapshot", type=Path, metavar="FOLDER", help="Snapshot originals before normalization")
    parser.add_argument("--expectations", type=Path, help="Expected outcomes from compare_levels.py")
    parser.add_argument("--snapshots", type=Path, nargs=2, help="Original C++ and Rust snapshot files")
    arguments = parser.parse_args()

    if arguments.snapshot is not None:
        if arguments.expectations is None:
            parser.error("--snapshot requires --expectations")
        expected = json.loads(arguments.expectations.read_text(encoding="utf-8"))
        print(json.dumps(snapshot_folder(arguments.snapshot, expected), indent=2))
        return
    if arguments.list_lossy is not None:
        for path in sorted(list_files(arguments.list_lossy)):
            if is_lossy(arguments.list_lossy / path):
                print(path)
        return
    if arguments.cpp is None or arguments.rust is None:
        parser.error("the C++ and the Rust folder are required")

    differences, output_count, identical = compare_folders(arguments)
    print(f"Compared {output_count} output files, {identical} byte for byte identical")
    if differences:
        print(f"{len(differences)} output difference(s):")
        for difference in differences:
            print(f"  {difference}")
        sys.exit(1)
    print("No output differences")


def compare_folders(arguments: argparse.Namespace) -> tuple[list[str], int, int]:
    """Compares normalized folder contents and original snapshots using the parsed comparison options."""
    lossy = None
    if arguments.lossy is not None:
        lines = arguments.lossy.read_text(encoding="utf-8").splitlines()
        lossy = lossy_outputs([Path(line) for line in lines if line.strip()])

    cpp_files = list_files(arguments.cpp)
    rust_files = list_files(arguments.rust)
    differences = [f"{path}: only in the C++ folder" for path in sorted(cpp_files - rust_files)]
    differences += [f"{path}: only in the Rust folder" for path in sorted(rust_files - cpp_files)]

    for path in sorted(path for path in cpp_files & rust_files if path.suffix.lower() not in AIFF_EXTENSIONS):
        if (arguments.cpp / path).read_bytes() != (arguments.rust / path).read_bytes():
            differences.append(f"{path}: the remaining source differs between the folders")

    preserved = set()
    if arguments.snapshots is not None:
        for label, folder, snapshot_path in zip(
            ("C++", "Rust"), (arguments.cpp, arguments.rust), arguments.snapshots, strict=True
        ):
            snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
            differences += [f"{label}: {difference}" for difference in check_preservation(folder, snapshot)]
            preserved.update(
                Path(path) for path, row in snapshot.items() if row["expected"]["status"] in {"failed", "untouched"}
            )
    outputs = sorted(
        path for path in cpp_files & rust_files if path.suffix.lower() in AIFF_EXTENSIONS and path not in preserved
    )
    identical = 0
    for path in outputs:
        tolerance = arguments.tolerance if lossy is None or path.with_suffix("") in lossy else None
        file_differences = compare_outputs(arguments.cpp / path, arguments.rust / path, tolerance, arguments.mode)
        if not file_differences and (arguments.cpp / path).read_bytes() == (arguments.rust / path).read_bytes():
            identical += 1
        differences += file_differences

    return differences, len(outputs), identical


if __name__ == "__main__":
    main()
