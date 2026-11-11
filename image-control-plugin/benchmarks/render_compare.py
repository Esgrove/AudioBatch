# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Compare two saved ImageControl render benchmark captures without running native programs.

Usage: uv run --script benchmarks/render_compare.py --compare FIRST --compare SECOND --artifacts ABSOLUTE_DIRECTORY
Each input can be a capture directory, raw.json, summary.json with sibling raw.json, or one native JSON report.
Version-1 summaries without p99 remain supported. All statistics are recomputed from retained raw timings.
A summary alone cannot establish workload identity and is rejected instead of guessing from display case names.
Both inputs must contain exactly matching case/workload identities. Shared implementations compare like with like.
Two single-implementation inputs may compare different implementations. Repeated captures are independent,
not paired experiments. Ratios are second divided by first and are descriptive, not regression verdicts.
Hardware, OS, backend, compiler, formats, and timing differences are retained explicitly.
Cross-machine results do not isolate an OS effect. Physical presentation and device deadlines are not measured.
Batch-size mismatches retain descriptive CPU/GPU samples but suppress batch-wall ratios because synchronization
overhead is nonmatched. Legacy captures retain their recorded native batch size, never an inferred default.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import struct
from dataclasses import dataclass
from pathlib import Path

import render_benchmark as benchmark
import render_workload as workload

MAX_CAPTURE_BYTES = 256 * 1024 * 1024
MIN_COMPARISON_INPUTS = 2
RAW_FIELDS = {
    "sequence",
    "case",
    "requested",
    "run",
    "implementation",
    "process_wall_ns",
    "workload_manifest",
    "native_report",
    "report",
}
CASE_FIELDS = {"width", "height", "scale", "mode", "particles"}
HOST_FIELDS = ("os", "machine", "cpu")
CONFIGURATION_FIELDS = (
    "batch_frames",
    "backend",
    "gpu",
    "vendor",
    "driver",
    "compiler",
    "build_type",
    "accumulation_format",
    "output_format",
    "targets",
    "capabilities",
    "timing_method",
)


@dataclass(frozen=True)
class SavedCapture:
    """Validated portable raw evidence, source identities, and observed host provenance."""

    source: Path
    records: list[dict[str, object]]
    provenance: dict[str, object]
    files: list[dict[str, object]]


def read_json(path: Path) -> object:
    """Read bounded portable evidence with duplicate-key, finite-number, and strict JSON validation."""
    workload.require(path.is_file(), f"saved evidence file is missing: {path}")
    workload.require(path.stat().st_size <= MAX_CAPTURE_BYTES, "saved capture exceeds storage limit")
    value = json.loads(
        path.read_text(encoding="utf-8"),
        parse_constant=benchmark.reject_json_constant,
        object_pairs_hook=benchmark.unique_object,
    )
    benchmark.validate_json_numbers(value)
    return value


def case_from_record(value: object) -> benchmark.RenderCase:
    """Validate physical case geometry without trusting a display label as a comparison identity."""
    value = benchmark.object_keys(value, CASE_FIELDS, "saved requested case")
    width = benchmark.count(value["width"], "width", 1, benchmark.MAX_TARGET_SIZE)
    height = benchmark.count(value["height"], "height", 1, benchmark.MAX_TARGET_SIZE)
    scale = benchmark.finite_number(value["scale"], "scale", positive=True)
    workload.require(benchmark.MIN_SCALE <= scale <= benchmark.MAX_SCALE, "invalid saved scale")
    workload.require(value["mode"] in ("particles", "solid"), "invalid saved mode")
    particles = benchmark.count(value["particles"], "particles", 0, workload.MAX_PARTICLES)
    return benchmark.RenderCase(width, height, scale, value["mode"], particles)


def validate_manifest(value: object) -> dict[str, object]:
    """Validate the original version-1 workload self-hash without requiring its original absolute file path."""
    fields = {
        "schema_version",
        "generator",
        "seed",
        "header",
        "layout",
        "bytes",
        "sha256",
        "fnv1a64",
        "manifest_sha256",
    }
    manifest = benchmark.object_keys(value, fields, "workload manifest")
    workload.require(
        type(manifest["schema_version"]) is int and manifest["schema_version"] == 1, "unsupported manifest schema"
    )
    workload.require(manifest["generator"] == "imagecontrol-render-lcg32-v1", "unsupported workload generator")
    benchmark.count(manifest["seed"], "manifest seed", 0, workload.UINT32_MAX)
    header = benchmark.object_keys(
        manifest["header"],
        {"frame_count", "particles_per_frame", "step_seconds", "persistence_seconds"},
        "manifest header",
    )
    workload.validate_parameters(workload.WorkloadHeader(**header))
    expected_layout = {"magic": "ICRB0001", "header_bytes": 24, "particle_bytes": 32}
    workload.require(manifest["layout"] == expected_layout, "unsupported workload layout")
    expected_bytes = 24 + header["frame_count"] * header["particles_per_frame"] * 32
    workload.require(
        type(manifest["bytes"]) is int and manifest["bytes"] == expected_bytes, "manifest byte count differs"
    )
    for field, length in (("sha256", 64), ("fnv1a64", 16), ("manifest_sha256", 64)):
        value = manifest[field]
        workload.require(
            isinstance(value, str) and re.fullmatch(f"[0-9a-f]{{{length}}}", value) is not None, f"invalid {field}"
        )
    unsigned = {key: value for key, value in manifest.items() if key != "manifest_sha256"}
    workload.require(workload.canonical_hash(unsigned) == manifest["manifest_sha256"], "manifest self-hash differs")
    return manifest


def saved_native_report(value: object) -> dict[str, object]:
    """Normalize only the recorded legacy Rust batch field, never assume a default batch for saved evidence."""
    workload.require(isinstance(value, dict), "native report must be an object")
    report = dict(value)
    details = report.get("details")
    timing = details.get("timing") if isinstance(details, dict) else None
    if "batch_frames" not in report:
        workload.require(
            isinstance(timing, dict) and "completion_batch_frames" in timing, "saved batch_frames is missing"
        )
        report["batch_frames"] = timing["completion_batch_frames"]
    if isinstance(timing, dict) and "completion_batch_frames" in timing:
        workload.require(
            report["batch_frames"] == timing["completion_batch_frames"], "saved batch_frames contradicts native details"
        )
    return report


def native_record(report: object) -> dict[str, object]:
    """Wrap one native report while making absent SHA-256 and host provenance explicit, never inventing them."""
    report = saved_native_report(report)
    workload.require(report.keys() >= benchmark.NATIVE_FIELDS, "incomplete native report")
    case = case_from_record(
        {
            "width": report["width"],
            "height": report["height"],
            "scale": report["scale"],
            "mode": report["mode"],
            "particles": report["particles_per_frame"],
        }
    )
    return {
        "sequence": 0,
        "case": case.name,
        "requested": {
            "width": case.width,
            "height": case.height,
            "scale": case.scale,
            "mode": case.mode,
            "particles": case.particles,
        },
        "run": 0,
        "implementation": report["implementation"],
        "process_wall_ns": report["batch_wall_ns"],
        "workload_manifest": None,
        "native_report": "standalone native report",
        "report": report,
    }


def validate_record(value: object) -> dict[str, object]:
    """Require exact raw accounting and validate each embedded native schema against retained workload metadata."""
    record = dict(benchmark.object_keys(value, RAW_FIELDS, "saved raw record"))
    report = record["report"]
    report = saved_native_report(report)
    workload.require(report.keys() >= benchmark.NATIVE_FIELDS, "incomplete native report")
    case = case_from_record(record["requested"])
    workload.require(record["case"] == case.name, "saved case label differs from physical case")
    implementation = record["implementation"]
    workload.require(implementation in ("cpp", "rust"), "invalid saved implementation")
    benchmark.count(record["run"], "run", 0, benchmark.MAX_RUNS - 1)
    benchmark.count(record["sequence"], "sequence", 0, benchmark.MAX_CASES * benchmark.MAX_RUNS * 2)
    benchmark.finite_number(record["process_wall_ns"], "process_wall_ns", positive=True)
    benchmark.text(record["native_report"], "native_report")
    frames = benchmark.count(report["frames"], "frames", 1, benchmark.MAX_MEASURED_FRAMES)
    warmup = benchmark.count(report["warmup"], "warmup", 0, benchmark.MAX_WARMUP_FRAMES)
    if record["workload_manifest"] is not None:
        manifest = validate_manifest(record["workload_manifest"])
        input_frames = manifest["header"]["frame_count"]
        workload.require(manifest["header"]["particles_per_frame"] == case.particles, "manifest particle count differs")
    else:
        identity = report["input_fnv1a64"]
        workload.require(
            isinstance(identity, str) and re.fullmatch("[0-9a-f]{16}", identity) is not None,
            "invalid native input hash",
        )
        input_frames = report.get("input_frames", 1)
        benchmark.count(input_frames, "input_frames", 1, workload.MAX_INPUT_FRAMES)
        header = workload.WorkloadHeader(
            input_frames, case.particles, report["step_seconds"], report["persistence_seconds"]
        )
        workload.validate_parameters(header)
        manifest = {
            "fnv1a64": identity,
            "header": {"step_seconds": header.step_seconds, "persistence_seconds": header.persistence_seconds},
        }
    record["report"] = report
    options = argparse.Namespace(frames=frames, warmup=warmup, input_frames=input_frames)
    benchmark.validate_report(report, implementation, case, manifest, options)
    return record


def validate_records(records: object) -> list[dict[str, object]]:
    """Reject duplicate runs, inconsistent implementation matrices, and mixed workloads within one case."""
    workload.require(isinstance(records, list) and bool(records), "saved raw records must be a nonempty array")
    validated = [validate_record(record) for record in records]
    workload.require(len({record["sequence"] for record in validated}) == len(validated), "duplicate saved sequence")
    implementations = tuple(sorted({record["implementation"] for record in validated}))
    benchmark.summarize_runs(validated, implementations)
    for implementation in implementations:
        runs = {record["run"] for record in validated if record["implementation"] == implementation}
        for name in {record["case"] for record in validated}:
            actual_runs = {
                record["run"]
                for record in validated
                if record["implementation"] == implementation and record["case"] == name
            }
            workload.require(actual_runs == runs, "saved case run sets differ")
    for implementation in implementations:
        selected = [record for record in validated if record["implementation"] == implementation]
        names = {record["case"] for record in selected}
        workload.require(len(names) <= benchmark.MAX_CASES, "saved capture exceeds case limit")
        for name in names:
            group = [record for record in selected if record["case"] == name]
            expected_identity = record_identity(group[0])
            for record in group[1:]:
                workload.require(
                    record_identity(record) == expected_identity, "workload identity varies within saved case"
                )
                workload.require(
                    all(record["report"][field] == group[0]["report"][field] for field in CONFIGURATION_FIELDS),
                    "native configuration varies within saved case",
                )
    return validated


def check_summary(actual: object, expected: object) -> None:
    """Accept legacy version-1 summaries lacking p99, but reject any contradictory retained statistic."""
    if isinstance(expected, dict):
        workload.require(isinstance(actual, dict), "saved summary object differs")
        required = set(expected) - {"p99_ns", "batch_frames"}
        workload.require(required <= actual.keys() <= expected.keys(), "saved summary keys differ")
        for key, value in actual.items():
            check_summary(value, expected[key])
    elif isinstance(expected, list):
        workload.require(
            isinstance(actual, list) and len(actual) == len(expected), "saved summary case/run count differs"
        )
        for left, right in zip(actual, expected, strict=True):
            check_summary(left, right)
    else:
        workload.require(
            type(actual) in (int, float) if type(expected) in (int, float) else type(actual) is type(expected),
            "saved summary value type differs",
        )
        workload.require(actual == expected, "saved summary statistic differs from raw evidence")


def load_capture(path: Path) -> SavedCapture:
    """Load portable capture evidence, resolving siblings locally rather than following original executable paths."""
    path = path.expanduser().resolve()
    if path.is_dir():
        workload.require(
            not (path / "failure.json").exists(), "failed capture cannot be compared as successful evidence"
        )
        path = path / "summary.json" if (path / "summary.json").is_file() else path / "raw.json"
    workload.require(
        not path.with_name("failure.json").exists(), "failed capture cannot be compared as successful evidence"
    )
    value = read_json(path)
    files = [benchmark.file_identity(path)]
    provenance = {}
    if isinstance(value, dict) and "cases" in value:
        summary = benchmark.object_keys(value, {"schema_version", "provenance", "cases"}, "saved summary")
        workload.require(
            type(summary["schema_version"]) is int and summary["schema_version"] == 1, "unsupported summary schema"
        )
        workload.require(isinstance(summary["provenance"], dict), "saved provenance must be an object")
        provenance = summary["provenance"]
        raw_path = path.with_name("raw.json")
        records = validate_records(read_json(raw_path))
        files.append(benchmark.file_identity(raw_path))
        implementations = tuple(sorted({record["implementation"] for record in records}))
        check_summary(summary["cases"], benchmark.summarize_runs(records, implementations))
    else:
        records = validate_records(value if isinstance(value, list) else [native_record(value)])
    provenance_path = path.with_name("provenance.json")
    if provenance_path.is_file():
        observed = read_json(provenance_path)
        workload.require(isinstance(observed, dict), "saved provenance must be an object")
        workload.require(
            not provenance or provenance == observed, "summary provenance differs from retained provenance"
        )
        provenance = observed
        files.append(benchmark.file_identity(provenance_path))
    return SavedCapture(path, records, provenance, files)


def record_identity(record: dict[str, object]) -> tuple[object, ...]:
    """Pair by native binary semantics, never by implementation, case labels, run order, or filesystem location."""
    report = record["report"]
    values = []
    for field in benchmark.IDENTITY_FIELDS:
        value = report[field]
        if field in {"scale", "step_seconds", "persistence_seconds"}:
            value = struct.pack("<f", value)
        values.append(value)
    manifest = record["workload_manifest"]
    values.append(manifest["sha256"] if manifest is not None else None)
    return tuple(values)


def compare_group(first: list[dict[str, object]], second: list[dict[str, object]]) -> dict[str, object]:
    """Compare independent process means with explicit method gates and no invented paired-run inference."""
    left, right = first[0]["report"], second[0]["report"]
    left_identity, right_identity = record_identity(first[0]), record_identity(second[0])
    workload.require(left_identity[:-1] == right_identity[:-1], "saved comparison case identity differs")
    if left_identity[-1] is not None and right_identity[-1] is not None:
        workload.require(left_identity[-1] == right_identity[-1], "saved workload SHA-256 differs")
    cpu_first = [statistics.mean(record["report"]["cpu_frame_ns"]) for record in first]
    cpu_second = [statistics.mean(record["report"]["cpu_frame_ns"]) for record in second]
    gpu_method = left["timing_method"]["gpu"]
    same_gpu = gpu_method == right["timing_method"]["gpu"] and gpu_method != "unsupported"
    gpu_first = (
        [
            statistics.mean(record["report"]["gpu_frame_ns"])
            if gpu_method == "per-frame-timestamps"
            else record["report"]["gpu_batch_ns"]
            for record in first
        ]
        if same_gpu
        else None
    )
    gpu_second = (
        [
            statistics.mean(record["report"]["gpu_frame_ns"])
            if gpu_method == "per-frame-timestamps"
            else record["report"]["gpu_batch_ns"]
            for record in second
        ]
        if same_gpu
        else None
    )
    return {
        "case": first[0]["case"],
        "first_implementation": first[0]["implementation"],
        "second_implementation": second[0]["implementation"],
        "identity": {field: left[field] for field in benchmark.IDENTITY_FIELDS},
        "workload_sha256": {"first": left_identity[-1], "second": right_identity[-1]},
        "hash_scope": "sha256-and-fnv1a64"
        if left_identity[-1] is not None and right_identity[-1] is not None
        else "fnv1a64-only",
        "process_runs": {"first": len(first), "second": len(second)},
        "per_run": {
            side: [
                {
                    "cpu_frame_ns": benchmark.sample_summary(record["report"]["cpu_frame_ns"]),
                    "gpu_frame_ns": benchmark.sample_summary(record["report"]["gpu_frame_ns"])
                    if record["report"]["gpu_frame_ns"] is not None
                    else None,
                    "gpu_batch_ns": record["report"]["gpu_batch_ns"],
                    "batch_wall_ns": record["report"]["batch_wall_ns"],
                }
                for record in records
            ]
            for side, records in (("first", first), ("second", second))
        },
        "cpu_run_mean_ns": {
            "first": benchmark.sample_summary(cpu_first),
            "second": benchmark.sample_summary(cpu_second),
        },
        "gpu_run_mean_ns": {
            "first": benchmark.sample_summary(gpu_first),
            "second": benchmark.sample_summary(gpu_second),
        }
        if same_gpu
        else None,
        "second_over_first": {
            "cpu": statistics.mean(cpu_second) / statistics.mean(cpu_first)
            if left["timing_method"]["cpu"] == right["timing_method"]["cpu"] and statistics.mean(cpu_first)
            else None,
            "gpu": statistics.mean(gpu_second) / statistics.mean(gpu_first)
            if same_gpu and statistics.mean(gpu_first)
            else None,
            "batch_wall": statistics.mean([record["report"]["batch_wall_ns"] for record in second])
            / statistics.mean([record["report"]["batch_wall_ns"] for record in first])
            if left["batch_frames"] == right["batch_frames"]
            else None,
        },
        "configuration_differences": {
            field: {"first": left[field], "second": right[field]}
            for field in CONFIGURATION_FIELDS
            if left[field] != right[field]
        },
    }


def compare_captures(first: SavedCapture, second: SavedCapture) -> dict[str, object]:
    """Require complete matching case sets and retain hardware/platform confounders rather than isolating OS effects."""
    first_implementations = sorted({record["implementation"] for record in first.records})
    second_implementations = sorted({record["implementation"] for record in second.records})
    if first_implementations == second_implementations:
        pairs = list(zip(first_implementations, second_implementations, strict=True))
    else:
        workload.require(
            len(first_implementations) == len(second_implementations) == 1, "saved implementation sets differ"
        )
        pairs = [(first_implementations[0], second_implementations[0])]
    comparisons = []
    for first_implementation, second_implementation in pairs:
        first_records = [record for record in first.records if record["implementation"] == first_implementation]
        second_records = [record for record in second.records if record["implementation"] == second_implementation]
        first_cases = {record["case"] for record in first_records}
        workload.require(
            first_cases == {record["case"] for record in second_records}, "saved comparison case sets differ"
        )
        comparisons.extend(
            compare_group(
                [record for record in first_records if record["case"] == name],
                [record for record in second_records if record["case"] == name],
            )
            for name in sorted(first_cases)
        )
    missing = [
        f"{side}.{field}"
        for side, capture in (("first", first), ("second", second))
        for field in HOST_FIELDS
        if not capture.provenance.get(field) or capture.provenance[field] == "unavailable"
    ]
    differences = {
        field: {"first": first.provenance.get(field), "second": second.provenance.get(field)}
        for field in HOST_FIELDS
        if first.provenance.get(field) != second.provenance.get(field)
    }
    return {
        "schema_version": 1,
        "comparison_kind": "independent-saved-captures",
        "ratio_direction": "second-over-first",
        "sources": {
            "first": {"path": str(first.source), "files": first.files, "provenance": first.provenance},
            "second": {"path": str(second.source), "files": second.files, "provenance": second.provenance},
        },
        "host_differences": differences,
        "missing_host_provenance": missing,
        "interpretation": (
            "Descriptive hardware/platform comparison. Differences do not isolate an OS effect, "
            "establish equivalence, or prove a regression."
        ),
        "cases": comparisons,
    }


def parser() -> argparse.ArgumentParser:
    """Expose side-effect-free help and repeated explicit saved-evidence inputs with standard artifact aliases."""
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument(
        "-c", "--compare", type=Path, action="append", required=True, help="repeat exactly twice, first then second"
    )
    result.add_argument("-A", "--artifacts", type=Path, required=True, help="new or empty absolute output directory")
    return result


def main(arguments: list[str] | None = None) -> int:
    """Write a validated comparison without builds, native process execution, or modification of source captures."""
    command = parser()
    options = command.parse_args(arguments)
    try:
        workload.require(len(options.compare) == MIN_COMPARISON_INPUTS, "supply exactly two --compare paths")
        workload.require(options.artifacts.is_absolute(), "artifacts must be an absolute path")
        workload.require(
            not options.artifacts.exists() or (options.artifacts.is_dir() and not any(options.artifacts.iterdir())),
            "artifacts must be new or empty",
        )
        first, second = (load_capture(path) for path in options.compare)
        comparison = compare_captures(first, second)
    except (ValueError, OSError) as error:
        command.error(str(error))
    options.artifacts.mkdir(parents=True, exist_ok=True)
    workload.write_json(options.artifacts / "comparison.json", comparison)
    print(
        f"Compared {len(comparison['cases'])} saved case/implementation groups: {options.artifacts / 'comparison.json'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
