# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check paired callback benchmark evidence using synthetic children, never native timings or builds."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import callback_benchmark as runner


def case(duration: float = 100.0, **overrides: object) -> dict[str, object]:
    """Supply a short synthetic workload with exact consumption and checksum evidence."""
    return {
        "case": "dry",
        "rate": 48_000,
        "block": 64,
        "mode": "paced",
        "callback_ns": [duration] * 3,
        "callback_cpu_ns": duration * 2,
        "process_cpu_ns": duration * 4,
        "frames_consumed": 192,
        "frames_emitted": 192,
        "callbacks": 3,
        "checksum": "sample-bits-sha256",
        "silent_callbacks": 0,
        "transition_ns": [duration * 5],
        **overrides,
    }


def child(report: object) -> list[str]:
    """Print exact report bytes from the current interpreter, avoiding any shell or benchmark executable."""
    return [sys.executable, "-c", f"print({(runner.PREFIX + json.dumps(report))!r})"]


def cpp_report() -> dict[str, object]:
    """Model the frozen C++ version-1 measured-phase report, whose time fields declare seconds."""
    return {
        "schema_version": 1,
        "implementation": "cpp",
        "case": "dry",
        "mode": "paced",
        "smoke": False,
        "valid": True,
        "sample_rate": 48_000,
        "block_frames": 64,
        "callback_count": 2,
        "callback_seconds": [0.000001, 0.000002],
        "callback_thread_cpu_seconds": 0.000003,
        "process_cpu_seconds": 0.000004,
        "callback_source_progress_frames": [64, 64],
        "callback_excluded": [False, False],
        "expected_source_frames": 128,
        "source_progress_frames": 128,
        "buffered_source_frames_consumed": 119,
        "non_silent_frames": 128,
        "silent_blocks": 0,
        "excluded_blocks": 0,
        "unexpected_silent_blocks": 0,
        "invalid_samples": 0,
        "source_failure": False,
        "output_checksum": "frozen checksum",
        "source_fixture": "sine_stereo_44100_16.wav",
        "source_identity_fnv1a64": "source identity",
        "source_sample_rate": 44_100,
        "control_transitions": [{"operation": "gain-edit", "seconds": 0.000005}],
    }


def rust_report(*, cold_only: bool = False) -> dict[str, object]:
    """Model frozen Rust raw phases and batch accounting with deliberately distinct cold/warm/steady values."""
    phases = ["cold"] if cold_only else ["cold", "warmup", "steady", "steady"]
    callbacks, batches = [], []
    for index, phase in enumerate(phases):
        frames = [32, 63, 127][index % 3]
        callbacks.append(
            {
                "index": index,
                "phase": phase,
                "frames_requested": frames,
                "duration_ns": (index + 1) * 1000,
                "callback_invoked": True,
                "intentional_exclusion": False,
                "non_silent_frames": frames,
                "checksum": f"checksum-{index}",
            }
        )
        batches.append(
            {
                "start": index,
                "end": index + 1,
                "phase": phase,
                "source_frames_consumed": frames,
                "stale_frames_discarded": 0,
                "intentional_exclusion": False,
                "transition": "attach" if phase == "steady" else None,
                "transition_ack_ns": 20_000 + index if phase == "steady" else None,
                "callback_thread_cpu_ns": (index + 1) * 100,
                "process_cpu_ns": (index + 1) * 200,
            }
        )
    total_frames = sum(item["frames_requested"] for item in callbacks)
    return {
        "schema_version": 1,
        "implementation": "rust",
        "workload": "dry",
        "sample_format": "i16",
        "mode": "latency",
        "sample_rate": 48_000,
        "blocks": [32, 63, 127],
        "fixture": "integer_step_251_v1",
        "cold_only": cold_only,
        "callback_count": len(callbacks),
        "cold_callbacks": 1,
        "warmup_callbacks": int(not cold_only),
        "measured_callbacks": 0 if cold_only else 2,
        "frames_requested": total_frames,
        "steady_frames_requested": 0 if cold_only else 159,
        "valid_frames_emitted": total_frames,
        "non_silent_frames_emitted": total_frames,
        "silent_frames": 0,
        "source_frames_consumed": total_frames,
        "stale_frames_discarded": 0,
        "excluded_callbacks": 0,
        "underruns": 0,
        "callbacks": callbacks,
        "batches": batches,
    }


class NativeSchemaContracts(unittest.TestCase):
    """Adapt only explicitly selected frozen native schemas, preserving the original measured subset and units."""

    def test_cpp_declared_seconds_convert_to_ns_and_preserve_accounting(self) -> None:
        """Callback, CPU, and transition fields convert explicitly without confusing playhead and resampler reads."""
        result = runner.normalize_report(cpp_report(), "", {}, "cpp")[0]
        assert result["callback_ns"] == [1000, 2000]
        assert result["callback_cpu_ns"] == 3000
        assert result["process_cpu_ns"] == 4000
        assert result["transition_ns"] == [5000]
        assert result["callback_frames"] == [64, 64]
        assert result["frames_consumed"] == 119
        assert result["semantic_checks"]["source_progress_frames"] == 128
        assert result["frames_emitted"] == 128
        assert result["checksum"] == "frozen checksum"

    def test_rust_steady_samples_select_matching_cpu_audio_and_checksum(self) -> None:
        """Global totals include warmup, so select raw steady callbacks and exactly their raw batches instead."""
        native = rust_report()
        result = runner.normalize_report(native, "", {}, "rust")[0]
        assert result["case"] == "dry:i16:steady"
        assert result["callback_ns"] == [3000, 4000]
        assert result["callback_frames"] == [127, 32]
        assert result["callbacks"] == 2
        assert result["callback_cpu_ns"] == 700
        assert result["process_cpu_ns"] == 1400
        assert result["frames_consumed"] == result["frames_emitted"] == 159
        assert result["transition_ns"] == [20_002, 20_003]
        checksums = [item["checksum"] for item in native["callbacks"] if item["phase"] == "steady"]
        assert result["checksum"] == hashlib.sha256(json.dumps(checksums).encode()).hexdigest()
        assert native["callback_count"] == 4

    def test_rust_cold_only_selects_the_actual_cold_cpu_batch(self) -> None:
        """Cold-only CPU summaries at top level are steady-only zeros, and must not replace real batch CPU."""
        native = rust_report(cold_only=True)
        native["cpu"] = {"callback_thread_ns": 0, "process_ns": 0}
        result = runner.normalize_report(native, "", {}, "rust")[0]
        assert result["case"] == "dry:i16:cold"
        assert result["callback_ns"] == [1000]
        assert result["callback_cpu_ns"] == 100
        assert result["process_cpu_ns"] == 200
        assert result["frames_consumed"] == 32
        assert "transition_ns" not in result

    def test_unknown_units_versions_implementation_and_invalid_flags_fail(self) -> None:
        """No ambiguous unit declaration or false native validity marker is rescued by plausible time numbers."""
        for schema, original in [("cpp", cpp_report()), ("rust", rust_report())]:
            for field, value in [
                ("time_unit", "microseconds"),
                ("schema_version", 2),
                ("implementation", "unknown"),
                ("valid", False),
            ]:
                with self.subTest(schema=schema, field=field), self.assertRaises(ValueError):  # noqa: PT027
                    runner.normalize_report(original | {field: value}, "", {}, schema)
        with self.assertRaises(ValueError):  # noqa: PT027
            runner.normalize_report(rust_report(), "", {}, "unknown")

    def test_rust_cpu_only_modes_fail_descriptively_without_inventing_timings(self) -> None:
        """Untimed paced/idle CPU reports remain raw evidence, not fake all-zero latency observations."""
        for mode in ("paced", "idle_cpu"):
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, "CPU-only.*MODE=latency"):  # noqa: PT027
                runner.normalize_report(rust_report() | {"mode": mode}, "", {}, "rust")

    def test_cpp_ready_idle_is_distinct_and_contention_requires_active_work(self) -> None:
        """Do not accept silent-only active tests or conflate unready and ready idle callback paths."""
        idle = cpp_report()
        idle.update(
            case="idle-ready",
            callback_readiness="ready",
            callback_source_progress_frames=[0, 0],
            expected_source_frames=0,
            source_progress_frames=0,
            buffered_source_frames_consumed=0,
            non_silent_frames=0,
            silent_blocks=2,
        )
        actual = runner.normalize_cpp(idle)
        assert actual["semantic_checks"]["callback_readiness"] == "ready"
        idle["callback_readiness"] = "not-ready"
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.normalize_cpp(idle)
        idle.update(case="control-contention", callback_readiness="ready", callback_excluded=[True, True])
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.normalize_cpp(idle)

    def test_cpp_valid_flag_cannot_hide_raw_count_or_audio_errors(self) -> None:
        """Independent report assertions prevent an invalid active workload being mistaken for a speedup."""
        changes = [
            {"smoke": True},
            {"timing_valid": False},
            {"source_failure": True},
            {"invalid_samples": 1},
            {"unexpected_silent_blocks": 1},
            {"callback_count": 3},
            {"callback_source_progress_frames": [64, 0]},
            {"source_progress_frames": 127},
            {"non_silent_frames": 129},
            {"buffered_source_frames_consumed": 0},
            {"excluded_blocks": 1},
        ]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):  # noqa: PT027
                runner.normalize_report(cpp_report() | change, "", {}, "cpp")

    def test_rust_raw_coverage_underruns_and_aggregate_counts_are_checked(self) -> None:
        """Reject warmup/steady mismatch, missing batches, changed accounting, or skipped callback work."""
        invalid = [
            rust_report() | change
            for change in [
                {"underruns": 1},
                {"measured_callbacks": 3},
                {"frames_requested": 1},
                {"valid_frames_emitted": 0},
                {"source_frames_consumed": 0},
            ]
        ]
        for container, index, field, value in [
            ("callbacks", 2, "phase", "warmup"),
            ("callbacks", 2, "callback_invoked", False),
            ("callbacks", 2, "duration_ns", None),
            ("batches", 2, "source_frames_consumed", 0),
            ("batches", 2, "start", 1),
        ]:
            native = rust_report()
            native[container][index][field] = value
            invalid.append(native)
        invalid.append(rust_report() | {"batches": rust_report()["batches"][:-1]})
        for native in invalid:
            with self.subTest(native=native), self.assertRaises(ValueError):  # noqa: PT027
                runner.normalize_report(native, "", {}, "rust")

    def test_report_key_and_post_normalization_field_map_still_work(self) -> None:
        """Native adapters are explicit and composable with an outer report wrapper and canonical field mapping."""
        result = runner.normalize_report({"native": cpp_report()}, "native", {"case": "case"}, "cpp")
        assert result == runner.normalize_report(cpp_report(), "", {}, "cpp")


class ReportContracts(unittest.TestCase):
    """Validate one explicit normalized schema without heuristic field aliases or unit guesses."""

    def test_single_list_wrapper_and_explicit_nested_schema(self) -> None:
        """Accept equivalent container forms and named native fields only when mapped explicitly."""
        expected = [case()]
        for source in (case(), [case()], {"cases": [case()]}):
            assert runner.normalize_report(source, "", {}) == expected
        source = {"benchmark": {"measurements": [{"native": case()}]}}
        mapping = {field: f"native.{field}" for field in (*runner.REQUIRED_FIELDS, "transition_ns")}
        assert runner.normalize_report(source, "benchmark.measurements", mapping) == expected
        assert runner.get_path(source, "benchmark.measurements.0.native.case") == "dry"

    def test_reject_missing_duplicate_empty_and_invalid_case_data(self) -> None:
        """A faster incomplete or malformed workload cannot enter a performance comparison."""
        invalid = [[], {"cases": []}, [case(), case()], "not an object", {}]
        for field in runner.REQUIRED_FIELDS:
            incomplete = case()
            del incomplete[field]
            invalid.append(incomplete)
        for field, values in {
            "case": ["", 1],
            "mode": ["", None],
            "rate": [0, True, 48_000.0],
            "block": [0, -1],
            "callback_ns": [[], [1], [1, -1, 1], [1, float("nan"), 1], [1, True, 1]],
            "callback_cpu_ns": [[1], float("inf"), -1],
            "process_cpu_ns": [float("nan"), "1"],
            "callbacks": [0, 2, False],
            "frames_consumed": [-1, 1.5],
            "frames_emitted": [-1],
            "checksum": ["", None, True],
            "silent_callbacks": [4, -1],
            "transition_ns": [[], [-1]],
        }.items():
            invalid.extend(case(**{field: value}) for value in values)
        for source in invalid:
            with self.subTest(source=source), self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
                runner.normalize_report(source, "", {})

    def test_irregular_callback_deadlines_use_actual_frame_sizes(self) -> None:
        """Large blocks use their own deadlines rather than the workload's minimum block size."""
        record = case(rate=1000, callback_frames=[1, 10, 1], callback_ns=[500_000, 5_000_000, 2_000_000])
        result = runner.metrics(record)
        assert result["deadline_misses"] == 1
        assert result["deadline_ratio.max"] == 2
        assert result["deadline_ratio.p99"] == 1.97
        for frames in ([1], [1, 0, 1], [1, True, 1], [1, 1.5, 1]):
            with self.subTest(frames=frames), self.assertRaises(ValueError):  # noqa: PT027
                runner.normalize_report(case(callback_frames=frames), "", {})

    def test_optional_transition_and_per_callback_cpu_samples(self) -> None:
        """Allow no transition workload, while requiring actual samples for an explicitly mapped transition field."""
        record = case(callback_cpu_ns=[1, 2, 3])
        del record["transition_ns"]
        assert runner.normalize_report(record, "", {}) == [record]
        assert runner.metrics(record)["callback_cpu_ns"] == 6
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.normalize_report(record, "", {"transition_ns": "missing"})

    def test_marker_is_unique_across_both_streams(self) -> None:
        """Unrelated diagnostics are allowed, but missing, duplicate, or malformed report lines fail."""
        marker = (runner.PREFIX + json.dumps(case()) + "\n").encode()
        assert runner.extract_report(b"log\n\xff\n", marker) == case()
        libtest_prefix = b"test callback_benchmark::production_callback ... "
        assert runner.extract_report(libtest_prefix + marker + b"ok\n", b"") == case()
        for stdout, stderr in [(b"", b""), (marker, marker), (runner.PREFIX.encode() + b"{\n", b"")]:
            with self.subTest(stdout=stdout, stderr=stderr), self.assertRaises(ValueError):  # noqa: PT027
                runner.extract_report(stdout, stderr)

    def test_stream_parser_bounds_report_size_and_skips_large_diagnostics(self) -> None:
        """Retain diagnostic files without requiring their complete contents in parser working memory."""
        marker = (runner.PREFIX + json.dumps(case()) + "\n").encode()
        diagnostics = b"x" * 65_536 + b"\n" + b"\xff log\n"
        with mock.patch.object(runner, "OUTPUT_SCAN_CHUNK_BYTES", 17):
            assert runner.extract_report(diagnostics + marker, b"") == case()
        with (
            mock.patch.object(runner, "MAX_REPORT_BYTES", 32),
            self.assertRaisesRegex(ValueError, "maximum byte size"),  # noqa: PT027 - stdlib-only tests
        ):
            runner.extract_report(marker, b"")
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.extract_report(marker + marker, b"")
        assert runner.extract_report(marker.rstrip(b"\n"), b"") == case()

    def test_changed_native_validation_contract_cannot_reuse_legacy_acceptance(self) -> None:
        """A stronger oracle or changed cold setup must produce separately labelled benchmark evidence."""
        legacy = rust_report()
        corrected = copy.deepcopy(legacy)
        corrected["benchmark_contract"] = "transition_oracle_and_callback_start_pacing_v2"
        corrected["pacing_scope"] = "callback-start"
        for item in corrected["callbacks"]:
            item["expected_active_frames"] = item["non_silent_frames"]
            item["expected_reset_count"] = 0
        for batch in corrected["batches"]:
            batch.update(
                expected_active_frames=batch["source_frames_consumed"],
                expected_reset_count=0,
                source_position=0,
                generation=1,
                insert_changes_applied=0,
                source_frames_prepared=batch["source_frames_consumed"],
                insert_failed=False,
                stream_lost=False,
                ended=False,
                playing=True,
                insert_settled=True,
            )
        corrected["expected_active_frames"] = corrected["source_frames_consumed"]
        corrected["expected_steady_active_frames"] = corrected["steady_frames_requested"]
        corrected["expected_reset_count"] = 0
        left = runner.normalize_rust(legacy)
        right = runner.normalize_rust(corrected)
        comparison = runner.compare_pairs([{"baseline": [left], "candidate": [right]}])
        assert comparison["semantic_issues"]
        assert comparison["outcome"] == "REGRESSION"
        missing_oracle = copy.deepcopy(corrected)
        del missing_oracle["callbacks"][2]["expected_active_frames"]
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.normalize_rust(missing_oracle)
        failed_batch = copy.deepcopy(corrected)
        failed_batch["batches"][2]["insert_failed"] = True
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.normalize_rust(failed_batch)
        corrected["insert_failed"] = True
        with self.assertRaises(ValueError):  # noqa: PT027 - stdlib-only tests
            runner.normalize_rust(corrected)

    def test_metrics_keep_tail_cpu_deadline_and_audio_work_separate(self) -> None:
        """Summaries use exact raw durations and consumed audio, with no source-audio division for idle cases."""
        result = runner.metrics(case(callback_ns=[10, 20, 30], transition_ns=[100, 200]))
        assert result["callback_ns.p50"] == 20
        assert result["callback_ns.p95"] == 29
        assert result["callback_ns.p99"] == 29.8
        assert result["callback_ns.max"] == 30
        assert result["transition_ns.p50"] == 150
        assert result["callback_cpu_ns"] == 200
        assert result["callback_ns_per_source_audio_second"] == 15_000
        assert result["deadline_misses"] == 0
        assert "callback_ns_per_source_audio_second" not in runner.metrics(case(frames_consumed=0))


class ComparisonContracts(unittest.TestCase):
    """Keep confidence bounds descriptive, reproducible, paired, and independent of callback sample count."""

    def test_fixed_seed_bounds_and_direction(self) -> None:
        """Only one-sided bounds strictly away from zero establish a measured direction."""
        for values, outcome in [
            ([1, 2, 3], "REGRESSION"),
            ([-1, -2, -3], "PASS"),
            ([-10, 10], "INCONCLUSIVE"),
            ([0, 0], "INCONCLUSIVE"),
            ([3], "INCONCLUSIVE"),
        ]:
            with self.subTest(values=values):
                result = runner.paired_bounds(values)
                assert result == runner.paired_bounds(values)
                assert result["outcome"] == outcome
                if len(values) == 1:
                    assert result["confidence_interval"] is None

    def test_pairs_not_callbacks_are_the_resampling_unit(self) -> None:
        """Three paired processes remain three observations despite each process containing many callback samples."""
        pairs = [{"baseline": [case(100)], "candidate": [case(100 + delta)]} for delta in [1, 2, 3]]
        result = runner.compare_pairs(pairs)
        measured = result["comparisons"][0]["metrics"]["callback_ns.p50"]
        assert measured["paired_differences"] == [1, 2, 3]
        assert measured["confidence_interval"] == runner.paired_bounds([1, 2, 3])["confidence_interval"]
        assert result["outcome"] == "REGRESSION"
        assert not result["semantic_issues"]

    def test_no_averaging_away_regressing_cases_or_tails(self) -> None:
        """One slow workload or tail fails even when another case becomes much faster."""
        baseline = [case(100), case(100, case="wet")]
        candidate = [case(1), case(100, case="wet", callback_ns=[1, 1, 200])]
        result = runner.compare_pairs([{"baseline": baseline, "candidate": candidate}] * 3)
        assert result["outcome"] == "REGRESSION"
        wet = next(item for item in result["comparisons"] if item["identity"][0] == "wet")
        assert wet["metrics"]["callback_ns.max"]["outcome"] == "REGRESSION"

    def test_semantic_and_case_coverage_changes_are_explicit(self) -> None:
        """Checksums, callback counts, consumption, emission, silence, and missing cases cannot look like speedups."""
        for field, changed in [
            ("checksum", "different"),
            ("frames_consumed", 0),
            ("frames_emitted", 0),
            ("silent_callbacks", 1),
            ("callbacks", 4),
        ]:
            with self.subTest(field=field):
                result = runner.compare_pairs([{"baseline": [case()], "candidate": [case(**{field: changed})]}])
                assert result["outcome"] == "REGRESSION"
                assert any(field in issue for issue in result["semantic_issues"])
        for candidate in [[], [case(case="other")], [case(transition_ns=[1, 2])]]:
            result = runner.compare_pairs([{"baseline": [case()], "candidate": candidate}])
            assert result["outcome"] == "REGRESSION"
            assert result["semantic_issues"]

    def test_frame_sequence_and_native_semantic_changes_are_not_timing_speedups(self) -> None:
        """Equal minimum block size and equal total frames cannot hide a different callback deadline sequence."""
        left = case(callback_frames=[32, 64, 96], semantic_checks={"fixture": "same"})
        for right in [
            case(callback_frames=[32, 96, 64], semantic_checks={"fixture": "same"}),
            case(callback_frames=[32, 64, 96], semantic_checks={"fixture": "changed"}),
        ]:
            result = runner.compare_pairs([{"baseline": [left], "candidate": [right]}])
            assert result["outcome"] == "REGRESSION"
            assert result["semantic_issues"]

    def test_faster_measured_metrics_pass_with_observed_discrete_counts(self) -> None:
        """An unchanged zero observed deadline-miss count is not a continuous timing-equivalence claim."""
        result = runner.compare_pairs([{"baseline": [case(100)], "candidate": [case(50)]}] * 3)
        assert result["outcome"] == "PASS"
        assert result["comparisons"][0]["observed_deadline_misses"] == {"baseline": [0, 0, 0], "candidate": [0, 0, 0]}

    def test_identical_results_are_not_evidence_of_equivalence(self) -> None:
        """Baseline-versus-baseline noise and one-pair captures cannot accidentally establish no regression."""
        for pairs in [
            [{"baseline": [case()], "candidate": [case()]}] * 3,
            [{"baseline": [case(100)], "candidate": [case(1)]}],
        ]:
            assert runner.compare_pairs(pairs)["outcome"] == "INCONCLUSIVE"


class InvocationContracts(unittest.TestCase):
    """Exercise exact argv, provenance, isolated storage, and failure artifacts without native audio processing."""

    def setUp(self) -> None:
        """Give every invocation a disposable absolute artifact path with spaces."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-callback-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "evidence with spaces"

    def invoke(
        self, baseline: list[str], candidate: list[str] | None = None, *extra: str
    ) -> tuple[int, dict[str, object]]:
        """Drive the public parser/main function and read its durable summary."""
        arguments = ["--baseline-command", json.dumps(baseline), "--pairs=2", "--artifacts", str(self.root), *extra]
        if candidate is not None:
            arguments.extend(["--candidate-command", json.dumps(candidate)])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = runner.main(arguments)
        return status, json.loads((self.root / "summary.json").read_text(encoding="utf-8"))

    def test_alternation_metadata_hashes_and_source_reports(self) -> None:
        """Persist caller build assertions and exact executable bytes alongside ordered, independent process runs."""
        command = child({"cases": [case()]})
        metadata = {"compiler": "caller clang version", "flags": ["-O3"], "fixture_sha256": "fixture identity"}
        status, summary = self.invoke(command, command, "--baseline-metadata", json.dumps(metadata))
        assert status == 3
        assert summary["outcome"] == "INCONCLUSIVE"
        assert summary["baseline_vs_baseline"]
        assert [(run["pair"], run["role"]) for run in summary["runs"]] == [
            (1, "baseline"),
            (1, "candidate"),
            (2, "candidate"),
            (2, "baseline"),
        ]
        manifest = json.loads((self.root / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["baseline"]["metadata"] == metadata
        assert manifest["bootstrap"]["seed"] == runner.BOOTSTRAP_SEED
        path = Path(sys.executable).resolve()
        expected = hashlib.sha256(path.read_bytes()).hexdigest()
        assert summary["executable_identities"]["baseline"]["sha256"] == expected
        directory = self.root / "pair-0001-baseline"
        assert json.loads((directory / "source-report.json").read_text()) == {"cases": [case()]}
        assert json.loads((directory / "normalized.json").read_text()) == [case()]
        expected_stdout = (runner.PREFIX + json.dumps({"cases": [case()]}) + "\n").encode()
        assert (directory / "stdout.bin").read_bytes() == expected_stdout

    def test_environment_argv_storage_and_raw_bytes(self) -> None:
        """Force temporary application storage and preserve arbitrary byte diagnostics, literal argv, and overrides."""
        source = (
            "import json, os, sys; from pathlib import Path; "
            "storage = os.environ['AUDIOBATCH_DATA_ROOT']; "
            "assert Path(storage).is_absolute(); assert storage == os.environ['TMPDIR']; "
            "assert os.environ['VALUE'] == sys.argv[1]; "
            "Path(storage, 'only-temporary').write_text('isolated'); "
            "sys.stderr.buffer.write(b'raw\\xff\\r\\n'); "
            f"print({(runner.PREFIX + json.dumps(case()))!r})"
        )
        literal = "a value; $(not-shell) with spaces"
        status, summary = self.invoke(
            [sys.executable, "-c", source, literal],
            None,
            "--capture-only",
            "--env",
            json.dumps({"VALUE": "shared", "AUDIOBATCH_DATA_ROOT": "/forbidden"}),
            "--baseline-env",
            json.dumps({"VALUE": literal}),
        )
        assert status == 0
        assert summary["outcome"] == "CAPTURED"
        directory = self.root / "pair-0001-baseline"
        assert (directory / "stderr.bin").read_bytes() == b"raw\xff\r\n"
        result = json.loads((directory / "run.json").read_text())
        assert result["command"][-1] == literal
        assert not Path(result["isolated_storage"]).exists()

    def test_native_field_mapping_retains_both_source_and_normalized_evidence(self) -> None:
        """A caller can adapt nested native fields without losing their original report or making unit guesses."""
        native = {"native_report": {"results": [{"measurements": case()}]}}
        mapping = {field: f"measurements.{field}" for field in (*runner.REQUIRED_FIELDS, "transition_ns")}
        status, summary = self.invoke(
            child(native),
            None,
            "--capture-only",
            "--report-key=native_report.results",
            "--field-map",
            json.dumps(mapping),
        )
        assert status == 0
        assert summary["outcome"] == "CAPTURED"
        directory = self.root / "pair-0001-baseline"
        assert json.loads((directory / "source-report.json").read_text()) == native
        assert json.loads((directory / "normalized.json").read_text()) == [case()]

    def test_native_cpp_capture_retains_full_raw_report(self) -> None:
        """The CLI selects native normalization while preserving byte-exact original process output."""
        native = cpp_report()
        status, summary = self.invoke(child(native), None, "--capture-only", "--native-schema=cpp")
        assert status == 0
        assert summary["outcome"] == "CAPTURED"
        directory = self.root / "pair-0001-baseline"
        assert (directory / "stdout.bin").read_bytes() == (runner.PREFIX + json.dumps(native) + "\n").encode()
        assert json.loads((directory / "source-report.json").read_text()) == native
        normalized = json.loads((directory / "normalized.json").read_text())[0]
        assert normalized["callback_ns"] == [1000, 2000]
        manifest = json.loads((self.root / "manifest.json").read_text())
        assert manifest["native_schema"] == "cpp"

    def test_native_cpu_only_failure_keeps_original_measurements(self) -> None:
        """Unsupported timed comparisons still leave every original CPU sample available for separate analysis."""
        native = rust_report() | {"mode": "paced"}
        status, summary = self.invoke(child(native), None, "--capture-only", "--native-schema=rust")
        assert status == 1
        assert summary["outcome"] == "FAILED"
        directory = self.root / "pair-0001-baseline"
        assert json.loads((directory / "source-report.json").read_text()) == native
        assert not (directory / "normalized.json").exists()
        assert "CPU-only" in summary["errors"][0]

    def test_capture_only_with_two_commands_keeps_alternating_pairs(self) -> None:
        """A capture can retain evidence from both binaries without running an acceptance comparison."""
        status, summary = self.invoke(child(case()), child(case(101)), "--capture-only")
        assert status == 0
        assert summary["outcome"] == "CAPTURED"
        assert len(summary["runs"]) == 4
        assert "comparisons" not in summary

    def test_successful_exit_without_report_is_not_a_capture(self) -> None:
        """A zero process status cannot stand in for missing or incomplete callback evidence."""
        status, summary = self.invoke([sys.executable, "-c", "print('ordinary logs only')"], None, "--capture-only")
        assert status == 1
        assert summary["outcome"] == "FAILED"
        result = json.loads((self.root / "pair-0001-baseline/run.json").read_text())
        assert result["returncode"] == 0
        assert "exactly one" in result["error"]

    def test_nonzero_and_missing_reports_fail_and_keep_streams(self) -> None:
        """Do not compare an exit failure even if it already emitted plausible benchmark JSON."""
        command = child(case())
        command[2] += "; import sys; print('failure', file=sys.stderr); sys.exit(7)"
        status, summary = self.invoke(command, command)
        assert status == 1
        assert summary["outcome"] == "FAILED"
        directory = self.root / "pair-0001-baseline"
        result = json.loads((directory / "run.json").read_text())
        assert result["returncode"] == 7
        assert (directory / "stderr.bin").read_bytes() == b"failure\n"
        assert runner.PREFIX.encode() in (directory / "stdout.bin").read_bytes()

    def test_launch_failure_retains_error_without_starting_comparison(self) -> None:
        """A missing binary still creates useful failure provenance and empty raw stream artifacts."""
        status, summary = self.invoke([str(self.root / "missing benchmark")], None, "--capture-only")
        assert status == 1
        assert summary["outcome"] == "FAILED"
        result = json.loads((self.root / "pair-0001-baseline/run.json").read_text())
        assert result["returncode"] is None
        assert "missing benchmark" in result["error"]
        assert (self.root / "pair-0001-baseline/stdout.bin").read_bytes() == b""

    @unittest.skipUnless(sys.platform == "darwin", "process-group cleanup is tested on macOS")
    def test_timeout_terminates_workers_and_retains_output(self) -> None:
        """Stop inherited-pipe workers as well as their parent, preserving partial diagnostics."""
        source = (
            "import subprocess, sys, time; "
            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
            "print('before deadline', flush=True); time.sleep(30)"
        )
        with mock.patch.object(runner.os, "killpg", wraps=os.killpg) as terminate:
            status, summary = self.invoke([sys.executable, "-c", source], None, "--capture-only", "--timeout=1")
        assert status == 1
        assert summary["outcome"] == "FAILED"
        terminate.assert_called_once()
        result = json.loads((self.root / "pair-0001-baseline/run.json").read_text())
        assert result["timed_out"]
        assert (self.root / "pair-0001-baseline/stdout.bin").read_bytes() == b"before deadline\n"

    def test_executable_changes_within_and_between_runs_are_rejected(self) -> None:
        """Hash changes invalidate the evidence rather than mixing builds into a timing distribution."""
        options = runner.parse_options(
            ["--baseline-command", json.dumps(child(case())), "--capture-only", "--artifacts", str(self.root)]
        )
        self.root.mkdir()
        identity = runner.executable_identity(options.baseline_command, os.environ.copy(), Path.cwd())
        changed = identity | {"sha256": "changed"}
        with mock.patch.object(runner, "executable_identity", side_effect=[identity, changed]):
            result = runner.capture_run(options, "baseline", 1)
        assert result["outcome"] == "FAILED"
        assert "changed during" in result["error"]
        other = self.root / "second invocation"
        self.root = other
        with mock.patch.object(runner, "executable_identity", side_effect=[identity, identity, changed, changed]):
            status, summary = self.invoke(child(case()), None, "--capture-only")
        assert status == 1
        assert "changed between" in summary["errors"][0]

    def test_faster_synthetic_report_has_pass_exit_status(self) -> None:
        """Exercise aggregate measured PASS independently of baseline-only and inconclusive capture outcomes."""
        status, summary = self.invoke(child(case(100)), child(case(50)))
        assert status == 0
        assert summary["outcome"] == "PASS"

    def test_artifacts_are_not_silently_overwritten(self) -> None:
        """An existing run remains intact instead of being overwritten with stale or mixed evidence."""
        self.invoke(child(case()), None, "--capture-only")
        before = (self.root / "summary.json").read_bytes()
        with contextlib.redirect_stderr(io.StringIO()):
            status = runner.main(
                ["--baseline-command", json.dumps(child(case(200))), "--capture-only", "--artifacts", str(self.root)]
            )
        assert status == 1
        assert (self.root / "summary.json").read_bytes() == before


class WindowsCleanupContracts(unittest.TestCase):
    """Check the Windows process-tree termination policy without claiming native platform execution."""

    def test_timeout_uses_system_taskkill_with_exact_pid_and_tree_flags(self) -> None:
        """The utility location is independent of the benchmark PATH, and no shell parses the PID."""
        process = mock.Mock(pid=123)
        with (
            mock.patch.object(runner.sys, "platform", "win32"),
            mock.patch.dict(os.environ, {"SYSTEMROOT": "/mock system root"}),
            mock.patch.object(runner.subprocess, "run") as terminate,
        ):
            runner.terminate_process_tree(process)
        utility = str(Path("/mock system root") / "System32" / "taskkill.exe")
        assert terminate.call_args.args[0] == [utility, "/PID", "123", "/T", "/F"]
        assert terminate.call_args.kwargs["timeout"] == 10
        process.wait.assert_called_once_with(timeout=10)
        process.kill.assert_not_called()

    def test_missing_tree_utility_falls_back_to_killing_and_reaping_parent(self) -> None:
        """Even a failed system utility cannot leave the immediate hung benchmark process unreaped."""
        process = mock.Mock(pid=123)
        with (
            mock.patch.object(runner.sys, "platform", "win32"),
            mock.patch.dict(os.environ, {"SYSTEMROOT": "/mock system root"}),
            mock.patch.object(runner.subprocess, "run", side_effect=OSError("missing utility")),
        ):
            runner.terminate_process_tree(process)
        process.kill.assert_called_once()
        process.wait.assert_called_once_with(timeout=10)


class OptionContracts(unittest.TestCase):
    """Malformed commands or incomplete policies fail before any executable or artifact operation."""

    def test_short_aliases_preserve_commands_json_and_policy(self) -> None:
        """Every alias, including role-specific metadata, produces the same validated namespace."""
        values = [
            ("--baseline-command", "-b", json.dumps([sys.executable, "-c", "print(-1)", "--flag"])),
            ("--candidate-command", "-k", json.dumps([sys.executable, "--version"])),
            ("--pairs", "-P", "2"),
            ("--artifacts", "-A", str(Path(tempfile.gettempdir()).resolve() / "unused artifacts")),
            ("--cwd", "-d", str(Path.cwd())),
            ("--timeout", "-t", "12.5"),
            ("--report-key", "-K", "benchmark.cases"),
            ("--native-schema", "-N", "rust"),
            ("--field-map", "-F", '{"case": "case"}'),
            ("--env", "-e", '{"SHARED": "with spaces"}'),
            ("--baseline-env", "-u", '{"ROLE": "baseline"}'),
            ("--candidate-env", "-U", '{"ROLE": "candidate"}'),
            ("--baseline-metadata", "-M", '{"flags": ["-O3"]}'),
            ("--candidate-metadata", "-m", '{"flags": ["-O2"]}'),
        ]
        long = [token for option, _, value in values for token in (option, value)]
        short = [token for _, option, value in values for token in (option, value)]
        assert runner.parse_options([*short, "-q"]) == runner.parse_options([*long, "--capture-only"])

    def test_usage_errors(self) -> None:
        """Reject shell strings, invalid environments, relative artifact paths, and nonfinite timeouts."""
        valid = [
            "--baseline-command",
            json.dumps(child(case())),
            "--candidate-command",
            json.dumps(child(case())),
            "--artifacts",
            str(Path(tempfile.gettempdir()).resolve() / "unused-callback-artifacts"),
        ]
        invalid = [
            ["--baseline-command", "echo benchmark"],
            ["--baseline-command", "[]"],
            ["--baseline-command", '["binary", 1]'],
            ["--baseline-command", json.dumps(["binary", "\u0000"])],
            ["--env", json.dumps({"BAD=KEY": "value"})],
            ["--pairs=0"],
            ["--timeout=0"],
            ["--timeout=nan"],
            ["--timeout=inf"],
            ["--timeout=3601"],
            ["--env=[]"],
            ["--env", '{"X": 1}'],
            ["--candidate-metadata", '{"X": NaN}'],
            ["--artifacts=relative"],
            ["--field-map", '{"unknown": "field"}'],
        ]
        for options in invalid:
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:  # noqa: PT027 - stdlib-only tests
                    runner.parse_options(valid + options)
                assert raised.exception.code == 2
        without_candidate = copy.copy(valid)
        index = without_candidate.index("--candidate-command")
        del without_candidate[index : index + 2]
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):  # noqa: PT027
            runner.parse_options(without_candidate)


if __name__ == "__main__":
    unittest.main(verbosity=2)
