# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Validate benchmark evidence and record reproducibility metadata outside measured native callbacks."""

import argparse
import hashlib
import json
import math
import os
import platform
import re
import subprocess
from datetime import UTC, datetime
from pathlib import Path
from typing import NoReturn

LATENCY_BY_RATE = {44_100: 4352, 48_000: 4352, 96_000: 8448, 192_000: 16_640}
MAXIMUM_BLOCK = 4096
PREPARATION_TIMINGS = ("module_load_seconds", "preparation_seconds")


def fail(message: str) -> NoReturn:
    """Reject invalid benchmark evidence with an actionable command-line diagnostic."""
    raise ValueError(message)


def checked_bundle(root: Path, bundle: Path, mode: str) -> None:
    """
    Authorize canonical repository or explicit Cargo target descendants, never installed-plugin discovery.

    Only the selected target's ordinary bundle may be absent before building.
    Relative Cargo targets follow the invocation directory, matching the shared shell and native-host policy.
    """
    if not bundle.is_absolute():
        fail("bundle must be absolute")
    root = root.resolve(strict=True)
    resolved = bundle.resolve()
    target = os.environ.get("CARGO_TARGET_DIR")
    directory = Path(target).resolve() if target else root / "rust/target"
    if resolved.suffix != ".vst3" or not (
        resolved.is_relative_to(root) or (target and resolved.is_relative_to(directory))
    ):
        fail("bundle must be inside the repository or the explicit CARGO_TARGET_DIR build root")
    default = (directory / "bundled/ImageControl RS.vst3").resolve()
    if not resolved.is_dir() and (mode != "building" or resolved != default):
        fail("selected build-tree bundle is missing")


def report(path: Path, frames: int, case: str, signal: str = "tones") -> dict:
    """Check independent geometry and signal sanity, not a checksum recorded from the implementation."""
    value = json.loads(path.read_text(encoding="utf-8"))
    if signal not in ("tones", "moving-noise") or value.get("signal", "tones") != signal:
        fail(f"{path.name}: signal does not match independently requested {signal}")
    channels = 1 if case == "mono-input" else 2
    expected = {
        "schema": 1,
        "plugin": "ImageControl RS",
        "case": case,
        "frames": frames,
        "channels": channels,
        "finite_samples": frames * channels,
        "mode": "realtime-warm-callbacks",
    }
    for name, wanted in expected.items():
        if value.get(name) != wanted:
            fail(f"{path.name}: {name} does not match independently requested {wanted}")
    timings = PREPARATION_TIMINGS if any(name in value for name in PREPARATION_TIMINGS) else ()
    for name in ("energy", "peak", "callback_seconds", *timings):
        number = value.get(name)
        if not isinstance(number, int | float) or not math.isfinite(number) or number <= 0:
            fail(f"{path.name}: {name} must be finite and positive")
    if not isinstance(value.get("callbacks"), int) or not 0 < value["callbacks"] <= frames:
        fail("invalid callback count")
    rate = value.get("sample_rate")
    if rate not in LATENCY_BY_RATE:
        fail("unexpected sample rate")
    if value.get("latency_samples") != LATENCY_BY_RATE[rate]:
        fail("reported latency differs from independent rate geometry")
    blocks = value.get("blocks")
    if (
        not isinstance(blocks, list)
        or not blocks
        or not all(isinstance(block, int) and 1 <= block <= MAXIMUM_BLOCK for block in blocks)
    ):
        fail("invalid host block schedule")
    if not re.fullmatch(r"[0-9a-f]{16}", value.get("checksum", "")):
        fail("invalid output bit checksum")
    return value


def compare(first: Path, second: Path, frames: int, case: str, signal: str = "tones") -> None:
    """Require deterministic non-timing evidence across fresh isolated process invocations."""
    left = report(first, frames, case, signal)
    right = report(second, frames, case, signal)
    for key in left.keys() | right.keys():
        if key not in ("callback_seconds", *PREPARATION_TIMINGS) and left.get(key) != right.get(key):
            fail(f"nondeterministic benchmark evidence: {key}")


def command(arguments: list[str], root: Path) -> dict:
    """Capture a bounded metadata command verbatim without shell evaluation or application initialization."""
    result = subprocess.run(  # noqa: S603
        arguments, cwd=root, capture_output=True, text=True, check=False, timeout=20
    )
    return {"command": arguments, "status": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def identity(path: Path) -> dict:
    """Fingerprint exact executable bytes, not a plugin version string that can survive a rebuild."""
    with path.open("rb") as source:
        checksum = hashlib.file_digest(source, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": checksum}


def metadata(options: argparse.Namespace) -> None:
    """Save machine, compiler, source, actual binary identities, and explicit measurement boundaries."""
    root = options.root.resolve(strict=True)
    bundle = options.bundle.resolve(strict=True)
    files = sorted(path for path in (bundle / "Contents").rglob("*") if path.is_file())
    if not files:
        fail("bundle has no Contents files")
    value = {
        "schema": 1,
        "recorded_at": datetime.now(UTC).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "runs": options.runs,
        "warmup": options.warmup,
        "built_this_run": options.built == "true" and options.custom_bundle == "false",
        "host_built_this_run": options.built == "true",
        "bundle_built_this_run": options.built == "true" and options.custom_bundle == "false",
        "custom_bundle_selected": options.custom_bundle == "true",
        "requested_profile": "release",
        "profile_note": "Custom/--no-build bytes cannot establish their build profile, but retain exact hashes.",
        "wall_time_scope": "process startup, module load, prepare, warmup, copies, callbacks, checks, teardown",
        "callback_time_scope": "sum of warmed vst3-host process calls only, with per-call timer overhead",
        "environment": {
            key: os.environ[key]
            for key in (
                "RUSTFLAGS",
                "CARGO_ENCODED_RUSTFLAGS",
                "CARGO_TARGET_DIR",
                "CARGO_BUILD_TARGET",
                "CARGO_PROFILE_RELEASE_LTO",
                "CARGO_PROFILE_RELEASE_CODEGEN_UNITS",
                "MACOSX_DEPLOYMENT_TARGET",
            )
            if key in os.environ
        },
        "host": identity(options.host.resolve(strict=True)),
        "bundle": [identity(path) for path in files],
        "configuration": [
            identity(root / relative)
            for relative in (
                "Cargo.toml",
                "Cargo.lock",
                ".cargo/config.toml",
                "image-control-plugin/rust/crates/dsp/Cargo.toml",
                "image-control-plugin/rust/crates/plugin/Cargo.toml",
                "image-control-plugin/rust/benchmarks/workload.rs",
                "image-control-plugin/rust/benchmarks/artifacts.py",
                "image-control-plugin/rust/benchmark.sh",
                "image-control-plugin/rust/common.sh",
                "image-control-plugin/rust/crates/plugin/examples/support/artifacts.rs",
                "image-control-plugin/rust/crates/dsp/benches/dsp.rs",
                "image-control-plugin/rust/crates/plugin/examples/processing_benchmark.rs",
            )
            if (root / relative).is_file()
        ],
        "commands": [
            command(arguments, root)
            for arguments in (
                ["rustc", "-Vv"],
                ["cargo", "-V"],
                ["hyperfine", "--version"],
                ["git", "rev-parse", "HEAD"],
                ["git", "status", "--short"],
                ["git", "diff", "--", "image-control-plugin/rust", "Cargo.toml", "Cargo.lock", ".cargo/config.toml"],
            )
        ],
    }
    if platform.system() == "Darwin":
        value["commands"].append(command(["sysctl", "-n", "machdep.cpu.brand_string"], root))
    (options.directory / "metadata.json").write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    """Keep the helper's positional protocol small and independently runnable through uv."""
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate-bundle", help="validate an explicit local build-tree bundle")
    validate.add_argument("root", type=Path)
    validate.add_argument("bundle", type=Path)
    validate.add_argument("mode", choices=("building", "existing"))
    check = subparsers.add_parser("check", help="check independent counts and deterministic signal evidence")
    check.add_argument("first", type=Path)
    check.add_argument("second", type=Path)
    check.add_argument("frames", type=int)
    check.add_argument("case")
    check.add_argument("signal", nargs="?", choices=("tones", "moving-noise"), default="tones")
    record = subparsers.add_parser("metadata", help="record exact machine/build provenance")
    for name in ("root", "directory", "host", "bundle"):
        record.add_argument(name, type=Path)
    record.add_argument("runs", type=int)
    record.add_argument("warmup", type=int)
    record.add_argument("built", choices=("true", "false"))
    record.add_argument("custom_bundle", choices=("true", "false"), nargs="?", default="false")
    arguments = parser.parse_args()
    if arguments.command == "validate-bundle":
        checked_bundle(arguments.root, arguments.bundle, arguments.mode)
    elif arguments.command == "check":
        compare(arguments.first, arguments.second, arguments.frames, arguments.case, arguments.signal)
    else:
        metadata(arguments)


if __name__ == "__main__":
    main()
