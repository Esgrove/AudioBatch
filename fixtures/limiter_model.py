# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy", "scipy"]
# ///
"""
Fit exploratory Pro-L 2 preset models from full-file release-render measurements.

The 24 observations come from four music files and one saved L2 preset.
They are numerical measurements only, with no audio or proprietary plugin state.
Validation holds out complete tracks, never random gain points from the same track.
Run through uv, optionally passing --output to save coefficients and validation data.
These fits are preset-specific research models, not a universal limiter contract.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np
from scipy.optimize import least_squares

if TYPE_CHECKING:
    from collections.abc import Callable

    from numpy.typing import NDArray

GAINS = (0.0, 1.0, 2.0, 3.0, 4.0, 6.0)
CEILING_OFFSET = -0.3
# Each tuple contains source LUFS-I, hard-clipped LUFS-I, and measured L2 output LUFS-I.
TRACKS = {
    "Love Language": (
        -10.8243,
        (-10.8243, -9.8398, -8.9767, -8.2230, -7.5373, -6.2739),
        (-11.1270, -10.2378, -9.6740, -9.2158, -8.7972, -8.0662),
    ),
    "Family Affair": (
        -11.1205,
        (-11.1205, -10.1330, -9.1929, -8.2998, -7.4532, -5.8990),
        (-11.4216, -10.4905, -9.6939, -8.9804, -8.3493, -7.3561),
    ),
    "The Drum": (
        -8.6760,
        (-8.6760, -7.6939, -6.7819, -5.9452, -5.1855, -3.8527),
        (-8.9773, -8.0856, -7.4484, -6.9371, -6.5254, -5.9877),
    ),
    "Pork": (
        -13.3286,
        (-13.3286, -12.3325, -11.3533, -10.3924, -9.4567, -7.6792),
        (-13.6298, -12.6452, -11.6946, -10.7813, -9.9186, -8.4090),
    ),
}


def source_saturation(parameters: NDArray, source: NDArray, gain: NDArray, _clipped: NDArray) -> NDArray:
    """Apply a smooth loudness ceiling with a fitted transition width."""
    ceiling, width = parameters
    gained_excess = width * np.logaddexp(0.0, (source + gain - ceiling) / width)
    original_excess = width * np.logaddexp(0.0, (source - ceiling) / width)
    return source + CEILING_OFFSET + gain - gained_excess + original_excess


def content_aware(parameters: NDArray, source: NDArray, gain: NDArray, clipped: NDArray) -> NDArray:
    """Estimate additional limiter attenuation from measured hard-clip loudness loss."""
    scale, exponent = parameters
    loss = np.maximum(source + gain - clipped, 0.0)
    return clipped + CEILING_OFFSET - scale * loss**exponent


def errors(predicted: NDArray, measured: NDArray) -> dict[str, float]:
    """Summarize signed residuals in LU without interpreting them as confidence bounds."""
    residual = predicted - measured
    return {
        "mae_lu": float(np.mean(np.abs(residual))),
        "rmse_lu": float(np.sqrt(np.mean(residual**2))),
        "maximum_absolute_error_lu": float(np.max(np.abs(residual))),
    }


def fit_model(
    model: Callable,
    initial: list[float],
    bounds: tuple[list[float], list[float]],
    observations: NDArray,
) -> dict:
    """Fit all points and independently refit four times with one whole track withheld."""
    source, gain, clipped, measured, track = observations.T
    fitted = least_squares(lambda p: model(p, source, gain, clipped) - measured, initial, bounds=bounds)
    predicted = model(fitted.x, source, gain, clipped)
    held_out = np.empty_like(measured)
    folds = []
    for index, name in enumerate(TRACKS):
        test = track == index
        train = ~test
        fit = least_squares(
            lambda p, train=train: model(p, source[train], gain[train], clipped[train]) - measured[train],
            initial,
            bounds=bounds,
        )
        held_out[test] = model(fit.x, source[test], gain[test], clipped[test])
        folds.append({"track": name, "coefficients": fit.x.tolist(), **errors(held_out[test], measured[test])})
    return {
        "coefficients": fitted.x.tolist(),
        "training": errors(predicted, measured),
        "leave_one_track_out": errors(held_out, measured),
        "leave_one_track_out_positive_gain": errors(held_out[gain > 0.0], measured[gain > 0.0]),
        "fitted_predictions": predicted.reshape(len(TRACKS), len(GAINS)).tolist(),
        "held_out_predictions": held_out.reshape(len(TRACKS), len(GAINS)).tolist(),
        "folds": folds,
    }


def main() -> None:
    """Print reproducible coefficients, comparisons, and held-out errors, optionally saving JSON."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="Write the full fit report as JSON")
    args = parser.parse_args()
    observations = np.array(
        [
            (source, gain, clip, output, index)
            for index, (source, clips, outputs) in enumerate(TRACKS.values())
            for gain, clip, output in zip(GAINS, clips, outputs, strict=True)
        ]
    )
    source, gain, clipped, measured, _ = observations.T
    report = {
        "scope": {
            "plugin": "FabFilter Pro-L 2 v2.2.6.0",
            "preset": "L2",
            "render_profile": "release",
            "output_format": "16-bit AIFF",
            "measurement_date": "2026-11-04",
            "tracks": len(TRACKS),
            "observations": len(observations),
            "source_lufs_range": [float(source.min()), float(source.max())],
            "gain_db_range": [min(GAINS), max(GAINS)],
            "ceiling_offset_lu": CEILING_OFFSET,
            "warning": "Exploratory preset-specific fit. Four tracks cannot establish generalization or uncertainty.",
        },
        "data": [
            {"track": name, "source": source, "gains": GAINS, "hard_clip": clips, "limiter": outputs}
            for name, (source, clips, outputs) in TRACKS.items()
        ],
        "baselines": {
            "gain_only": errors(source + gain, measured),
            "hard_clip": errors(clipped, measured),
            "hard_clip_with_ceiling": errors(clipped + CEILING_OFFSET, measured),
        },
        "source_saturation": fit_model(source_saturation, [-6.0, 2.0], ([-30.0, 0.05], [5.0, 20.0]), observations),
        "content_aware": fit_model(content_aware, [1.5, 0.7], ([0.0, 0.05], [20.0, 3.0]), observations),
    }
    text = json.dumps(report, indent=2, allow_nan=False)
    print(text)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
