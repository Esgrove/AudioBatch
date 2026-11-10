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
import base64
import csv
import io
import json
import os
import shutil
import signal
import sqlite3
import string
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
from unittest.mock import patch
from xml.sax.saxutils import quoteattr

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
    test_plugin: ClassVar[Path | None] = None

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

    def plugin_preset(
        self, gain: float = 0.5, width: float = 0.5, latency: int = 256, *, named: bool = False
    ) -> Path | str:
        """Write each host's native preset independently for the repository's known test effect, without conversion."""
        repository = FIXTURES.parent
        candidates = [
            self.test_plugin,
            Path(os.environ["AUDIOBATCH_TEST_VST3"]) if "AUDIOBATCH_TEST_VST3" in os.environ else None,
            repository
            / "test-plugin/cmake-build-release/AudioBatchTestPlugin_artefacts/Release/VST3/AudioBatch Test MS.vst3",
            repository
            / "test-plugin/cmake-build-debug/AudioBatchTestPlugin_artefacts/Debug/VST3/AudioBatch Test MS.vst3",
            repository / "test-plugin/build/AudioBatchTestPlugin_artefacts/Release/VST3/AudioBatch Test MS.vst3",
            repository
            / "cpp/cmake-build-mac-debug/test-plugin/AudioBatchTestPlugin_artefacts/Debug/VST3/AudioBatch Test MS.vst3",
        ]
        bundle = next((candidate.resolve() for candidate in candidates if candidate and candidate.is_dir()), None)
        assert bundle is not None, "Build AudioBatch Test MS or set AUDIOBATCH_TEST_VST3 before CLI contracts"

        # JUCE's public XML-in-binary plugin state envelope. The component accepts this without private wrapper data.
        def binary_xml(text: str) -> bytes:
            """Encode the test plugin's documented parameter tree in JUCE's little-endian state envelope."""
            encoded = text.encode("utf-8")
            return struct.pack("<II", 0x21324356, len(encoded)) + encoded + b"\0"

        component = binary_xml(
            f'<AudioBatchTestMS><PARAM id="gain" value="{gain}"/><PARAM id="width" value="{width}"/>'
            f'<PARAM id="latency" value="{latency}"/></AudioBatchTestMS>'
        )
        rust = self.executable.name.startswith("audiobatch-rs")
        name = "CLI Test MS"
        if rust:
            folder = self.storage / "AudioBatch-rs/chain-presets"
            path = folder / f"{name}.json" if named else self.root / "preset.json"
            preset = {
                "name": name,
                "version": 1,
                "slots": [
                    {
                        "plugin": {
                            "name": "AudioBatch Test MS",
                            "class_id": "ABCDEF019182FAEB4162747341626D73",
                            "bundle": str(bundle),
                        },
                        "enabled": True,
                        "component_state": base64.b64encode(component).decode(),
                        "controller_state": "",
                    }
                ],
            }
            text = json.dumps(preset)
        else:
            # JUCE's host envelope uses its historical little-endian six-bit memory-block encoding internally.
            alphabet = "." + string.ascii_uppercase + string.ascii_lowercase + string.digits + "+"
            bits = int.from_bytes(component, "little")
            packed = f"{len(component)}." + "".join(
                alphabet[(bits >> shift) & 63] for shift in range(0, len(component) * 8, 6)
            )
            state = base64.b64encode(
                binary_xml(f"<VST3PluginState><IComponent>{packed}</IComponent></VST3PluginState>")
            ).decode()
            unique_id = 0
            for word in (0xABCDEF01, 0x9182FAEB, 0x41627473, 0x41626D73):
                unique_id = (unique_id * 31 + word) & 0xFFFFFFFF
            folder = self.storage / "AudioBatch/settings/Chain Presets"
            path = folder / f"{name}.xml" if named else self.root / "preset.xml"
            text = (
                f'<PLUGIN_CHAIN_PRESET name="{name}" version="1"><PLUGIN_CHAIN><SLOT enabled="1" state="{state}">'
                f'<PLUGIN name="AudioBatch Test MS" format="VST3" file={quoteattr(str(bundle))} '
                f'uniqueId="{unique_id:x}" '
                'numInputs="2" numOutputs="2"/></SLOT></PLUGIN_CHAIN></PLUGIN_CHAIN_PRESET>'
            )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return name if named else path

    def test_nonfinite_pcm_fails_cleanly_and_reuses_cached_failure(self) -> None:
        """NaN and both infinities in either channel must never publish successful metrics or rendered audio."""
        for label, value in [("nan", float("nan")), ("positive-inf", float("inf")), ("negative-inf", -float("inf"))]:
            for channel in range(2):
                with self.subTest(value=label, channel=channel):
                    source = self.audio / f"{label}-{channel}.wav"
                    samples = [0.25, -0.25] * 4410
                    samples[20 + channel] = value
                    payload = struct.pack(f"<{len(samples)}f", *samples)
                    source.write_bytes(
                        struct.pack(
                            "<4sI4s4sIHHIIHH4sI",
                            b"RIFF",
                            len(payload) + 36,
                            b"WAVE",
                            b"fmt ",
                            16,
                            3,
                            2,
                            44100,
                            44100 * 8,
                            8,
                            32,
                            b"data",
                            len(payload),
                        )
                        + payload
                    )
                    original = source.read_bytes()
                    first = self.analyze(source, expected=2)[0]
                    assert first["status"] == "failed"
                    assert "Non-finite" in first["error"]
                    assert "NaN" not in json.dumps(first)
                    assert "Infinity" not in json.dumps(first)
                    self.mark_cache_timestamp(source)
                    cached = self.analyze(source, expected=2)[0]
                    assert cached["status"] == "failed"
                    assert self.database_row(source)["updated_at_ms"] == 1
                    for output_format in ["json", "csv", "table"]:
                        result = self.invoke("-nFD", "-o", output_format, source, expected=2)
                        assert "Non-finite" in result.stderr
                        assert source.read_bytes() == original
                        assert not source.with_suffix(".aif").exists()

    def test_process_matches_independent_plugin_outputs(self) -> None:
        """Actual executable rendering restores native preset state, folds mono, and compensates latency exactly."""
        cases = [
            ("stereo_split_44100_16.wav", 0.5, 0.5, 0, "ms_gain_0.5_width_0.5_stereo_split_44100_16.wav"),
            ("stereo_split_44100_16.wav", 1, 0, 0, "ms_gain_1_width_0_stereo_split_44100_16.wav"),
            ("stereo_split_44100_16.wav", 1, 2, 0, "ms_gain_1_width_2_stereo_split_44100_16.wav"),
            ("sine_mono_44100_16.wav", 0.5, 1, 0, "ms_gain_0.5_width_1_sine_mono_44100_16.wav"),
            ("stereo_split_44100_16.wav", 0.5, 0.5, 256, "ms_gain_0.5_width_0.5_latency_256_stereo_split_44100_16.wav"),
        ]
        for index, (fixture, gain, width, latency, expected) in enumerate(cases):
            with self.subTest(expected=expected):
                source = self.copy_fixture(fixture, f"case{index}.wav")
                preset = self.plugin_preset(gain, width, latency, named=index == 0)
                result = self.invoke(
                    "process", "--preset", preset, "--delete", "--jobs", "2", "--output", "json", source
                )
                records = json.loads(result.stdout)
                assert len(records) == 1, result.stdout
                assert records[0]["status"] == "analyzed", result.stdout
                output = source.with_suffix(".aif")
                assert records[0]["path"] == str(output)
                assert not source.exists()
                data = output.read_bytes()
                position = data.index(b"SSND")
                size = struct.unpack_from(">I", data, position + 4)[0]
                offset = struct.unpack_from(">I", data, position + 8)[0]
                samples = data[position + 16 + offset : position + 8 + size]
                with wave.open(str(FIXTURES / "plugin_expected" / expected), "rb") as reader:
                    expected_pcm = reader.readframes(reader.getnframes())
                swapped = bytearray(samples)
                swapped[0::2], swapped[1::2] = samples[1::2], samples[0::2]
                assert swapped == expected_pcm, "Rendered PCM must match the independent plugin oracle exactly"
                self.database_row(output)
                fresh = self.analyze(output, refresh=True)[0]
                assert fresh | {"status": records[0]["status"]} == records[0]

    def test_process_recurse_prechain_gain_and_conflicts(self) -> None:
        """Recurse/jobs reach the common discovery service and normalization runs before, never after, the chain."""
        preset = self.plugin_preset(0.5, 1, 0)
        mono = self.copy_fixture("sine_mono_44100_16.wav", "nested/mono.wav")
        result = self.invoke("process", "-p", preset, "-rDnj2", "--peak", "0", "-o", "json", self.audio)
        rows = json.loads(result.stdout)
        assert len(rows) == 1
        assert rows[0]["peakDbfs"] == -6.02, "Half gain must follow normalization to 0 dBFS"
        assert mono.with_suffix(".aif").exists()
        loudness = self.copy_fixture("sine_mono_44100_16.wav", "loudness.wav")
        result = self.invoke("process", "-p", preset, "-Dn", "--lufs", "-20", "-o", "json", loudness)
        assert abs(json.loads(result.stdout)[0]["integratedLufs"] - (-26.02)) < 0.01
        first = self.copy_fixture(destination="conflict.wav")
        second = self.copy_fixture("sine_stereo_44100_16.flac", "conflict.flac")
        result = self.invoke("process", "-p", preset, "-Dj2", "-o", "json", first, second, expected=2)
        rows = json.loads(result.stdout)
        assert len(rows) == 2
        assert sum(row["status"] == "failed" for row in rows) == 1
        assert "Another selected file" in result.stdout

    def test_process_preserves_source_metadata(self) -> None:
        """Native preset rendering uses the production metadata copier before publishing its AIFF output."""
        preset = self.plugin_preset(0.5, 1, 0)
        source = self.copy_fixture("tagged_stereo_44100_16.flac", "tagged.flac")
        self.invoke("process", "-p", preset, "-D", "-o", "json", source)
        data = source.with_suffix(".aif").read_bytes()
        assert b"ID3 " in data
        for frame, text in [(b"TIT2", b"Test Tone"), (b"TPE1", b"AudioBatch"), (b"TALB", b"Fixtures")]:
            assert frame in data
            assert text in data

    def test_process_custom_gain_refusal_and_mixed_failures(self) -> None:
        """Persisted gain overrides pre-chain normalization, refusal uses unity, and unreadable files stay visible."""
        preset = self.plugin_preset(0.5, 1, 0)
        source = self.copy_fixture("sine_mono_44100_16.wav", "custom.wav")
        self.analyze(source)
        with closing(sqlite3.connect(self.database)) as connection, connection:
            connection.execute(
                "UPDATE file_analysis SET has_custom_gain = 1, custom_gain_db = -6.020599913279624 WHERE file_path = ?",
                (str(source),),
            )
        result = self.invoke("process", "-p", preset, "-Dn", "--peak", "0", "-o", "json", source)
        assert abs(json.loads(result.stdout)[0]["peakDbfs"] - (-18.06)) < 0.01
        silent = self.copy_fixture("silence_stereo_44100_16.wav", "silent.wav")
        broken = self.copy_fixture("broken.wav")
        result = self.invoke("process", "-p", preset, "-Dn", "-o", "json", silent, broken, expected=2)
        rows = json.loads(result.stdout)
        assert len(rows) == 2
        assert sum(row["status"] == "failed" for row in rows) == 1
        assert silent.with_suffix(".aif").exists()
        assert broken.exists()
        assert "Not normalized before the plugins" in result.stderr

    def test_process_rejects_damaged_slots_without_touching_files(self) -> None:
        """Never render a recovered partial chain, even with force and source deletion requested."""
        source = self.copy_fixture("sine_mono_44100_16.wav")
        output = source.with_suffix(".aif")
        output.write_bytes(b"existing output must survive")
        original = source.read_bytes()
        for damage in ["description", "state", "enabled"]:
            with self.subTest(damage=damage):
                preset = Path(self.plugin_preset())
                if preset.suffix == ".json":
                    value = json.loads(preset.read_text())
                    damaged = json.loads(json.dumps(value["slots"][0]))
                    if damage == "description":
                        damaged.pop("plugin")
                    elif damage == "state":
                        damaged["component_state"] = "not base64!"
                    else:
                        damaged["enabled"] = "garbage"
                    value["slots"].append(damaged)
                    preset.write_text(json.dumps(value))
                else:
                    text = preset.read_text()
                    damaged = (
                        '<SLOT enabled="1"/>'
                        if damage == "description"
                        else (text[text.index("<SLOT ") : text.index("</SLOT>") + len("</SLOT>")])
                    )
                    if damage == "state":
                        start = damaged.index('state="') + len('state="')
                        end = damaged.index('"', start)
                        damaged = damaged[:start] + "not base64!" + damaged[end:]
                    elif damage == "enabled":
                        damaged = damaged.replace('enabled="1"', 'enabled="garbage"')
                    preset.write_text(text.replace("</PLUGIN_CHAIN>", damaged + "</PLUGIN_CHAIN>"))
                result = self.invoke("process", "-p", preset, "-FD", "-o", "json", source, expected=1)
                assert "preset" in result.stderr.lower()
                assert source.read_bytes() == original
                assert output.read_bytes() == b"existing output must survive"

    def test_process_validation_and_output_safety(self) -> None:
        """Invalid arguments/presets do not modify sources, and force/delete do not require normalization."""
        source = self.copy_fixture()
        for arguments in [
            ("process",),
            ("process", "--preset", "missing"),
            ("process", "--preset", "missing", "--peak", "-1"),
            ("process", "--preset", "missing", "-n", "--peak", "-1", "--lufs", "-14"),
            ("process", "--preset", "missing", "--jobs", "0"),
        ]:
            self.invoke(*arguments, source, expected=1)
            assert source.exists()
            assert not source.with_suffix(".aif").exists()
        preset = self.plugin_preset(1, 1, 0)
        output = source.with_suffix(".aif")
        output.write_bytes(b"sentinel")
        self.invoke("process", "-p", preset, "-D", "-o", "json", source, expected=2)
        assert source.exists()
        assert output.read_bytes() == b"sentinel"
        result = self.invoke("process", "-p", preset, "-FD", "-o", "csv", source)
        assert next(iter(csv.DictReader(io.StringIO(result.stdout))))["path"] == str(output)
        assert not source.exists()
        self.invoke("process", "-p", preset, "-D", output)
        native = Path(preset)
        native.write_text(
            '{"name":"Empty", "version":1, "slots":[]}'
            if native.suffix == ".json"
            else '<PLUGIN_CHAIN_PRESET version="1"><PLUGIN_CHAIN/></PLUGIN_CHAIN_PRESET>'
        )
        unchanged = output.read_bytes()
        self.invoke("process", "-p", native, "-D", output, expected=1)
        assert output.read_bytes() == unchanged
        native.write_text("not a preset")
        self.invoke("process", "-p", native, "-D", output, expected=1)
        assert output.read_bytes() == unchanged

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
                assert row["analysis_version"] == 16
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
                assert "process" in result.stdout
                assert "preset" in result.stdout
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

    def interrupt_run(  # noqa: PLR0913 - Independent subprocess controls keep each cancellation boundary explicit.
        self,
        source: Path,
        *,
        normalize: bool,
        twice: bool = False,
        preset: Path | str | None = None,
        additional: tuple[Path, ...] = (),
        output_format: str = "json",
        ready_for: Path | None = None,
    ) -> tuple[str, str]:
        """Interrupt a live isolated child at a file-system readiness boundary, with bounded cleanup."""
        environment = os.environ.copy()
        environment.update(AUDIOBATCH_DATA_ROOT=str(self.storage), NO_COLOR="1", TERM="dumb")
        command = [str(self.executable)]
        if preset is not None:
            command += ["process", "--preset", str(preset), "--delete"]
        command += ["--jobs", "1", "--output", output_format, "--sort", "name"]
        if normalize:
            command += ["--normalize", "--delete", "--peak", "-1"]
        command.extend(str(path) for path in (source, *additional))
        baseline = set(self.audio.rglob("*"))
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
                        (
                            any(path not in baseline for path in self.audio.rglob("*"))
                            and (
                                ready_for is None
                                or any(
                                    ready_for.stem in path.name and path not in baseline
                                    for path in self.audio.rglob("*")
                                )
                            )
                        )
                        if normalize or preset is not None
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

    def test_ctrl_c_processing_keeps_original_and_removes_temporary_outputs(self) -> None:
        """Interrupt a real plugin render after temporary-file creation and drain lifecycle work without deadlock."""
        preset = self.plugin_preset(0.5, 1, 256)
        source = self.long_source()
        self.analyze(source)
        size = source.stat().st_size
        stdout, _ = self.interrupt_run(source, normalize=False, preset=preset)
        assert json.loads(stdout) == []
        assert source.stat().st_size == size
        assert list(self.audio.iterdir()) == [source]

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

    def test_ctrl_c_multifile_normalization_reports_only_completed_work(self) -> None:
        """Interrupted JSON and CSV keep committed outputs and genuine failures, never cancelled/unstarted rows."""
        for output_format in ["json", "csv"]:
            with self.subTest(output_format=output_format):
                folder = self.audio / output_format
                folder.mkdir()
                committed = self.copy_fixture(destination=f"{output_format}/a-committed.wav")
                failure = self.copy_fixture("broken.wav", f"{output_format}/b-failed.wav")
                active = self.long_source()
                active.rename(folder / "c-active.wav")
                active = folder / "c-active.wav"
                unstarted = self.copy_fixture(destination=f"{output_format}/d-unstarted.wav")
                self.analyze(committed, failure, active, unstarted, expected=2)
                stdout, stderr = self.interrupt_run(
                    committed,
                    normalize=True,
                    additional=(failure, active, unstarted),
                    output_format=output_format,
                    ready_for=active,
                )
                rows = json.loads(stdout) if output_format == "json" else list(csv.DictReader(io.StringIO(stdout)))
                assert {Path(row["path"]).name for row in rows} == {"a-committed.aif", "b-failed.wav"}
                assert {row["status"] for row in rows} == {"analyzed", "failed"}
                assert "Analysis failed" in stderr
                assert "b-failed.wav" in stderr
                assert "Normalization failed" not in stderr
                assert "not processed" not in stderr
                assert not committed.exists()
                assert committed.with_suffix(".aif").exists()
                assert active.exists()
                assert unstarted.exists()
                assert set(folder.iterdir()) == {committed.with_suffix(".aif"), failure, active, unstarted}

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


class RunnerOptionContracts(unittest.TestCase):
    """Check runner aliases without launching an audio application or resolving executable paths."""

    def test_aliases_preserve_typed_paths_and_unittest_passthrough(self) -> None:
        """Only owned runner options are consumed, including when unittest arguments follow a terminator."""
        parser = build_parser()
        for passthrough in (["-v", "CliIntegration.test_help"], ["--", "-v", "-negative-test-name"]):
            short = parser.parse_known_args(
                ["-e", "CLI path", "-p", "plugin path", "-A", "artifact path", *passthrough]
            )
            long = parser.parse_known_args(
                [
                    "--cli",
                    "CLI path",
                    "--test-plugin",
                    "plugin path",
                    "--artifacts",
                    "artifact path",
                    *passthrough,
                ]
            )
            assert short == long
            assert short[0] == argparse.Namespace(
                cli=Path("CLI path"), test_plugin=Path("plugin path"), artifacts=Path("artifact path")
            )
            assert short[1] == passthrough


class PluginDiscoveryContracts(unittest.TestCase):
    """Find the test effect in the standalone build script's documented output directories."""

    def test_standalone_build_directories_are_discovered(self) -> None:
        """A clean checkout needs neither the legacy build directory nor an explicit bundle override."""
        for profile in ["Release", "Debug"]:
            with (
                self.subTest(profile=profile),
                tempfile.TemporaryDirectory(prefix="audiobatch-plugin-discovery-") as name,
            ):
                root = Path(name).resolve()
                bundle = (
                    root
                    / f"test-plugin/cmake-build-{profile.lower()}/AudioBatchTestPlugin_artefacts"
                    / profile
                    / "VST3/AudioBatch Test MS.vst3"
                )
                bundle.mkdir(parents=True)
                case = CliIntegration("test_process_matches_independent_plugin_outputs")
                case.root = root
                case.storage = root / "storage"
                case.executable = root / "audiobatch-rs"
                case.test_plugin = None
                with patch(f"{__name__}.FIXTURES", root / "fixtures"), patch.dict(os.environ, {}, clear=True):
                    preset = case.plugin_preset()
                assert isinstance(preset, Path)
                contents = json.loads(preset.read_text(encoding="utf-8"))
                assert contents["slots"][0]["plugin"]["bundle"] == str(bundle)


def build_parser() -> argparse.ArgumentParser:
    """Describe runner options separately from the unittest arguments passed through unchanged."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-e", "--cli", type=Path, required=True, help="Built C++ or Rust CLI executable")
    parser.add_argument("-p", "--test-plugin", type=Path, help="Built AudioBatch Test MS VST3 bundle")
    parser.add_argument("-A", "--artifacts", type=Path, help="Keep per-case diagnostics after a failure")
    return parser


def main() -> int:
    """Configure one executable and run the portable process-contract suite."""
    arguments, test_arguments = build_parser().parse_known_args()
    CliIntegration.executable = arguments.cli.resolve(strict=True)
    CliIntegration.test_plugin = arguments.test_plugin.resolve(strict=True) if arguments.test_plugin else None
    CliIntegration.artifacts = arguments.artifacts.resolve() if arguments.artifacts else None
    program = unittest.main(argv=[sys.argv[0], *test_arguments], verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
