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
With a latency, the plugin delays its output, and both hosts render the delayed tail and drop the delayed start,
so a correct latency compensation writes the same samples as the plugin without latency.
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

# Source fixture, gain, width, and latency in samples of each expected output.
CASES = [
    ("stereo_split_44100_16.wav", 0.5, 0.5, 0),
    ("stereo_split_44100_16.wav", 1.0, 0.0, 0),
    ("stereo_split_44100_16.wav", 1.0, 2.0, 0),
    ("sine_mono_44100_16.wav", 0.5, 1.0, 0),
    ("stereo_split_44100_16.wav", 0.5, 0.5, 256),
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


def process(samples: np.ndarray, gain: float, width: float, latency: int) -> np.ndarray:
    """Apply the plugin arithmetic to int16 samples and return the floored int16 output a compensating host writes."""
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

    output = compensate_latency(delay(output, latency), latency)
    scaled = output.astype(np.float32) * PCM_SCALE
    limited = np.clip(scaled, np.float32(-32768.0), np.float32(32767.0))
    return np.floor(limited).astype(np.int16)


def delay(output: np.ndarray, latency: int) -> np.ndarray:
    """Delay the plugin output by the latency, the way its delay line does, while the host renders the tail."""
    silence = np.zeros((latency, output.shape[1]), dtype=output.dtype)
    return np.concatenate([silence, output])


def compensate_latency(rendered: np.ndarray, latency: int) -> np.ndarray:
    """Drop the delayed start the way both hosts compensate the reported latency, keeping the source length."""
    return rendered[latency:]


def expected_name(source: str, gain: float, width: float, latency: int) -> str:
    """Return the expected file name, such as ms_gain_0.5_width_0.5_stereo_split_44100_16.wav."""
    latency_part = f"latency_{latency}_" if latency else ""
    return f"ms_gain_{gain:g}_width_{width:g}_{latency_part}{source}"


def main() -> None:
    """Write every expected output into the plugin_expected directory."""
    OUTPUT_DIRECTORY.mkdir(exist_ok=True)
    for source, gain, width, latency in CASES:
        samples, sample_rate = read_pcm16(FIXTURES / source)
        output = process(samples, gain, width, latency)
        destination = OUTPUT_DIRECTORY / expected_name(source, gain, width, latency)
        write_pcm16(destination, output, sample_rate)
        print(f"{destination.relative_to(FIXTURES)}: {len(output)} frames")


if __name__ == "__main__":
    main()
