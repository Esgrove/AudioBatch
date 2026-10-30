#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Compares CLI JSON results against each other and independent fixture expectations.

Known fixture outcomes and durations come from the committed analysis goldens.
Normalization expectations are calculated before either CLI touches its copies,
including silence, excessive gain, and deterministic output conflicts.
Unexpected failures never pass just because both implementations agree.
Frame counts are exact when the CLI exposes them, and durations use its millisecond precision.
"""

import argparse
import json
import math
import os
import sys
from pathlib import Path

PEAK_TARGET_TOLERANCE = 0.01
LOUDNESS_TARGET_TOLERANCE = 0.1
TRUE_PEAK_CEILING = -0.3
TRUE_PEAK_CEILING_TOLERANCE = 0.1
DURATION_TOLERANCE = 0.001000001
MAXIMUM_GAIN_DB = 40
SILENT_LOUDNESS = -1000
GOLDENS = Path(__file__).resolve().parents[1] / "crates/core/tests/golden"
AUDIO_EXTENSIONS = {".wav", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".m4a", ".mp4", ".ogg"}
LEVEL_FIELDS = ["peakDbfs", "truePeakDbtp", "integratedLufs", "maxShortTermLufs"]
EXACT_FIELDS = ["sampleRate", "channels", "bitsPerSample", "status"]
FRAME_FIELDS = ["lengthInSamples", "frameCount", "frames"]


def fixture_properties(path: Path) -> dict[str, str]:
    """Returns committed properties only for a recognized fixture collection and file name."""
    collection = {"fixtures": "fixtures", "normalization": "normalization"}.get(path.parent.name)
    candidates = (
        [GOLDENS / collection / f"{path.name}.txt"]
        if collection
        else [GOLDENS / name / f"{path.name}.txt" for name in ("fixtures", "normalization")]
    )
    matches = [candidate for candidate in candidates if candidate.is_file()]
    if len(matches) != 1:
        return {}
    golden = matches[0]
    return dict(line.split(" = ", 1) for line in golden.read_text(encoding="utf-8").splitlines() if " = " in line)


def golden_number(properties: dict, field: str) -> float:
    """Returns a numeric golden value without its bit representation."""
    return float(properties[field].split()[0])


def expected_failure(properties: dict, peak: float | None, lufs: float | None) -> bool:
    """Returns whether the committed fixture must fail analysis or the requested normalization."""
    if properties.get("status") == "Failed":
        return True
    if not properties or (peak is None and lufs is None):
        return False
    amplitude = abs(golden_number(properties, "overall_peak"))
    if amplitude <= 0:
        return True
    if lufs is None:
        gain = peak - 20 * math.log10(amplitude)
    else:
        loudness = golden_number(properties, "integrated_lufs")
        if loudness <= SILENT_LOUDNESS:
            return True
        true_peak = max(amplitude, golden_number(properties, "overall_true_peak"))
        gain = min(lufs - loudness, TRUE_PEAK_CEILING - 20 * math.log10(true_peak))
    return gain > MAXIMUM_GAIN_DB


def input_files(paths: list[Path], recurse: bool) -> list[Path]:
    """Returns the supported nonhidden inputs, independently of either CLI's discovery results."""
    files = set()
    for path in paths:
        candidates = path.rglob("*") if path.is_dir() and recurse else path.iterdir() if path.is_dir() else [path]
        files.update(
            candidate.resolve()
            for candidate in candidates
            if candidate.is_file()
            and candidate.suffix.lower() in AUDIO_EXTENSIONS
            and not any(part.startswith(".") for part in candidate.relative_to(path.parent).parts)
        )
    return sorted(files)


def path_key(path: Path, folder: Path | None) -> str:
    """Returns the same absolute or folder-relative identity for expectations and CLI rows."""
    return os.path.relpath(path.resolve(), folder.resolve()) if folder is not None else os.path.normpath(str(path))


def expected_results(
    files: list[Path], folder: Path | None = None, *, peak: float | None = None, lufs: float | None = None
) -> dict[str, dict]:
    """Builds a pre-operation outcome and length oracle from source paths and committed goldens."""
    normalized = peak is not None or lufs is not None
    claimed = set()
    expected = {}
    for path in sorted(files, key=lambda source: (source.suffix.lower() != ".aif", source)):
        properties = fixture_properties(path)
        failed = expected_failure(properties, peak, lufs)
        output = path.with_suffix(".aif") if normalized else path
        if normalized and properties.get("status") != "Failed":
            if output in claimed:
                failed = True
            claimed.add(output)
        result_path = path if failed else output
        row = {"status": "failed" if failed else "analyzed", "source": path_key(path, folder)}
        if properties and not failed:
            row["durationSeconds"] = round(golden_number(properties, "duration_seconds"), 3)
            row["frameCount"] = int(properties["length_in_samples"])
        expected[path_key(result_path, folder)] = row
    return expected


def missed_peak(row: dict, target: float) -> str | None:
    """Returns why a normalized output missed the sample peak target."""
    peak = row["peakDbfs"]
    if peak is None or not math.isfinite(peak) or abs(peak - target) > PEAK_TARGET_TOLERANCE:
        return f"peak {peak} dBFS instead of {target}"
    return None


def missed_loudness(row: dict, target: float) -> str | None:
    """Returns why an output missed the loudness target without stopping at the true peak ceiling."""
    loudness = row["integratedLufs"]
    if loudness is None:
        return None
    true_peak = row["truePeakDbtp"]
    at_ceiling = true_peak is not None and abs(true_peak - TRUE_PEAK_CEILING) <= TRUE_PEAK_CEILING_TOLERANCE
    if not math.isfinite(loudness) or (
        abs(loudness - target) > LOUDNESS_TARGET_TOLERANCE and not (at_ceiling and loudness < target)
    ):
        return f"loudness {loudness} LUFS and true peak {true_peak} dBTP for a {target} LUFS target"
    return None


def load(path: Path, folder: Path | None) -> dict[str, dict]:
    """Loads result rows without allowing a duplicate path to hide an earlier result."""
    indexed = {}
    for row in json.loads(path.read_text(encoding="utf-8")):
        key = path_key(Path(row["path"]), folder)
        if key in indexed:
            raise ValueError(f"duplicate result path: {key}")
        indexed[key] = row
    return indexed


def compare_rows(name: str, cpp: dict, rust: dict, tolerance: float) -> list[str]:
    """Returns format, status, duration, optional frame count, and level differences."""
    fields = EXACT_FIELDS + [field for field in FRAME_FIELDS if field in cpp or field in rust]
    differences = [
        f"{name}: {field} C++ {cpp.get(field)} Rust {rust.get(field)}"
        for field in fields
        if cpp.get(field) != rust.get(field)
    ]
    for field in [*LEVEL_FIELDS, "durationSeconds"]:
        cpp_value = cpp.get(field)
        rust_value = rust.get(field)
        allowed = DURATION_TOLERANCE if field == "durationSeconds" else tolerance
        if cpp_value is None or rust_value is None:
            if cpp_value is not rust_value or field == "durationSeconds":
                differences.append(f"{name}: {field} C++ {cpp_value} Rust {rust_value}")
        elif not (math.isfinite(cpp_value) and math.isfinite(rust_value)) or abs(cpp_value - rust_value) > allowed:
            differences.append(f"{name}: {field} C++ {cpp_value} Rust {rust_value}")
    return differences


def check_expected(name: str, label: str, row: dict, expected: dict) -> list[str]:
    """Checks one implementation against its expected outcome and independent source length."""
    differences = []
    if row["status"] != expected["status"]:
        differences.append(f"{name}: {label} status {row['status']}, expected {expected['status']}")
    if expected["status"] == "failed" or row["status"] == "failed":
        return differences
    if "durationSeconds" in expected:
        duration = row.get("durationSeconds")
        if (
            duration is None
            or not math.isfinite(duration)
            or abs(duration - expected["durationSeconds"]) > DURATION_TOLERANCE
        ):
            differences.append(f"{name}: {label} duration {duration}, expected {expected['durationSeconds']}")
    differences.extend(
        f"{name}: {label} {field} {row[field]}, expected {expected['frameCount']}"
        for field in FRAME_FIELDS
        if field in row and "frameCount" in expected and row[field] != expected["frameCount"]
    )
    return differences


def compare_results(
    cpp_rows: dict,
    rust_rows: dict,
    expected: dict,
    tolerance: float,
    target: tuple[float | None, float | None] = (None, None),
) -> list[str]:
    """Compares every expected path, including paths omitted by both implementations."""
    peak, lufs = target
    differences = []
    for path in sorted(set(cpp_rows) | set(rust_rows) | set(expected)):
        cpp = cpp_rows.get(path)
        rust = rust_rows.get(path)
        name = str(path)
        oracle = expected.get(path)
        if oracle is None:
            differences.append(f"{name}: unexpected result path")
        for label, row in (("C++", cpp), ("Rust", rust)):
            if row is None:
                differences.append(f"{name}: missing {label} result")
                continue
            if oracle is not None:
                differences += check_expected(name, label, row, oracle)
            if row["status"] != "failed":
                reason = (
                    missed_peak(row, peak)
                    if peak is not None
                    else missed_loudness(row, lufs)
                    if lufs is not None
                    else None
                )
                if reason is not None:
                    differences.append(f"{name}: {label} output missed the target, {reason}")
        if cpp is not None and rust is not None:
            differences += compare_rows(name, cpp, rust, tolerance)
    return differences


def main() -> None:
    """Writes pre-operation expectations or validates the two CLI result files."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cpp", type=Path, nargs="?", help="JSON output of the C++ CLI")
    parser.add_argument("rust", type=Path, nargs="?", help="JSON output of the Rust CLI")
    parser.add_argument("-t", "--tolerance", type=float, default=0.05, help="Allowed level difference in dB or LU")
    parser.add_argument("-f", "--folders", type=Path, nargs=2, metavar="FOLDER", help="Normalized folders")
    parser.add_argument("--expectations", type=Path, help="Pre-operation expectations JSON")
    parser.add_argument("--write-expectations", type=Path, help="Write expectations before invoking the CLIs")
    parser.add_argument("--inputs", type=Path, nargs="+", help="Original input files or directories")
    parser.add_argument("--base", type=Path, help="Folder to make written expectation paths relative to")
    parser.add_argument("-r", "--recurse", action="store_true", help="Discover inputs recursively")
    target = parser.add_mutually_exclusive_group()
    target.add_argument("-p", "--peak", type=float, help="Sample peak target the outputs must reach")
    target.add_argument("-l", "--lufs", type=float, help="Integrated loudness target the outputs must reach")
    arguments = parser.parse_args()
    if arguments.write_expectations is not None:
        if not arguments.inputs:
            parser.error("--write-expectations requires --inputs")
        expected = expected_results(
            input_files(arguments.inputs, arguments.recurse), arguments.base, peak=arguments.peak, lufs=arguments.lufs
        )
        if not expected:
            parser.error("no audio inputs found for the expectation oracle")
        arguments.write_expectations.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")
        return
    if arguments.cpp is None or arguments.rust is None:
        parser.error("the C++ and Rust JSON files are required")
    cpp_folder, rust_folder = arguments.folders or (None, None)
    cpp_rows = load(arguments.cpp, cpp_folder)
    rust_rows = load(arguments.rust, rust_folder)
    expected = (
        json.loads(arguments.expectations.read_text(encoding="utf-8"))
        if arguments.expectations is not None
        else {
            path: {"status": "failed" if fixture_properties(Path(path)).get("status") == "Failed" else "analyzed"}
            for path in set(cpp_rows) | set(rust_rows)
        }
    )
    differences = compare_results(cpp_rows, rust_rows, expected, arguments.tolerance, (arguments.peak, arguments.lufs))
    if not expected:
        differences.append("no expected audio results")
    print(f"Compared {len(expected)} expected files with a tolerance of {arguments.tolerance}")
    if differences:
        print(f"{len(differences)} difference(s):")
        for difference in differences:
            print(f"  {difference}")
        sys.exit(1)
    print("No differences")


if __name__ == "__main__":
    main()
