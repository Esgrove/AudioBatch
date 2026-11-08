#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2.1", "soundfile>=0.12"]
# ///
"""
Generates stereo test signals for checking ImageControl's spatial spectrogram by eye, into test-signals/output.

Play them through the plugin in a host to see where each sound lands and how wide it draws.
A pan here is the level difference between the channels in dB, with the louder channel at full level
and the quieter one lowered by the difference, so the files show the stereo position curve directly.
The 12 dB pans land half way to the edge at the default pan midpoint,
and the 32 dB pans nearly at the edge at the default pan edge.

- sine_1k_<pan>: a 1 kHz sine at the centre, at 6, 12, 20, and 32 dB to either side, and in one channel only.
- sine_1k_pan_steps: the same 1 kHz sine stepping through all of those pans in equal steps of under a second.
- sine_90_<pan>: a 90 Hz sine, where the bass resolution is the hardest, at the centre, 12 dB left, and right only.
- sine_sweep_log: a centred logarithmic sweep from 20 Hz to 20 kHz, which shows the particle width across frequency.
- comb_<pattern>: sines an octave apart from 40 Hz to 10240 Hz, all centred, all 12 dB left,
  alternating between left only and right only, alternating 12 dB left and right,
  and fanned from left only at 40 Hz to right only at 10240 Hz.
- white_<variant> and pink_<variant>: full spectrum noise with identical channels at the centre,
  independent channels, the left channel only, 12 dB left, and in anti-phase.

The files are 48 kHz 32-bit float WAV, 10 seconds long, and every run writes the same samples.
Files that already exist are kept, so run with --force to write them again.

Usage: uv run --script image-control-plugin/test-signals/generate.py [--force]
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np
import soundfile as sf

if TYPE_CHECKING:
    from collections.abc import Callable

OUTPUT_DIRECTORY = Path(__file__).resolve().parent / "output"
SAMPLE_RATE = 48_000
DURATION_SECONDS = 10.0
SAMPLES = int(SAMPLE_RATE * DURATION_SECONDS)
SINE_GAIN_DB = -12.0
COMB_TONE_GAIN_DB = -24.0
NOISE_RMS_DB = -18.0
SWEEP_START_HZ = 20.0
SWEEP_END_HZ = 20_000.0
COMB_FREQUENCIES = [40.0 * 2.0**octave for octave in range(9)]
# Gain changes between the pan steps ramp over this long, so the steps do not click.
STEP_RAMP_SECONDS = 0.01
SEED = 20_261_108


@dataclass(frozen=True)
class Pan:
    """A stereo placement as the gains of the left and right channels."""

    name: str
    left: float
    right: float


def level_difference(decibels: float, side: str) -> Pan:
    """Returns the pan with the given channel louder by decibels, or with the other channel silent when infinite."""
    quieter = 0.0 if np.isinf(decibels) else 10.0 ** (-decibels / 20.0)
    label = "only" if np.isinf(decibels) else f"{decibels:g}"
    if side == "L":
        return Pan(f"L{label}", 1.0, quieter)
    return Pan(f"R{label}", quieter, 1.0)


CENTRE = Pan("C", 1.0, 1.0)
SINE_PANS = [
    CENTRE,
    *(level_difference(decibels, side) for decibels in (6.0, 12.0, 20.0, 32.0, np.inf) for side in ("L", "R")),
]
# From left only through the centre to right only, one pan per comb tone.
FAN_PANS = [
    *(level_difference(decibels, "L") for decibels in (np.inf, 32.0, 20.0, 12.0)),
    CENTRE,
    *(level_difference(decibels, "R") for decibels in (12.0, 20.0, 32.0, np.inf)),
]


def gain(decibels: float) -> float:
    """Returns the linear gain of a level in dB."""
    return 10.0 ** (decibels / 20.0)


def time_axis() -> np.ndarray:
    """Returns the time of every sample in seconds."""
    return np.arange(SAMPLES) / SAMPLE_RATE


def sine(frequency: float, decibels: float) -> np.ndarray:
    """Returns a sine of a frequency at a peak level in dBFS."""
    return gain(decibels) * np.sin(2.0 * np.pi * frequency * time_axis())


def log_sweep(decibels: float) -> np.ndarray:
    """Returns a logarithmic sine sweep from SWEEP_START_HZ to SWEEP_END_HZ over the whole duration."""
    rate = np.log(SWEEP_END_HZ / SWEEP_START_HZ) / DURATION_SECONDS
    phase = 2.0 * np.pi * SWEEP_START_HZ * (np.exp(rate * time_axis()) - 1.0) / rate
    return gain(decibels) * np.sin(phase)


def white_noise(generator: np.random.Generator) -> np.ndarray:
    """Returns white noise at NOISE_RMS_DB."""
    noise = generator.standard_normal(SAMPLES)
    return noise * gain(NOISE_RMS_DB) / np.sqrt(np.mean(noise**2))


def pink_noise(generator: np.random.Generator) -> np.ndarray:
    """Returns pink noise at NOISE_RMS_DB, shaped from white noise to fall 3 dB per octave."""
    spectrum = np.fft.rfft(generator.standard_normal(SAMPLES))
    frequencies = np.fft.rfftfreq(SAMPLES, 1.0 / SAMPLE_RATE)
    shaping = np.zeros_like(frequencies)
    shaping[1:] = 1.0 / np.sqrt(frequencies[1:])
    noise = np.fft.irfft(spectrum * shaping, SAMPLES)
    return noise * gain(NOISE_RMS_DB) / np.sqrt(np.mean(noise**2))


def place(signal: np.ndarray, pan: Pan) -> np.ndarray:
    """Returns a mono signal placed in stereo with a pan, as samples by channels."""
    return np.stack([signal * pan.left, signal * pan.right], axis=1)


def pan_steps(signal: np.ndarray, pans: list[Pan]) -> np.ndarray:
    """Returns a mono signal stepping through pans in equal steps over the whole duration."""
    indices = np.arange(SAMPLES) * len(pans) // SAMPLES
    gains = np.array([[pan.left, pan.right] for pan in pans])[indices]
    ramp = int(SAMPLE_RATE * STEP_RAMP_SECONDS)
    kernel = np.ones(ramp) / ramp
    smoothed = np.stack([np.convolve(gains[:, channel], kernel, mode="same") for channel in range(2)], axis=1)
    return signal[:, np.newaxis] * smoothed


def comb(pans: list[Pan]) -> np.ndarray:
    """Returns the octave comb with each tone placed at its own pan."""
    return sum(
        place(sine(frequency, COMB_TONE_GAIN_DB), pan) for frequency, pan in zip(COMB_FREQUENCIES, pans, strict=True)
    )


def noise_variants(name: str, make: Callable[[np.random.Generator], np.ndarray], seed: int) -> dict[str, np.ndarray]:
    """Returns the noise of one colour at the centre, independent per channel, left only, 12 dB left, and inverted."""
    noise = make(np.random.default_rng(seed))
    independent = make(np.random.default_rng(seed + 1))
    return {
        f"{name}_centre": place(noise, CENTRE),
        f"{name}_uncorrelated": np.stack([noise, independent], axis=1),
        f"{name}_Lonly": place(noise, level_difference(np.inf, "L")),
        f"{name}_L12": place(noise, level_difference(12.0, "L")),
        f"{name}_antiphase": place(noise, Pan("antiphase", 1.0, -1.0)),
    }


def signals() -> dict[str, np.ndarray]:
    """Returns every test signal by its file name without the extension."""
    one_kilohertz = sine(1000.0, SINE_GAIN_DB)
    ninety = sine(90.0, SINE_GAIN_DB)
    hard_alternating = [level_difference(np.inf, "L" if index % 2 == 0 else "R") for index in range(9)]
    soft_alternating = [level_difference(12.0, "L" if index % 2 == 0 else "R") for index in range(9)]
    return {
        **{f"sine_1k_{pan.name}": place(one_kilohertz, pan) for pan in SINE_PANS},
        "sine_1k_pan_steps": pan_steps(one_kilohertz, SINE_PANS),
        **{
            f"sine_90_{pan.name}": place(ninety, pan)
            for pan in (CENTRE, level_difference(12.0, "L"), level_difference(np.inf, "R"))
        },
        "sine_sweep_log": place(log_sweep(SINE_GAIN_DB), CENTRE),
        "comb_centre": comb([CENTRE] * 9),
        "comb_L12": comb([level_difference(12.0, "L")] * 9),
        "comb_alternating_hard": comb(hard_alternating),
        "comb_alternating_12": comb(soft_alternating),
        "comb_fan": comb(FAN_PANS),
        **noise_variants("white", white_noise, SEED),
        **noise_variants("pink", pink_noise, SEED + 2),
    }


def main() -> None:
    """Writes every test signal that does not exist yet, or all of them with --force."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="write the files again even when they exist")
    arguments = parser.parse_args()

    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    written = 0
    for name, audio in signals().items():
        path = OUTPUT_DIRECTORY / f"{name}.wav"
        if path.exists() and not arguments.force:
            continue
        peak = float(np.max(np.abs(audio)))
        if peak > 1.0:
            message = f"{name} peaks at {peak:.3f}, above full scale"
            raise ValueError(message)
        sf.write(path, audio.astype(np.float32), SAMPLE_RATE, subtype="FLOAT")
        written += 1
    print(f"Wrote {written} test signals to {OUTPUT_DIRECTORY}")


if __name__ == "__main__":
    main()
