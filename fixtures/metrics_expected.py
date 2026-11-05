#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2.0", "scipy>=1.14"]
# ///
"""
Compute independent whole-file source statistics with NumPy, or verify them with --check.

Existing PCM16 WAV fixtures are decoded directly with the standard library.
Synthetic float32 cases exercise DC, inversion, full-scale endpoints, and multichannel behavior.
Ratios use whole-file true peak minus the specified loudness scalar, not a time-local PSR maximum.
"""

import argparse
import json
import shutil
import subprocess
import wave
from pathlib import Path

import numpy as np
from scipy.signal import lfilter

ROOT = Path(__file__).resolve().parent
OUTPUT = ROOT / "metrics_expected.json"
STEREO_CHANNELS = 2
PCM16_BYTES = 2
FIXTURES = [
    "sine_mono_24000_16.wav",
    "sine_stereo_44100_16.wav",
    "stereo_split_44100_16.wav",
    "silence_stereo_44100_16.wav",
]


def statistics(samples: np.ndarray) -> dict:
    """Calculate source-only statistics from decoded float32 samples using batch centered moments."""
    samples = samples.astype(np.float64)
    means = samples.mean(axis=0)
    centered = samples - means
    energy = np.mean(samples * samples)
    peak = np.max(np.abs(samples))
    channels = samples.shape[1]
    denominator = np.sqrt(np.sum(centered[:, 0] ** 2) * np.sum(centered[:, -1] ** 2))
    correlation = (
        1.0
        if channels == 1
        else float(np.sum(centered[:, 0] * centered[:, 1]) / denominator)
        if channels == STEREO_CHANNELS and denominator > 0
        else None
    )
    return {
        "crestFactorDb": float(20.0 * np.log10(peak / np.sqrt(energy))) if energy > 0 and peak > 0 else None,
        "dcOffsets": means.tolist(),
        "clipCounts": np.sum(np.abs(samples) >= 1.0, axis=0).tolist(),
        "stereoCorrelation": correlation,
    }


def loudness_ratios(samples: np.ndarray, rate: int) -> dict:
    """Measure fixture ratios with batch SciPy filters, NumPy energy windows, and a windowed-sinc interpolator."""
    tangent = np.tan(np.pi * 1681.974450955533 / rate)
    quality = 0.7071752369554196
    high = 10.0 ** (3.999843853973347 / 20.0)
    band = high**0.4996667741545416
    normalization = 1.0 + tangent / quality + tangent * tangent
    shelf_b = (
        np.array(
            [
                high + band * tangent / quality + tangent * tangent,
                2.0 * (tangent * tangent - high),
                high - band * tangent / quality + tangent * tangent,
            ]
        )
        / normalization
    )
    shelf_a = np.array(
        [
            1.0,
            2.0 * (tangent * tangent - 1.0) / normalization,
            (1.0 - tangent / quality + tangent * tangent) / normalization,
        ]
    )
    tangent = np.tan(np.pi * 38.13547087602444 / rate)
    normalization = 1.0 + tangent / 0.5003270373238773 + tangent * tangent
    high_a = [
        1.0,
        2.0 * (tangent * tangent - 1.0) / normalization,
        (1.0 - tangent / 0.5003270373238773 + tangent * tangent) / normalization,
    ]
    filtered = lfilter(
        np.convolve(shelf_b, [1.0, -2.0, 1.0]), np.convolve(shelf_a, high_a), samples.astype(np.float64), axis=0
    )
    channel_weight = 2.0 if samples.shape[1] == 1 else 1.0
    frame_energy = np.sum(filtered * filtered, axis=1) * channel_weight
    block = round(rate * 0.1)
    integrated_energies = [
        float(frame_energy[end - block * 4 : end].mean()) for end in range(block * 4, len(samples) + 1, block)
    ]
    absolute_gate = 10.0 ** ((-70.0 + 0.691) / 10.0)
    admitted = np.array([energy for energy in integrated_energies if energy >= absolute_gate])
    if admitted.size == 0:
        return {"plrDb": None, "psrDb": None}
    integrated = -0.691 + 10.0 * np.log10(admitted[admitted >= admitted.mean() * 0.1].mean())
    short_energy = max(
        float(frame_energy[max(0, end - block * 30) : end].sum()) / (block * 30)
        for end in range(block, len(samples) + block, block)
    )
    short = -0.691 + 10.0 * np.log10(short_energy)
    taps = 48
    phase_count = 4
    positions = np.arange(taps, dtype=np.float64)
    coefficients = (
        (0.5 - 0.5 * np.cos(2.0 * np.pi * positions / taps)) * np.sinc((positions - taps * 0.5) / phase_count)
    ).astype(np.float32)
    true_peak = float(np.max(np.abs(samples)))
    for channel in samples.T:
        for phase in range(phase_count):
            interpolated = np.convolve(channel, coefficients[phase::phase_count])[: len(channel)]
            true_peak = max(true_peak, float(np.max(np.abs(interpolated))))
    peak_db = 20.0 * np.log10(true_peak)
    return {"plrDb": float(peak_db - integrated), "psrDb": float(peak_db - short)}


def expected() -> dict:
    """Return shared fixture and exact synthetic PCM expectations without invoking either application."""
    cases = []
    for name in FIXTURES:
        with wave.open(str(ROOT / name), "rb") as source:
            if source.getsampwidth() != PCM16_BYTES:
                message = "Expected PCM16 fixture"
                raise ValueError(message)
            pcm = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2")
            samples = (pcm.astype(np.float32) / 32768.0).reshape(-1, source.getnchannels())
            rate = source.getframerate()
        cases.append({"fixture": name, **statistics(samples), **loudness_ratios(samples, rate)})
    left = np.array([-1.25, -1.0, -0.5, 0.0, 0.5, 1.0, 1.25, 0.25], dtype=np.float32)
    synthetic = {
        "mono": left[:, None],
        "identical": np.column_stack([left, left]),
        "inverted": np.column_stack([left, -left]),
        "dc": np.full((8, 2), [0.25, -0.5], dtype=np.float32),
        "one_silent": np.column_stack([left, np.zeros(8, dtype=np.float32)]),
        "silence": np.zeros((8, 2), dtype=np.float32),
        "multichannel": np.column_stack([left, left, -left]),
    }
    return {
        "fixtures": cases,
        "signals": [
            {"name": name, "planes": samples.T.tolist(), **statistics(samples)} for name, samples in synthetic.items()
        ],
    }


def main() -> None:
    """Write reproducible expectations, or fail when committed values differ."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    jq = shutil.which("jq")
    if jq is None:
        message = "jq not found in path, it formats the expected JSON like format.sh"
        raise SystemExit(message)
    text = subprocess.run(  # noqa: S603
        [jq, "--indent", "2", "."],
        input=json.dumps(expected(), indent=2, allow_nan=False),
        text=True,
        capture_output=True,
        check=True,
    ).stdout
    if arguments.check:
        if not OUTPUT.is_file() or OUTPUT.read_text() != text:
            message = "Source metric expectations are stale"
            raise SystemExit(message)
    else:
        OUTPUT.write_text(text)


if __name__ == "__main__":
    main()
