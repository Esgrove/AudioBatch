# /// script
# requires-python = ">=3.11"
# dependencies = []
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
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from contextlib import closing
from pathlib import Path
from typing import ClassVar

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
