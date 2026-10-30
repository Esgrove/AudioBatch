#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0", "numpy>=2"]
# ///
"""
Prepares isolated CLI benchmark inputs and validates results outside hyperfine's measured commands.

Sources are only read, and each gets a numbered directory to avoid output-name conflicts.
A private workspace marker constrains destructive preparation to directories this helper created.
The existing parity oracles check every measured run and compare the final outputs of both implementations.
"""

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

import compare_levels as levels
import compare_outputs as outputs

RUST_DIRECTORY = Path(__file__).resolve().parents[1]
ROOT_DIRECTORY = RUST_DIRECTORY.parent
MARKER_NAME = ".audiobatch-cli-benchmark.json"
TARGETS = {"analyze": {}, "peak": {"peak": 0.0}, "lufs": {"lufs": -14.0}}


def write_json(path: Path, value: object) -> None:
    """Writes readable JSON with a final newline."""
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def command_output(command: list[str]) -> str:
    """Captures machine metadata without turning an unavailable optional command into a benchmark failure."""
    try:
        completed = subprocess.run(command, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError) as error:
        return f"Unavailable: {error}"
    return completed.stdout.strip()


def fingerprint(path: Path) -> str:
    """Hashes a file without retaining an entire audio file or executable in memory."""
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def discover_sources(corpus: Path) -> tuple[list[Path], list[dict]]:
    """Rejects pointers and symlinks, then excludes independently known normalization failures."""
    if not corpus.is_dir():
        raise ValueError(f"Corpus directory not found: {corpus}")
    sources = []
    excluded = []
    for source in sorted(corpus.rglob("*")):
        if source.is_symlink():
            raise ValueError(f"Corpus symlinks are not allowed: {source}")
        if not source.is_file() or source.suffix.lower() not in levels.AUDIO_EXTENSIONS:
            continue
        if any(part.startswith(".") for part in source.relative_to(corpus).parts):
            continue
        with source.open("rb") as audio:
            if audio.read(80).startswith(b"version https://git-lfs.github.com/spec/v1"):
                raise ValueError(f"Git LFS pointer instead of audio: {source}. Run git lfs pull first.")
        properties = levels.fixture_properties(source)
        if any(
            levels.expected_failure(properties, target.get("peak"), target.get("lufs")) for target in TARGETS.values()
        ):
            excluded.append({"path": str(source), "reason": "Known analysis or normalization failure"})
        else:
            sources.append(source)
    if not sources:
        raise ValueError(f"No usable audio files in {corpus}. Generate fixtures/benchmark/output or run git lfs pull.")
    return sources, excluded


def select_corpora(corpus: str = "auto") -> list[Path]:
    """Combines both fixture collections in auto mode, or restricts input to one explicit directory."""
    if corpus != "auto":
        return [Path(corpus).resolve()]
    generated = ROOT_DIRECTORY / "fixtures/benchmark/output"
    corpora = []
    if generated.is_dir() and any(
        path.is_file() and path.suffix.lower() in levels.AUDIO_EXTENSIONS for path in generated.rglob("*")
    ):
        corpora.append(generated)
    corpora.append(ROOT_DIRECTORY / "fixtures/normalization")
    return corpora


def workspace(path: Path) -> dict:
    """Requires a marker bound to this exact private directory before copying or removing anything."""
    if path.is_symlink() or path.resolve() != path:
        raise ValueError(f"Workspace must be a physical absolute directory: {path}")
    state = json.loads((path / MARKER_NAME).read_text(encoding="utf-8"))
    if state["workspace"] != str(path) or path == Path(path.anchor):
        raise ValueError(f"Invalid private workspace: {path}")
    return state


def initialize(arguments: argparse.Namespace) -> None:
    """Copies a safe, conflict-free seed corpus and records the machine, binaries, and benchmark settings."""
    work = arguments.work
    if work.is_symlink() or work.resolve() != work or any(work.iterdir()):
        raise ValueError("Initialization requires an empty physical temporary directory")
    corpora = select_corpora(arguments.corpus)
    sources = []
    excluded = []
    for corpus in corpora:
        selected, skipped = discover_sources(corpus)
        sources.extend(selected)
        excluded.extend(skipped)
    for implementation, binary in (("cpp", arguments.cpp), ("rust", arguments.rust)):
        help_text = command_output([str(binary), "--help"])
        for option in ("--delete", "--jobs", "--refresh"):
            if option not in help_text:
                raise ValueError(f"{implementation} CLI does not advertise {option}. Rebuild the release CLI: {binary}")
    state = {"workspace": str(work), "results": str(arguments.results.resolve())}
    write_json(work / MARKER_NAME, state)
    seed = work / "seed"
    seed.mkdir()
    (arguments.results / "raw").mkdir()
    manifest = []
    for index, source in enumerate(sources, start=1):
        destination = seed / f"{index:04d}" / source.name
        destination.parent.mkdir()
        shutil.copy2(source, destination)
        manifest.append(
            {"source": str(source), "relative": str(destination.relative_to(seed)), "sha256": fingerprint(source)}
        )
    for mode, target in TARGETS.items():
        expected = levels.expected_results(levels.input_files([seed], recurse=True), seed, **target)
        if not expected or any(row["status"] != "analyzed" for row in expected.values()):
            raise ValueError(f"Benchmark selection unexpectedly contains failures in {mode} mode")
        write_json(work / f"{mode}-expectations.json", expected)
        if mode != "analyze":
            write_json(work / f"{mode}-before.json", outputs.snapshot_folder(seed, expected))
    (work / "lossy.txt").write_text(
        "".join(f"{path}\n" for path in sorted(outputs.list_files(seed)) if outputs.is_lossy(seed / path)),
        encoding="utf-8",
    )
    cpu_command = ["sysctl", "-n", "machdep.cpu.brand_string"] if sys.platform == "darwin" else ["lscpu"]
    metadata = {
        "started_utc": datetime.now(UTC).isoformat(),
        "git_commit": command_output(["git", "-C", str(ROOT_DIRECTORY), "rev-parse", "HEAD"]),
        "git_status": command_output(["git", "-C", str(ROOT_DIRECTORY), "status", "--short"]),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu": command_output(cpu_command),
        "logical_cpus": os.cpu_count(),
        "jobs": sorted({1, arguments.jobs}),
        "runs": arguments.runs,
        "warmup": arguments.warmup,
        "corpus_selection": arguments.corpus,
        "corpora": [str(corpus) for corpus in corpora],
        "run_order": "C++ then Rust for each measured pair, warmup once per command before its first measurement",
        "files": manifest,
        "excluded": excluded,
        "profile": "release",
        "cpp_build_type_attestation": os.environ.get("AUDIOBATCH_CPP_BUILD_TYPE"),
        "cli": {
            name: {
                "path": str(binary),
                "version": command_output([str(binary), "--version"]),
                "sha256": fingerprint(binary),
            }
            for name, binary in (("cpp", arguments.cpp), ("rust", arguments.rust))
        },
        "tools": {
            name: command_output([name, "--version"]) for name in ("hyperfine", "uv", "rustc", "cargo", "clang++")
        },
        "build_environment": {
            name: os.environ.get(name) for name in ("RUSTFLAGS", "CARGO_BUILD_RUSTFLAGS", "CXXFLAGS", "CC", "CXX")
        },
        "cargo_config": (RUST_DIRECTORY / ".cargo/config.toml").read_text(encoding="utf-8"),
        "workflow": "--refresh --recurse --jobs N --sort path --output json, normalization also --normalize --delete",
        "normalization_targets": {"peak_dbfs": 0, "integrated_lufs": -14},
        "filesystem_cache": "warm, no OS cache flushing, preparation reads and copies the source audio",
        "preparation": "fresh source copies and isolated home/config/cache for every invocation, outside timing",
        "config_isolation": "HOME, CFFIXED_USER_HOME and XDG variables, best effort, native APIs may ignore overrides",
        "normalization_timing": (
            "whole CLI workflow, including initial analysis, decoding, writing, disposal and output analysis"
        ),
    }
    write_json(arguments.results / "metadata.json", metadata)
    print(f"Corpora: {', '.join(map(str, corpora))}, {len(manifest)} files, {len(excluded)} known failures excluded")


def prepare(work: Path, implementation: str) -> None:
    """Resets only the selected implementation's private directory before each measured invocation."""
    workspace(work)
    destination = work / implementation
    if destination.is_symlink():
        raise ValueError(f"Refusing to remove a symlink: {destination}")
    if destination.exists():
        shutil.rmtree(destination)
    home = destination / "home"
    for directory in ("config", "data", "cache", "Library/Application Support", "Library/Logs"):
        (home / directory).mkdir(parents=True, exist_ok=True)
    shutil.copytree(work / "seed", destination / "input", copy_function=shutil.copy2)


def result_path(results: Path, mode: str, jobs: int, implementation: str) -> Path:
    """Returns the CLI JSON captured for a single benchmark command."""
    return results / f"{mode}-jobs-{jobs}-{implementation}.json"


def check(work: Path, results: Path, implementation: str, mode: str, jobs: int) -> None:
    """Checks each warmup and measured invocation independently, outside its timing window."""
    workspace(work)
    folder = work / implementation / "input"
    rows = levels.load(result_path(results, mode, jobs, implementation), folder)
    expected = json.loads((work / f"{mode}-expectations.json").read_text(encoding="utf-8"))
    target = TARGETS[mode]
    differences = levels.compare_results(rows, rows, expected, 0.05, (target.get("peak"), target.get("lufs")))
    if mode != "analyze":
        snapshot = json.loads((work / f"{mode}-before.json").read_text(encoding="utf-8"))
        differences += outputs.check_preservation(folder, snapshot)
    if differences:
        raise ValueError("Benchmark correctness failed:\n" + "\n".join(differences))


def compare(work: Path, results: Path, mode: str, jobs: int) -> None:
    """Runs the existing cross-implementation level and normalized-file oracles after each benchmark pair."""
    workspace(work)
    command = [
        "uv",
        "run",
        "--quiet",
        "--script",
        str(RUST_DIRECTORY / "scripts/compare_levels.py"),
        str(result_path(results, mode, jobs, "cpp")),
        str(result_path(results, mode, jobs, "rust")),
        "--expectations",
        str(work / f"{mode}-expectations.json"),
        "--folders",
        str(work / "cpp/input"),
        str(work / "rust/input"),
    ]
    if mode != "analyze":
        command += [f"--{mode}", str(0 if mode == "peak" else -14)]
    subprocess.run(command, check=True)
    if mode != "analyze":
        subprocess.run(
            [
                "uv",
                "run",
                "--quiet",
                "--script",
                str(RUST_DIRECTORY / "scripts/compare_outputs.py"),
                str(work / "cpp/input"),
                str(work / "rust/input"),
                "--mode",
                mode,
                "--lossy",
                str(work / "lossy.txt"),
                "--snapshots",
                str(work / f"{mode}-before.json"),
                str(work / f"{mode}-before.json"),
            ],
            check=True,
        )


def wall_mean(result: dict) -> float:
    """Reads wall seconds from hyperfine 1.x or its metric-oriented 2.x JSON export."""
    if "mean" in result:
        return result["mean"]
    return result["summary"]["time_wall_clock"]["mean"]


def measurements(result: dict) -> list[dict]:
    """Reads individual metrics from hyperfine 2.x, or wall times and exit codes from 1.x."""
    if "measurements" in result:
        return result["measurements"]
    times = result["times"]
    return [
        {"time_wall_clock": {"value": seconds, "unit": "second"}, "exit_code": exit_code}
        for seconds, exit_code in zip(times, result["exit_codes"], strict=True)
    ]


def metric_summary(values: list[float], unit: str) -> dict:
    """Recomputes sample statistics from individual runs instead of averaging one-run summaries."""
    return {
        "unit": unit,
        "count": len(values),
        "mean": statistics.mean(values),
        "stddev": statistics.stdev(values) if len(values) > 1 else None,
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
    }


def combine(work: Path, results: Path, mode: str, jobs: int) -> None:
    """Combines alternating one-run exports, retaining raw metrics and recomputing all available statistics."""
    workspace(work)
    metadata = json.loads((results / "metadata.json").read_text(encoding="utf-8"))
    commands = [f"{implementation} {mode} jobs={jobs}" for implementation in ("cpp", "rust")]
    collected = {command: [] for command in commands}
    raw_exports = []
    for run in range(1, metadata["runs"] + 1):
        path = Path("raw") / f"{mode}-jobs-{jobs}-run-{run}.json"
        exported = json.loads((results / path).read_text(encoding="utf-8"))
        if [row.get("name", row["command"]) for row in exported["results"]] != commands:
            raise ValueError(f"Unexpected benchmark command order in {path}")
        raw_exports.append(str(path))
        for row in exported["results"]:
            samples = measurements(row)
            if len(samples) != 1 or samples[0]["exit_code"] != 0:
                raise ValueError(f"Expected one successful measurement per command in {path}")
            collected[row.get("name", row["command"])].extend(samples)
    combined = []
    markdown = [
        "| Command | Mean [s] | Stddev [s] | Median [s] | Min [s] | Max [s] | Runs |",
        "|:---|---:|---:|---:|---:|---:|---:|",
    ]
    for command, samples in collected.items():
        metrics = samples[0].keys() - {"exit_code"}
        if "time_wall_clock" not in metrics or any(sample.keys() - {"exit_code"} != metrics for sample in samples):
            raise ValueError(f"Inconsistent or missing hyperfine metrics for {command}")
        summary = {}
        for metric in sorted(metrics):
            values = [sample[metric]["value"] for sample in samples]
            unit = samples[0][metric].get("unit", "count")
            if any(
                not math.isfinite(value) or value < 0 or (metric == "time_wall_clock" and value == 0)
                for value in values
            ) or any(sample[metric].get("unit", "count") != unit for sample in samples):
                raise ValueError(f"Invalid hyperfine {metric} measurements for {command}")
            if metric == "time_wall_clock" and unit != "second":
                raise ValueError("Hyperfine wall times must use seconds")
            summary[metric] = metric_summary(values, unit)
        combined.append({"command": command, "measurements": samples, "summary": summary})
        wall = summary["time_wall_clock"]
        deviation = f"{wall['stddev']:.6f}" if wall["stddev"] is not None else "n/a"
        markdown.append(
            f"| `{command}` | {wall['mean']:.6f} | {deviation} | {wall['median']:.6f} | "
            f"{wall['min']:.6f} | {wall['max']:.6f} | {wall['count']} |"
        )
    write_json(
        results / f"{mode}-jobs-{jobs}.json",
        {"format": "audiobatch-cli-benchmark-v1", "raw_exports": raw_exports, "results": combined},
    )
    (results / f"{mode}-jobs-{jobs}.md").write_text("\n".join(markdown) + "\n", encoding="utf-8")


def report(work: Path, results: Path) -> None:
    """Combines timing exports with metadata and informational ratios without performance thresholds."""
    workspace(work)
    metadata = json.loads((results / "metadata.json").read_text(encoding="utf-8"))
    summaries = []
    markdown = [
        "# CLI benchmark",
        "",
        "Timing is informational and has no pass/fail threshold. Correctness checks passed.",
        "",
        f"Corpora: {', '.join(f'`{corpus}`' for corpus in metadata['corpora'])}, "
        f"{len(metadata['files'])} files, {metadata['runs']} measured runs per command.",
        "Measured runs alternate C++ and Rust. Each command warms up only before its first measured run.",
        f"CPU: {metadata['cpu']}",
        "",
        "Fresh temporary copies and configuration are prepared outside timing. Filesystem cache is warm.",
        (
            "Normalization measures the whole CLI workflow, "
            "including initial and output analysis, writing, and permanent disposal."
        ),
        "Config/cache isolation uses HOME, CFFIXED_USER_HOME and XDG variables on a best-effort basis.",
        "",
    ]
    for jobs in metadata["jobs"]:
        for mode in TARGETS:
            exported = json.loads((results / f"{mode}-jobs-{jobs}.json").read_text(encoding="utf-8"))
            cpp, rust = (wall_mean(row) for row in exported["results"])
            if not all(math.isfinite(value) and value > 0 for value in (cpp, rust)):
                raise ValueError("Invalid or missing hyperfine wall time")
            summary = {
                "mode": mode,
                "jobs": jobs,
                "cpp_seconds": cpp,
                "rust_seconds": rust,
                "rust_over_cpp": rust / cpp,
            }
            summaries.append(summary)
            markdown.append(f"## {mode}, jobs={jobs}")
            markdown.append(
                f"C++ {cpp:.3f} s, Rust {rust:.3f} s. Rust/C++ {rust / cpp:.3f}x, C++/Rust {cpp / rust:.3f}x."
            )
            markdown.append("")
            markdown.append((results / f"{mode}-jobs-{jobs}.md").read_text(encoding="utf-8"))
    write_json(results / "summary.json", {"metadata": metadata, "benchmarks": summaries})
    (results / "summary.md").write_text("\n".join(markdown) + "\n", encoding="utf-8")


def main() -> None:
    """Dispatches internal benchmark preparation and validation operations."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    initialization = commands.add_parser("init")
    initialization.add_argument("work", type=Path)
    initialization.add_argument("results", type=Path)
    initialization.add_argument("--cpp", type=Path, required=True)
    initialization.add_argument("--rust", type=Path, required=True)
    initialization.add_argument("--jobs", type=int, required=True)
    initialization.add_argument("--runs", type=int, required=True)
    initialization.add_argument("--warmup", type=int, required=True)
    initialization.add_argument("--corpus", default="auto")
    for name in ("prepare", "check", "compare", "combine", "report"):
        command = commands.add_parser(name)
        command.add_argument("work", type=Path)
        if name != "prepare":
            command.add_argument("results", type=Path)
        if name in {"prepare", "check"}:
            command.add_argument("implementation", choices=("cpp", "rust"))
        if name in {"check", "compare", "combine"}:
            command.add_argument("mode", choices=TARGETS)
            command.add_argument("jobs", type=int)
    arguments = parser.parse_args()
    try:
        if arguments.command == "init":
            initialize(arguments)
        elif arguments.command == "prepare":
            prepare(arguments.work, arguments.implementation)
        elif arguments.command == "check":
            check(arguments.work, arguments.results, arguments.implementation, arguments.mode, arguments.jobs)
        elif arguments.command == "compare":
            compare(arguments.work, arguments.results, arguments.mode, arguments.jobs)
        elif arguments.command == "combine":
            combine(arguments.work, arguments.results, arguments.mode, arguments.jobs)
        else:
            report(arguments.work, arguments.results)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"CLI benchmark: {error}\n")


if __name__ == "__main__":
    main()
