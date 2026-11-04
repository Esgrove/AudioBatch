# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil"]
# ///
"""
Executable-level CLI contracts shared by both implementations.

Run with uv run --script tests/cli_integration.py --cli /absolute/path/to/cli.
Each case uses copied fixtures and an explicit application-storage root.
No invocation normalizes committed fixtures or uses the system trash.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave
from contextlib import closing
from pathlib import Path
from typing import ClassVar

import psutil

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures"
TONE = "sine_stereo_44100_16.wav"
TIMEOUT_SECONDS = 30
FIELDS = [
    "path",
    "name",
    "format",
    "sampleRate",
    "channels",
    "bitsPerSample",
    "durationSeconds",
    "peakDbfs",
    "truePeakDbtp",
    "integratedLufs",
    "maxShortTermLufs",
    "plrDb",
    "psrDb",
    "crestFactorDb",
    "dcOffsets",
    "clipCounts",
    "clipsTotal",
    "stereoCorrelation",
    "status",
    "error",
]


class CliIntegration(unittest.TestCase):
    """Test process boundaries without duplicating the numerical parity matrix."""

    executable: ClassVar[Path]
    artifacts: ClassVar[Path | None] = None

    def setUp(self) -> None:
        """Create isolated audio, working-directory, and persistent-storage trees."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-cli-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.audio = self.root / "Ääni ja kuva"
        self.audio.mkdir()
        self.storage = self.root / "storage"
        self.database = self.storage / "AudioBatch" / "analysis.db"
        self.invocations: list[dict[str, object]] = []
        self.addCleanup(self.save_failure_artifacts)

    def save_failure_artifacts(self) -> None:
        """Retain command diagnostics when unittest records a case failure."""
        if self.artifacts is None or self._outcome is None or self._outcome.result is None:
            return
        result = self._outcome.result
        if not any(
            test is self or getattr(test, "test_case", None) is self for test, _ in [*result.failures, *result.errors]
        ):
            return
        destination = self.artifacts / self._testMethodName
        destination.mkdir(parents=True, exist_ok=True)
        (destination / "commands.json").write_text(
            json.dumps(self.invocations, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        if self.storage.exists():
            shutil.copytree(self.storage, destination / "storage", dirs_exist_ok=True)

    def copy_fixture(self, name: str = TONE, destination: str | None = None) -> Path:
        """Copy a committed fixture under this case's disposable audio directory."""
        output = self.audio / (destination or name)
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(FIXTURES / name, output)
        return output

    def invoke(
        self,
        *arguments: str | Path,
        expected: int = 0,
        cwd: Path | None = None,
        data_root: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Run a bounded child with isolated storage and capture both streams separately."""
        environment = os.environ.copy()
        environment.update(
            AUDIOBATCH_DATA_ROOT=str(self.storage) if data_root is None else data_root,
            AUDIOBATCH_DELETE="0",
            NO_COLOR="1",
            CLICOLOR="0",
            CLICOLOR_FORCE="0",
            TERM="dumb",
        )
        command = [str(self.executable), *(str(argument) for argument in arguments)]
        entry: dict[str, object] = {"command": command, "cwd": str(cwd or self.audio)}
        self.invocations.append(entry)
        try:
            result = subprocess.run(  # noqa: S603
                command,
                cwd=cwd or self.audio,
                env=environment,
                capture_output=True,
                encoding="utf-8",
                errors="strict",
                timeout=TIMEOUT_SECONDS,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            entry.update(timeout=True, stdout=str(error.stdout), stderr=str(error.stderr))
            self.fail(f"Process timed out: {command!r}\n{error}")
        entry.update(exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
        assert result.returncode == expected, json.dumps(entry, ensure_ascii=False, indent=2)
        assert "\x1b[" not in result.stdout, "Machine-readable/nonterminal output must not contain ANSI escapes"
        return result

    def analyze(self, *paths: str | Path, refresh: bool = False, expected: int = 0) -> list[dict[str, object]]:
        """Parse the actual executable's complete JSON stream, rejecting stray diagnostics."""
        arguments = ["--output", "json", "--jobs", "1", "--sort", "name"]
        if refresh:
            arguments.append("--refresh")
        result = self.invoke(*arguments, *paths, expected=expected)
        records = json.loads(result.stdout)
        assert isinstance(records, list)
        assert self.database.is_file(), "The CLI must be rebuilt with AUDIOBATCH_DATA_ROOT support before testing"
        for record in records:
            assert set(record) == set(FIELDS)
        return records

    def database_row(self, path: Path) -> dict[str, object]:
        """Read persistent identity and measurement state after a child has exited."""
        assert self.database.is_file(), f"Missing isolated database: {self.database}"
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.row_factory = sqlite3.Row
            row = connection.execute("SELECT * FROM file_analysis WHERE file_path = ?", (str(path),)).fetchone()
        assert row is not None, f"No cached result for {path}"
        return dict(row)

    def mark_cache_timestamp(self, path: Path) -> None:
        """Set a persistence sentinel without changing source identity or measured values."""
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.execute("UPDATE file_analysis SET updated_at_ms = 1 WHERE file_path = ?", (str(path),))

    def test_help_version_and_usage_streams(self) -> None:
        """Help/version avoid application storage and usage errors leave stdout empty."""
        help_result = self.invoke("--help")
        assert "--normalize" in help_result.stdout
        assert "--output" in help_result.stdout
        assert help_result.stderr == ""
        version = self.invoke("--version")
        assert version.stdout.strip()
        assert version.stderr == ""
        assert not self.storage.exists(), "Help/version must not initialize cache or logging"
        invalid = self.invoke("--definitely-not-an-option", expected=1)
        assert invalid.stdout == ""
        assert "--definitely-not-an-option" in invalid.stderr
        invalid_target = self.invoke("--peak", "-1", expected=1)
        assert invalid_target.stdout == ""
        assert "--normalize" in invalid_target.stderr

    def test_invalid_storage_root_fails_closed(self) -> None:
        """An invalid explicit root must not send any writes to normal application storage."""
        source = self.copy_fixture()
        for root in ("relative-storage", "", str(source)):
            with self.subTest(root=root):
                result = self.invoke("--output", "json", source, data_root=root, expected=1)
                assert result.stdout == ""
                assert "AUDIOBATCH_DATA_ROOT" in result.stderr
                assert not (self.audio / "relative-storage").exists()
                assert not self.storage.exists()

    def test_source_metric_fields_and_cache_roundtrip(self) -> None:
        """Both executable front ends expose independent source statistics without gain projections."""
        oracle = json.loads((FIXTURES / "metrics_expected.json").read_text())
        for expected in oracle["fixtures"]:
            with self.subTest(fixture=expected["fixture"]):
                source = self.copy_fixture(expected["fixture"])
                record = self.analyze(source)[0]
                assert record["clipCounts"] == expected["clipCounts"]
                assert record["clipsTotal"] == sum(expected["clipCounts"])
                for field in ("plrDb", "psrDb", "crestFactorDb", "stereoCorrelation"):
                    actual, reference = record[field], expected[field]
                    if reference is None:
                        assert actual is None
                    else:
                        tolerance = 0.006001 if field != "stereoCorrelation" else 0.000000501
                        assert abs(actual - reference) <= tolerance
                assert len(record["dcOffsets"]) == len(expected["dcOffsets"])
                assert all(
                    abs(actual - reference) <= 0.000000501
                    for actual, reference in zip(record["dcOffsets"], expected["dcOffsets"], strict=True)
                )
                row = self.database_row(source)
                assert row["analysis_version"] == 14
                assert (row["stereo_correlation"] is None) == (expected["stereoCorrelation"] is None)
                cached = self.analyze(source)[0]
                assert cached | {"status": record["status"]} == record
                csv_rows = list(csv.DictReader(io.StringIO(self.invoke("--output", "csv", source).stdout)))
                assert json.loads(csv_rows[0]["clipCounts"]) == expected["clipCounts"]
                assert json.loads(csv_rows[0]["dcOffsets"]) == cached["dcOffsets"]

    def test_structured_mixed_results_and_exit_status(self) -> None:
        """Successful and failed files survive JSON/CSV output with diagnostics on stderr."""
        tone = self.copy_fixture(destination='Łódź, "テスト".wav' if os.name != "nt" else "Łódź, テスト.wav")
        broken = self.copy_fixture("broken.wav")
        records = self.analyze(tone, broken, expected=2)
        assert {record["path"] for record in records} == {str(tone), str(broken)}
        by_name = {record["name"]: record for record in records}
        assert by_name[tone.name]["status"] == "analyzed"
        assert by_name[tone.name]["peakDbfs"] == -6.02
        assert by_name[tone.name]["sampleRate"] == 44100
        assert by_name[broken.name]["status"] == "failed"
        assert by_name[broken.name]["error"]
        assert broken.name in str(self.invocations[-1]["stderr"])
        result = self.invoke("--refresh", "--output", "csv", tone, broken, expected=2)
        reader = csv.DictReader(io.StringIO(result.stdout))
        assert reader.fieldnames == FIELDS
        rows = list(reader)
        assert len(rows) == 2
        assert {row["name"] for row in rows} == {tone.name, broken.name}
        assert all(None not in row for row in rows), "CSV fields must remain correctly escaped"
        assert broken.name in result.stderr

    def test_discovery_relative_unicode_duplicates_and_recursion(self) -> None:
        """The entry point resolves cwd/default roots and preserves discovery semantics."""
        first = self.copy_fixture(destination="A Łódź テスト.wav")
        second = self.copy_fixture("sine_mono_24000_16.wav", "nested/B child.wav")
        (self.audio / "ignored.txt").write_text("not audio", encoding="utf-8")
        records = self.analyze()
        assert [record["path"] for record in records] == [str(first)]
        records = self.analyze(".", first.name, first)
        assert [record["path"] for record in records] == [str(first)]
        result = self.invoke("--recurse", "--sort", "name", "--output", "json", ".", first.name)
        records = json.loads(result.stdout)
        assert [record["name"] for record in records] == sorted([first.name, second.name], key=str.casefold)
        assert {record["path"] for record in records} == {str(first), str(second)}

    def test_restart_cache_reuse_identity_invalidation_and_refresh(self) -> None:
        """Separate processes reuse cached measurements and replace them only when required."""
        source = self.copy_fixture()
        cold = self.analyze(source)[0]
        assert cold["status"] == "analyzed"
        initial = self.database_row(source)
        assert initial["file_size"] == source.stat().st_size
        self.mark_cache_timestamp(source)
        warm = self.analyze(source)[0]
        assert warm["status"] == "cached"
        assert self.database_row(source)["updated_at_ms"] == 1
        assert warm["peakDbfs"] == cold["peakDbfs"]
        source_stat = source.stat()
        os.utime(source, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns + 2_000_000_000))
        changed = self.analyze(source)[0]
        assert changed["status"] == "analyzed"
        updated = self.database_row(source)
        assert updated["modified_time_ms"] != initial["modified_time_ms"]
        assert updated["updated_at_ms"] != 1
        self.mark_cache_timestamp(source)
        refreshed = self.analyze(source, refresh=True)[0]
        assert refreshed["status"] == "analyzed"
        assert self.database_row(source)["updated_at_ms"] != 1

    def test_loudness_target_and_true_peak_ceiling(self) -> None:
        """Loudness mode reaches a safe target while respecting the shared true-peak ceiling."""
        source = self.copy_fixture()
        result = self.invoke("--normalize", "--delete", "--lufs", "-14", "--output", "json", source)
        record = json.loads(result.stdout)[0]
        assert abs(float(record["integratedLufs"]) + 14.0) <= 0.02
        assert float(record["truePeakDbtp"]) <= -0.29
        assert Path(str(record["path"])).is_file()
        assert not source.exists()

    def test_force_preserves_existing_output_until_explicitly_requested(self) -> None:
        """An output collision fails without force and replaces it only with an explicit force option."""
        source = self.copy_fixture(destination="force.wav")
        output = self.copy_fixture("sine_stereo_44100_16.aif", "force.aif")
        original = output.read_bytes()
        rejected = self.invoke("--normalize", "--delete", "--peak", "-1", "--output", "json", source, expected=2)
        assert json.loads(rejected.stdout)[0]["status"] == "failed"
        assert source.exists()
        assert output.read_bytes() == original
        forced = self.invoke("--normalize", "--delete", "--force", "--peak", "-1", "--output", "json", source)
        assert json.loads(forced.stdout)[0]["peakDbfs"] == -1.0
        assert output.exists()
        assert output.read_bytes() != original
        assert not source.exists()
        assert list(self.audio.iterdir()) == [output]

    def test_list_and_completion_leave_machine_output_clean(self) -> None:
        """An isolated empty plugin list never scans installed plugins and completions need no storage."""
        listed = self.invoke("list")
        assert listed.stdout == ""
        assert "No plugins found" in listed.stderr
        completion_root = self.root / "completion-only"
        for shell in ("bash", "zsh", "fish", "powershell"):
            with self.subTest(shell=shell):
                result = self.invoke("completion", shell, data_root=str(completion_root))
                assert result.stdout.strip()
                assert "normalize" in result.stdout
                assert result.stderr == ""
        assert not completion_root.exists()

    def test_gain_refusal_and_shared_output_name_keep_originals(self) -> None:
        """Near-silent inputs and colliding batch destinations fail without losing originals."""
        silent = self.copy_fixture("silence_stereo_44100_16.wav", "silent.wav")
        original = silent.read_bytes()
        result = self.invoke("--normalize", "--delete", "--output", "json", silent, expected=2)
        assert json.loads(result.stdout)[0]["status"] == "failed"
        assert silent.read_bytes() == original
        assert not silent.with_suffix(".aif").exists()
        quiet = self.audio / "near-silent.wav"
        with wave.open(str(quiet), "wb") as audio:
            audio.setparams((2, 2, 44100, 44100, "NONE", "not compressed"))
            audio.writeframes(struct.pack("<hhhh", 1, 1, -1, -1) * 22050)
        result = self.invoke("--normalize", "--delete", "--output", "json", quiet, expected=2)
        assert "40 dB" in str(json.loads(result.stdout)[0]["error"])
        assert quiet.exists()
        assert not quiet.with_suffix(".aif").exists()
        first = self.copy_fixture(destination="shared.wav")
        second = self.copy_fixture("sine_stereo_44100_16.flac", "shared.flac")
        result = self.invoke("--normalize", "--delete", "--output", "json", first, second, expected=2)
        records = json.loads(result.stdout)
        assert len(records) == 2
        assert sorted(record["status"] for record in records) == ["analyzed", "failed"]
        assert first.with_suffix(".aif").exists()
        assert sum(path.exists() for path in (first, second)) == 1

    def test_in_place_aif_keeps_path_and_cleans_temporary_outputs(self) -> None:
        """AIFF normalization publishes at the original path and retains no transaction files."""
        source = self.copy_fixture("sine_stereo_44100_16.aif")
        result = self.invoke("--normalize", "--delete", "--peak", "-1", "--output", "json", source)
        record = json.loads(result.stdout)[0]
        assert record["path"] == str(source)
        assert record["peakDbfs"] == -1.0
        assert list(self.audio.iterdir()) == [source]
        assert self.analyze(source, refresh=True)[0]["peakDbfs"] == -1.0

    def long_source(self) -> Path:
        """Create a sparse thirty-second PCM input so cancellation cannot race a one-second decode."""
        source = self.audio / "interrupt.wav"
        rate = 96000
        frames = rate * 30
        size = frames * 4
        with source.open("wb") as audio:
            audio.write(
                struct.pack(
                    "<4sI4s4sIHHIIHH4sI",
                    b"RIFF",
                    size + 36,
                    b"WAVE",
                    b"fmt ",
                    16,
                    1,
                    2,
                    rate,
                    rate * 4,
                    4,
                    16,
                    b"data",
                    size,
                )
            )
            audio.write(struct.pack("<hh", 16384, -16384) * rate)
            audio.seek(44 + size - 4)
            audio.write(struct.pack("<hh", 16384, -16384))
        return source

    def interrupt_run(self, source: Path, *, normalize: bool, twice: bool = False) -> tuple[str, str]:
        """Interrupt a live isolated child at a file-system readiness boundary, with bounded cleanup."""
        environment = os.environ.copy()
        environment.update(AUDIOBATCH_DATA_ROOT=str(self.storage), NO_COLOR="1", TERM="dumb")
        command = [str(self.executable), "--jobs", "1", "--output", "json"]
        if normalize:
            command += ["--normalize", "--delete", "--peak", "-1"]
        command.append(str(source))
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        with subprocess.Popen(  # noqa: S603
            command,
            cwd=self.audio,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            creationflags=flags,
        ) as child:
            stdout: list[str] = []
            stderr: list[str] = []

            def drain(stream: io.TextIOBase, output: list[str]) -> None:
                """Drain pipes concurrently so a verbose debug child cannot deadlock before readiness."""
                output.append(stream.read())

            readers = [
                threading.Thread(target=drain, args=(child.stdout, stdout)),
                threading.Thread(target=drain, args=(child.stderr, stderr)),
            ]
            for reader in readers:
                reader.start()
            deadline = time.monotonic() + TIMEOUT_SECONDS
            try:
                while child.poll() is None:
                    ready = (
                        any(path != source for path in self.audio.iterdir())
                        if normalize
                        else any(Path(file.path) == source for file in psutil.Process(child.pid).open_files())
                    )
                    if ready:
                        break
                    if time.monotonic() >= deadline:
                        self.fail("Child did not reach the requested cancellation boundary")
                    threading.Event().wait(0.005)
                assert child.poll() is None, "Child completed before the interruption boundary"
                child.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
                if twice:
                    # Two separate deliveries exercise the force-exit path without assuming cleanup is slow.
                    threading.Event().wait(0.001)
                    if child.poll() is None:
                        child.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
                assert child.wait(timeout=TIMEOUT_SECONDS) == 130
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=TIMEOUT_SECONDS)
                for reader in readers:
                    reader.join(timeout=TIMEOUT_SECONDS)
                    assert not reader.is_alive()
            text = ("".join(stdout), "".join(stderr))
            self.invocations.append(
                {"command": command, "exit_code": child.returncode, "stdout": text[0], "stderr": text[1]}
            )
            return text

    def test_ctrl_c_analysis_cancels_cleanly(self) -> None:
        """The first interrupt drains analysis without touching the input or printing partial JSON."""
        source = self.long_source()
        original = source.stat().st_size
        stdout, _ = self.interrupt_run(source, normalize=False)
        assert stdout == ""
        assert source.stat().st_size == original
        assert list(self.audio.iterdir()) == [source]

    def test_repeated_ctrl_c_exits_with_interrupted_status(self) -> None:
        """Repeated interrupts cannot hang shutdown or turn cancellation into a crash status."""
        source = self.long_source()
        self.interrupt_run(source, normalize=False, twice=True)
        assert source.exists()

    def test_ctrl_c_normalization_keeps_original_and_removes_temporary_output(self) -> None:
        """Interrupt only after rendering starts, proving temporary output cleanup rather than startup exit."""
        source = self.long_source()
        self.analyze(source)
        original = source.stat().st_size
        self.interrupt_run(source, normalize=True)
        assert source.stat().st_size == original
        assert list(self.audio.iterdir()) == [source]

    def test_normalization_disposal_output_identity_and_restart(self) -> None:
        """CLI options reach the writer/disposer and persist the committed output, not its source."""
        source = self.copy_fixture(destination="Łódź テスト.wav")
        self.analyze(source)
        result = self.invoke("--normalize", "--delete", "--peak", "-1", "--output", "json", source)
        records = json.loads(result.stdout)
        assert len(records) == 1
        output = Path(str(records[0]["path"]))
        assert output.parent == source.parent
        assert output.suffix.lower() in {".aif", ".aiff"}
        assert output.is_file()
        assert not source.exists()
        assert records[0]["peakDbfs"] == -1.0
        assert records[0]["status"] == "analyzed"
        cached = self.database_row(output)
        assert cached["file_size"] == output.stat().st_size
        with closing(sqlite3.connect(self.database)) as connection, connection:
            count = connection.execute(
                "SELECT count(*) FROM file_analysis WHERE file_path = ?", (str(source),)
            ).fetchone()
        assert count == (0,)
        restarted = self.analyze(output)[0]
        assert restarted["status"] == "cached"
        assert restarted["peakDbfs"] == -1.0
        reanalyzed = self.analyze(output, refresh=True)[0]
        assert reanalyzed["status"] == "analyzed"
        assert reanalyzed["peakDbfs"] == -1.0, "Decode the committed PCM rather than trust the writer's cached result"
        assert self.database_row(output)["file_size"] == output.stat().st_size
        assert list(self.audio.iterdir()) == [output], "No unfinished output files may remain"


def main() -> int:
    """Configure one executable and run the portable process-contract suite."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True, help="Built C++ or Rust CLI executable")
    parser.add_argument("--artifacts", type=Path, help="Keep per-case diagnostics after a failure")
    arguments, test_arguments = parser.parse_known_args()
    CliIntegration.executable = arguments.cli.resolve(strict=True)
    CliIntegration.artifacts = arguments.artifacts.resolve() if arguments.artifacts else None
    program = unittest.main(argv=[sys.argv[0], *test_arguments], verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
