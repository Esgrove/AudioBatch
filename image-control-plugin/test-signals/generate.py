#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2.1", "soundfile>=0.12"]
# ///
"""
Generates deterministic stereo signals for ImageControl display checks and manual Width/Recover listening comparisons.

Play them through the plugin in a host to inspect stereo placement or compare processing settings.
These are synthetic inputs, not listening-test results or evidence of lossless recovery.
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
- listening_reverb_like_<variant>: centred dry attacks with synthetic, decaying independent or pure-Side tails.
- listening_pure_side_<variant>: anti-phase octave comb and logarithmic sweep, with no source Mid.
- listening_phase_<variant>: quadrature channel pairs and Mid/Side quadrature mixtures of both signs,
  including a Mid/Side equal-amplitude pair that stresses phase-dependent cancellation and peak addition.
- listening_hard_<side>_transient_bass: decaying 55 Hz bursts and short broadband attacks in one channel only.

The files are 48 kHz 32-bit float WAV, 10 seconds long, and every run writes the same samples.
Files that already exist are kept, so run with --force to write them again.
--group listening selects only the Width/Recover inputs, and --output chooses a different directory.
Compare Recover Off/on below Width 1, then check Delta, Mono, and output peaks.
Both signs of the adversarial pair and both hard-pan directions matter because recovery is not L/R-swap invariant.

Usage: uv run --script image-control-plugin/test-signals/generate.py [--force] [--group listening] [--output PATH]
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
LISTENING_PEAK_DB = -12.0
LISTENING_TONE_GAIN_DB = -30.0
PHASE_FREQUENCIES = (80.0, 380.0, 2400.0, 11_000.0)
BURST_TIMES_SECONDS = (0.5, 3.0, 5.5, 8.0)
REVERB_DECAY_SECONDS = 0.6
BASS_DECAY_SECONDS = 0.22
DRY_DECAY_SECONDS = 0.015
TRANSIENT_DECAY_SECONDS = 0.008
ATTACK_SECONDS = 0.003


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


def spatial_signals() -> dict[str, np.ndarray]:
    """Returns the original spatial-display inputs, without changing their samples or names."""
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


def from_mid_side(mid: np.ndarray, side: np.ndarray) -> np.ndarray:
    """Decodes source Mid and Side without a gain-compensating normalization."""
    return np.stack([mid + side, mid - side], axis=1)


def listening_level(audio: np.ndarray) -> np.ndarray:
    """Sets input peak headroom only, preserving channel ratios and phase, not matching processed loudness."""
    peak = float(np.max(np.abs(audio)))
    return audio * gain(LISTENING_PEAK_DB) / peak if peak > 0.0 else audio


def burst_envelope(decay_seconds: float) -> np.ndarray:
    """Returns repeated exponential decays with short smooth onsets and a quiet final tail."""
    time = time_axis()
    envelope = np.zeros(SAMPLES)
    for onset in BURST_TIMES_SECONDS:
        elapsed = np.maximum(time - onset, 0.0)
        attack = np.minimum(elapsed / ATTACK_SECONDS, 1.0)
        envelope += attack**2 * np.exp(-elapsed / decay_seconds)
    return envelope


def reverb_like_signals() -> dict[str, np.ndarray]:
    """Builds synthetic diffuse decays around centred attacks, without modeling a measured room."""
    tail = burst_envelope(REVERB_DECAY_SECONDS)
    attack = burst_envelope(DRY_DECAY_SECONDS)
    dry = attack * (sine(220.0, SINE_GAIN_DB) + sine(1760.0, SINE_GAIN_DB))
    left = pink_noise(np.random.default_rng(SEED + 10))
    right = pink_noise(np.random.default_rng(SEED + 11))
    side = pink_noise(np.random.default_rng(SEED + 12))
    return {
        "listening_reverb_like_stereo": listening_level(np.stack([dry + tail * left, dry + tail * right], axis=1)),
        "listening_reverb_like_side_tail": listening_level(from_mid_side(dry, tail * side)),
    }


def phase_signals() -> dict[str, np.ndarray]:
    """Builds quadrature mixtures of both signs and amplitudes to expose phase-dependent recovery limits."""
    phase = 2.0 * np.pi * np.asarray(PHASE_FREQUENCIES)[:, np.newaxis] * time_axis()
    in_phase = gain(LISTENING_TONE_GAIN_DB) * np.sum(np.sin(phase), axis=0)
    quadrature = gain(LISTENING_TONE_GAIN_DB) * np.sum(np.cos(phase), axis=0)
    return {
        "listening_phase_channel_quadrature": listening_level(np.stack([in_phase, quadrature], axis=1)),
        "listening_phase_channel_quadrature_swapped": listening_level(np.stack([quadrature, in_phase], axis=1)),
        "listening_phase_mid_side_plus": listening_level(from_mid_side(quadrature, in_phase)),
        "listening_phase_mid_side_minus": listening_level(from_mid_side(-quadrature, in_phase)),
        "listening_phase_mid_side_half_plus": listening_level(from_mid_side(quadrature * 0.5, in_phase)),
        "listening_phase_mid_side_half_minus": listening_level(from_mid_side(-quadrature * 0.5, in_phase)),
    }


def listening_signals() -> dict[str, np.ndarray]:
    """Returns deterministic manual-comparison inputs, not a claim that any listening evaluation has been performed."""
    bass = burst_envelope(BASS_DECAY_SECONDS) * sine(55.0, SINE_GAIN_DB)
    transient = burst_envelope(TRANSIENT_DECAY_SECONDS) * white_noise(np.random.default_rng(SEED + 13))
    hard = listening_level(bass + transient)
    pure_side = Pan("side", 1.0, -1.0)
    return {
        **reverb_like_signals(),
        "listening_pure_side_comb": listening_level(comb([pure_side] * len(COMB_FREQUENCIES))),
        "listening_pure_side_sweep": place(log_sweep(LISTENING_PEAK_DB), pure_side),
        **phase_signals(),
        **{f"listening_hard_{side}_transient_bass": place(hard, level_difference(np.inf, side)) for side in ("L", "R")},
    }


def signals(group: str = "all") -> dict[str, np.ndarray]:
    """Returns the selected collection, keeping the original spatial signals unchanged."""
    result = spatial_signals() if group in ("all", "spatial") else {}
    if group in ("all", "listening"):
        result.update(listening_signals())
    return result


def main() -> None:
    """Writes selected inputs that do not exist yet, or replaces them with --force."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="write the files again even when they exist")
    parser.add_argument(
        "--group", choices=("all", "spatial", "listening"), default="all", help="select the input collection"
    )
    parser.add_argument("--output", type=Path, default=OUTPUT_DIRECTORY, help="directory for generated WAV files")
    arguments = parser.parse_args()

    arguments.output.mkdir(parents=True, exist_ok=True)
    written = 0
    for name, audio in signals(arguments.group).items():
        path = arguments.output / f"{name}.wav"
        if path.exists() and not arguments.force:
            continue
        if audio.shape != (SAMPLES, 2) or not np.all(np.isfinite(audio)):
            message = f"{name} has an invalid stereo shape or non-finite samples"
            raise ValueError(message)
        peak = float(np.max(np.abs(audio)))
        if peak > 1.0:
            message = f"{name} peaks at {peak:.3f}, above full scale"
            raise ValueError(message)
        sf.write(path, audio.astype(np.float32), SAMPLE_RATE, subtype="FLOAT")
        written += 1
    print(f"Wrote {written} test signals to {arguments.output}")


if __name__ == "__main__":
    main()
