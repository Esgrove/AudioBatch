#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0", "numpy>=2"]
# ///
"""
Checks AAC reconstruction independently with Core Audio, FFmpeg, and both release CLIs on macOS.

The exact fixture is regenerated without modifying it, then a PNS-disabled control changes only that encoder option.
Raw ADTS decoding verifies the 1024-frame edit-list origin independently of either application.
No implementation-specific PCM is stored and the existing -72 dBFS output tolerance is unchanged.
Every normalization operates on temporary copies with isolated application storage and permanent temporary deletion.

Run from rust/ with uv run --script scripts/test_aac_reconstruction.py
    --cpp-cli ../cpp/cmake-build-mac-release/AudioBatchCli_artefacts/Release/audiobatch
    --rust-cli target/release/audiobatch-rs [--input additional.m4a ...]
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import compare_outputs
import numpy as np

SAMPLE_RATE = 44_100
PRIMING_FRAMES = 1024
PCM16_FULL_SCALE = 32_768
FLOAT_RECONSTRUCTION_LIMIT = 0.000001
PARITY_LIMIT_DBFS = -72.0


def run(command: list[str], *, data_root: Path | None = None) -> bytes:
    """Runs trusted explicit executables with bounded waits and optional isolated application storage."""
    environment = dict(os.environ)
    if data_root is not None:
        environment["AUDIOBATCH_DATA_ROOT"] = str(data_root)
    return subprocess.run(  # noqa: S603
        command, check=True, capture_output=True, env=environment, timeout=120
    ).stdout


def decoded(path: Path) -> np.ndarray:
    """Returns independent floating-point PCM without sample-rate conversion or a forced channel layout."""
    channels = int(
        run(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "a:0",
                "-show_entries",
                "stream=channels",
                "-of",
                "csv=p=0",
                str(path),
            ]
        )
    )
    return np.frombuffer(run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-"]), dtype="<f4").reshape(
        -1, channels
    )


def native_decode(source: Path, destination: Path) -> np.ndarray:
    """Uses Apple's independent conversion executable at the unchanged source rate and float precision."""
    run(["afconvert", "-f", "WAVE", "-d", "LEF32", str(source), str(destination)])
    return decoded(destination)


def residual(first: np.ndarray, second: np.ndarray) -> float:
    """Rejects geometry changes and reports maximum full-scale and PCM16-step residuals."""
    if first.shape != second.shape:
        message = f"Decoded geometry differs: {first.shape} versus {second.shape}"
        raise AssertionError(message)
    maximum = float(np.max(np.abs(first.astype(np.float64) - second), initial=0))
    print(f"  {len(first)} frames, maximum {maximum:.12g} ({maximum * PCM16_FULL_SCALE:.9g} PCM16 steps)")
    return maximum


def render(directory: Path, *, mono: bool, pns: bool) -> Path:
    """Recreates the committed tone encoder invocation, changing only channel count or PNS when requested."""
    tone = "0.5*sin(2*PI*1000*t)"
    expression = tone if mono else f"{tone}|{tone}"
    path = directory / f"{'mono' if mono else 'stereo'}-pns-{int(pns)}.m4a"
    options = [] if pns else ["-aac_pns", "0"]
    run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            f"aevalsrc={expression}:s={SAMPLE_RATE}:d=1",
            "-map_metadata",
            "-1",
            "-fflags",
            "+bitexact",
            "-flags:a",
            "+bitexact",
            "-c:a",
            "aac",
            "-b:a",
            "128k",
            *options,
            str(path),
        ]
    )
    return path


def check_reconstruction(directory: Path) -> list[Path]:
    """Separates PNS reconstruction from MDCT, channel layout, edit-list trimming, and priming."""
    controls = []
    for mono in (False, True):
        for pns in (True, False):
            path = render(directory, mono=mono, pns=pns)
            controls.append(path)
            profile = json.loads(
                run(
                    [
                        "ffprobe",
                        "-v",
                        "error",
                        "-select_streams",
                        "a:0",
                        "-show_entries",
                        "stream=profile,sample_rate",
                        "-of",
                        "json",
                        str(path),
                    ]
                )
            )["streams"][0]
            if profile != {"profile": "LC", "sample_rate": str(SAMPLE_RATE)}:
                message = f"Unexpected AAC profile or SBR rate: {profile}"
                raise AssertionError(message)
            native = native_decode(path, path.with_suffix(".wav"))
            independent = decoded(path)[:SAMPLE_RATE]
            print(f"Independent native/FFmpeg control {path.name}:")
            maximum = residual(native, independent)
            if native.shape != (SAMPLE_RATE, 1 if mono else 2):
                message = f"Unexpected playable timeline for {path.name}"
                raise AssertionError(message)
            if not pns and maximum > FLOAT_RECONSTRUCTION_LIMIT:
                message = "PNS-disabled control exceeds the independent float reconstruction limit"
                raise AssertionError(message)
            if pns and maximum <= FLOAT_RECONSTRUCTION_LIMIT:
                message = "The control no longer distinguishes native PNS, revisit the reconstruction evidence"
                raise AssertionError(message)
    source = controls[0]
    fixture = Path(__file__).resolve().parents[2] / "fixtures/sine_stereo_44100_aac.m4a"
    if source.read_bytes() != fixture.read_bytes():
        print("Encoder version differs from the committed fixture, using the committed bytes for the timeline check")
    print(f"Committed AAC sha256: {hashlib.sha256(fixture.read_bytes()).hexdigest()}")
    raw = directory / "untrimmed.aac"
    run(["ffmpeg", "-v", "error", "-y", "-i", str(fixture), "-c:a", "copy", "-f", "adts", str(raw)])
    raw_native = native_decode(raw, directory / "raw-native.wav")
    mp4_native = native_decode(fixture, directory / "mp4-native.wav")
    print("Native ADTS versus MP4 after the independently recorded 1024-frame priming:")
    if residual(mp4_native, raw_native[PRIMING_FRAMES : PRIMING_FRAMES + SAMPLE_RATE]) > FLOAT_RECONSTRUCTION_LIMIT:
        message = "Native MP4 and raw ADTS timelines disagree"
        raise AssertionError(message)
    return [fixture, *controls]


def check_outputs(inputs: list[Path], directory: Path, cpp: Path, rust: Path) -> None:
    """Runs both production CLIs on copied inputs and enforces the unchanged strict output comparator."""
    for index, source in enumerate(inputs):
        for mode, option, target in [("peak", "--peak", "0"), ("lufs", "--lufs", "-14")]:
            outputs = []
            for implementation, executable in [("cpp", cpp), ("rust", rust)]:
                folder = directory / f"{index}-{mode}-{implementation}"
                folder.mkdir()
                copied = folder / source.name
                shutil.copy2(source, copied)
                run(
                    [
                        str(executable),
                        "--refresh",
                        "--normalize",
                        "--delete",
                        option,
                        target,
                        "--output",
                        "json",
                        str(copied),
                    ],
                    data_root=directory / f"storage-{implementation}",
                )
                outputs.append(copied.with_suffix(".aif"))
            print(f"Strict {mode} output {source.name}:")
            differences = compare_outputs.compare_outputs(*outputs, PARITY_LIMIT_DBFS, mode)
            if differences:
                raise AssertionError("\n".join(differences))
            _, first = compare_outputs.read_chunks(outputs[0])
            _, second = compare_outputs.read_chunks(outputs[1])
            first_pcm, _ = compare_outputs.decode_samples(first)
            second_pcm, _ = compare_outputs.decode_samples(second)
            steps = int(np.max(np.abs(first_pcm - second_pcm), initial=0))
            differing = int(np.count_nonzero(first_pcm != second_pcm))
            print(f"  {steps} maximum PCM steps, {differing} differing samples")


def build_parser() -> argparse.ArgumentParser:
    """Describe executable and copied-input selection without running reconstruction diagnostics."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-C", "--cpp-cli", type=Path, required=True)
    parser.add_argument("-R", "--rust-cli", type=Path, required=True)
    parser.add_argument(
        "-i", "--input", type=Path, action="append", default=[], help="Additional original AAC input, copied"
    )
    return parser


def main() -> None:
    """Runs bounded native diagnostics and production parity without writing fixtures or normal application storage."""
    arguments = build_parser().parse_args()
    with tempfile.TemporaryDirectory(prefix="audiobatch-aac-") as temporary:
        directory = Path(temporary)
        inputs = check_reconstruction(directory)
        check_outputs(inputs + arguments.input, directory, arguments.cpp_cli.resolve(), arguments.rust_cli.resolve())
    print("AAC reconstruction and strict parity checks passed")


if __name__ == "__main__":
    main()
