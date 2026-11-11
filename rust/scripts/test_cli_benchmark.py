#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0", "numpy>=2", "pytest>=8"]
# ///
"""
Exercises benchmark safety, correctness failures, export compatibility, and release selection without audio CLIs.

Run with uv run --script scripts/test_cli_benchmark.py from rust/.
All filesystem changes are restricted to temporary directories.
"""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import cli_benchmark as benchmark
import pytest
import test_parity

BASH_EXECUTABLE = shutil.which("bash")
if BASH_EXECUTABLE is None:
    message = "Bash is required to test CLI benchmark shell entry points"
    raise FileNotFoundError(message)


class TestCorpusSafety(unittest.TestCase):
    """Checks that corpus selection cannot normalize originals or benchmark unsupported fixtures silently."""

    def test_rejects_lfs_pointers_and_empty_corpus(self) -> None:
        """Missing audio fails explicitly instead of producing deceptively fast empty runs."""
        with tempfile.TemporaryDirectory() as temporary:
            corpus = Path(temporary)
            with pytest.raises(ValueError, match="No usable audio"):
                benchmark.discover_sources(corpus)
            (corpus / "pointer.wav").write_text("version https://git-lfs.github.com/spec/v1\n", encoding="utf-8")
            with pytest.raises(ValueError, match="Git LFS pointer"):
                benchmark.discover_sources(corpus)

    def test_excludes_known_normalization_failures(self) -> None:
        """The normalization timing corpus is successful work, not silent failures or trash disposal."""
        with tempfile.TemporaryDirectory() as temporary:
            corpus = Path(temporary)
            for name in ("valid.wav", "silent.wav"):
                (corpus / name).write_bytes(b"audio")
            with (
                patch.object(
                    benchmark.levels, "fixture_properties", side_effect=lambda source: {"silent": source.stem}
                ),
                patch.object(
                    benchmark.levels, "expected_failure", side_effect=lambda props, *_: props["silent"] == "silent"
                ),
            ):
                sources, excluded = benchmark.discover_sources(corpus)
            assert [source.name for source in sources] == ["valid.wav"]
            assert len(excluded) == 1

    def test_auto_combines_both_collections(self) -> None:
        """Generated long audio supplements normalization fixtures instead of replacing them."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "fixtures/benchmark/output"
            generated.mkdir(parents=True)
            (generated / "long.wav").write_bytes(b"audio")
            normalization = root / "fixtures/normalization"
            with patch.object(benchmark, "ROOT_DIRECTORY", root):
                assert benchmark.select_corpora() == [generated, normalization]
                assert benchmark.select_corpora("auto") == [generated, normalization]
                assert benchmark.select_corpora(str(normalization)) == [normalization.resolve()]

    def test_windows_excludes_juce_unsupported_extensions(self) -> None:
        """Unsupported Windows inputs never reach fixture probing or either timed CLI."""
        with tempfile.TemporaryDirectory() as temporary:
            corpus = Path(temporary)
            for name in ("valid.wav", "tone.aac", "tone.m4a", "tone.mp4", "tone.alac", "upper.M4A", "tone.aifc"):
                (corpus / name).write_bytes(b"audio")
            with (
                patch.object(benchmark.sys, "platform", "win32"),
                patch.object(benchmark.levels, "fixture_properties", return_value={}) as properties,
                patch.object(benchmark.levels, "expected_failure", return_value=False),
            ):
                sources, excluded = benchmark.discover_sources(corpus)
            assert [source.name for source in sources] == ["valid.wav"]
            properties.assert_called_once_with(corpus / "valid.wav")
            assert {Path(row["path"]).name for row in excluded} == {
                "tone.aac",
                "tone.m4a",
                "tone.mp4",
                "tone.alac",
                "upper.M4A",
                "tone.aifc",
            }
            assert all("Windows" in row["reason"] for row in excluded)

    def test_auto_without_generated_audio_uses_normalization(self) -> None:
        """An absent or empty generated collection is optional, but normalization is always selected."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "fixtures/benchmark/output"
            with patch.object(benchmark, "ROOT_DIRECTORY", root):
                for present in (False, True):
                    if present:
                        generated.mkdir(parents=True)
                        (generated / "README.txt").touch()
                    assert benchmark.select_corpora("auto") == [root / "fixtures/normalization"]

    def test_initialize_keeps_both_corpora_and_same_name_sources(self) -> None:
        """The combined manifest and isolated seeds retain distinct files even when their names match."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            work = root / "work"
            results = root / "results"
            work.mkdir()
            results.mkdir()
            config = root / ".cargo/config.toml"
            config.parent.mkdir()
            config.write_text('[build]\nrustflags = ["-C", "target-cpu=native"]\n', encoding="utf-8")
            sources = []
            for folder in ("fixtures/benchmark/output", "fixtures/normalization"):
                source = root / folder / "tone.aif"
                source.parent.mkdir(parents=True)
                test_parity.write_aiff(source)
                sources.append(source)
            arguments = argparse.Namespace(
                work=work, results=results, corpus="auto", cpp=sources[0], rust=sources[1], jobs=2, runs=3, warmup=1
            )
            with (
                patch.object(benchmark, "ROOT_DIRECTORY", root),
                patch.object(benchmark, "command_output", return_value="--delete --jobs --refresh"),
            ):
                benchmark.initialize(arguments)
            metadata = json.loads((results / "metadata.json").read_text(encoding="utf-8"))
            assert metadata["corpora"] == [str(source.parent) for source in sources]
            assert [row["source"] for row in metadata["files"]] == list(map(str, sources))
            assert len(benchmark.levels.input_files([work / "seed"], recurse=True)) == 2
            assert (results / "raw").is_dir()

    @unittest.skipIf(os.name == "nt", "Creating Windows symlinks requires privileges")
    def test_rejects_corpus_symlinks(self) -> None:
        """An external user path must not become an input that normalization can follow."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corpus = root / "corpus"
            corpus.mkdir()
            original = root / "user.wav"
            original.write_bytes(b"original")
            (corpus / "link.wav").symlink_to(original)
            with pytest.raises(ValueError, match="symlinks are not allowed"):
                benchmark.discover_sources(corpus)
            assert original.read_bytes() == b"original"


class TestPrivatePreparation(unittest.TestCase):
    """Checks the private workspace boundary and fresh-copy behavior."""

    def test_prepare_requires_workspace_marker(self) -> None:
        """A user directory without the private marker is never removed."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            original = work / "cpp/user.wav"
            original.parent.mkdir()
            original.write_bytes(b"original")
            with pytest.raises(FileNotFoundError):
                benchmark.prepare(work, "cpp")
            assert original.read_bytes() == b"original"

    def test_prepare_copies_and_restores_without_touching_seed(self) -> None:
        """Every invocation gets actual copies with no stale normalized outputs or cache."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            seed = work / "seed/0001/tone.wav"
            seed.parent.mkdir(parents=True)
            seed.write_bytes(b"original")
            benchmark.prepare(work, "cpp")
            copied = work / "cpp/input/0001/tone.wav"
            copied.write_bytes(b"normalized")
            (work / "cpp/home/cache/stale").touch()
            benchmark.prepare(work, "cpp")
            assert copied.read_bytes() == b"original"
            assert seed.read_bytes() == b"original"
            assert not (work / "cpp/home/cache/stale").exists()

    def test_initialization_separates_colliding_outputs_and_preserves_sources(self) -> None:
        """The same stem in two formats is copied into separate private directories before normalization."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            corpus, work, results = (root / name for name in ("corpus", "work", "results"))
            for directory in (corpus, work, results):
                directory.mkdir()
            for name in ("tone.aif", "tone.aiff"):
                test_parity.write_aiff(corpus / name)
            originals = {path.name: path.read_bytes() for path in corpus.iterdir()}
            arguments = argparse.Namespace(
                work=work,
                results=results,
                corpus=str(corpus),
                cpp=Path(__file__),
                rust=Path(__file__),
                jobs=2,
                runs=3,
                warmup=1,
            )
            with patch.object(benchmark, "command_output", return_value="--delete --jobs --refresh"):
                benchmark.initialize(arguments)
            expected = json.loads((work / "peak-expectations.json").read_text(encoding="utf-8"))
            assert set(expected) == {str(Path("0001") / "tone.aif"), str(Path("0002") / "tone.aif")}
            assert all(row["status"] == "analyzed" for row in expected.values())
            benchmark.prepare(work, "rust")
            (work / "rust/input/0001/tone.aif").write_bytes(b"rewritten")
            assert {path.name: path.read_bytes() for path in corpus.iterdir()} == originals

    def test_marker_must_match_physical_workspace(self) -> None:
        """A copied marker does not grant permission to clear a different directory."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work / "other")})
            with pytest.raises(ValueError, match="Invalid private workspace"):
                benchmark.prepare(work, "rust")


def write_pair(results: Path, run: int, *, version: int = 2, exit_code: int = 0) -> None:
    """Builds a one-run hyperfine pair with known wall and optional memory measurements."""
    rows = []
    for implementation, seconds in (("cpp", run * 2.0), ("rust", float(run))):
        row = {"command": f"{implementation} analyze jobs=1"}
        if version == 1:
            row.update(times=[seconds], exit_codes=[exit_code])
        else:
            row["name"] = row["command"]
            row["command"] = f"env IMPLEMENTATION={implementation} /path/to/cli --refresh"
            row["measurements"] = [
                {
                    "time_wall_clock": {"value": seconds, "unit": "second"},
                    "memory_peak_resident": {"value": run * 1024.0, "unit": "byte"},
                    "exit_code": exit_code,
                }
            ]
        rows.append(row)
    benchmark.write_json(results / "raw" / f"analyze-jobs-1-run-{run}.json", {"results": rows})


class TestBenchmarkExports(unittest.TestCase):
    """Checks aggregation against individual samples from both supported hyperfine schemas."""

    def test_combines_raw_samples_and_recomputes_statistics(self) -> None:
        """One-run exports become actual sample statistics without dropping available metrics."""
        for version in (1, 2):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary).resolve()
                (work / "raw").mkdir()
                benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
                benchmark.write_json(work / "metadata.json", {"runs": 3})
                for run in range(1, 4):
                    write_pair(work, run, version=version)
                benchmark.combine(work, work, "analyze", 1)
                combined = json.loads((work / "analyze-jobs-1.json").read_text(encoding="utf-8"))
                cpp, rust = combined["results"]
                assert len(combined["raw_exports"]) == 3
                assert len(cpp["measurements"]) == 3
                assert cpp["summary"]["time_wall_clock"] == {
                    "unit": "second",
                    "count": 3,
                    "mean": 4.0,
                    "stddev": 2.0,
                    "median": 4.0,
                    "min": 2.0,
                    "max": 6.0,
                }
                assert benchmark.wall_mean(rust) == 2.0
                if version != 1:
                    assert cpp["summary"]["memory_peak_resident"]["mean"] == 2048.0
                assert "4.000000 | 2.000000" in (work / "analyze-jobs-1.md").read_text(encoding="utf-8")

    def test_single_run_has_no_sample_standard_deviation(self) -> None:
        """A single measured sample remains valid and does not claim zero uncertainty."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            (work / "raw").mkdir()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            benchmark.write_json(work / "metadata.json", {"runs": 1})
            write_pair(work, 1)
            benchmark.combine(work, work, "analyze", 1)
            combined = json.loads((work / "analyze-jobs-1.json").read_text(encoding="utf-8"))
            assert combined["results"][0]["summary"]["time_wall_clock"]["stddev"] is None
            assert "n/a" in (work / "analyze-jobs-1.md").read_text(encoding="utf-8")

    def test_rejects_failed_missing_and_misordered_samples(self) -> None:
        """Failures, missing runs, invalid metrics, and unexpected command order cannot produce a report."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            (work / "raw").mkdir()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            benchmark.write_json(work / "metadata.json", {"runs": 1})
            with pytest.raises(FileNotFoundError):
                benchmark.combine(work, work, "analyze", 1)
            for version in (1, 2):
                write_pair(work, 1, version=version, exit_code=1)
                with pytest.raises(ValueError, match="successful measurement"):
                    benchmark.combine(work, work, "analyze", 1)
            path = work / "raw/analyze-jobs-1-run-1.json"
            for mutation in ("order", "missing", "invalid", "unit"):
                write_pair(work, 1)
                exported = json.loads(path.read_text(encoding="utf-8"))
                if mutation == "order":
                    exported["results"].reverse()
                elif mutation == "missing":
                    exported["results"][0]["measurements"] = []
                elif mutation == "invalid":
                    exported["results"][0]["measurements"][0]["time_wall_clock"]["value"] = -1.0
                else:
                    exported["results"][0]["measurements"][0]["time_wall_clock"]["unit"] = "millisecond"
                benchmark.write_json(path, exported)
                reason = {
                    "order": "Unexpected benchmark command order",
                    "missing": "successful measurement",
                    "invalid": "Invalid hyperfine",
                    "unit": "Hyperfine wall times must use seconds",
                }[mutation]
                with self.subTest(mutation=mutation), pytest.raises(ValueError, match=reason):
                    benchmark.combine(work, work, "analyze", 1)


@unittest.skipIf(os.name == "nt", "Exercises the Bash benchmark entry point")
class TestBenchmarkSequence(unittest.TestCase):
    """Runs the real shell scheduler with a recording shim instead of audio CLIs or actual timings."""

    def test_alternates_runs_and_warms_up_only_once(self) -> None:
        """Each mode and worker count exports alternating pairs with setup and oracles outside timed commands."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            rust = root / "rust"
            scripts = rust / "scripts"
            scripts.mkdir(parents=True)
            shutil.copy2(benchmark.RUST_DIRECTORY / "scripts/cli_benchmark.sh", scripts)
            (rust / "common.sh").write_text(
                'find_cpp_cli() { printf "%s\\n" "$RUST_DIR/target/release/audiobatch"; }\n'
                'executable_name() { printf "%s\\n" "$1"; }\n'
                'require_command() { command -v "$1" > /dev/null; }\n',
                encoding="utf-8",
            )
            for name in ("audiobatch", "audiobatch-rs"):
                binary = rust / "target/release" / name
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.touch(mode=0o755)
            tools = root / "tools"
            tools.mkdir()
            for name in ("uv", "hyperfine"):
                tool = tools / name
                tool.write_text(
                    '#!/bin/bash\nprintf "%s\\0" "${0##*/}" "$@" >> "$BENCHMARK_RECORD"\n'
                    'printf "\\n" >> "$BENCHMARK_RECORD"\n',
                    encoding="utf-8",
                )
                tool.chmod(0o755)
            record = root / "record"
            environment = {
                **os.environ,
                "PATH": f"{tools}{os.pathsep}{os.environ['PATH']}",
                "BENCHMARK_RECORD": str(record),
            }
            environment.pop("GITHUB_STEP_SUMMARY", None)
            # Only checked-in shell code runs, with fixture paths passed separately as arguments.
            subprocess.run(  # noqa: S603
                [
                    BASH_EXECUTABLE,
                    str(scripts / "cli_benchmark.sh"),
                    "--no-build",
                    "--jobs",
                    "2",
                    "--runs",
                    "3",
                    "--warmup",
                    "2",
                ],
                check=True,
                capture_output=True,
                env=environment,
            )
            calls = [line.rstrip("\0").split("\0") for line in record.read_text(encoding="utf-8").splitlines()]
            initialization = calls.pop(0)
            assert initialization[-2:] == ["--corpus", "auto"]
            assert initialization[5] == "init"
            # EXIT cleanup must still find its workspace after the workflow function returns.
            assert not Path(initialization[6]).exists()
            for jobs in (1, 2):
                for mode in benchmark.TARGETS:
                    for run in range(1, 4):
                        timing = calls.pop(0)
                        assert timing[0] == "hyperfine"
                        assert timing[timing.index("--runs") + 1] == "1"
                        assert timing[timing.index("--warmup") + 1] == ("2" if run == 1 else "0")
                        assert timing[timing.index("--export-json") + 1].endswith(
                            f"raw/{mode}-jobs-{jobs}-run-{run}.json"
                        )
                        assert [
                            timing[index + 1] for index, value in enumerate(timing) if value == "--command-name"
                        ] == [f"{implementation} {mode} jobs={jobs}" for implementation in ("cpp", "rust")]
                        for index, value in enumerate(timing):
                            if value == "--prepare":
                                assert " prepare " in timing[index + 1]
                            elif value == "--conclude":
                                assert " check " in timing[index + 1]
                                timed_command = timing[index + 2]
                                assert timed_command.startswith("env ")
                                assert "cli_benchmark.py" not in timed_command
                        assert calls.pop(0)[5] == "compare"
                    assert calls.pop(0)[5] == "combine"
            assert calls.pop(0)[5] == "report"
            assert calls == []


class TestBenchmarkValidation(unittest.TestCase):
    """Checks that timing results cannot hide omitted inputs or malformed exports."""

    def test_missing_cli_rows_fail_correctness(self) -> None:
        """Even identical empty CLI JSON is a correctness failure."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            benchmark.write_json(work / "analyze-expectations.json", {"tone.wav": {"status": "analyzed"}})
            benchmark.write_json(benchmark.result_path(work, "analyze", 1, "cpp"), [])
            with pytest.raises(ValueError, match=r"missing C\+\+ result"):
                benchmark.check(work, work, "cpp", "analyze", 1)

    def test_normalized_output_must_reach_target(self) -> None:
        """Successful status alone cannot certify a normalization run."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            benchmark.write_json(work / "peak-expectations.json", {"tone.aif": {"status": "analyzed"}})
            benchmark.write_json(work / "peak-before.json", {})
            row = {"path": str(work / "cpp/input/tone.aif"), **test_parity.result()}
            benchmark.write_json(benchmark.result_path(work, "peak", 1, "cpp"), [row])
            with pytest.raises(ValueError, match="missed the target"):
                benchmark.check(work, work, "cpp", "peak", 1)

    def test_reads_both_hyperfine_export_formats(self) -> None:
        """Hyperfine 1.x and 2.x store wall time statistics under different keys."""
        assert benchmark.wall_mean({"mean": 1.25}) == 1.25
        assert benchmark.wall_mean({"summary": {"time_wall_clock": {"mean": 2.5}}}) == 2.5
        with pytest.raises(KeyError):
            benchmark.wall_mean({"summary": {"time_cpu": {"mean": 0.25}}})

    def test_combines_hyperfine_metrics_with_unitless_counters(self) -> None:
        """Alternating measurements retain wall times and counters in both supported hyperfine formats."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            benchmark.write_json(work / "metadata.json", {"runs": 2})
            (work / "raw").mkdir()
            for run in (1, 2):
                results = [
                    {
                        "command": f"{implementation} peak jobs=1",
                        "measurements": [
                            {
                                "time_wall_clock": {"value": float(run), "unit": "second"},
                                "cpu_cycles": {"value": run * 100},
                                "exit_code": 0,
                            }
                        ],
                    }
                    for implementation in ("cpp", "rust")
                ]
                benchmark.write_json(work / f"raw/peak-jobs-1-run-{run}.json", {"results": results})
            benchmark.combine(work, work, "peak", 1)
            combined = json.loads((work / "peak-jobs-1.json").read_text(encoding="utf-8"))
            summary = combined["results"][0]["summary"]
            assert summary["time_wall_clock"]["mean"] == 1.5
            assert summary["cpu_cycles"]["mean"] == 150
            assert summary["cpu_cycles"]["unit"] == "count"

    def test_report_exports_metadata_and_ratio(self) -> None:
        """The combined report retains build settings and both direction ratios in readable output."""
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary).resolve()
            benchmark.write_json(work / benchmark.MARKER_NAME, {"workspace": str(work)})
            metadata = {"jobs": [1], "corpora": ["test"], "files": [], "runs": 3, "cpu": "test CPU"}
            benchmark.write_json(work / "metadata.json", metadata)
            for mode in benchmark.TARGETS:
                benchmark.write_json(work / f"{mode}-jobs-1.json", {"results": [{"mean": 2.0}, {"mean": 1.0}]})
                (work / f"{mode}-jobs-1.md").write_text("Hyperfine export\n", encoding="utf-8")
            benchmark.report(work, work)
            summary = json.loads((work / "summary.json").read_text(encoding="utf-8"))
            assert summary["metadata"] == metadata
            assert summary["benchmarks"][0]["rust_over_cpp"] == 0.5
            assert "Rust/C++ 0.500x, C++/Rust 2.000x" in (work / "summary.md").read_text(encoding="utf-8")


@unittest.skipIf(os.name == "nt", "Tests POSIX CMake release artifact paths")
class TestReleaseSelection(unittest.TestCase):
    """Checks that a newer debug binary cannot silently become a performance baseline."""

    def test_release_precedes_newer_debug_and_unknown_copy(self) -> None:
        """A verified release build wins regardless of the other binaries' modification times."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            rust = root / "rust"
            rust.mkdir()
            release = root / "cpp/cmake-build-mac-release/AudioBatchCli_artefacts/Release/audiobatch"
            debug = root / "cpp/cmake-build-mac-debug/AudioBatchCli_artefacts/Debug/audiobatch"
            copied = root / "cpp/audiobatch"
            for binary in (release, debug, copied):
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.touch(mode=0o755)
            os.utime(release, (1, 1))
            script = (
                'source "$1"; REPO="$2"; REPO_ROOT="$(dirname "$REPO")"; '
                "unset AUDIOBATCH_CPP_CLI AUDIOBATCH_CPP_BUILD_TYPE; find_cpp_cli release"
            )
            selected = subprocess.run(  # noqa: S603
                [BASH_EXECUTABLE, "-c", script, "test", str(benchmark.RUST_DIRECTORY / "common.sh"), str(rust)],
                text=True,
                capture_output=True,
                check=True,
            )
            assert Path(selected.stdout.strip()).resolve() == release

    def test_cmake_cache_overrules_misleading_directory_name(self) -> None:
        """A single-config Debug binary in a release-named folder is not a valid release baseline."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            build = root / "cmake-build-mac-release"
            binary = build / "AudioBatchCli_artefacts/audiobatch"
            binary.parent.mkdir(parents=True)
            binary.touch(mode=0o755)
            (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Debug\n", encoding="utf-8")
            script = 'source "$1"; AUDIOBATCH_CPP_CLI="$2"; unset AUDIOBATCH_CPP_BUILD_TYPE; find_cpp_cli release'
            selected = subprocess.run(  # noqa: S603
                [BASH_EXECUTABLE, "-c", script, "test", str(benchmark.RUST_DIRECTORY / "common.sh"), str(binary)],
                text=True,
                capture_output=True,
                check=False,
            )
            assert selected.returncode != 0

    def test_explicit_debug_is_rejected_even_with_release_attestation(self) -> None:
        """Attestation cannot override a known Debug build directory."""
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "Debug/audiobatch"
            binary.parent.mkdir()
            binary.touch(mode=0o755)
            script = 'source "$1"; AUDIOBATCH_CPP_CLI="$2"; AUDIOBATCH_CPP_BUILD_TYPE=Release; find_cpp_cli release'
            selected = subprocess.run(  # noqa: S603
                [BASH_EXECUTABLE, "-c", script, "test", str(benchmark.RUST_DIRECTORY / "common.sh"), str(binary)],
                text=True,
                capture_output=True,
                check=False,
            )
            assert selected.returncode != 0
            assert "release C++ CLI is required" in selected.stderr


class TestBuildTypeTraversal(unittest.TestCase):
    """Checks root termination with POSIX and native Windows path tools."""

    def test_unchanging_parent_terminates_build_type_search(self) -> None:
        """A native Windows dirname can return the drive root unchanged instead of a POSIX root."""
        bash = shutil.which("bash")
        if bash is None:
            self.fail("Bash is required to test build type traversal")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "audiobatch"
            binary.touch(mode=0o755)
            script = (
                'source "$1"; cd "$3"; ROOT=$(pwd -P); '
                'dirname() { printf "%s\\n" "$ROOT"; }; '
                'unset AUDIOBATCH_CPP_BUILD_TYPE; cpp_cli_build_type "$2"'
            )
            selected = subprocess.run(  # noqa: S603
                [
                    bash,
                    "-c",
                    script,
                    "test",
                    (benchmark.RUST_DIRECTORY / "common.sh").as_posix(),
                    binary.as_posix(),
                    root.as_posix(),
                ],
                text=True,
                capture_output=True,
                check=True,
                timeout=5,
            )
            assert selected.stdout.strip() == "unknown"


if __name__ == "__main__":
    unittest.main()
