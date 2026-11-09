#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Compares ImageControl's VST3 and Audio Unit on macOS by rendering the same files through each with the AudioBatch CLI.

The script builds ImageControlPresetWriter, which loads the installed VST3 and Audio Unit
and writes an AudioBatch chain preset for each with the same non-neutral settings.
It then renders the 60-second stereo files of fixtures/benchmark/output through each preset with `audiobatch process`,
timing every run on fresh copies with isolated application storage,
and compares every render's validated AIFF metadata and 16-bit PCM, sample by sample.
Repeated renders must be deterministic, and paired run order is counterbalanced.
Any malformed audio, metadata mismatch, or PCM mismatch fails the command.

Install the current build first with `./install.sh --validate`,
and generate the inputs once with `uv run fixtures/benchmark/generate.py`.

Usage: uv run --script image-control-plugin/benchmarks/compare_formats.py [--cli PATH] [--runs N] [--jobs N]
"""

import argparse
import json
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

PLUGIN_DIRECTORY = Path(__file__).resolve().parent.parent
REPOSITORY = PLUGIN_DIRECTORY.parent
BUILD_DIRECTORY = PLUGIN_DIRECTORY / "cmake-build-benchmark"
WRITER = BUILD_DIRECTORY / "ImageControlPresetWriter_artefacts" / "Release" / "ImageControlPresetWriter"
INPUT_DIRECTORY = REPOSITORY / "fixtures" / "benchmark" / "output"
INPUTS = ["music_stereo_44100_24.wav", "music_stereo_48000_24.wav", "noise_stereo_44100_24.wav"]
FORMATS = ["VST3", "AU"]
PCM_BITS = 16
FORM_HEADER_SIZE = 12
CHUNK_HEADER_SIZE = 8
COMM_SIZE = 18
SSND_HEADER_SIZE = 8
EXTENDED_EXPONENT_LIMIT = 0x7FFF
EXTENDED_EXPONENT_BIAS = 16383
EXTENDED_FRACTION_BITS = 63
MINIMUM_RUNS = 2
PROCESS_TIMEOUT_SECONDS = 600
BUILD_TIMEOUT_SECONDS = 1200
INSTALLED = {
    "VST3": Path.home() / "Library/Audio/Plug-Ins/VST3/ImageControl.vst3",
    "AU": Path.home() / "Library/Audio/Plug-Ins/Components/ImageControl.component",
}


def build_preset_writer() -> None:
    """Configure and build the preset writer in Release, in its own build directory."""
    cmake = shutil.which("cmake")
    if cmake is None:
        sys.exit("CMake not found")
    subprocess.run(  # noqa: S603
        [
            cmake,
            "-S",
            str(PLUGIN_DIRECTORY),
            "-B",
            str(BUILD_DIRECTORY),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DIMAGE_CONTROL_BUILD_TESTS=ON",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        timeout=BUILD_TIMEOUT_SECONDS,
    )
    subprocess.run(  # noqa: S603
        [cmake, "--build", str(BUILD_DIRECTORY), "--config", "Release", "--target", "ImageControlPresetWriter"],
        check=True,
        stdout=subprocess.DEVNULL,
        timeout=BUILD_TIMEOUT_SECONDS,
    )


def write_presets(directory: Path) -> dict[str, Path]:
    """Write the chain preset of each installed format and return them by format."""
    subprocess.run(  # noqa: S603
        [str(WRITER), str(directory)], check=True, stdout=subprocess.DEVNULL, timeout=PROCESS_TIMEOUT_SECONDS
    )
    presets = {name: directory / f"ImageControl {name}.xml" for name in FORMATS}
    missing = [name for name, path in presets.items() if not path.is_file()]
    if missing:
        sys.exit(f"No preset was written for {', '.join(missing)}. Is the plugin installed?")
    return presets


def render(cli: str, preset: Path, work: Path, jobs: int) -> tuple[float, dict[str, Path]]:
    """Render fresh copies of the inputs through a preset, and return the wall time and the outputs by input."""
    if work.exists():
        shutil.rmtree(work)
    audio = work / "audio"
    storage = work / "storage"
    audio.mkdir(parents=True)
    storage.mkdir()
    sources = []
    for name in INPUTS:
        source = audio / name
        shutil.copyfile(INPUT_DIRECTORY / name, source)
        sources.append(source)

    environment = os.environ | {"AUDIOBATCH_DATA_ROOT": str(storage)}
    command = [cli, "process", "--preset", str(preset), "--delete", "--jobs", str(jobs), "--output", "json"]
    start = time.perf_counter()
    result = subprocess.run(  # noqa: S603
        command + [str(source) for source in sources],
        check=False,
        env=environment,
        capture_output=True,
        text=True,
        timeout=PROCESS_TIMEOUT_SECONDS,
    )
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        sys.exit(f"audiobatch process failed with {preset.name}:\n{result.stderr}")
    records = json.loads(result.stdout)
    if not isinstance(records, list) or len(records) != len(INPUTS):
        sys.exit(f"Rendering returned an unexpected record count with {preset.name}")
    failed = [record["path"] for record in records if record.get("status") != "analyzed"]
    if failed:
        sys.exit(f"Rendering failed with {preset.name}: {failed}")
    return elapsed, {name: (audio / name).with_suffix(".aif") for name in INPUTS}


@dataclass(frozen=True)
class PcmAudio:
    """Validated AIFF metadata and interleaved signed samples, excluding unrelated metadata chunks."""

    channels: int
    frames: int
    bits: int
    sample_rate: Fraction
    samples: tuple[int, ...]


def invalid_aiff(path: Path, reason: str) -> None:
    """Reject malformed or unsupported audio without treating it as a successful PCM comparison."""
    message = f"{path.name}: {reason}"
    raise ValueError(message)


def read_chunks(path: Path) -> dict[bytes, bytes]:
    """Walk the bounded FORM container, respecting chunk sizes and odd-byte padding, never searching payloads."""
    data = path.read_bytes()
    if len(data) < FORM_HEADER_SIZE or data[:4] != b"FORM" or data[8:FORM_HEADER_SIZE] != b"AIFF":
        invalid_aiff(path, "expected an uncompressed AIFF FORM")
    if struct.unpack_from(">I", data, 4)[0] + CHUNK_HEADER_SIZE != len(data):
        invalid_aiff(path, "FORM length does not match the file length")
    chunks = {}
    position = FORM_HEADER_SIZE
    while position < len(data):
        if len(data) - position < CHUNK_HEADER_SIZE:
            invalid_aiff(path, "truncated chunk header")
        kind, size = struct.unpack_from(">4sI", data, position)
        start = position + CHUNK_HEADER_SIZE
        end = start + size
        padded_end = end + (size & 1)
        if padded_end > len(data):
            invalid_aiff(path, "truncated chunk payload or padding")
        if kind in {b"COMM", b"SSND"}:
            if kind in chunks:
                invalid_aiff(path, f"duplicate {kind.decode('ascii')} chunk")
            chunks[kind] = data[start:end]
        position = padded_end
    if b"COMM" not in chunks or b"SSND" not in chunks:
        invalid_aiff(path, "COMM and SSND chunks are required")
    return chunks


def read_audio(path: Path) -> PcmAudio:
    """Validate COMM metadata, exact frame payload length, and SSND offset before decoding 16-bit PCM."""
    chunks = read_chunks(path)
    common = chunks[b"COMM"]
    if len(common) != COMM_SIZE:
        invalid_aiff(path, "invalid AIFF COMM size")
    channels, frames, bits, exponent, mantissa = struct.unpack(">hIhHQ", common)
    if channels not in {1, 2} or frames == 0 or bits != PCM_BITS:
        invalid_aiff(path, "expected nonempty mono or stereo 16-bit PCM")
    # Extended precision must be finite, positive, and normalized. Keep its exact value for metadata comparison.
    if not 0 < exponent < EXTENDED_EXPONENT_LIMIT or mantissa < 1 << EXTENDED_FRACTION_BITS:
        invalid_aiff(path, "invalid extended-precision sample rate")
    power = exponent - EXTENDED_EXPONENT_BIAS - EXTENDED_FRACTION_BITS
    sample_rate = Fraction(mantissa << max(power, 0), 1 << max(-power, 0))
    sound = chunks[b"SSND"]
    if len(sound) < SSND_HEADER_SIZE:
        invalid_aiff(path, "truncated SSND header")
    offset, _block_size = struct.unpack_from(">II", sound)
    start = SSND_HEADER_SIZE + offset
    expected_bytes = channels * frames * (PCM_BITS // 8)
    if start > len(sound) or len(sound) - start != expected_bytes:
        invalid_aiff(path, "SSND offset or PCM length disagrees with COMM")
    samples = struct.unpack(f">{channels * frames}h", sound[start:])
    return PcmAudio(channels, frames, bits, sample_rate, samples)


def read_pcm(path: Path) -> list[int]:
    """Read validated 16-bit big-endian PCM samples, interleaved."""
    return list(read_audio(path).samples)


def compare(first: Path, second: Path) -> tuple[int, int, int]:
    """Return sample differences, rejecting mismatched metadata even when the sample counts agree."""
    first_audio = read_audio(first)
    second_audio = read_audio(second)
    first_metadata = (first_audio.channels, first_audio.frames, first_audio.bits, first_audio.sample_rate)
    second_metadata = (second_audio.channels, second_audio.frames, second_audio.bits, second_audio.sample_rate)
    if first_metadata != second_metadata:
        message = f"{first.name}: incompatible COMM metadata: {first_metadata} versus {second_metadata}"
        raise ValueError(message)
    differences = [abs(a - b) for a, b in zip(first_audio.samples, second_audio.samples, strict=True) if a != b]
    return len(first_audio.samples), len(differences), max(differences, default=0)


def require_identical(first: Path, second: Path, context: str) -> None:
    """Fail the benchmark on any PCM mismatch rather than merely printing an adverse verdict."""
    samples, differing, largest = compare(first, second)
    if differing:
        message = f"{context}: {differing}/{samples} samples differ, largest by {largest} PCM steps"
        raise ValueError(message)


def benchmark_runs(cli: str, presets: dict[str, Path], root: Path, runs: int, jobs: int) -> dict[str, list[float]]:
    """Compare every render across formats and against each format's first run, with counterbalanced run order."""
    timings: dict[str, list[float]] = {name: [] for name in FORMATS}
    references: dict[str, dict[str, Path]] = {}
    for run in range(runs):
        outputs: dict[str, dict[str, Path]] = {}
        order = FORMATS if run % 2 == 0 else list(reversed(FORMATS))
        for name in order:
            elapsed, outputs[name] = render(cli, presets[name], root / f"run-{run + 1}" / name, jobs)
            timings[name].append(elapsed)
            print(f"Run {run + 1} {name}: {elapsed:.2f} s", flush=True)
            if name in references:
                for input_name in INPUTS:
                    require_identical(
                        references[name][input_name], outputs[name][input_name], f"{name} run {run + 1} {input_name}"
                    )
            else:
                references[name] = outputs[name]
        for input_name in INPUTS:
            require_identical(outputs["VST3"][input_name], outputs["AU"][input_name], f"run {run + 1} {input_name}")
    print(f"All {runs} render pairs match exactly, including repeated-render determinism.")
    return timings


def positive_integer(value: str) -> int:
    """Accept only positive CLI counts so empty or nonsensical benchmark runs cannot succeed."""
    parsed = int(value)
    if parsed <= 0:
        message = "must be a positive integer"
        raise argparse.ArgumentTypeError(message)
    return parsed


def main() -> None:
    """Build the preset writer, render each format several times, and print the timings and the comparison."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default=shutil.which("audiobatch"), help="AudioBatch CLI executable")
    parser.add_argument(
        "--runs", type=positive_integer, default=4, help="Renders per format, an even count of at least 2"
    )
    parser.add_argument("--jobs", type=positive_integer, default=1, help="Files rendered at once")
    arguments = parser.parse_args()
    if arguments.runs < MINIMUM_RUNS or arguments.runs % 2 != 0:
        parser.error("--runs must be even and at least 2 for counterbalanced timing and repeat determinism")

    if sys.platform != "darwin":
        sys.exit("Audio Units exist only on macOS")
    if arguments.cli is None:
        sys.exit("AudioBatch CLI not found. Pass --cli or install it with cpp/install.sh")
    for name, path in INSTALLED.items():
        if not path.exists():
            sys.exit(f"The {name} is not installed at {path}. Run image-control-plugin/install.sh --validate")
    missing_inputs = [name for name in INPUTS if not (INPUT_DIRECTORY / name).is_file()]
    if missing_inputs:
        sys.exit(f"Missing inputs {missing_inputs}. Run uv run fixtures/benchmark/generate.py")

    build_preset_writer()
    with tempfile.TemporaryDirectory(prefix="imagecontrol-formats-") as temporary:
        root = Path(temporary)
        presets = write_presets(root / "presets")
        timings = benchmark_runs(arguments.cli, presets, root, arguments.runs, arguments.jobs)

        audio_seconds = 60 * len(INPUTS)
        print(f"\nRendering {len(INPUTS)} files of 60 s with {arguments.jobs} job(s), {arguments.runs} runs:")
        for name in FORMATS:
            median = statistics.median(timings[name])
            best = min(timings[name])
            print(f"  {name:5} median {median:.2f} s, best {best:.2f} s, {audio_seconds / median:.0f}x real time")


def entry_point() -> None:
    """Turn malformed audio, PCM mismatches, and subprocess failures into an unsuccessful command exit."""
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    entry_point()
