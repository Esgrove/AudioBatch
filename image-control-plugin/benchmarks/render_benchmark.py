# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Capture paired production-renderer offscreen benchmarks, without builds or installations.

Supply one or both explicit Release native executables with --cpp and/or --rust and a new --artifacts directory.
Use render_compare.py --compare PATH --compare PATH --artifacts DIRECTORY for saved-report comparisons.
The default bounded matrix is 1280x720 physical pixels, scale 1, particles/solid, and 0/4096/16384/65536 particles.
Repeat --size, --scale, --mode, or --particles to choose a custom matrix. Targets are never larger than 4096 per axis.
Runs are sequential. With both implementations, order alternates cpp/rust then rust/cpp.
Single-implementation captures retain repeated process runs without cross-implementation ratios.
Every process warms up before its measured frame samples.
Raw native reports/stdout/stderr, workloads, manifests, executable identities, host/source provenance,
and per-run/per-case summaries are retained. No claim is made about physical presentation, full UI, or audio work.

Native CLI: --input PATH --width W --height H --scale F --mode particles|solid --frames N --warmup N --report PATH.
Input bytes and the native schema are versioned separately from the existing CPU benchmark reports.
Native reports require actual batch_frames from 1 through 32. Batch-size differences are nonmatched synchronization
overhead, not a controlled throughput comparison. Shared simulated frame steps are at most 0.1 seconds.
GPU timing may be per-frame timestamps, synchronized batch completion, or explicitly unsupported.
Comparisons only pair identical case identities and timing methods. Ratios are descriptive, not regression verdicts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import struct
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from itertools import product
from pathlib import Path

import render_workload as workload

REPOSITORY = Path(__file__).resolve().parents[2]
SCHEMA_VERSION = 1
RENDER_SCOPE = "offscreen-production-renderer"
MAX_TARGET_SIZE = 4096
MAX_SCALE = 8.0
MIN_SCALE = 0.125
MAX_MEASURED_FRAMES = 10_000
MAX_WARMUP_FRAMES = 10_000
MAX_RUNS = 100
MAX_CASES = 256
PAIRED_IMPLEMENTATION_COUNT = 2
MAX_BATCH_FRAMES = 32
MAX_REPORT_BYTES = 16 * 1024 * 1024
DEFAULT_TIMEOUT_SECONDS = 120.0
MAX_TIMEOUT_SECONDS = 3600.0
GPU_METHODS = ("per-frame-timestamps", "synchronized-batch", "unsupported")
NATIVE_FIELDS = {
    "schema_version",
    "implementation",
    "render_scope",
    "input_fnv1a64",
    "width",
    "height",
    "scale",
    "mode",
    "frames",
    "warmup",
    "particles_per_frame",
    "step_seconds",
    "persistence_seconds",
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
    "cpu_frame_ns",
    "gpu_frame_ns",
    "gpu_batch_ns",
    "batch_wall_ns",
    "batch_frames",
    "timing_method",
}
# Explicit diagnostic extensions preserve native detail without allowing silent misspellings of required fields.
OPTIONAL_NATIVE_FIELDS = {
    "details",
    "benchmark",
    "build_profile",
    "native_cpu_enabled",
    "optimized_build",
    "opt_level",
    "input",
    "input_format",
    "input_frames",
    "frame_step_seconds",
    "warmup_frames",
    "requested_frames",
    "encoded_frames",
    "completed_frames",
    "physical_width",
    "physical_height",
    "logical_width",
    "logical_height",
    "bloom_format",
    "solid_field_format",
    "bloom_levels",
    "completed_wall_ms",
    "completed_frames_per_second",
    "gpu_timing_enabled",
    "gpu_timing_status",
    "cpu_timing_boundary",
    "gpu_timing_boundary",
    "throughput_timing_boundary",
    "measurement_history",
    "cpu_frame_ms",
    "gpu_frame_ms",
    "gpu_frame_indices",
}
IDENTITY_FIELDS = (
    "render_scope",
    "input_fnv1a64",
    "width",
    "height",
    "scale",
    "mode",
    "frames",
    "warmup",
    "particles_per_frame",
    "step_seconds",
    "persistence_seconds",
)


@dataclass(frozen=True)
class RenderCase:
    """Requested physical output geometry, scale, renderer mode, and input particle count."""

    width: int
    height: int
    scale: float
    mode: str
    particles: int

    @property
    def name(self) -> str:
        """Produce a filesystem-safe stable label without rounding away a custom scale."""
        return f"{self.width}x{self.height}-s{self.scale:g}-{self.mode}-p{self.particles}"


def finite_number(value: object, field: str, *, positive: bool = False) -> float:
    """Reject booleans, overflow, nonfinite values, and negative measurements."""
    try:
        valid = type(value) in (int, float) and math.isfinite(value) and (value > 0 if positive else value >= 0)
    except OverflowError:
        valid = False
    workload.require(valid, f"{field} must be finite and {'positive' if positive else 'nonnegative'}")
    return float(value)


def count(value: object, field: str, minimum: int, maximum: int) -> int:
    """Check integer counters exactly instead of accepting booleans or truncated floats."""
    workload.require(type(value) is int and minimum <= value <= maximum, f"invalid {field}")
    return value


def object_keys(value: object, fields: set[str], name: str) -> dict[str, object]:
    """Reject missing or unknown versioned schema keys, retaining a clear failure boundary."""
    workload.require(isinstance(value, dict), f"{name} must be an object")
    workload.require(
        set(value) == fields, f"{name} keys differ: missing={fields - value.keys()}, extra={value.keys() - fields}"
    )
    return value


def text(value: object, field: str) -> str:
    """Require explicit provenance strings instead of silently substituting a hardware identity."""
    workload.require(isinstance(value, str) and bool(value.strip()), f"{field} must be a nonempty string")
    return value


def duration_samples(value: object, frames: int, field: str) -> list[float]:
    """Require one finite observation per measured frame, excluding warmup."""
    workload.require(isinstance(value, list) and len(value) == frames, f"{field} count differs from frames")
    return [finite_number(sample, field) for sample in value]


def validate_targets(report: dict[str, object], case: RenderCase) -> None:
    """Record actual physical intermediate allocations and require a matching output target."""
    targets = report["targets"]
    workload.require(isinstance(targets, list) and bool(targets), "targets must be a nonempty array")
    names = set()
    for value in targets:
        target = object_keys(value, {"name", "width", "height", "format"}, "target")
        name = text(target["name"], "target.name")
        workload.require(name not in names, "duplicate target name")
        names.add(name)
        width = count(target["width"], "target.width", 1, MAX_TARGET_SIZE)
        height = count(target["height"], "target.height", 1, MAX_TARGET_SIZE)
        text(target["format"], "target.format")
        if name == "output":
            workload.require((width, height) == (case.width, case.height), "actual output geometry differs")
            workload.require(target["format"] == report["output_format"], "output format differs from actual target")
    workload.require("output" in names, "actual output target is missing")


def validate_timings(report: dict[str, object], frames: int) -> None:
    """Disallow fabricated GPU samples and separate timestamp timing from synchronized batch timing."""
    timing = object_keys(report["timing_method"], {"cpu", "gpu"}, "timing_method")
    text(timing["cpu"], "timing_method.cpu")
    workload.require(timing["gpu"] in GPU_METHODS, "unsupported GPU timing method")
    capabilities = object_keys(report["capabilities"], {"gpu_timestamps"}, "capabilities")
    workload.require(type(capabilities["gpu_timestamps"]) is bool, "gpu_timestamps must be boolean")
    duration_samples(report["cpu_frame_ns"], frames, "cpu_frame_ns")
    finite_number(report["batch_wall_ns"], "batch_wall_ns", positive=True)
    count(report["batch_frames"], "batch_frames", 1, MAX_BATCH_FRAMES)
    if timing["gpu"] == "per-frame-timestamps":
        workload.require(capabilities["gpu_timestamps"], "timestamp samples require GPU timestamp capability")
        duration_samples(report["gpu_frame_ns"], frames, "gpu_frame_ns")
        workload.require(report["gpu_batch_ns"] is None, "timestamp runs must not masquerade as batch timing")
    elif timing["gpu"] == "synchronized-batch":
        workload.require(report["gpu_frame_ns"] is None, "batch GPU timing must not fabricate per-frame samples")
        finite_number(report["gpu_batch_ns"], "gpu_batch_ns", positive=True)
    else:
        workload.require(
            report["gpu_frame_ns"] is None and report["gpu_batch_ns"] is None, "unsupported GPU timing must be null"
        )


def validate_json_numbers(value: object) -> None:
    """Reject numeric overflow anywhere in diagnostic or caller evidence, including valid JSON exponent syntax."""
    if isinstance(value, dict):
        for item in value.values():
            validate_json_numbers(item)
    elif isinstance(value, list):
        for item in value:
            validate_json_numbers(item)
    elif type(value) in (int, float):
        try:
            finite = math.isfinite(value)
        except OverflowError:
            finite = False
        workload.require(finite, "non-finite JSON number")


def validate_diagnostics(report: dict[str, object], case: RenderCase, options: argparse.Namespace) -> None:
    """Check optional native count/geometry aliases instead of retaining contradictory diagnostic claims."""
    expected = {
        "requested_frames": options.frames,
        "encoded_frames": options.frames,
        "completed_frames": options.frames,
        "warmup_frames": options.warmup,
        "physical_width": case.width,
        "physical_height": case.height,
        "input_frames": options.input_frames,
    }
    for field, expected_value in expected.items():
        if field in report:
            workload.require(type(report[field]) is int and report[field] == expected_value, f"native {field} differs")
    for field, expected_value in {
        "logical_width": case.width / case.scale,
        "logical_height": case.height / case.scale,
    }.items():
        if field in report:
            value = finite_number(report[field], field, positive=True)
            workload.require(math.isclose(value, expected_value, rel_tol=1e-7), f"native {field} differs")
    if "gpu_frame_indices" in report:
        indices = report["gpu_frame_indices"]
        workload.require(isinstance(indices, list), "gpu_frame_indices must be an array")
        workload.require(
            all(type(index) is int and 0 <= index < options.frames for index in indices), "invalid GPU frame index"
        )
        workload.require(len(indices) == len(set(indices)), "duplicate GPU frame index")
        if report["gpu_frame_ns"] is not None:
            workload.require(indices == list(range(options.frames)), "GPU timing indices do not cover measured frames")


def validate_report(
    value: object, implementation: str, case: RenderCase, manifest: dict[str, object], options: argparse.Namespace
) -> dict[str, object]:
    """Validate native version, case identity, actual geometry, capabilities, provenance, and raw accounting."""
    frames, warmup = options.frames, options.warmup
    workload.require(isinstance(value, dict), "native report must be an object")
    fields = set(value)
    workload.require(fields >= NATIVE_FIELDS, f"native report missing keys: {NATIVE_FIELDS - fields}")
    workload.require(not fields - NATIVE_FIELDS - OPTIONAL_NATIVE_FIELDS, "unknown native report keys")
    report = value
    validate_json_numbers(report)
    validate_diagnostics(report, case, options)
    workload.require(
        type(report["schema_version"]) is int and report["schema_version"] == SCHEMA_VERSION,
        "unsupported native schema",
    )
    workload.require(report["implementation"] == implementation, "native implementation differs")
    workload.require(report["render_scope"] == RENDER_SCOPE, "native render scope differs")
    workload.require(report["input_fnv1a64"] == manifest["fnv1a64"], "native input hash differs")
    expected = {
        "width": case.width,
        "height": case.height,
        "mode": case.mode,
        "frames": frames,
        "warmup": warmup,
        "particles_per_frame": case.particles,
    }
    for field, expected_value in expected.items():
        workload.require(
            type(report[field]) is type(expected_value) and report[field] == expected_value, f"native {field} differs"
        )
    for field, expected_value in {
        "scale": case.scale,
        "step_seconds": manifest["header"]["step_seconds"],
        "persistence_seconds": manifest["header"]["persistence_seconds"],
    }.items():
        actual = finite_number(report[field], field, positive=True)
        workload.require(math.isclose(actual, expected_value, rel_tol=1e-7, abs_tol=0), f"native {field} differs")
    workload.require(report["step_seconds"] <= workload.MAX_STORED_STEP, "native step exceeds shared 0.1-second limit")
    for field in ("backend", "gpu", "vendor", "driver", "compiler", "accumulation_format", "output_format"):
        text(report[field], field)
    workload.require(report["build_type"] == "Release", "native benchmark requires a Release build")
    if "native_cpu_enabled" in report:
        workload.require(report["native_cpu_enabled"] is True, "native benchmark requires native CPU optimizations")
    if "opt_level" in report:
        workload.require(report["opt_level"] == "3", "native benchmark requires optimization level 3")
    if "optimized_build" in report:
        workload.require(report["optimized_build"] is True, "native benchmark requires optimized Release configuration")
    validate_targets(report, case)
    validate_timings(report, frames)
    return report


def reject_json_constant(value: str) -> None:
    """Reject NaN and Infinity even in nested native report evidence."""
    message = f"nonstandard JSON number: {value}"
    raise ValueError(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    """Reject duplicate JSON keys rather than silently validating only the last value."""
    result = {}
    for key, value in pairs:
        workload.require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_report(path: Path) -> object:
    """Read a bounded strict native JSON report without accepting nonstandard numeric literals."""
    workload.require(path.stat().st_size <= MAX_REPORT_BYTES, "native report exceeds storage limit")
    return json.loads(
        path.read_text(encoding="utf-8"), parse_constant=reject_json_constant, object_pairs_hook=unique_object
    )


def file_identity(path: Path) -> dict[str, object]:
    """Retain exact executable or workload bytes independently from caller-supplied build assertions."""
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    return {"path": str(path), "sha256": digest, "bytes": path.stat().st_size}


def run_native(command: list[str], options: list[str], directory: Path, timeout: float) -> float:
    """Run exactly one child without a shell and retain complete raw streams, including failures and timeouts."""
    directory.mkdir()
    workload.write_json(directory / "command.json", command + options)
    start = time.perf_counter_ns()
    with (directory / "stdout.txt").open("wb") as output, (directory / "stderr.txt").open("wb") as errors:
        result = subprocess.run(  # noqa: S603
            command + options,
            stdout=output,
            stderr=errors,
            check=False,
            timeout=timeout,
        )
    elapsed = time.perf_counter_ns() - start
    workload.require(result.returncode == 0, f"native runner exited {result.returncode}: {directory}")
    return float(elapsed)


def sample_summary(samples: list[float]) -> dict[str, float]:
    """Retain count, nearest-rank p95/p99, and maximum, without treating frames as independent runs."""
    ordered = sorted(samples)
    return {
        "count": len(samples),
        "mean_ns": statistics.mean(samples),
        "median_ns": statistics.median(samples),
        "p95_ns": ordered[math.ceil(len(ordered) * 0.95) - 1],
        "p99_ns": ordered[math.ceil(len(ordered) * 0.99) - 1],
        "min_ns": ordered[0],
        "max_ns": ordered[-1],
    }


def compare_pair(cpp: dict[str, object], rust: dict[str, object]) -> dict[str, object]:
    """Refuse mismatched workloads and only ratio timings with matching measurement boundaries."""
    float32_fields = {"scale", "step_seconds", "persistence_seconds"}
    for field in IDENTITY_FIELDS:
        left, right = cpp[field], rust[field]
        if field in float32_fields:
            # JSON serializers may choose different decimal spellings of the same native float32 value.
            left = struct.pack("<f", finite_number(left, field, positive=True))
            right = struct.pack("<f", finite_number(right, field, positive=True))
        workload.require(left == right, "comparison case identity differs")
    same_cpu_boundary = cpp["timing_method"]["cpu"] == rust["timing_method"]["cpu"]
    cpu_cpp, cpu_rust = statistics.mean(cpp["cpu_frame_ns"]), statistics.mean(rust["cpu_frame_ns"])
    result = {
        "cpu_rust_over_cpp": cpu_rust / cpu_cpp if cpu_cpp and same_cpu_boundary else None,
        "gpu_rust_over_cpp": None,
        "configuration_differences": {
            field: {"cpp": cpp[field], "rust": rust[field]}
            for field in (
                "batch_frames",
                "backend",
                "gpu",
                "vendor",
                "driver",
                "accumulation_format",
                "output_format",
                "targets",
                "timing_method",
            )
            if cpp[field] != rust[field]
        },
    }
    method = cpp["timing_method"]["gpu"]
    if method == rust["timing_method"]["gpu"] and method != "unsupported":
        gpu_cpp = statistics.mean(cpp["gpu_frame_ns"]) if method == "per-frame-timestamps" else cpp["gpu_batch_ns"]
        gpu_rust = statistics.mean(rust["gpu_frame_ns"]) if method == "per-frame-timestamps" else rust["gpu_batch_ns"]
        result["gpu_rust_over_cpp"] = gpu_rust / gpu_cpp if gpu_cpp else None
    return result


def summarize_runs(
    records: list[dict[str, object]], expected_implementations: tuple[str, ...] = ("cpp", "rust")
) -> list[dict[str, object]]:
    """Summarize independent process means by case, while preserving matched repeated-run ratios."""
    cases = sorted({record["case"] for record in records})
    result = []
    for name in cases:
        selected = [record for record in records if record["case"] == name]
        by_pair = {}
        for record in selected:
            key = (record["run"], record["implementation"])
            workload.require(key not in by_pair, "duplicate comparison run")
            by_pair[key] = record["report"]
        runs = sorted({record["run"] for record in selected})
        pairs = []
        workload.require(
            {implementation for _, implementation in by_pair} == set(expected_implementations),
            "unexpected comparison implementation",
        )
        for run in runs:
            workload.require(
                all((run, implementation) in by_pair for implementation in expected_implementations),
                "missing comparison implementation",
            )
            if len(expected_implementations) == PAIRED_IMPLEMENTATION_COUNT:
                pairs.append(compare_pair(by_pair[run, "cpp"], by_pair[run, "rust"]))
        implementations = {}
        for implementation in expected_implementations:
            reports = [by_pair[run, implementation] for run in runs]
            implementations[implementation] = {
                "process_runs": len(reports),
                "cpu_run_mean_ns": sample_summary([statistics.mean(report["cpu_frame_ns"]) for report in reports]),
                "batch_wall_ns": sample_summary([report["batch_wall_ns"] for report in reports]),
                "per_run": [
                    {
                        "cpu_frame_ns": sample_summary(report["cpu_frame_ns"]),
                        "gpu_frame_ns": sample_summary(report["gpu_frame_ns"])
                        if report["gpu_frame_ns"] is not None
                        else None,
                        "gpu_batch_ns": report["gpu_batch_ns"],
                        "timing_method": report["timing_method"],
                    }
                    for report in reports
                ],
            }
        result.append({"case": name, "implementations": implementations, "paired_run_ratios": pairs})
    return result


def native_arguments(case: RenderCase, source: Path, report: Path, frames: int, warmup: int) -> list[str]:
    """Keep the exact shared native interface explicit and independent from Python-only orchestration options."""
    return [
        "--input",
        str(source),
        "--width",
        str(case.width),
        "--height",
        str(case.height),
        "--scale",
        str(case.scale),
        "--mode",
        case.mode,
        "--frames",
        str(frames),
        "--warmup",
        str(warmup),
        "--report",
        str(report),
    ]


def selected_implementations(options: argparse.Namespace) -> tuple[str, ...]:
    """Select only explicitly supplied binaries, preserving the paired cpp/rust order when both exist."""
    return tuple(implementation for implementation in ("cpp", "rust") if getattr(options, implementation) is not None)


def run_matrix(options: argparse.Namespace, cases: list[RenderCase]) -> list[dict[str, object]]:
    """Counterbalance implementation order sequentially for each case and retain every original native report."""
    artifacts = options.artifacts
    inputs = artifacts / "inputs"
    inputs.mkdir()
    manifests = {}
    for particles in sorted({case.particles for case in cases}):
        path = inputs / f"particles-{particles}.icrb"
        header = workload.WorkloadHeader(options.input_frames, particles, options.step, options.persistence)
        manifests[particles] = workload.write_workload(path, header, options.seed)
    records = []
    sequence = 0
    for case_index, case in enumerate(cases):
        for run in range(options.runs):
            implementations = selected_implementations(options)
            if (case_index * options.runs + run) % 2:
                implementations = tuple(reversed(implementations))
            for implementation in implementations:
                directory = artifacts / f"{sequence:05d}-{case.name}-run{run}-{implementation}"
                source = inputs / f"particles-{case.particles}.icrb"
                manifest = manifests[case.particles]
                workload.require(
                    file_identity(source)["sha256"] == manifest["sha256"], "workload changed during benchmark"
                )
                report_path = directory / "native.json"
                arguments = native_arguments(case, source, report_path, options.frames, options.warmup)
                elapsed = run_native([str(getattr(options, implementation))], arguments, directory, options.timeout)
                report = validate_report(load_report(report_path), implementation, case, manifest, options)
                workload.require(
                    report.get("native_cpu_enabled") is True,
                    "measured runs require recorded native CPU optimizations; rebuild the benchmark",
                )
                records.append(
                    {
                        "sequence": sequence,
                        "case": case.name,
                        "requested": asdict(case),
                        "run": run,
                        "implementation": implementation,
                        "process_wall_ns": elapsed,
                        "workload_manifest": manifest,
                        "native_report": str(report_path),
                        "report": report,
                    }
                )
                sequence += 1
                workload.write_json(artifacts / "raw.json", records)
    for particles, manifest in manifests.items():
        path = inputs / f"particles-{particles}.icrb"
        workload.require(file_identity(path)["sha256"] == manifest["sha256"], "workload changed during benchmark")
    return records


def command_output(command: list[str]) -> str:
    """Read bounded host/source provenance without requiring a build tool or changing the repository."""
    try:
        result = subprocess.run(  # noqa: S603
            command,
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    return result.stdout.strip() if result.returncode == 0 else "unavailable"


def provenance(options: argparse.Namespace) -> dict[str, object]:
    """Separate observed host/executable identities from explicit caller build metadata and native GPU metadata."""
    cpu = platform.processor()
    if sys.platform == "darwin":
        cpu = command_output(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"])
    elif sys.platform == "win32":
        cpu = os.environ.get("PROCESSOR_IDENTIFIER", cpu)
    return {
        "captured_utc": datetime.now(UTC).isoformat(),
        "os": platform.platform(),
        "machine": platform.machine(),
        "cpu": cpu or "unavailable",
        "python": platform.python_version(),
        "revision": command_output(["git", "rev-parse", "HEAD"]),
        "worktree_status": command_output(["git", "status", "--porcelain"]),
        "executables": {
            implementation: file_identity(getattr(options, implementation))
            for implementation in selected_implementations(options)
        },
        "caller_metadata": {"cpp": options.cpp_metadata, "rust": options.rust_metadata},
        "render_scope": RENDER_SCOPE,
        "scope_excludes": [
            "physical presentation",
            "full editor UI",
            "analyzer",
            "audio processing",
            "device deadlines",
        ],
        "comparison": "descriptive repeated process runs, no regression or equivalence verdict",
    }


def size_argument(value: str) -> tuple[int, int]:
    """Parse physical dimensions before any directory or child-process side effects."""
    try:
        width, height = (int(part) for part in value.lower().split("x"))
        count(width, "width", 1, MAX_TARGET_SIZE)
        count(height, "height", 1, MAX_TARGET_SIZE)
    except ValueError as error:
        message = "size must be WIDTHxHEIGHT, each between 1 and 4096"
        raise argparse.ArgumentTypeError(message) from error
    return width, height


def metadata_argument(value: str) -> dict[str, object]:
    """Accept caller provenance as explicit finite JSON, without confusing assertions with measured native data."""
    try:
        result = json.loads(value, parse_constant=reject_json_constant, object_pairs_hook=unique_object)
        workload.require(isinstance(result, dict), "metadata must be a JSON object")
        validate_json_numbers(result)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    return result


def parser() -> argparse.ArgumentParser:
    """Expose explicit executables, bounded custom matrices, and no-build-only operation with side-effect-free help."""
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument("-C", "--cpp", type=Path, help="explicit existing C++ native Release executable")
    result.add_argument("-R", "--rust", type=Path, help="explicit existing Rust native Release executable")
    result.add_argument("-A", "--artifacts", type=Path, required=True, help="new or empty absolute artifact directory")
    result.add_argument(
        "-N", "--no-build", action="store_true", default=True, help="use existing binaries, always enabled"
    )
    result.add_argument("-r", "--runs", type=int, default=3, help="process runs per implementation/case, at least two")
    result.add_argument("-f", "--frames", type=int, default=120, help="measured frames per process, excluding warmup")
    result.add_argument("-w", "--warmup", type=int, default=30, help="untimed warmup frames per process")
    result.add_argument(
        "-z", "--size", type=size_argument, action="append", help="repeat physical WIDTHxHEIGHT, default 1280x720"
    )
    result.add_argument(
        "-s", "--scale", type=float, action="append", help="repeat logical-to-physical scale, default 1"
    )
    result.add_argument(
        "-m", "--mode", choices=("particles", "solid"), action="append", help="repeat modes, default both"
    )
    result.add_argument(
        "-p", "--particles", type=int, action="append", help="repeat particle counts, default standard four"
    )
    result.add_argument(
        "-i", "--input-frames", type=int, default=1, help="stored workload frames, cycled by native runners"
    )
    result.add_argument(
        "-e", "--seed", type=int, default=workload.DEFAULT_SEED, help="deterministic unsigned workload seed"
    )
    result.add_argument(
        "-S", "--step", type=float, default=workload.DEFAULT_STEP, help="simulated frame step in seconds"
    )
    result.add_argument(
        "-P", "--persistence", type=float, default=workload.DEFAULT_PERSISTENCE, help="renderer persistence in seconds"
    )
    result.add_argument(
        "-t", "--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS, help="per-process timeout in seconds"
    )
    result.add_argument(
        "-M", "--cpp-metadata", type=metadata_argument, default={}, help="caller C++ build provenance JSON object"
    )
    result.add_argument(
        "-U", "--rust-metadata", type=metadata_argument, default={}, help="caller Rust build provenance JSON object"
    )
    return result


def validate_options(options: argparse.Namespace) -> list[RenderCase]:
    """Validate all requested work before writing artifacts or executing a native benchmark."""
    count(options.runs, "runs", 2, MAX_RUNS)
    count(options.frames, "frames", 1, MAX_MEASURED_FRAMES)
    count(options.warmup, "warmup", 0, MAX_WARMUP_FRAMES)
    count(options.seed, "seed", 0, workload.UINT32_MAX)
    finite_number(options.step, "step", positive=True)
    workload.require(options.step <= workload.MAX_STEP, "requested step exceeds shared 0.1-second limit")
    finite_number(options.timeout, "timeout", positive=True)
    workload.require(options.timeout <= MAX_TIMEOUT_SECONDS, "timeout exceeds 3600 seconds")
    workload.require(options.artifacts.is_absolute(), "artifacts must be an absolute path")
    workload.require(
        not options.artifacts.exists() or (options.artifacts.is_dir() and not any(options.artifacts.iterdir())),
        "artifacts must be new or empty",
    )
    workload.require(bool(selected_implementations(options)), "supply at least one of --cpp or --rust")
    for implementation in selected_implementations(options):
        executable = getattr(options, implementation).expanduser().resolve()
        workload.require(executable.is_file(), f"{implementation} executable does not exist")
        setattr(options, implementation, executable)
    scales = options.scale or [1.0]
    for scale in scales:
        finite_number(scale, "scale", positive=True)
        workload.require(scale <= MAX_SCALE, "scale exceeds 8")
        workload.require(scale >= MIN_SCALE, "scale must be at least 0.125")
    # Native renderers store scale as float32. Pair the actual shared value rather than unequal JSON round trips.
    scales = [struct.unpack("<f", struct.pack("<f", value))[0] for value in scales]
    particles = options.particles if options.particles is not None else workload.STANDARD_PARTICLE_COUNTS
    for particle_count in particles:
        workload.validate_parameters(
            workload.WorkloadHeader(options.input_frames, particle_count, options.step, options.persistence)
        )
    sizes, modes = options.size or [(1280, 720)], options.mode or ["particles", "solid"]
    workload.require(len(sizes) * len(scales) * len(modes) * len(particles) <= MAX_CASES, "matrix exceeds 256 cases")
    combinations = product(sizes, scales, modes, particles)
    cases = [
        RenderCase(width, height, scale, mode, particle_count)
        for (width, height), scale, mode, particle_count in combinations
    ]
    workload.require(len(cases) <= MAX_CASES, "matrix exceeds 256 cases")
    workload.require(len({case.name for case in cases}) == len(cases), "duplicate or colliding case identity")
    return cases


def main(arguments: list[str] | None = None) -> int:
    """Capture validated native evidence and descriptive summaries, returning nonzero on any incomplete run."""
    command = parser()
    options = command.parse_args(arguments)
    try:
        cases = validate_options(options)
    except (ValueError, OSError) as error:
        command.error(str(error))
    options.artifacts.mkdir(parents=True, exist_ok=True)
    try:
        evidence = provenance(options)
        workload.write_json(options.artifacts / "provenance.json", evidence)
        workload.write_json(
            options.artifacts / "matrix.json",
            {
                "schema_version": SCHEMA_VERSION,
                "cases": [asdict(case) for case in cases],
                "frames": options.frames,
                "warmup": options.warmup,
                "runs": options.runs,
                "step_seconds": options.step,
                "persistence_seconds": options.persistence,
            },
        )
        records = run_matrix(options, cases)
        current = {
            implementation: file_identity(getattr(options, implementation))
            for implementation in selected_implementations(options)
        }
        workload.require(current == evidence["executables"], "executable changed during benchmark")
        summary = {
            "schema_version": SCHEMA_VERSION,
            "provenance": evidence,
            "cases": summarize_runs(records, selected_implementations(options)),
        }
        workload.write_json(options.artifacts / "summary.json", summary)
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        workload.write_json(options.artifacts / "failure.json", {"error": str(error), "type": type(error).__name__})
        print(f"Render benchmark failed: {error}", file=sys.stderr)
        return 1
    print(f"Captured {len(records)} native runs across {len(cases)} cases: {options.artifacts / 'summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
