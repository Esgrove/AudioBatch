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
and compares the 16-bit PCM the two formats wrote, sample by sample.

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
from pathlib import Path

PLUGIN_DIRECTORY = Path(__file__).resolve().parent.parent
REPOSITORY = PLUGIN_DIRECTORY.parent
BUILD_DIRECTORY = PLUGIN_DIRECTORY / "cmake-build-benchmark"
WRITER = BUILD_DIRECTORY / "ImageControlPresetWriter_artefacts" / "Release" / "ImageControlPresetWriter"
INPUT_DIRECTORY = REPOSITORY / "fixtures" / "benchmark" / "output"
INPUTS = ["music_stereo_44100_24.wav", "music_stereo_48000_24.wav", "noise_stereo_44100_24.wav"]
FORMATS = ["VST3", "AU"]
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
    )
    subprocess.run(  # noqa: S603
        [cmake, "--build", str(BUILD_DIRECTORY), "--config", "Release", "--target", "ImageControlPresetWriter"],
        check=True,
        stdout=subprocess.DEVNULL,
    )


def write_presets(directory: Path) -> dict[str, Path]:
    """Write the chain preset of each installed format and return them by format."""
    subprocess.run([str(WRITER), str(directory)], check=True, stdout=subprocess.DEVNULL)  # noqa: S603
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
        command + [str(source) for source in sources], check=False, env=environment, capture_output=True, text=True
    )
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        sys.exit(f"audiobatch process failed with {preset.name}:\n{result.stderr}")
    records = json.loads(result.stdout)
    failed = [record["path"] for record in records if record.get("status") != "analyzed"]
    if failed:
        sys.exit(f"Rendering failed with {preset.name}: {failed}")
    return elapsed, {name: (audio / name).with_suffix(".aif") for name in INPUTS}


def read_pcm(path: Path) -> list[int]:
    """Read the 16-bit big-endian PCM samples of an AIFF file's SSND chunk, interleaved."""
    data = path.read_bytes()
    position = data.index(b"SSND")
    size = struct.unpack_from(">I", data, position + 4)[0]
    offset = struct.unpack_from(">I", data, position + 8)[0]
    samples = data[position + 16 + offset : position + 8 + size]
    return list(struct.unpack(f">{len(samples) // 2}h", samples))


def compare(first: Path, second: Path) -> tuple[int, int, int]:
    """Return the sample count, the number of differing samples, and the largest difference in 16-bit steps."""
    first_pcm = read_pcm(first)
    second_pcm = read_pcm(second)
    if len(first_pcm) != len(second_pcm):
        sys.exit(f"{first.name}: the formats wrote {len(first_pcm)} and {len(second_pcm)} samples")
    differences = [abs(a - b) for a, b in zip(first_pcm, second_pcm, strict=True) if a != b]
    return len(first_pcm), len(differences), max(differences, default=0)


def main() -> None:
    """Build the preset writer, render each format several times, and print the timings and the comparison."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default=shutil.which("audiobatch"), help="AudioBatch CLI executable")
    parser.add_argument("--runs", type=int, default=3, help="Renders per format, alternating the formats")
    parser.add_argument("--jobs", type=int, default=1, help="Files rendered at once")
    arguments = parser.parse_args()

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
        timings: dict[str, list[float]] = {name: [] for name in FORMATS}
        outputs: dict[str, dict[str, Path]] = {}
        for run in range(arguments.runs):
            for name in FORMATS:
                elapsed, outputs[name] = render(arguments.cli, presets[name], root / name, arguments.jobs)
                timings[name].append(elapsed)
                print(f"Run {run + 1} {name}: {elapsed:.2f} s", flush=True)

        audio_seconds = 60 * len(INPUTS)
        print(f"\nRendering {len(INPUTS)} files of 60 s with {arguments.jobs} job(s), {arguments.runs} runs:")
        for name in FORMATS:
            median = statistics.median(timings[name])
            best = min(timings[name])
            print(f"  {name:5} median {median:.2f} s, best {best:.2f} s, {audio_seconds / median:.0f}x real time")

        print("\nOutput comparison, VST3 against AU:")
        for input_name in INPUTS:
            samples, differing, largest = compare(outputs["VST3"][input_name], outputs["AU"][input_name])
            verdict = "identical" if differing == 0 else f"{differing} differ, largest by {largest}"
            print(f"  {input_name}: {samples} samples, {verdict}")


if __name__ == "__main__":
    main()
