# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Capture hardware-free callback benchmarks and compare independent paired process runs.

Commands are JSON argv arrays, never shell expressions. Each successful child emits exactly one line prefixed with
CALLBACK_BENCHMARK_JSON= on stdout or stderr. Its JSON is a case object, a list of cases, or a cases wrapper object,
optionally selected with --report-key (a dotted path). --field-map maps canonical fields to dotted paths per case.
Required fields: case, rate, block, mode, callback_ns (one raw duration per callback),
callback_cpu_ns (total or samples), process_cpu_ns (total), frames_consumed, frames_emitted, callbacks,
checksum, silent_callbacks.
Optional transition_ns is a nonempty array of transition durations. Times are nanoseconds; counters are exact integers.
CPU samples, when supplied, contain one entry per callback. Optional callback_frames provides exact per-call deadlines.
Without --native-schema, fields are nanoseconds with no inferred unit conversions.
--native-schema cpp converts the frozen version-1 *_seconds fields explicitly to nanoseconds.
--native-schema rust selects latency-mode steady callbacks, or cold callbacks for cold_only reports.
CPU and source consumption come from matching raw batches.
Rust paced/idle_cpu reports contain no per-call latency and are unsupported, not substituted with zero timings.
The --report-key selects a native object first. --field-map, if supplied, applies after native normalization.
Native validation flags, underruns, raw counts, frame accounting, and deterministic workload evidence are checked.

Supply Release/compiler/flags/JUCE/plugin/fixture/queue/hardening identities through --baseline-metadata and
--candidate-metadata JSON objects. The runner retains these caller assertions, executable SHA-256 identities, raw byte
streams, original reports, and normalized reports. It does not build anything or discover installed plugins.
Use the same argv for both sides to characterize baseline-versus-baseline noise. --capture-only permits baseline alone.

Each case/metric uses a fixed-seed percentile bootstrap of MEAN PAIRED RUN differences (candidate minus baseline),
not individual autocorrelated callback samples. A positive lower 95% bound is REGRESSION; a negative upper bound is PASS
for that measured metric. Bounds touching zero, or fewer than two pairs, are INCONCLUSIVE, not proof of equivalence.
Deadline misses are observed discrete counts: an increase flags a regression; equality is not proof of future safety.
There is no percentage tolerance or claim of hard realtime safety. Exit codes: 0 captured/pass, 1 failed/regressed,
3 inconclusive (2 is a command-line usage error). Artifacts must name a new or empty absolute directory.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
import platform
import random
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from io import BytesIO
from pathlib import Path
from typing import BinaryIO

PREFIX = "CALLBACK_BENCHMARK_JSON="
BOOTSTRAP_SEED = 739391
BOOTSTRAP_RESAMPLES = 10_000
CONFIDENCE_LEVEL = 0.95
DEFAULT_TIMEOUT_SECONDS = 120.0
MAX_TIMEOUT_SECONDS = 3600.0
MIN_COMPARISON_PAIRS = 2
IDENTITY_FIELDS = ("case", "rate", "block", "mode")
SEMANTIC_FIELDS = ("frames_consumed", "frames_emitted", "callbacks", "checksum", "silent_callbacks")
REQUIRED_FIELDS = (*IDENTITY_FIELDS, *SEMANTIC_FIELDS, "callback_ns", "callback_cpu_ns", "process_cpu_ns")
OPTIONAL_FIELDS = ("transition_ns", "callback_frames", "semantic_checks")
NANOSECONDS_PER_SECOND = 1_000_000_000
# Bound parser working storage while retaining complete process output on disk.
# One marker may contain a complete native matrix with tens of thousands of callback observations.
MAX_REPORT_BYTES = 64 * 1024 * 1024
OUTPUT_SCAN_CHUNK_BYTES = 64 * 1024


def write_json(path: Path, value: object) -> None:
    """Retain machine-readable evidence without nonstandard JSON numeric constants."""
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def get_path(value: object, path: str) -> object:
    """Select explicit dotted object keys or array indexes without evaluating expressions."""
    for component in path.split(".") if path else []:
        if isinstance(value, dict) and component in value:
            value = value[component]
        elif isinstance(value, list) and component.isdecimal() and int(component) < len(value):
            value = value[int(component)]
        else:
            message = f"missing report path: {path}"
            raise ValueError(message)
    return value


def number(value: object, field: str) -> float:
    """Require finite nonnegative measurements, rejecting booleans and unsupported scalar types."""
    try:
        valid = type(value) in (int, float) and math.isfinite(value) and value >= 0
    except OverflowError:
        valid = False
    if not valid:
        message = f"{field} must be a finite nonnegative number"
        raise ValueError(message)
    return float(value)


def samples(value: object, field: str) -> list[float]:
    """Validate raw samples before calculating run-level summaries."""
    if not isinstance(value, list) or not value:
        message = f"{field} must be a nonempty array"
        raise ValueError(message)
    return [number(entry, field) for entry in value]


def require(condition: bool, message: str) -> None:
    """Use one descriptive validation failure contract for native accounting assertions."""
    if not condition:
        raise ValueError(message)


def native_count(record: dict[str, object], field: str) -> int:
    """Read exact nonnegative native counters without truncating float values or accepting booleans."""
    value = get_path(record, field)
    require(type(value) is int and value >= 0, f"native {field} must be a nonnegative integer")
    return value


def seconds_to_ns(value: object, field: str) -> float:
    """Convert only a declared seconds field, never infer time units from magnitudes."""
    return number(number(value, field) * NANOSECONDS_PER_SECOND, field)


def native_list(record: dict[str, object], field: str) -> list[object]:
    """Require a native array before checking phase boundaries or sample counts."""
    value = get_path(record, field)
    require(isinstance(value, list), f"native {field} must be an array")
    return value


def normalize_cpp(record: dict[str, object]) -> dict[str, object]:
    """Adapt frozen C++ measured-phase seconds and preserve its independent audio validity checks."""
    require(record.get("valid") is True, "C++ native report valid must be true")
    require(record.get("smoke") is False, "C++ smoke reports do not contain measured callback timing")
    if "timing_valid" in record:
        require(record["timing_valid"] is True, "C++ timing_valid must be true")
    require(record.get("source_failure") is False, "C++ source_failure must be false")
    for field in ("invalid_samples", "unexpected_silent_blocks"):
        require(native_count(record, field) == 0, f"C++ {field} must be zero")
    for field in ("underruns", "buffer_underruns"):
        if field in record:
            require(native_count(record, field) == 0, f"C++ {field} must be zero")
    count, block = native_count(record, "callback_count"), native_count(record, "block_frames")
    raw = native_list(record, "callback_seconds")
    progress = native_list(record, "callback_source_progress_frames")
    excluded = native_list(record, "callback_excluded")
    require(count > 0 and block > 0, "C++ callback_count and block_frames must be positive")
    require(len(raw) == len(progress) == len(excluded) == count, "C++ measured callback array counts disagree")
    require(all(type(value) is bool for value in excluded), "C++ exclusion entries must be booleans")
    idle = record.get("case") in {"idle", "idle-ready"}
    readiness = "ready" if record.get("case") != "idle" else "not-ready"
    if "callback_readiness" in record:
        require(record["callback_readiness"] == readiness, "C++ readiness differs from declared workload")
    expected = [0 if idle or exclusion else block for exclusion in excluded]
    require(
        all(type(value) is int for value in progress) and progress == expected,
        "C++ raw callback source progress differs from admitted frames",
    )
    total = sum(expected)
    if record.get("case") == "control-contention":
        require(any(excluded) and not all(excluded), "C++ contention must measure both excluded and admitted callbacks")
        require(total > 0, "C++ active contention must measure actual source rendering")
    require(
        native_count(record, "expected_source_frames") == total
        and native_count(record, "source_progress_frames") == total,
        "C++ aggregate source progress disagrees with raw callbacks",
    )
    require(
        native_count(record, "excluded_blocks") == sum(excluded), "C++ excluded_blocks disagrees with raw callbacks"
    )
    emitted = native_count(record, "non_silent_frames")
    require(emitted <= total, "C++ emitted frames exceed admitted source frames")
    consumed = native_count(record, "buffered_source_frames_consumed")
    require(
        (total == 0 and consumed == 0) or (total > 0 and consumed > 0),
        "C++ buffered consumption does not match active/idle workload",
    )
    transitions = native_list(record, "control_transitions")
    require(all(isinstance(item, dict) for item in transitions), "C++ control transitions must be objects")
    case = {
        "case": get_path(record, "case"),
        "rate": native_count(record, "sample_rate"),
        "block": block,
        "mode": get_path(record, "mode"),
        "callbacks": count,
        "callback_ns": [seconds_to_ns(value, "callback_seconds") for value in raw],
        "callback_frames": [block] * count,
        "callback_cpu_ns": seconds_to_ns(
            get_path(record, "callback_thread_cpu_seconds"), "callback_thread_cpu_seconds"
        ),
        "process_cpu_ns": seconds_to_ns(get_path(record, "process_cpu_seconds"), "process_cpu_seconds"),
        "frames_consumed": consumed,
        "frames_emitted": emitted,
        "silent_callbacks": native_count(record, "silent_blocks"),
        "checksum": get_path(record, "output_checksum"),
        "semantic_checks": {
            "source_progress_frames": total,
            "callback_readiness": readiness,
            "callback_excluded": excluded,
            "source_fixture": get_path(record, "source_fixture"),
            "source_identity_fnv1a64": get_path(record, "source_identity_fnv1a64"),
            "source_sample_rate": get_path(record, "source_sample_rate"),
            "transition_operations": [get_path(item, "operation") for item in transitions],
        },
    }
    if transitions:
        case["transition_ns"] = [
            seconds_to_ns(get_path(item, "seconds"), "control_transitions.seconds") for item in transitions
        ]
    return case


def rust_samples(record: dict[str, object]) -> tuple[list[dict[str, object]], list[dict[str, object]], str]:
    """Validate complete raw callback and batch coverage before selecting a measured phase."""
    require(
        record.get("mode") == "latency",
        "Rust CPU-only paced/idle_cpu reports have no measured callback durations. "
        "Use MODE=latency. No zero timing is invented.",
    )
    require(type(record.get("cold_only")) is bool, "Rust cold_only must be a boolean")
    require(native_count(record, "underruns") == 0, "Rust underruns must be zero")
    if "insert_failed" in record:
        require(record["insert_failed"] is False, "Rust insert_failed must be false")
    raw, batches = native_list(record, "callbacks"), native_list(record, "batches")
    require(all(isinstance(item, dict) for item in [*raw, *batches]), "Rust callbacks and batches must contain objects")
    count = native_count(record, "callback_count")
    cold, warmup, steady = (
        native_count(record, field) for field in ("cold_callbacks", "warmup_callbacks", "measured_callbacks")
    )
    require(
        count == len(raw) == cold + warmup + steady and cold == 1,
        "Rust callback counts disagree with raw phase coverage",
    )
    phases = ["cold"] * cold + ["warmup"] * warmup + ["steady"] * steady
    blocks = native_list(record, "blocks")
    require(
        bool(blocks) and all(type(block) is int and block > 0 for block in blocks),
        "Rust blocks must be positive integer frame sizes",
    )
    for index, item in enumerate(raw):
        require(
            native_count(item, "index") == index and item.get("phase") == phases[index],
            "Rust raw callback index/phase coverage disagrees",
        )
        require(item.get("callback_invoked") is True, "Rust selected callback must actually be invoked")
        frames = native_count(item, "frames_requested")
        require(frames == blocks[index % len(blocks)], "Rust callback frame sizes differ from declared block sequence")
        emitted = native_count(item, "non_silent_frames")
        require(frames > 0 and emitted <= frames, "Rust callback emitted/requested frame counts are invalid")
        require(type(item.get("intentional_exclusion")) is bool, "Rust exclusion must be a boolean")
        require(not item["intentional_exclusion"] or emitted == 0, "Rust excluded callback emitted audio")
        if record.get("workload") in {"dry", "wet", "contention", "idle"}:
            expected_emitted = 0 if item["intentional_exclusion"] or record["workload"] == "idle" else frames
            require(emitted == expected_emitted, "Rust callback audio emission differs from admitted frames")
        require(isinstance(item.get("checksum"), str) and bool(item["checksum"]), "Rust callback checksum is missing")
        number(get_path(item, "duration_ns"), "duration_ns")
    cursor = 0
    for batch in batches:
        start, end = native_count(batch, "start"), native_count(batch, "end")
        require(start == cursor and start < end <= count, "Rust batches must cover every callback exactly once")
        require(
            all(item["phase"] == batch.get("phase") for item in raw[start:end]),
            "Rust batch straddles callback phase boundaries",
        )
        require(
            all(item["intentional_exclusion"] == batch.get("intentional_exclusion") for item in raw[start:end]),
            "Rust batch exclusion disagrees with callbacks",
        )
        for field in ("source_frames_consumed", "stale_frames_discarded"):
            native_count(batch, field)
        for field in ("callback_thread_cpu_ns", "process_cpu_ns"):
            number(get_path(batch, field), field)
        if record.get("workload") in {"dry", "wet", "contention", "idle"}:
            expected = sum(
                item["frames_requested"]
                for item in raw[start:end]
                if not item["intentional_exclusion"] and record["workload"] != "idle"
            )
            require(batch["source_frames_consumed"] == expected, "Rust batch consumption differs from admitted frames")
        cursor = end
    require(cursor == count, "Rust batches omit callbacks")
    aggregate = {
        "frames_requested": sum(item["frames_requested"] for item in raw),
        "steady_frames_requested": sum(item["frames_requested"] for item in raw if item["phase"] == "steady"),
        "valid_frames_emitted": sum(item["non_silent_frames"] for item in raw),
        "non_silent_frames_emitted": sum(item["non_silent_frames"] for item in raw),
        "silent_frames": sum(item["frames_requested"] - item["non_silent_frames"] for item in raw),
        "source_frames_consumed": sum(batch["source_frames_consumed"] for batch in batches),
        "stale_frames_discarded": sum(batch["stale_frames_discarded"] for batch in batches),
        "excluded_callbacks": sum(item["intentional_exclusion"] for item in raw),
    }
    for field, expected in aggregate.items():
        require(native_count(record, field) == expected, f"Rust aggregate {field} disagrees with raw samples")
    if record.get("benchmark_contract") == "transition_oracle_and_callback_start_pacing_v2":
        validate_rust_oracle(record, raw, batches)
    phase = "cold" if record["cold_only"] else "steady"
    require(not record["cold_only"] or count == 1, "Rust cold_only report contains non-cold samples")
    return (
        [item for item in raw if item["phase"] == phase],
        [batch for batch in batches if batch["phase"] == phase],
        phase,
    )


def validate_rust_oracle(
    record: dict[str, object], raw: list[dict[str, object]], batches: list[dict[str, object]]
) -> None:
    """Require revised transition-oracle evidence rather than trusting a success marker alone."""
    for item in raw:
        require(
            native_count(item, "expected_active_frames") == item["non_silent_frames"],
            "Rust oracle active frames disagree with emitted audio",
        )
        native_count(item, "expected_reset_count")
    for batch in batches:
        callbacks = raw[batch["start"] : batch["end"]]
        expected = sum(item["expected_active_frames"] for item in callbacks)
        resets = sum(item["expected_reset_count"] for item in callbacks)
        require(
            native_count(batch, "expected_active_frames") == expected == batch["source_frames_consumed"],
            "Rust oracle batch consumption disagrees with active windows",
        )
        require(native_count(batch, "expected_reset_count") == resets, "Rust oracle reset counts disagree")
        for field in ("insert_failed", "stream_lost", "ended"):
            require(batch.get(field) is False, f"Rust oracle batch {field} must be false")
        for field in ("playing", "insert_settled"):
            require(type(batch.get(field)) is bool, f"Rust oracle batch {field} must be a boolean")
        for field in ("source_position", "generation", "insert_changes_applied", "source_frames_prepared"):
            native_count(batch, field)
    active = sum(item["expected_active_frames"] for item in raw)
    steady = sum(item["expected_active_frames"] for item in raw if item["phase"] == "steady")
    require(native_count(record, "expected_active_frames") == active, "Rust oracle total active frames disagree")
    require(
        native_count(record, "expected_steady_active_frames") == steady,
        "Rust oracle steady active frames disagree",
    )
    require(
        native_count(record, "expected_reset_count") == sum(item["expected_reset_count"] for item in raw),
        "Rust oracle total reset counts disagree",
    )
    if record.get("workload") == "transitions" and not record["cold_only"]:
        require(steady > 0, "Rust transitions must render active audio during the measured phase")


def normalize_rust(record: dict[str, object]) -> dict[str, object]:
    """Select raw measured Rust latency callbacks and matching CPU/consumption batches without mixing warmup."""
    raw, batches, phase = rust_samples(record)
    require(bool(raw), "Rust report has no measured callbacks in the selected phase")
    frames = [item["frames_requested"] for item in raw]
    transitions = [batch for batch in batches if batch.get("transition") is not None]
    case = {
        "case": f"{get_path(record, 'workload')}:{get_path(record, 'sample_format')}:{phase}",
        "rate": native_count(record, "sample_rate"),
        "block": min(frames),
        "mode": "latency",
        "callback_frames": frames,
        "callback_ns": [item["duration_ns"] for item in raw],
        "callbacks": len(raw),
        "callback_cpu_ns": math.fsum(batch["callback_thread_cpu_ns"] for batch in batches),
        "process_cpu_ns": math.fsum(batch["process_cpu_ns"] for batch in batches),
        "frames_consumed": sum(batch["source_frames_consumed"] for batch in batches),
        "frames_emitted": sum(item["non_silent_frames"] for item in raw),
        "silent_callbacks": sum(item["non_silent_frames"] == 0 for item in raw),
        "checksum": hashlib.sha256(json.dumps([item["checksum"] for item in raw]).encode()).hexdigest(),
        "semantic_checks": {
            "checksum_scope": "sha256 of ordered selected callback checksum JSON array",
            "callback_excluded": [item["intentional_exclusion"] for item in raw],
            "stale_frames_discarded": sum(batch["stale_frames_discarded"] for batch in batches),
            "fixture": get_path(record, "fixture"),
            "blocks": get_path(record, "blocks"),
            "transition_operations": [batch["transition"] for batch in transitions],
            "benchmark_contract": record.get("benchmark_contract", "legacy_v1"),
            "native_initialization_policy": record.get("native_initialization_policy"),
            "pacing_scope": record.get("pacing_scope", "legacy_completion_boundary"),
        },
    }
    if transitions:
        case["transition_ns"] = [
            number(get_path(batch, "transition_ack_ns"), "transition_ack_ns") for batch in transitions
        ]
    return case


def normalize_native(record: dict[str, object], schema: str) -> dict[str, object]:
    """Select only known versioned native units, rejecting incompatible schema and explicit unit declarations."""
    require(schema in {"cpp", "rust"}, f"unsupported native schema: {schema}")
    require(
        record.get("implementation") == schema
        and type(record.get("schema_version")) is int
        and record["schema_version"] == 1,
        f"unsupported {schema} native implementation/schema_version",
    )
    unit = "seconds" if schema == "cpp" else "ns"
    for field in ("time_unit", "duration_unit", "unit"):
        if field in record:
            require(record[field] == unit, f"unknown/conflicting native {field}: expected {unit}")
    if "valid" in record:
        require(record["valid"] is True, "native report valid must be true")
    return normalize_cpp(record) if schema == "cpp" else normalize_rust(record)


def normalize_report(
    source: object, report_key: str, field_map: dict[str, str], native_schema: str | None = None
) -> list[dict[str, object]]:
    """Map an explicitly selected native schema into a small strict comparison contract."""
    selected = get_path(source, report_key)
    if isinstance(selected, dict) and "cases" in selected:
        selected = selected["cases"]
    records = selected if isinstance(selected, list) else [selected]
    if not records:
        message = "the benchmark report contains no cases"
        raise ValueError(message)
    normalized = []
    identities = set()
    for record in records:
        if not isinstance(record, dict):
            message = "each benchmark case must be an object"
            raise ValueError(message)  # noqa: TRY004 - one schema-validation exception contract
        canonical = normalize_native(record, native_schema) if native_schema is not None else record
        case = {field: get_path(canonical, field_map.get(field, field)) for field in REQUIRED_FIELDS}
        for field in OPTIONAL_FIELDS:
            path = field_map.get(field, field)
            # An explicitly mapped optional field is required. An absent default field is allowed.
            if field in field_map or field in canonical:
                case[field] = get_path(canonical, path)
        validate_case(case)
        identity = tuple(case[field] for field in IDENTITY_FIELDS)
        if identity in identities:
            message = f"duplicate benchmark case: {identity}"
            raise ValueError(message)
        identities.add(identity)
        normalized.append(case)
    return normalized


def validate_case(case: dict[str, object]) -> None:  # noqa: PLR0912 - explicit independent field contracts
    """Reject missing work, malformed counts, and measurements that cannot describe the same workload."""
    for field in ("case", "mode"):
        if not isinstance(case[field], str) or not case[field].strip():
            message = f"{field} must be a nonempty string"
            raise ValueError(message)
    for field in ("rate", "block", "frames_consumed", "frames_emitted", "callbacks", "silent_callbacks"):
        value = case[field]
        minimum = 1 if field in {"rate", "block", "callbacks"} else 0
        if type(value) is not int or value < minimum:
            message = f"{field} must be an integer >= {minimum}"
            raise ValueError(message)
    if case["silent_callbacks"] > case["callbacks"]:
        message = "silent_callbacks exceeds callbacks"
        raise ValueError(message)
    if len(samples(case["callback_ns"], "callback_ns")) != case["callbacks"]:
        message = "callback_ns sample count differs from callbacks"
        raise ValueError(message)
    cpu = case["callback_cpu_ns"]
    if isinstance(cpu, list):
        if len(samples(cpu, "callback_cpu_ns")) != case["callbacks"]:
            message = "callback_cpu_ns sample count differs from callbacks"
            raise ValueError(message)
    else:
        number(cpu, "callback_cpu_ns")
    number(case["process_cpu_ns"], "process_cpu_ns")
    checksum = case["checksum"]
    if not isinstance(checksum, str):
        number(checksum, "checksum")
    elif not checksum:
        message = "checksum must not be empty"
        raise ValueError(message)
    if "transition_ns" in case:
        samples(case["transition_ns"], "transition_ns")
    if "callback_frames" in case:
        frames = case["callback_frames"]
        if (
            not isinstance(frames, list)
            or len(frames) != case["callbacks"]
            or any(type(frame) is not int or frame <= 0 for frame in frames)
        ):
            message = "callback_frames must contain one positive integer per callback"
            raise ValueError(message)
    if "semantic_checks" in case and not isinstance(case["semantic_checks"], dict):
        message = "semantic_checks must be an object of deterministic workload evidence"
        raise ValueError(message)


def percentile(ordered: list[float], fraction: float) -> float:
    """Use linear interpolation consistently for callback summaries and bootstrap confidence bounds."""
    position = (len(ordered) - 1) * fraction
    left = int(position)
    right = min(left + 1, len(ordered) - 1)
    return ordered[left] + (ordered[right] - ordered[left]) * (position - left)


def metrics(case: dict[str, object]) -> dict[str, float]:
    """Summarize each run before resampling, keeping workloads and timing tails separate."""
    callback = samples(case["callback_ns"], "callback_ns")
    result = {}
    for field in ("callback_ns", "transition_ns"):
        if field not in case:
            continue
        ordered = sorted(samples(case[field], field))
        result.update(
            {
                f"{field}.{label}": percentile(ordered, quantile)
                for label, quantile in (("p50", 0.5), ("p95", 0.95), ("p99", 0.99), ("max", 1.0))
            }
        )
    cpu = case["callback_cpu_ns"]
    result["callback_cpu_ns"] = (
        math.fsum(samples(cpu, "callback_cpu_ns")) if isinstance(cpu, list) else number(cpu, "callback_cpu_ns")
    )
    result["process_cpu_ns"] = number(case["process_cpu_ns"], "process_cpu_ns")
    frames = case.get("callback_frames", [case["block"]] * len(callback))
    deadlines = [
        number(frame, "callback_frames") / number(case["rate"], "rate") * NANOSECONDS_PER_SECOND for frame in frames
    ]
    ratios = sorted(duration / deadline for duration, deadline in zip(callback, deadlines, strict=True))
    result["deadline_ratio.p99"] = percentile(ratios, 0.99)
    result["deadline_ratio.max"] = ratios[-1]
    result["deadline_misses"] = float(
        sum(duration > deadline for duration, deadline in zip(callback, deadlines, strict=True))
    )
    consumed = number(case["frames_consumed"], "frames_consumed")
    if consumed:
        audio_seconds = consumed / number(case["rate"], "rate")
        result["callback_ns_per_source_audio_second"] = math.fsum(callback) / audio_seconds
    return result


def paired_bounds(differences: list[float]) -> dict[str, object]:
    """Bootstrap whole paired runs, never pretending callbacks within a run are independent replicates."""
    mean = statistics.fmean(differences)
    if len(differences) < MIN_COMPARISON_PAIRS:
        return {"mean_difference": mean, "confidence_interval": None, "outcome": "INCONCLUSIVE"}
    generator = random.Random(BOOTSTRAP_SEED)  # noqa: S311 - deterministic resampling, not security
    means = sorted(
        statistics.fmean(generator.choices(differences, k=len(differences))) for _ in range(BOOTSTRAP_RESAMPLES)
    )
    alpha = (1.0 - CONFIDENCE_LEVEL) * 0.5
    lower, upper = percentile(means, alpha), percentile(means, 1.0 - alpha)
    outcome = "REGRESSION" if lower > 0 else "PASS" if upper < 0 else "INCONCLUSIVE"
    return {"mean_difference": mean, "confidence_interval": [lower, upper], "outcome": outcome}


def compare_pairs(pairs: list[dict[str, list[dict[str, object]]]]) -> dict[str, object]:  # noqa: PLR0912
    """Flag deterministic semantic/coverage changes separately from measured timing uncertainty."""
    issues = []
    observations: dict[str, dict[str, list[float]]] = {}
    expected_cases = None
    deadline_observations: dict[str, dict[str, list[float]]] = {}
    for pair_index, pair in enumerate(pairs, start=1):
        indexed = {
            role: {json.dumps([case[field] for field in IDENTITY_FIELDS]): case for case in cases}
            for role, cases in pair.items()
        }
        if expected_cases is None:
            expected_cases = set(indexed["baseline"])
        for role, cases in indexed.items():
            if set(cases) != expected_cases:
                issues.append(f"pair {pair_index} {role}: missing or changed benchmark cases")
        for identity in indexed["baseline"].keys() & indexed["candidate"].keys():
            baseline, candidate = indexed["baseline"][identity], indexed["candidate"][identity]
            issues.extend(
                f"pair {pair_index} {identity}: semantic change in {field}"
                for field in (*SEMANTIC_FIELDS, "callback_frames", "semantic_checks")
                if baseline.get(field) != candidate.get(field)
            )
            baseline_transitions = baseline.get("transition_ns", [])
            candidate_transitions = candidate.get("transition_ns", [])
            if len(baseline_transitions) != len(candidate_transitions):
                issues.append(f"pair {pair_index} {identity}: transition sample coverage changed")
            left, right = metrics(baseline), metrics(candidate)
            deadline_counts = deadline_observations.setdefault(identity, {"baseline": [], "candidate": []})
            deadline_counts["baseline"].append(left.pop("deadline_misses"))
            deadline_counts["candidate"].append(right.pop("deadline_misses"))
            if deadline_counts["candidate"][-1] > deadline_counts["baseline"][-1]:
                issues.append(f"pair {pair_index} {identity}: observed deadline misses increased")
            if left.keys() != right.keys():
                issues.append(f"pair {pair_index} {identity}: metric coverage changed")
            case_observations = observations.setdefault(identity, {})
            for metric in left.keys() & right.keys():
                case_observations.setdefault(metric, []).append(right[metric] - left[metric])
    comparisons = []
    for identity, values in sorted(observations.items()):
        measured = {
            metric: {"paired_differences": differences, **paired_bounds(differences)}
            for metric, differences in sorted(values.items())
        }
        comparisons.append(
            {
                "identity": json.loads(identity),
                "metrics": measured,
                "observed_deadline_misses": deadline_observations[identity],
            }
        )
    outcomes = [metric["outcome"] for case in comparisons for metric in case["metrics"].values()]
    if issues or "REGRESSION" in outcomes:
        outcome = "REGRESSION"
    elif outcomes and all(value == "PASS" for value in outcomes):
        outcome = "PASS"
    else:
        outcome = "INCONCLUSIVE"
    return {"outcome": outcome, "semantic_issues": issues, "comparisons": comparisons}


def executable_identity(command: list[str], environment: dict[str, str], cwd: Path) -> dict[str, object]:
    """Hash the exact launched executable, resolving PATH with the caller's environment rather than a shell."""
    executable = command[0]
    if not Path(executable).is_absolute():
        if Path(executable).name != executable:
            executable = str(cwd / executable)
        else:
            search_path = os.pathsep.join(
                str(cwd / directory) if not Path(directory).is_absolute() else directory
                for directory in environment.get("PATH", os.defpath).split(os.pathsep)
            )
            located = shutil.which(executable, path=search_path)
            if located is None:
                message = f"benchmark executable not found on supplied PATH: {executable}"
                raise FileNotFoundError(message)
            executable = located
    path = Path(executable).resolve()
    with path.open("rb") as binary:
        digest = hashlib.file_digest(binary, "sha256").hexdigest()
    return {"path": str(path), "sha256": digest, "bytes": path.stat().st_size}


def terminate_process_tree(process: subprocess.Popen[bytes]) -> None:
    """Bound failed probes and reap workers instead of leaving benchmark load behind after a timeout."""
    if sys.platform == "win32":
        taskkill = str(Path(os.environ["SYSTEMROOT"]) / "System32" / "taskkill.exe")
        try:
            subprocess.run(  # noqa: S603 - fixed operating-system utility, exact PID arguments
                [taskkill, "/PID", str(process.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            process.kill()
    else:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)


def scan_reports(stream: BinaryIO) -> list[bytes]:
    """Scan diagnostics in fixed chunks, retaining only bounded complete marker payloads."""
    prefixes = (PREFIX.encode(), ("test callback_benchmark::production_callback ... " + PREFIX).encode())
    prefix_limit = max(map(len, prefixes))
    beginning = bytearray()
    payload = bytearray()
    marker = False
    discarded = False
    reports = []
    while chunk := stream.read(OUTPUT_SCAN_CHUNK_BYTES):
        for fragment_index, piece in enumerate(chunk.split(b"\n")):
            fragment = piece
            if fragment_index:
                if marker:
                    reports.append(bytes(payload))
                    require(len(reports) <= 1, f"expected exactly one {PREFIX} report, found multiple")
                beginning.clear()
                payload.clear()
                marker = False
                discarded = False
            if discarded or not fragment:
                continue
            if not marker:
                taken = min(prefix_limit - len(beginning), len(fragment))
                beginning.extend(fragment[:taken])
                fragment = fragment[taken:]
                matched = next((prefix for prefix in prefixes if beginning.startswith(prefix)), None)
                if matched is not None:
                    marker = True
                    payload.extend(beginning[len(matched) :])
                    require(len(payload) <= MAX_REPORT_BYTES, "callback report exceeds maximum byte size")
                    beginning.clear()
                elif not any(prefix.startswith(beginning) for prefix in prefixes):
                    discarded = True
                    beginning.clear()
                    continue
            if marker:
                require(len(payload) + len(fragment) <= MAX_REPORT_BYTES, "callback report exceeds maximum byte size")
                payload.extend(fragment)
    if marker:
        reports.append(bytes(payload))
    require(len(reports) <= 1, f"expected exactly one {PREFIX} report, found multiple")
    return reports


def extract_report_streams(stdout: BinaryIO, stderr: BinaryIO) -> object:
    """Parse one bounded report without loading retained diagnostic streams into memory."""
    reports = scan_reports(stdout) + scan_reports(stderr)
    require(len(reports) == 1, f"expected exactly one {PREFIX} report, found {len(reports)}")
    return json.loads(reports[0])


def extract_report(stdout: bytes, stderr: bytes) -> object:
    """Keep the byte-oriented test seam while production scans retained files incrementally."""
    return extract_report_streams(BytesIO(stdout), BytesIO(stderr))


def capture_run(options: argparse.Namespace, role: str, pair: int) -> dict[str, object]:
    """Persist exact streams and provenance even when launch, timeout, schema, or binary-stability checks fail."""
    directory = options.artifacts / f"pair-{pair:04d}-{role}"
    directory.mkdir()
    command = getattr(options, f"{role}_command")
    overrides = options.env | getattr(options, f"{role}_env")
    environment = os.environ | overrides
    result: dict[str, object] = {
        "role": role,
        "pair": pair,
        "command": command,
        "environment_overrides": overrides,
        "returncode": None,
        "timed_out": False,
        "outcome": "FAILED",
    }
    stdout_path, stderr_path = directory / "stdout.bin", directory / "stderr.bin"
    stdout_path.touch()
    stderr_path.touch()
    try:
        before = executable_identity(command, environment, options.cwd)
        result["executable_before"] = before
        with tempfile.TemporaryDirectory(prefix="isolated-storage-", dir=directory) as storage:
            environment.update(dict.fromkeys(("AUDIOBATCH_DATA_ROOT", "TMPDIR", "TMP", "TEMP"), storage))
            result["isolated_storage"] = storage
            with (
                stdout_path.open("wb") as stdout,
                stderr_path.open("wb") as stderr,
                subprocess.Popen(  # noqa: S603 - explicitly supplied argv, no shell interpretation
                    [before["path"], *command[1:]],
                    cwd=options.cwd,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout,
                    stderr=stderr,
                    start_new_session=sys.platform != "win32",
                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0,
                ) as process,
            ):
                try:
                    process.wait(timeout=options.timeout)
                except subprocess.TimeoutExpired:
                    result["timed_out"] = True
                    terminate_process_tree(process)
                result["returncode"] = process.returncode
        after = executable_identity(command, environment, options.cwd)
        result["executable_after"] = after
        if before != after:
            message = "executable identity changed during the run"
            raise ValueError(message)  # noqa: TRY301 - preserve this failure in run.json
        if result["timed_out"] or result["returncode"] != 0:
            message = "benchmark timed out" if result["timed_out"] else "benchmark exited unsuccessfully"
            raise ValueError(message)  # noqa: TRY301 - preserve this failure in run.json
        with stdout_path.open("rb") as stdout, stderr_path.open("rb") as stderr:
            source = extract_report_streams(stdout, stderr)
        write_json(directory / "source-report.json", source)
        normalized = normalize_report(source, options.report_key, options.field_map, options.native_schema)
        write_json(directory / "normalized.json", normalized)
        result.update({"outcome": "CAPTURED", "cases": normalized})
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
    write_json(directory / "run.json", result)
    return result


def build_parser() -> argparse.ArgumentParser:
    """Describe benchmark commands and evidence options without touching artifacts or launching children."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "-b", "--baseline-command", required=True, help="JSON argv array; argv[0] is the benchmark executable"
    )
    parser.add_argument("-k", "--candidate-command", help="JSON argv array; same command as baseline measures noise")
    parser.add_argument("-P", "--pairs", type=int, default=10)
    parser.add_argument("-A", "--artifacts", type=Path, required=True)
    parser.add_argument("-d", "--cwd", type=Path, default=Path.cwd())
    parser.add_argument("-t", "--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("-q", "--capture-only", action="store_true")
    parser.add_argument("-K", "--report-key", default="")
    parser.add_argument(
        "-N", "--native-schema", choices=("cpp", "rust"), help="Explicit frozen version-1 native report adapter"
    )
    parser.add_argument(
        "-F", "--field-map", default="{}", help="JSON object mapping canonical fields to dotted source paths"
    )
    parser.add_argument("-e", "--env", default="{}", help="Shared JSON string-to-string environment overrides")
    for role, environment_short, metadata_short in (("baseline", "-u", "-M"), ("candidate", "-U", "-m")):
        parser.add_argument(environment_short, f"--{role}-env", default="{}")
        parser.add_argument(
            metadata_short,
            f"--{role}-metadata",
            default="{}",
            help="Caller-provided build/toolchain/fixture identities",
        )
    return parser


def parse_options(arguments: list[str] | None = None) -> argparse.Namespace:  # noqa: PLR0912
    """Keep commands, metadata, finite deadlines, and artifact destinations explicit."""
    parser = build_parser()
    options = parser.parse_args(arguments)
    if not options.capture_only and options.candidate_command is None:
        parser.error("--candidate-command is required unless --capture-only is supplied")
    if options.pairs < 1:
        parser.error("--pairs must be positive")
    if not 0 < options.timeout <= MAX_TIMEOUT_SECONDS:
        parser.error("--timeout must be finite, positive, and at most 3600 seconds")
    if not options.artifacts.is_absolute():
        parser.error("--artifacts must be absolute")
    options.cwd = options.cwd.resolve()
    for field in (
        "baseline_command",
        "candidate_command",
        "env",
        "baseline_env",
        "candidate_env",
        "baseline_metadata",
        "candidate_metadata",
        "field_map",
    ):
        raw = getattr(options, field)
        if raw is None:
            continue
        try:
            value = json.loads(raw)
            # Reject NaN/Infinity even in opaque caller metadata.
            json.dumps(value, allow_nan=False)
        except (ValueError, TypeError) as error:
            parser.error(f"invalid --{field.replace('_', '-')}: {error}")
        if field.endswith("command"):
            valid = (
                isinstance(value, list)
                and bool(value)
                and all(isinstance(item, str) and "\u0000" not in item for item in value)
                and bool(value[0])
            )
        else:
            valid = isinstance(value, dict)
            if valid and not field.endswith("metadata"):
                valid = all(isinstance(key, str) and isinstance(item, str) for key, item in value.items())
                if valid and field.endswith("env"):
                    valid = all(
                        key and "=" not in key and "\u0000" not in key and "\u0000" not in item
                        for key, item in value.items()
                    )
        if not valid:
            parser.error(f"invalid JSON structure for --{field.replace('_', '-')}")
        setattr(options, field, value)
    if set(options.field_map) - set(REQUIRED_FIELDS + OPTIONAL_FIELDS):
        parser.error("--field-map contains unknown canonical fields")
    return options


def main(arguments: list[str] | None = None) -> int:
    """Alternate baseline/candidate order and retain incomplete evidence rather than silently dropping failed pairs."""
    options = parse_options(arguments)
    if options.artifacts.exists() and (not options.artifacts.is_dir() or any(options.artifacts.iterdir())):
        print("FAILED: --artifacts must be a new or empty directory", file=sys.stderr)
        return 1
    options.artifacts.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema_version": 1,
        "started_utc": datetime.now(UTC).isoformat(),
        "cwd": str(options.cwd),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version,
        "pairs": options.pairs,
        "capture_only": options.capture_only,
        "timeout_seconds": options.timeout,
        "baseline": {"command": options.baseline_command, "metadata": options.baseline_metadata},
        "candidate": {"command": options.candidate_command, "metadata": options.candidate_metadata},
        "report_key": options.report_key,
        "native_schema": options.native_schema,
        "field_map": options.field_map,
        "bootstrap": {
            "seed": BOOTSTRAP_SEED,
            "resamples": BOOTSTRAP_RESAMPLES,
            "confidence_level": CONFIDENCE_LEVEL,
            "statistic": "mean(candidate - baseline) across paired runs",
        },
        "interpretation": "INCONCLUSIVE is not equivalence or a no-regression result; no tolerance is assumed.",
    }
    write_json(options.artifacts / "manifest.json", manifest)
    runs, pairs, failures = [], [], []
    identities = {}
    roles = ("baseline", "candidate") if options.candidate_command is not None else ("baseline",)
    for pair in range(1, options.pairs + 1):
        captured = {}
        for role in roles if pair % 2 else reversed(roles):
            run = capture_run(options, role, pair)
            runs.append({"pair": pair, "role": role, "outcome": run["outcome"]})
            identity = run.get("executable_before")
            if identity is not None and identities.setdefault(role, identity) != identity:
                failures.append(f"pair {pair} {role}: executable changed between runs")
            if run["outcome"] == "CAPTURED":
                captured[role] = run["cases"]
            else:
                failures.append(f"pair {pair} {role}: {run.get('error', 'capture failed')}")
        pairs.append(captured)
        if failures:
            break
    if failures:
        summary = {"outcome": "FAILED", "errors": failures}
    elif options.capture_only:
        summary = {"outcome": "CAPTURED"}
    else:
        summary = compare_pairs(pairs)
    summary.update(
        {
            "runs": runs,
            "executable_identities": identities,
            "baseline_vs_baseline": options.baseline_command == options.candidate_command
            and options.baseline_env == options.candidate_env,
        }
    )
    write_json(options.artifacts / "summary.json", summary)
    print(f"{summary['outcome']}: callback benchmark evidence: {options.artifacts}")
    return {"PASS": 0, "CAPTURED": 0, "FAILED": 1, "REGRESSION": 1, "INCONCLUSIVE": 3}[summary["outcome"]]


if __name__ == "__main__":
    sys.exit(main())
