#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Compares the JSON results the C++ and the Rust CLI report for the same files.

The levels of every file must agree within the tolerance, and the formats and statuses exactly.
Files that failed in both are skipped.
With a normalization target, every re-analyzed output must also reach it,
or stop at the true peak ceiling below a loudness target.
Normalized copies live in a folder per CLI, so they are matched by their path inside it.
Exits with code 1 when anything differs.

Usage: uv run compare_levels.py <C++ JSON> <Rust JSON> [--tolerance <dB>]
    [--folders <C++ folder> <Rust folder>] [--peak <dBFS> | --lufs <LUFS>]
"""

import argparse
import json
import os
import sys
from pathlib import Path

# A normalized peak lands on the target within the quantization of the output samples.
PEAK_TARGET_TOLERANCE = 0.01
# Loudness lands within the gating of the loudness measurement, or stops at the true peak ceiling.
LOUDNESS_TARGET_TOLERANCE = 0.1
TRUE_PEAK_CEILING = -0.3
TRUE_PEAK_CEILING_TOLERANCE = 0.1

LEVEL_FIELDS = ["peakDbfs", "truePeakDbtp", "integratedLufs", "maxShortTermLufs"]
EXACT_FIELDS = ["sampleRate", "channels", "bitsPerSample", "status"]


def missed_peak(row: dict, target: float) -> str | None:
    """Returns why a normalized output missed the peak target, or None when it reached it."""
    peak = row["peakDbfs"]
    if peak is None or abs(peak - target) > PEAK_TARGET_TOLERANCE:
        return f"peak {peak} dBFS instead of {target}"
    return None


def missed_loudness(row: dict, target: float) -> str | None:
    """Returns why a normalized output missed the loudness target, or None when it reached it or the ceiling."""
    loudness = row["integratedLufs"]
    if loudness is None:
        return None
    true_peak = row["truePeakDbtp"]
    at_ceiling = true_peak is not None and abs(true_peak - TRUE_PEAK_CEILING) <= TRUE_PEAK_CEILING_TOLERANCE
    if abs(loudness - target) > LOUDNESS_TARGET_TOLERANCE and not (at_ceiling and loudness < target):
        return f"loudness {loudness} LUFS and true peak {true_peak} dBTP for a {target} LUFS target"
    return None


def load(path: Path, folder: Path | None) -> dict[str, dict]:
    """Returns the result rows of a JSON file by their path, relative to the folder when there is one."""
    rows = json.loads(path.read_text(encoding="utf-8"))
    if folder is not None:
        base = folder.resolve()
        return {os.path.relpath(Path(row["path"]).resolve(), base): row for row in rows}
    return {os.path.normpath(row["path"]): row for row in rows}


def compare_rows(name: str, cpp: dict, rust: dict, tolerance: float) -> list[str]:
    """Returns the format, status, and level differences between the two results of one file."""
    differences = [
        f"{name}: {field} C++ {cpp[field]} Rust {rust[field]}" for field in EXACT_FIELDS if cpp[field] != rust[field]
    ]
    for field in LEVEL_FIELDS:
        cpp_value = cpp[field]
        rust_value = rust[field]
        if cpp_value is None or rust_value is None:
            if cpp_value is not rust_value:
                differences.append(f"{name}: {field} C++ {cpp_value} Rust {rust_value}")
        elif abs(cpp_value - rust_value) > tolerance:
            differences.append(f"{name}: {field} C++ {cpp_value} Rust {rust_value}")
    return differences


def main() -> None:
    """Compares the two JSON files given on the command line and exits with code 1 when they differ."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cpp", type=Path, help="JSON output of the C++ CLI")
    parser.add_argument("rust", type=Path, help="JSON output of the Rust CLI")
    parser.add_argument("-t", "--tolerance", type=float, default=0.05, help="Allowed level difference in dB or LU")
    parser.add_argument(
        "-f", "--folders", type=Path, nargs=2, metavar="FOLDER", help="Folders the C++ and the Rust CLI normalized"
    )
    target = parser.add_mutually_exclusive_group()
    target.add_argument("-p", "--peak", type=float, help="Sample peak target the outputs must reach")
    target.add_argument("-l", "--lufs", type=float, help="Integrated loudness target the outputs must reach")
    arguments = parser.parse_args()

    cpp_folder, rust_folder = arguments.folders or (None, None)
    cpp_rows = load(arguments.cpp, cpp_folder)
    rust_rows = load(arguments.rust, rust_folder)

    differences = []
    paths = sorted(set(cpp_rows) | set(rust_rows))
    for path in paths:
        cpp = cpp_rows.get(path)
        rust = rust_rows.get(path)
        name = Path(path).name
        if cpp is None or rust is None:
            differences.append(f"{name}: only in {'C++' if rust is None else 'Rust'} output")
            continue
        if cpp["status"] == "failed" and rust["status"] == "failed":
            continue
        if cpp["status"] != "failed":
            for label, row in (("C++", cpp), ("Rust", rust)):
                if arguments.peak is not None:
                    reason = missed_peak(row, arguments.peak)
                elif arguments.lufs is not None:
                    reason = missed_loudness(row, arguments.lufs)
                else:
                    reason = None
                if reason is not None:
                    differences.append(f"{name}: {label} output missed the target, {reason}")
        differences += compare_rows(name, cpp, rust, arguments.tolerance)

    print(f"Compared {len(paths)} files with a tolerance of {arguments.tolerance}")
    if differences:
        print(f"{len(differences)} difference(s):")
        for difference in differences:
            print(f"  {difference}")
        sys.exit(1)
    print("No differences")


if __name__ == "__main__":
    main()
