# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Test shared binary decoding, strict native evidence, paired scheduling, and subprocess failure retention."""

# The contract suite stays dependency-free and runs directly with standard-library unittest.
# ruff: noqa: PT027

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import io
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import render_benchmark as benchmark
import render_compare as comparator
import render_workload as workload

STUB = '''"""Emit native-shaped evidence without a GPU, builds, or installed plugins."""
import argparse
import json
import pathlib
import struct
import sys

parser = argparse.ArgumentParser()
for name in ("input", "width", "height", "scale", "mode", "frames", "warmup", "report"):
    parser.add_argument("--" + name, required=True)
options = parser.parse_args()
source = pathlib.Path(options.input).read_bytes()
magic, stored_frames, particles, step, persistence = struct.unpack_from("<8sIIff", source)
assert magic == b"ICRB0001"
identity = 0xcbf29ce484222325
for byte in source:
    identity = ((identity ^ byte) * 0x100000001b3) & 0xffffffffffffffff
width, height, frames = int(options.width), int(options.height), int(options.frames)
implementation = pathlib.Path(options.report).parent.name.rsplit("-", 1)[-1]
report = {
    "schema_version": 1, "implementation": implementation,
    "render_scope": "offscreen-production-renderer", "input_fnv1a64": f"{identity:016x}",
    "width": width, "height": height, "scale": float(options.scale), "mode": options.mode,
    "frames": frames, "warmup": int(options.warmup), "particles_per_frame": particles,
    "step_seconds": step, "persistence_seconds": persistence,
    "backend": "stub", "gpu": "test GPU", "vendor": "test vendor", "driver": "test driver",
    "compiler": "test compiler", "build_type": "Release", "native_cpu_enabled": True,
    "accumulation_format": "rgba16float", "output_format": "rgba8unorm",
    "targets": [{"name": "output", "width": width, "height": height, "format": "rgba8unorm"}],
    "capabilities": {"gpu_timestamps": False},
    "cpu_frame_ns": [100.0] * frames, "gpu_frame_ns": None, "gpu_batch_ns": 500.0,
    "batch_wall_ns": 1000.0, "batch_frames": 32,
    "timing_method": {"cpu": "production-render-call", "gpu": "synchronized-batch"},
}
if pathlib.Path(options.input).parent.parent.joinpath("invalid-report").exists():
    report["frames"] += 1
pathlib.Path(options.report).write_text(json.dumps(report), encoding="utf-8")
print("stub output")
print("stub diagnostic", file=sys.stderr)
'''


def native_report() -> dict[str, object]:
    """Encode independent native metadata for direct validator tests."""
    return {
        "schema_version": 1,
        "implementation": "cpp",
        "render_scope": "offscreen-production-renderer",
        "input_fnv1a64": "0123456789abcdef",
        "width": 128,
        "height": 64,
        "scale": 2.0,
        "mode": "particles",
        "frames": 2,
        "warmup": 1,
        "particles_per_frame": 4,
        "step_seconds": struct.unpack("<f", struct.pack("<f", 1 / 60))[0],
        "persistence_seconds": 1.0,
        "backend": "OpenGL",
        "gpu": "test GPU",
        "vendor": "test vendor",
        "driver": "test driver",
        "compiler": "test compiler",
        "build_type": "Release",
        "native_cpu_enabled": True,
        "accumulation_format": "rgba16float",
        "output_format": "rgba8unorm",
        "targets": [{"name": "output", "width": 128, "height": 64, "format": "rgba8unorm"}],
        "capabilities": {"gpu_timestamps": True},
        "cpu_frame_ns": [100, 200],
        "gpu_frame_ns": [300, 400],
        "gpu_batch_ns": None,
        "batch_wall_ns": 1000,
        "batch_frames": 32,
        "timing_method": {"cpu": "production-render-call", "gpu": "per-frame-timestamps"},
    }


class WorkloadContracts(unittest.TestCase):
    """Keep the common byte format deterministic and independently decodable."""

    def test_layout_and_integer_seed_determinism(self) -> None:
        """Verify explicit offsets, no padding, exact float32 values, and repeated stored frames."""
        header = workload.WorkloadHeader(2, 2, 1 / 60, 1)
        data = workload.generate_bytes(header, seed=1)
        assert len(data) == 24 + 2 * 2 * 32
        assert data[:8] == b"ICRB0001"
        assert struct.unpack_from("<II", data, 8) == (2, 2)
        assert data[24:88] == data[88:]
        assert data == workload.generate_bytes(header, seed=1)
        assert data != workload.generate_bytes(header, seed=2)
        assert hashlib.sha256(data).hexdigest() == "0083d95b5a061f94b27323a9e3597cb66fc029f8994643fb523bba433586f0e2"
        assert workload.fnv1a64(data) == "a7aac43cf32df9ae"
        first_x = ((1_664_525 + 1_013_904_223) >> 8) / (1 << 24)
        assert struct.unpack_from("<f", data, 24)[0] == first_x
        parsed = workload.decode_bytes(data)
        assert parsed.frame_count == 2
        assert parsed.particles_per_frame == 2
        assert parsed.step_seconds == struct.unpack("<f", struct.pack("<f", 1 / 60))[0]

    def test_zero_particles_and_standard_counts(self) -> None:
        """Empty uploads remain valid workloads instead of being silently removed from the matrix."""
        assert workload.STANDARD_PARTICLE_COUNTS == (0, 4096, 16_384, 65_536)
        for particles in workload.STANDARD_PARTICLE_COUNTS:
            data = workload.generate_bytes(workload.WorkloadHeader(1, particles, 0.0625, 1.0))
            assert workload.decode_bytes(data).particles_per_frame == particles
        assert len(workload.generate_bytes(workload.WorkloadHeader(3, 0, 0.0625, 1.0))) == 24

    def test_manifest_hash_and_fnv_vectors(self) -> None:
        """Self-hashes exclude their own field and input identities cover the whole binary header and payload."""
        assert workload.fnv1a64(b"") == "cbf29ce484222325"
        assert workload.fnv1a64(b"hello") == "a430d84680aabd0b"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input.icrb"
            manifest = workload.write_workload(path, workload.WorkloadHeader(1, 4, 0.0625, 1))
            assert manifest["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
            assert manifest["fnv1a64"] == workload.fnv1a64(path.read_bytes())
            digest = manifest.pop("manifest_sha256")
            assert digest == workload.canonical_hash(manifest)
            assert json.loads(path.with_suffix(".icrb.json").read_text())["manifest_sha256"] == digest

    def test_invalid_binary_fails(self) -> None:
        """Reject trailing data, truncation, unsupported versions, endian errors, and invalid particle channels."""
        valid = workload.generate_bytes(workload.WorkloadHeader(1, 1, 0.0625, 1))
        invalid = [b"", valid[:23], valid[:-1], valid + b"extra", b"ICRB0002" + valid[8:]]
        invalid.append(struct.pack(">8sIIff", b"ICRB0001", 1, 1, 0.0625, 1) + valid[24:])
        for value in (math.nan, math.inf, -1.0, 1.5):
            data = bytearray(valid)
            struct.pack_into("<f", data, 24, value)
            invalid.append(bytes(data))
        data = bytearray(valid)
        struct.pack_into("<f", data, 24 + 6 * 4, 0.0)
        invalid.append(bytes(data))
        for data in invalid:
            with self.subTest(data=data[:24]), self.assertRaises(ValueError):
                workload.decode_bytes(data)

    def test_invalid_parameters_fail_before_allocation(self) -> None:
        """Bound counts, seeds, simulated times, and total storage before generating records."""
        for header in (
            workload.WorkloadHeader(0, 1, 0.1, 1),
            workload.WorkloadHeader(1, -1, 0.1, 1),
            workload.WorkloadHeader(1, 65_537, 0.1, 1),
            workload.WorkloadHeader(1, 1, math.nan, 1),
            workload.WorkloadHeader(1, 1, 0, 1),
            workload.WorkloadHeader(1, 1, 0.5, 1),
            workload.WorkloadHeader(1, 1, 0.1, 4),
            workload.WorkloadHeader(240, 65_536, 0.1, 1),
        ):
            with self.subTest(header=header), self.assertRaises(ValueError):
                workload.generate_bytes(header)
        with self.assertRaises(ValueError):
            workload.generate_bytes(workload.WorkloadHeader(1, 1, 0.1, 1), seed=-1)


class NativeReportContracts(unittest.TestCase):
    """Validate complete native reports without permitting missing timings to look fast."""

    def validate(self, report: dict[str, object]) -> dict[str, object]:
        """Use an expected case and independently specified workload identity."""
        manifest = {
            "fnv1a64": "0123456789abcdef",
            "header": {"step_seconds": struct.unpack("<f", struct.pack("<f", 1 / 60))[0], "persistence_seconds": 1.0},
        }
        return benchmark.validate_report(
            report,
            "cpp",
            benchmark.RenderCase(128, 64, 2.0, "particles", 4),
            manifest,
            argparse.Namespace(frames=2, warmup=1, input_frames=1),
        )

    def test_complete_timestamp_report(self) -> None:
        """Accept valid finite per-frame observations and actual physical targets."""
        report = native_report()
        assert self.validate(report) == report

    def test_batch_and_unsupported_gpu_reports(self) -> None:
        """Accept explicitly bounded GPU timing capabilities, never substitute zero arrays."""
        report = native_report()
        report["capabilities"]["gpu_timestamps"] = False
        report["timing_method"]["gpu"] = "synchronized-batch"
        report["gpu_frame_ns"] = None
        report["gpu_batch_ns"] = 1000
        self.validate(report)
        report["timing_method"]["gpu"] = "unsupported"
        report["gpu_batch_ns"] = None
        self.validate(report)
        report["gpu_frame_ns"] = [0, 0]
        with self.assertRaises(ValueError):
            self.validate(report)

    def test_invalid_report_fields_fail(self) -> None:
        """Reject unsupported schemas, bool counters, mismatched cases, nonfinite samples, and missing provenance."""
        changes = {
            "schema_version": True,
            "implementation": "rust",
            "render_scope": "presented",
            "input_fnv1a64": "bad",
            "frames": 3,
            "warmup": True,
            "particles_per_frame": 3,
            "width": 64,
            "height": 128,
            "scale": 1.0,
            "step_seconds": 0.25,
            "mode": "solid",
            "persistence_seconds": 2.0,
            "cpu_frame_ns": [1],
            "gpu_frame_ns": [1, math.nan],
            "gpu_batch_ns": 10,
            "batch_wall_ns": math.inf,
            "compiler": "",
            "build_type": "Debug",
            "native_cpu_enabled": False,
            "opt_level": "0",
            "gpu": "",
            "targets": [],
            "capabilities": {"gpu_timestamps": False},
            "driver": None,
        }
        for field, value in changes.items():
            report = native_report()
            report[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(report)
        report = native_report()
        report["unknown"] = 1
        with self.assertRaises(ValueError):
            self.validate(report)
        del report["unknown"]
        del report["driver"]
        with self.assertRaises(ValueError):
            self.validate(report)

    def test_actual_target_geometry_and_format_fail(self) -> None:
        """Requested dimensions alone cannot hide a smaller actual output or wrong texture format."""
        for field, value in (("width", 64), ("height", 32), ("width", 4097), ("format", "wrong"), ("name", "other")):
            report = native_report()
            report["targets"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.validate(report)
        report = native_report()
        report["targets"].append(copy.deepcopy(report["targets"][0]))
        with self.assertRaises(ValueError):
            self.validate(report)

    def test_strict_json_rejects_duplicate_keys_and_constants(self) -> None:
        """Retain strict JSON rather than silently taking the last identity or a NaN timing."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "report.json"
            for contents in ('{"frames":1,"frames":2}', '{"x":NaN}', '{"x":Infinity}'):
                path.write_text(contents)
                with self.subTest(contents=contents), self.assertRaises(ValueError):
                    benchmark.load_report(path)

    def test_comparisons_require_identical_cases_and_boundaries(self) -> None:
        """Never compare unlike geometry/workloads or CPU timing boundaries, even when durations happen to match."""
        cpp = native_report()
        rust = copy.deepcopy(cpp)
        rust["implementation"] = "rust"
        rust["cpu_frame_ns"] = [200, 400]
        assert benchmark.compare_pair(cpp, rust)["cpu_rust_over_cpp"] == 2
        for field in benchmark.IDENTITY_FIELDS:
            changed = copy.deepcopy(rust)
            changed[field] = 2.5 if field in {"scale", "step_seconds", "persistence_seconds"} else "different"
            with self.subTest(field=field), self.assertRaises(ValueError):
                benchmark.compare_pair(cpp, changed)
        rust["timing_method"]["cpu"] = "different"
        comparison = benchmark.compare_pair(cpp, rust)
        assert comparison["cpu_rust_over_cpp"] is None
        assert "timing_method" in comparison["configuration_differences"]

    def test_float32_json_spellings_pair_by_bits(self) -> None:
        """A native serializer's shortest decimal spelling identifies the same stored float, not another case."""
        cpp, rust = native_report(), native_report()
        rust["step_seconds"] = 0.016666668
        assert struct.pack("<f", cpp["step_seconds"]) == struct.pack("<f", rust["step_seconds"])
        assert benchmark.compare_pair(cpp, rust)["cpu_rust_over_cpp"] == 1.0

    def test_gpu_incompatible_methods_are_not_ratioed(self) -> None:
        """A synchronized batch duration and timestamp sum have different boundaries."""
        cpp, rust = native_report(), native_report()
        rust["timing_method"]["gpu"] = "synchronized-batch"
        rust["gpu_frame_ns"] = None
        rust["gpu_batch_ns"] = 1000
        assert benchmark.compare_pair(cpp, rust)["gpu_rust_over_cpp"] is None

    def test_batch_frames_is_required_bounded_and_compared(self) -> None:
        """Missing or invalid synchronization policy is not valid new native evidence."""
        for value in (0, 33, True, 1.5):
            report = native_report()
            report["batch_frames"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.validate(report)
        report = native_report()
        del report["batch_frames"]
        with self.assertRaises(ValueError):
            self.validate(report)
        cpp, rust = native_report(), native_report()
        cpp["batch_frames"] = 16
        comparison = benchmark.compare_pair(cpp, rust)
        assert comparison["configuration_differences"]["batch_frames"] == {"cpp": 16, "rust": 32}
        assert comparison["cpu_rust_over_cpp"] == 1

    def test_native_diagnostic_extensions_preserve_strict_accounting(self) -> None:
        """Permit declared detailed native evidence, but reject contradictory completion counts and index arrays."""
        report = native_report()
        report.update(
            {
                "completed_frames": 2,
                "encoded_frames": 2,
                "requested_frames": 2,
                "warmup_frames": 1,
                "physical_width": 128,
                "physical_height": 64,
                "logical_width": 64.0,
                "logical_height": 32.0,
                "input_frames": 1,
                "gpu_frame_indices": [0, 1],
                "gpu_timing_boundary": "production passes",
            }
        )
        self.validate(report)
        for field, value in (
            ("completed_frames", 1),
            ("physical_width", 64),
            ("logical_height", 64),
            ("gpu_frame_indices", [1, 0]),
            ("gpu_frame_indices", [0, 0]),
        ):
            changed = copy.deepcopy(report)
            changed[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.validate(changed)

    def test_json_exponent_overflow_is_rejected(self) -> None:
        """A syntactically standard exponent must not smuggle Infinity into diagnostic or caller evidence."""
        with self.assertRaises(argparse.ArgumentTypeError):
            benchmark.metadata_argument('{"flags": {"overflow": 1e999}}')
        report = native_report()
        report["completed_wall_ms"] = math.inf
        with self.assertRaises(ValueError):
            self.validate(report)

    def test_summary_rejects_duplicate_or_missing_pairs(self) -> None:
        """One report cannot count as multiple independent runs or conceal a missing implementation."""
        record = {"case": "case", "run": 0, "implementation": "cpp", "report": native_report()}
        with self.assertRaises(ValueError):
            benchmark.summarize_runs([record])
        with self.assertRaises(ValueError):
            benchmark.summarize_runs([record, record])


class RunnerContracts(unittest.TestCase):
    """Execute tiny subprocess stubs in isolated directories, with no GPU or native builds."""

    def setUp(self) -> None:
        """Create isolated artifact destinations and one portable Python child script."""
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-render-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.stub = self.root / "stub.py"
        self.stub.write_text(STUB, encoding="utf-8")
        self.options = benchmark.parser().parse_args(
            [
                "--cpp",
                sys.executable,
                "--rust",
                sys.executable,
                "--artifacts",
                str(self.root / "artifacts"),
                "--runs",
                "2",
                "--frames",
                "2",
                "--warmup",
                "1",
                "--particles",
                "4",
                "--mode",
                "particles",
                "--size",
                "128x64",
                "--scale",
                "2",
            ]
        )

    def test_matrix_executes_alternating_pairs_and_retains_every_report(self) -> None:
        """Run real child processes, verify argument geometry, hash identity, raw streams, and pair summaries."""
        cases = benchmark.validate_options(self.options)
        self.options.artifacts.mkdir()
        original = benchmark.run_native

        def run_stub(command: list[str], arguments: list[str], directory: Path, timeout: float) -> float:
            """Keep orchestration real while replacing only the platform-native executable."""
            assert command == [str(Path(sys.executable).resolve())]
            return original([sys.executable, str(self.stub)], arguments, directory, timeout)

        with patch.object(benchmark, "run_native", side_effect=run_stub):
            records = benchmark.run_matrix(self.options, cases)
        assert [record["implementation"] for record in records] == ["cpp", "rust", "rust", "cpp"]
        assert [record["sequence"] for record in records] == [0, 1, 2, 3]
        assert len({record["native_report"] for record in records}) == 4
        for record in records:
            directory = Path(record["native_report"]).parent
            assert (directory / "stdout.txt").read_text() == "stub output\n"
            assert (directory / "stderr.txt").read_text() == "stub diagnostic\n"
            assert record["report"]["width"] == 128
            assert record["report"]["scale"] == 2
        summaries = benchmark.summarize_runs(records)
        assert summaries[0]["paired_run_ratios"] == [
            {"cpu_rust_over_cpp": 1.0, "gpu_rust_over_cpp": 1.0, "configuration_differences": {}},
            {"cpu_rust_over_cpp": 1.0, "gpu_rust_over_cpp": 1.0, "configuration_differences": {}},
        ]
        assert summaries[0]["implementations"]["cpp"]["process_runs"] == 2
        assert len(json.loads((self.options.artifacts / "raw.json").read_text())) == 4

    def test_subprocess_failure_retains_raw_diagnostics(self) -> None:
        """Never count a failed child as a valid timing report."""
        directory = self.root / "failed"
        with self.assertRaisesRegex(ValueError, "exited 7"):
            benchmark.run_native([sys.executable, "-c", "import sys; print('failed'); sys.exit(7)"], [], directory, 10)
        assert (directory / "stdout.txt").read_text() == "failed\n"
        assert (directory / "command.json").is_file()

    def test_supplied_workload_preserves_bytes_and_provenance(self) -> None:
        """Retain changing shared frame bytes and their supplied generator identity in every paired record."""
        source = self.root / "audio.icrb"
        header = workload.WorkloadHeader(2, 4, 1 / 30, 1.0)
        manifest = workload.write_workload(source, header)
        data = bytearray(source.read_bytes())
        data[-4:] = struct.pack("<f", 0.25)
        source.write_bytes(data)
        manifest["generator"] = "production-audio-test"
        manifest["sha256"] = hashlib.sha256(data).hexdigest()
        manifest["fnv1a64"] = workload.fnv1a64(data)
        del manifest["manifest_sha256"]
        manifest["manifest_sha256"] = workload.canonical_hash(manifest)
        workload.write_json(source.with_suffix(".icrb.json"), manifest)
        self.options.input = source
        self.options.particles = None
        cases = benchmark.validate_options(self.options)
        self.options.artifacts.mkdir()
        original = benchmark.run_native

        def run_stub(command: list[str], arguments: list[str], directory: Path, timeout: float) -> float:
            """Replace only native execution while retaining supplied-input scheduling and report validation."""
            return original([sys.executable, str(self.stub)], arguments, directory, timeout)

        with patch.object(benchmark, "run_native", side_effect=run_stub):
            records = benchmark.run_matrix(self.options, cases)
        assert len(records) == 4
        assert all(record["workload_manifest"] == manifest for record in records)
        assert (self.options.artifacts / "inputs/particles-4.icrb").read_bytes() == data

    def test_supplied_workload_rejects_bad_identity_before_artifacts(self) -> None:
        """Reject malformed provenance and ambiguous count overrides before creating a matrix directory."""
        source = self.root / "invalid.icrb"
        workload.write_workload(source, workload.WorkloadHeader(2, 4, 1 / 30, 1.0))
        self.options.input = source
        with self.assertRaisesRegex(ValueError, "omit --particles"):
            benchmark.validate_options(self.options)
        self.options.particles = None
        source.write_bytes(source.read_bytes()[:-1] + b"\x00")
        with self.assertRaises(ValueError):
            benchmark.validate_options(self.options)
        assert not self.options.artifacts.exists()

    def test_timeout_retains_raw_artifacts(self) -> None:
        """Bound a stalled child and retain the attempted command and native streams."""
        directory = self.root / "timeout"
        with self.assertRaises(subprocess.TimeoutExpired):
            benchmark.run_native([sys.executable, "-c", "while True: pass"], [], directory, 0.05)
        assert (directory / "command.json").is_file()
        assert (directory / "stderr.txt").is_file()

    def test_defaults_include_all_standard_counts_and_both_modes(self) -> None:
        """The no-build default matrix retains empty uploads and saturated particle workloads in both modes."""
        options = benchmark.parser().parse_args(
            [
                "-C",
                sys.executable,
                "-R",
                sys.executable,
                "-A",
                str(self.options.artifacts),
            ]
        )
        cases = benchmark.validate_options(options)
        assert len(cases) == 8
        assert {case.particles for case in cases} == set(workload.STANDARD_PARTICLE_COUNTS)
        assert {case.mode for case in cases} == {"particles", "solid"}
        assert options.no_build is True
        assert options.frames == 120
        assert options.warmup == 30
        assert options.runs == 3

    def test_each_implementation_captures_without_the_other(self) -> None:
        """Independent native runs retain the existing raw/summary shapes without fabricated paired ratios."""
        original = benchmark.run_native

        def run_stub(_command: list[str], arguments: list[str], directory: Path, timeout: float) -> float:
            """Exercise a complete single-implementation run using a real portable child process."""
            return original([sys.executable, str(self.stub)], arguments, directory, timeout)

        for implementation in ("cpp", "rust"):
            artifacts = self.root / implementation
            with (
                self.subTest(implementation=implementation),
                patch.object(benchmark, "run_native", side_effect=run_stub),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                assert (
                    benchmark.main(
                        [
                            f"--{implementation}",
                            sys.executable,
                            "--artifacts",
                            str(artifacts),
                            "--runs",
                            "2",
                            "--frames",
                            "2",
                            "--particles",
                            "0",
                            "--mode",
                            "solid",
                        ]
                    )
                    == 0
                )
            summary = json.loads((artifacts / "summary.json").read_text())
            raw = json.loads((artifacts / "raw.json").read_text())
            assert len(raw) == 2
            assert {record["implementation"] for record in raw} == {implementation}
            assert set(summary["cases"][0]["implementations"]) == {implementation}
            assert summary["cases"][0]["paired_run_ratios"] == []
            assert set(summary["provenance"]["executables"]) == {implementation}
            assert summary["cases"][0]["implementations"][implementation]["cpu_run_mean_ns"]["p99_ns"] == 100
            assert len(comparator.load_capture(artifacts).records) == 2

    def test_at_least_one_native_executable_is_required(self) -> None:
        """The parser supports either side, but never starts a capture without any explicit implementation."""
        options = benchmark.parser().parse_args(["--artifacts", str(self.root / "empty")])
        with self.assertRaisesRegex(ValueError, "at least one"):
            benchmark.validate_options(options)
        assert not (self.root / "empty").exists()

    def test_percentile_summary_includes_p99_and_maximum(self) -> None:
        """Use a deterministic nearest-rank tail statistic, preserving all existing summary keys."""
        summary = benchmark.sample_summary(list(range(1, 101)))
        assert summary["p95_ns"] == 95
        assert summary["p99_ns"] == 99
        assert summary["max_ns"] == 100
        assert benchmark.sample_summary([3])["p99_ns"] == 3

    def test_scale_uses_one_shared_float32_identity(self) -> None:
        """A custom decimal scale must produce one exact actual scale in both native JSON reports."""
        self.options.scale = [1.1]
        cases = benchmark.validate_options(self.options)
        assert cases[0].scale == struct.unpack("<f", struct.pack("<f", 1.1))[0]

    def test_successful_main_retains_provenance_and_summary(self) -> None:
        """Exercise the complete orchestrator through real stub subprocesses, including executable identities."""
        original = benchmark.run_native

        def run_stub(_command: list[str], arguments: list[str], directory: Path, timeout: float) -> float:
            """Replace only the native process with a portable report-producing child."""
            return original([sys.executable, str(self.stub)], arguments, directory, timeout)

        with patch.object(benchmark, "run_native", side_effect=run_stub), contextlib.redirect_stdout(io.StringIO()):
            assert (
                benchmark.main(
                    [
                        "--cpp",
                        sys.executable,
                        "--rust",
                        sys.executable,
                        "--artifacts",
                        str(self.options.artifacts),
                        "--runs",
                        "2",
                        "--frames",
                        "2",
                        "--particles",
                        "0",
                        "--mode",
                        "solid",
                        "--scale",
                        "1.1",
                    ]
                )
                == 0
            )
        summary = json.loads((self.options.artifacts / "summary.json").read_text())
        assert len(summary["cases"]) == 1
        assert summary["provenance"]["executables"]["cpp"]["sha256"]
        assert summary["provenance"]["cpu"]
        assert summary["provenance"]["os"]
        assert not (self.options.artifacts / "failure.json").exists()

    def test_invalid_options_have_no_side_effects(self) -> None:
        """Reject oversized matrices, bad numeric bounds, and output reuse before launching any process."""
        changes = {
            "runs": 1,
            "frames": 0,
            "warmup": -1,
            "scale": [math.inf],
            "particles": [-1],
            "seed": -1,
            "step": 0.5,
            "persistence": 4,
            "timeout": 0,
            "size": [(128, 64)] * 257,
            "artifacts": Path("relative"),
        }
        for field, value in changes.items():
            options = copy.deepcopy(self.options)
            setattr(options, field, value)
            with self.subTest(field=field), self.assertRaises(ValueError):
                benchmark.validate_options(options)
        assert not self.options.artifacts.exists()
        self.options.artifacts.mkdir()
        (self.options.artifacts / "unrelated").write_text("keep")
        with self.assertRaises(ValueError):
            benchmark.validate_options(self.options)
        assert (self.options.artifacts / "unrelated").read_text() == "keep"

    def test_help_has_no_executable_or_filesystem_requirements(self) -> None:
        """Each standalone entry point prints help successfully without inspecting paths or writing outputs."""
        for script in ("render_benchmark.py", "render_workload.py", "render_compare.py"):
            result = subprocess.run(  # noqa: S603
                [sys.executable, str(Path(__file__).with_name(script)), "--help"],
                cwd=self.root,
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            assert result.returncode == 0
            assert "usage:" in result.stdout
        assert not self.options.artifacts.exists()

    def test_main_retains_failure_json(self) -> None:
        """A malformed native report fails the run, retains the report, and never writes a success summary."""
        original = benchmark.run_native

        def run_bad_stub(_command: list[str], arguments: list[str], directory: Path, timeout: float) -> float:
            """Tell the child to deliberately misreport its measured frame count."""
            (self.options.artifacts / "invalid-report").touch()
            return original([sys.executable, str(self.stub)], arguments, directory, timeout)

        with (
            patch.object(benchmark, "run_native", side_effect=run_bad_stub),
            contextlib.redirect_stderr(io.StringIO()),
            patch.object(benchmark, "provenance", return_value={}),
        ):
            assert (
                benchmark.main(
                    [
                        "--cpp",
                        sys.executable,
                        "--rust",
                        sys.executable,
                        "--artifacts",
                        str(self.options.artifacts),
                        "--runs",
                        "2",
                        "--frames",
                        "2",
                        "--particles",
                        "4",
                        "--mode",
                        "particles",
                    ]
                )
                == 1
            )
        assert (self.options.artifacts / "failure.json").is_file()
        assert not (self.options.artifacts / "summary.json").exists()
        assert len(list(self.options.artifacts.glob("*/native.json"))) == 1


class SavedComparisonContracts(unittest.TestCase):
    """Validate portable saved evidence without executing native runners or mutating source captures."""

    def setUp(self) -> None:
        """Keep each saved capture and comparison output isolated."""
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-saved-render-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def capture(self, name: str, implementation: str = "cpp", factor: float = 1.0) -> Path:
        """Write a complete small version-1 capture with independently retained raw workloads and provenance."""
        directory = self.root / name
        directory.mkdir()
        manifest = workload.write_workload(directory / "input.icrb", workload.WorkloadHeader(1, 4, 1 / 60, 1))
        records = []
        for run in range(2):
            report = native_report()
            report["implementation"] = implementation
            report["input_fnv1a64"] = manifest["fnv1a64"]
            report["cpu_frame_ns"] = [value * factor for value in report["cpu_frame_ns"]]
            report["gpu_frame_ns"] = [value * factor for value in report["gpu_frame_ns"]]
            report["batch_wall_ns"] *= factor
            records.append(
                {
                    "sequence": run,
                    "case": "128x64-s2-particles-p4",
                    "requested": {
                        "width": 128,
                        "height": 64,
                        "scale": 2.0,
                        "mode": "particles",
                        "particles": 4,
                    },
                    "run": run,
                    "implementation": implementation,
                    "process_wall_ns": 2000,
                    "workload_manifest": manifest,
                    "native_report": "/original/machine/native.json",
                    "report": report,
                }
            )
        provenance = {"os": "macOS test", "cpu": "CPU one", "machine": "arm64"}
        workload.write_json(directory / "raw.json", records)
        workload.write_json(directory / "provenance.json", provenance)
        workload.write_json(
            directory / "summary.json",
            {
                "schema_version": 1,
                "provenance": provenance,
                "cases": benchmark.summarize_runs(records, (implementation,)),
            },
        )
        return directory

    def rewrite_raw(self, directory: Path, records: list[dict[str, object]]) -> None:
        """Replace test evidence explicitly while leaving its stale summary available for rejection tests."""
        workload.write_json(directory / "raw.json", records)

    def test_saved_capture_comparison_is_independent_and_directional(self) -> None:
        """Repeated captures compare process means, not fabricated pairs or autocorrelated frame significance."""
        first, second = self.capture("first"), self.capture("second", factor=2)
        result = comparator.compare_captures(
            comparator.load_capture(first), comparator.load_capture(second / "summary.json")
        )
        assert result["comparison_kind"] == "independent-saved-captures"
        assert result["ratio_direction"] == "second-over-first"
        assert result["cases"][0]["second_over_first"] == {"cpu": 2, "gpu": 2, "batch_wall": 2}
        assert result["cases"][0]["hash_scope"] == "sha256-and-fnv1a64"
        assert result["cases"][0]["cpu_run_mean_ns"]["first"]["p99_ns"] == 150
        assert result["host_differences"] == {}
        assert result["missing_host_provenance"] == []
        assert "do not isolate an OS effect" in result["interpretation"]

    def test_legacy_summary_without_p99_remains_readable(self) -> None:
        """Version-1 baseline summaries need no rewrite when the comparator recomputes new tail metrics."""
        directory = self.capture("legacy")
        summary = json.loads((directory / "summary.json").read_text())

        def remove_p99(value: object) -> None:
            """Simulate existing successful summaries containing only p95 and maximum."""
            if isinstance(value, dict):
                value.pop("p99_ns", None)
                for item in value.values():
                    remove_p99(item)
            elif isinstance(value, list):
                for item in value:
                    remove_p99(item)

        remove_p99(summary)
        workload.write_json(directory / "summary.json", summary)
        before = (directory / "summary.json").read_bytes()
        assert len(comparator.load_capture(directory).records) == 2
        assert (directory / "summary.json").read_bytes() == before

    def test_summary_contradictions_and_unsupported_schema_fail(self) -> None:
        """Reject edited summary statistics and unknown schema versions instead of trusting saved aggregates."""
        directory = self.capture("invalid")
        summary = json.loads((directory / "summary.json").read_text())
        summary["cases"][0]["implementations"]["cpp"]["cpu_run_mean_ns"]["mean_ns"] = 1
        workload.write_json(directory / "summary.json", summary)
        with self.assertRaisesRegex(ValueError, "statistic"):
            comparator.load_capture(directory)
        summary["schema_version"] = 2
        workload.write_json(directory / "summary.json", summary)
        with self.assertRaisesRegex(ValueError, "schema"):
            comparator.load_capture(directory)

    def test_missing_raw_evidence_is_not_inferred_from_summary_labels(self) -> None:
        """A summary without workload identity cannot be compared safely, even when visible case names match."""
        directory = self.capture("summary-only")
        with (
            patch.object(Path, "is_file", autospec=True, side_effect=lambda path: path.name != "raw.json"),
            self.assertRaisesRegex(ValueError, "missing"),
        ):
            comparator.load_capture(directory / "summary.json")

    def test_hardware_platform_and_native_differences_are_explicit(self) -> None:
        """Saved comparisons identify confounders and never label a cross-machine difference as an OS effect."""
        first, second = self.capture("first"), self.capture("second", implementation="rust")
        changed = json.loads((second / "raw.json").read_text())
        for record in changed:
            record["report"]["gpu"] = "GPU two"
            record["report"]["backend"] = "Metal"
            record["report"]["compiler"] = "Rust test compiler"
        self.rewrite_raw(second, changed)
        workload.write_json(second / "provenance.json", {"os": "Windows test", "cpu": "CPU two", "machine": "x86_64"})
        result = comparator.compare_captures(
            comparator.load_capture(first / "raw.json"), comparator.load_capture(second / "raw.json")
        )
        assert set(result["host_differences"]) == {"os", "cpu", "machine"}
        assert {"gpu", "backend", "compiler"} <= result["cases"][0]["configuration_differences"].keys()
        assert result["cases"][0]["first_implementation"] == "cpp"
        assert result["cases"][0]["second_implementation"] == "rust"

    def test_mismatched_workloads_cases_and_partial_captures_fail(self) -> None:
        """Matching labels cannot hide different input bytes, warmup counts, or incomplete process matrices."""
        first = self.capture("first")
        first_capture = comparator.load_capture(first)
        for field, value in (("input_fnv1a64", "fedcba9876543210"), ("warmup", 2)):
            report = native_report()
            report[field] = value
            path = self.root / f"{field}.json"
            workload.write_json(path, report)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "identity"):
                comparator.compare_captures(first_capture, comparator.load_capture(path))
        records = json.loads((first / "raw.json").read_text())
        self.rewrite_raw(first, [records[0], records[0]])
        with self.assertRaises(ValueError):
            comparator.load_capture(first / "raw.json")

    def test_manifest_self_hash_is_verified(self) -> None:
        """Saved metadata cannot silently replace a deterministic generator seed or SHA identity."""
        directory = self.capture("manifest")
        records = json.loads((directory / "raw.json").read_text())
        records[0]["workload_manifest"]["seed"] += 1
        self.rewrite_raw(directory, records)
        with self.assertRaisesRegex(ValueError, "self-hash"):
            comparator.load_capture(directory / "raw.json")

    def test_native_reports_compare_with_missing_host_provenance_marked(self) -> None:
        """Standalone reports retain available FNV identity while explicitly lacking manifest SHA and host evidence."""
        first, second = self.root / "one.json", self.root / "two.json"
        workload.write_json(first, native_report())
        report = native_report()
        report["implementation"] = "rust"
        report["timing_method"]["cpu"] = "different CPU scope"
        workload.write_json(second, report)
        result = comparator.compare_captures(comparator.load_capture(first), comparator.load_capture(second))
        assert result["cases"][0]["hash_scope"] == "fnv1a64-only"
        assert result["cases"][0]["second_over_first"]["cpu"] is None
        assert len(result["missing_host_provenance"]) == 6

    def test_raw_native_schema_and_changed_case_set_fail(self) -> None:
        """Unknown native versions and partial geometry matrices never compare under matching filenames."""
        directory = self.capture("native-schema")
        records = json.loads((directory / "raw.json").read_text())
        records[0]["report"]["schema_version"] = 2
        self.rewrite_raw(directory, records)
        with self.assertRaisesRegex(ValueError, "schema"):
            comparator.load_capture(directory / "raw.json")
        first = self.capture("first")
        path = self.root / "other-geometry.json"
        report = native_report()
        report["width"] = 256
        report["targets"][0]["width"] = 256
        workload.write_json(path, report)
        with self.assertRaisesRegex(ValueError, "case sets"):
            comparator.compare_captures(comparator.load_capture(first), comparator.load_capture(path))

    def test_valid_but_different_manifest_sha_is_not_compared(self) -> None:
        """Even matching FNV hashes cannot override conflicting retained SHA-256 input identities."""
        first, second = self.capture("first"), self.capture("second")
        records = json.loads((second / "raw.json").read_text())
        for record in records:
            manifest = record["workload_manifest"]
            manifest["sha256"] = "a" * 64
            manifest["manifest_sha256"] = workload.canonical_hash(
                {key: value for key, value in manifest.items() if key != "manifest_sha256"}
            )
        self.rewrite_raw(second, records)
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            comparator.compare_captures(comparator.load_capture(first), comparator.load_capture(second / "raw.json"))

    def test_gpu_methods_and_failed_capture_are_not_silently_accepted(self) -> None:
        """Incompatible GPU evidence gets no ratio, and a saved failure cannot masquerade as a successful capture."""
        first, second = self.root / "first.json", self.root / "second.json"
        workload.write_json(first, native_report())
        report = native_report()
        report["timing_method"]["gpu"] = "unsupported"
        report["gpu_frame_ns"] = None
        workload.write_json(second, report)
        result = comparator.compare_captures(comparator.load_capture(first), comparator.load_capture(second))
        assert result["cases"][0]["second_over_first"]["gpu"] is None
        assert result["cases"][0]["gpu_run_mean_ns"] is None
        assert result["cases"][0]["per_run"]["first"][0]["cpu_frame_ns"]["p99_ns"] == 200
        directory = self.capture("failed")
        workload.write_json(directory / "failure.json", {"error": "failure"})
        with self.assertRaisesRegex(ValueError, "failed capture"):
            comparator.load_capture(directory)

    def test_batch_mismatch_suppresses_throughput_and_keeps_legacy_recorded_sizes(self) -> None:
        """The historical C++16/Rust32 captures remain readable but never claim matched batch overhead."""
        first, second = self.root / "batch16.json", self.root / "batch32.json"
        report = native_report()
        report["batch_frames"] = 16
        workload.write_json(first, report)
        legacy = native_report()
        del legacy["batch_frames"]
        legacy["implementation"] = "rust"
        legacy["details"] = {"timing": {"completion_batch_frames": 32}}
        workload.write_json(second, legacy)
        result = comparator.compare_captures(comparator.load_capture(first), comparator.load_capture(second))
        case = result["cases"][0]
        assert case["configuration_differences"]["batch_frames"] == {"first": 16, "second": 32}
        assert case["second_over_first"]["batch_wall"] is None
        assert case["second_over_first"]["cpu"] == 1
        contradictory = native_report()
        contradictory["batch_frames"] = 16
        contradictory["details"] = {"timing": {"completion_batch_frames": 32}}
        workload.write_json(second, contradictory)
        with self.assertRaisesRegex(ValueError, "contradicts"):
            comparator.load_capture(second)
        missing = native_report()
        del missing["batch_frames"]
        workload.write_json(second, missing)
        with self.assertRaisesRegex(ValueError, "batch_frames"):
            comparator.load_capture(second)

    def test_shared_step_limit_preserves_float32_point_one(self) -> None:
        """The maximum legal step survives float32 serialization without permitting a half-second input."""
        data = workload.generate_bytes(workload.WorkloadHeader(1, 1, 0.1, 1))
        assert workload.decode_bytes(data).step_seconds == workload.MAX_STORED_STEP
        with self.assertRaisesRegex(ValueError, "0.1-second"):
            workload.generate_bytes(workload.WorkloadHeader(1, 1, 0.5, 1))

    def test_comparison_cli_creates_only_requested_output(self) -> None:
        """The offline command does not execute binaries or modify either source artifact directory."""
        first, second = self.capture("first"), self.capture("second")
        output = self.root / "comparison"
        before = (first / "summary.json").read_bytes()
        with (
            patch.object(benchmark, "run_native", side_effect=AssertionError("must not execute")),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            assert comparator.main(["-c", str(first), "-c", str(second), "-A", str(output)]) == 0
        assert (output / "comparison.json").is_file()
        assert (first / "summary.json").read_bytes() == before
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            comparator.main(["-c", str(first), "-A", str(self.root / "invalid")])
        assert not (self.root / "invalid").exists()


if __name__ == "__main__":
    unittest.main()
