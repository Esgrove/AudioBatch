#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2.0"]
# ///
"""
Writes the outputs the AudioBatch Test MS plugin must produce from the 16-bit fixture tones.

Both applications process a file through the plugin chain and write 16-bit AIFF,
and their tests compare that output against these files sample by sample.
The script repeats the arithmetic of test-plugin/src/MidSideTestProcessor.cpp in float32, operation by operation,
then floors to 16 bits without dither and saturates, the way both applications write their output.
Each expected file is named after the parameter values and the source fixture.
The files are written as 16-bit WAV with the standard library, so only numpy is needed.
"""

import sys
import wave
from pathlib import Path

import numpy as np

FIXTURES = Path(__file__).resolve().parent
OUTPUT_DIRECTORY = FIXTURES / "plugin_expected"

PCM_SCALE = np.float32(32768.0)
HALF = np.float32(0.5)
SAMPLE_WIDTH_BYTES = 2
STEREO_CHANNELS = 2

# Source fixture, gain, and width of each expected output.
CASES = [
    ("stereo_split_44100_16.wav", 0.5, 0.5),
    ("stereo_split_44100_16.wav", 1.0, 0.0),
    ("stereo_split_44100_16.wav", 1.0, 2.0),
    ("sine_mono_44100_16.wav", 0.5, 1.0),
]


def read_pcm16(path: Path) -> tuple[np.ndarray, int]:
    """Read a 16-bit WAV file into a frames by channels int16 array and its sample rate."""
    with wave.open(str(path), "rb") as reader:
        if reader.getsampwidth() != SAMPLE_WIDTH_BYTES:
            sys.exit(f"{path.name} is not 16-bit PCM")
        channels = reader.getnchannels()
        sample_rate = reader.getframerate()
        frames = reader.readframes(reader.getnframes())
    samples = np.frombuffer(frames, dtype="<i2").reshape(-1, channels)
    return samples, sample_rate


def write_pcm16(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """Write a frames by channels int16 array as a 16-bit WAV file."""
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(samples.shape[1])
        writer.setsampwidth(SAMPLE_WIDTH_BYTES)
        writer.setframerate(sample_rate)
        writer.writeframes(samples.astype("<i2").tobytes())


def process(samples: np.ndarray, gain: float, width: float) -> np.ndarray:
    """Apply the plugin arithmetic to int16 samples and return the floored int16 output."""
    audio = samples.astype(np.float32) / PCM_SCALE
    gain_value = np.float32(gain)
    width_value = np.float32(width)

    if audio.shape[1] == STEREO_CHANNELS:
        left = audio[:, 0]
        right = audio[:, 1]
        mid = (left + right) * HALF
        side = ((left - right) * HALF) * width_value
        output = np.stack([(mid + side) * gain_value, (mid - side) * gain_value], axis=1)
    else:
        output = audio * gain_value

    scaled = output.astype(np.float32) * PCM_SCALE
    limited = np.clip(scaled, np.float32(-32768.0), np.float32(32767.0))
    return np.floor(limited).astype(np.int16)


def expected_name(source: str, gain: float, width: float) -> str:
    """Return the expected file name, such as ms_gain_0.5_width_0.5_stereo_split_44100_16.wav."""
    return f"ms_gain_{gain:g}_width_{width:g}_{source}"


def main() -> None:
    """Write every expected output into the plugin_expected directory."""
    OUTPUT_DIRECTORY.mkdir(exist_ok=True)
    for source, gain, width in CASES:
        samples, sample_rate = read_pcm16(FIXTURES / source)
        output = process(samples, gain, width)
        destination = OUTPUT_DIRECTORY / expected_name(source, gain, width)
        write_pcm16(destination, output, sample_rate)
        print(f"{destination.relative_to(FIXTURES)}: {len(output)} frames")


if __name__ == "__main__":
    main()
