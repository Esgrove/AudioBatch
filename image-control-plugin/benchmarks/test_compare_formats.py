# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest>=8,<10"]
# ///
"""Exercise AIFF parsing, comparison failure, every-render checks, determinism, and counterbalanced scheduling."""

from __future__ import annotations

import argparse
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import compare_formats as comparator
import pytest

SAMPLE_RATE = bytes.fromhex("400eac44000000000000")
OTHER_SAMPLE_RATE = bytes.fromhex("400ebb80000000000000")


def chunk(kind: bytes, payload: bytes) -> bytes:
    """Encode a chunk with the required odd-length padding."""
    return kind + struct.pack(">I", len(payload)) + payload + b"\0" * (len(payload) & 1)


def form(*chunks: bytes) -> bytes:
    """Wrap chunks in an exactly sized AIFF FORM."""
    payload = b"AIFF" + b"".join(chunks)
    return b"FORM" + struct.pack(">I", len(payload)) + payload


def common(channels: int = 2, frames: int = 2, bits: int = 16, rate: bytes = SAMPLE_RATE) -> bytes:
    """Encode COMM independently so each metadata field can be varied in malformed fixtures."""
    return struct.pack(">hIh", channels, frames, bits) + rate


def audio(
    samples: tuple[int, ...] = (1, -2, 3, -4),
    *,
    metadata: bytes | None = None,
    offset: bytes = b"",
    extra: bytes = b"",
) -> bytes:
    """Make tiny independently encoded PCM fixtures, including misleading SSND text in an unrelated chunk."""
    metadata = common() if metadata is None else metadata
    sound = struct.pack(">II", len(offset), 0) + offset + struct.pack(f">{len(samples)}h", *samples)
    return form(chunk(b"NAME", b"SSND text"), chunk(b"COMM", metadata), extra, chunk(b"SSND", sound))


class AiffContracts(unittest.TestCase):
    """Validate container boundaries, COMM metadata, and PCM equality without installed plugins or builds."""

    def setUp(self) -> None:
        """Create an isolated directory for tiny synthetic files."""
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-comparison-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def write(self, name: str, data: bytes) -> Path:
        """Write an AIFF fixture and return its path."""
        destination = self.root / name
        destination.write_bytes(data)
        return destination

    def test_chunk_walk_padding_offset_and_ssnd_after_metadata(self) -> None:
        """Ignore embedded SSND identifiers, odd metadata chunks, and the declared sound offset."""
        path = self.write("valid.aif", audio(offset=b"padding", extra=chunk(b"ANNO", b"odd")))
        parsed = comparator.read_audio(path)
        assert parsed.samples == (1, -2, 3, -4)
        assert parsed.sample_rate == 44_100
        assert (parsed.channels, parsed.frames, parsed.bits) == (2, 2, 16)

    def test_ssnd_can_precede_comm(self) -> None:
        """Chunk order is not fixed, but each required chunk must exist exactly once."""
        common = struct.pack(">hIh", 1, 1, 16) + SAMPLE_RATE
        sound = struct.pack(">IIh", 0, 0, -42)
        path = self.write("reordered.aif", form(chunk(b"SSND", sound), chunk(b"COMM", common)))
        assert comparator.read_pcm(path) == [-42]

    def test_malformed_containers_fail(self) -> None:
        """Reject truncation, duplicate/missing required chunks, bad offsets, and non-AIFF containers."""
        valid = audio()
        common = chunk(b"COMM", struct.pack(">hIh", 2, 2, 16) + SAMPLE_RATE)
        invalid = {
            "empty": b"",
            "wrong form": b"RIFF" + valid[4:],
            "compressed form": valid[:8] + b"AIFC" + valid[12:],
            "short form": valid[:-1],
            "trailing data": valid + b"trailing",
            "truncated chunk header": form(b"SSND"),
            "truncated chunk payload": form(b"COMM" + struct.pack(">I", 100) + b"short"),
            "missing padding": form(chunk(b"ANNO", b"odd")[:-1]),
            "missing COMM": form(chunk(b"SSND", b"\0" * 16)),
            "missing SSND": form(common),
            "duplicate COMM": form(common, common, chunk(b"SSND", b"\0" * 16)),
            "duplicate SSND": form(common, chunk(b"SSND", b"\0" * 16), chunk(b"SSND", b"\0" * 16)),
            "short COMM": form(chunk(b"COMM", b"short"), chunk(b"SSND", b"\0" * 16)),
            "short SSND": form(common, chunk(b"SSND", b"short")),
            "bad offset": form(common, chunk(b"SSND", struct.pack(">II", 100, 0))),
            "short PCM": audio(samples=(1, 2)),
            "long PCM": audio(samples=(1, 2, 3, 4, 5)),
        }
        for name, data in invalid.items():
            with self.subTest(name=name), pytest.raises(ValueError, match=r"invalid[.]aif:"):
                comparator.read_audio(self.write("invalid.aif", data))

    def test_invalid_comm_metadata_fails(self) -> None:
        """Reject unsupported channels/precision, empty audio, and invalid extended-precision sample rates."""
        for data in [
            audio(metadata=common(channels=0)),
            audio(metadata=common(channels=3)),
            audio(metadata=common(frames=0)),
            audio(metadata=common(bits=24)),
            audio(metadata=common(rate=b"\0" * 10)),
            audio(metadata=common(rate=bytes.fromhex("c00eac44000000000000"))),
            audio(metadata=common(rate=bytes.fromhex("7fffac44000000000000"))),
            audio(metadata=common(rate=bytes.fromhex("400e0000000000000000"))),
        ]:
            with self.subTest(data=data), pytest.raises(ValueError, match=r"invalid[.]aif:"):
                comparator.read_audio(self.write("invalid.aif", data))

    def test_compare_rejects_metadata_mismatch_with_same_samples(self) -> None:
        """Sample count equality must not hide different rates, channel counts, or frame counts."""
        first = self.write("first.aif", audio())
        for data in [audio(metadata=common(rate=OTHER_SAMPLE_RATE)), audio(metadata=common(channels=1, frames=4))]:
            with self.subTest(data=data), pytest.raises(ValueError, match="COMM metadata"):
                comparator.compare(first, self.write("second.aif", data))

    def test_compare_ignores_metadata_but_fails_any_pcm_difference(self) -> None:
        """Equal audio may carry different descriptive chunks, while even one integer PCM step is a failure."""
        first = self.write("first.aif", audio())
        same = self.write("same.aif", audio(extra=chunk(b"ANNO", b"different tags")))
        changed = self.write("changed.aif", audio(samples=(1, -2, 4, -4)))
        assert comparator.compare(first, same) == (4, 0, 0)
        assert comparator.compare(first, changed) == (4, 1, 1)
        with pytest.raises(ValueError, match="1/4 samples differ"):
            comparator.require_identical(first, changed, "cross-format")


class RenderContracts(unittest.TestCase):
    """Record render order and inject mismatches without running AudioBatch or touching installed plugins."""

    def setUp(self) -> None:
        """Keep all fake renders isolated and silence benchmark progress."""
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-renders-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.calls: list[str] = []
        self.outputs: list[Path] = []
        self.presets = {name: Path(name) for name in comparator.FORMATS}
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))

    def render(self, _cli: str, preset: Path, work: Path, _jobs: int) -> tuple[float, dict[str, Path]]:
        """Produce distinct files for every fake render so overwrite bugs cannot hide nondeterminism."""
        self.calls.append(preset.name)
        work.mkdir(parents=True)
        outputs = {}
        for name in comparator.INPUTS:
            output = work / f"{name}.aif"
            output.write_bytes(audio())
            outputs[name] = output
            self.outputs.append(output)
        return 1.0, outputs

    def test_counterbalances_and_compares_every_render(self) -> None:
        """Each format leads twice, each run is compared, and all repetitions are compared to their first render."""
        with (
            patch.object(comparator, "render", side_effect=self.render),
            patch.object(comparator, "require_identical", wraps=comparator.require_identical) as compare,
        ):
            timings = comparator.benchmark_runs("cli", self.presets, self.root, 4, 1)
        assert self.calls == ["VST3", "AU", "AU", "VST3", "VST3", "AU", "AU", "VST3"]
        assert timings == {"VST3": [1.0] * 4, "AU": [1.0] * 4}
        assert len(set(self.outputs)) == len(self.outputs)
        assert compare.call_count == len(comparator.INPUTS) * (4 + 2 * 3)

    def test_early_pair_mismatch_is_not_hidden_by_later_success(self) -> None:
        """Fail on a first-run format difference even if later renders would have matched."""

        def mismatching_render(cli: str, preset: Path, work: Path, jobs: int) -> tuple[float, dict[str, Path]]:
            elapsed, outputs = self.render(cli, preset, work, jobs)
            if len(self.calls) == 2:
                outputs[comparator.INPUTS[0]].write_bytes(audio(samples=(1, -2, 3, -5)))
            return elapsed, outputs

        with (
            patch.object(comparator, "render", side_effect=mismatching_render),
            pytest.raises(ValueError, match="run 1"),
        ):
            comparator.benchmark_runs("cli", self.presets, self.root, 4, 1)
        assert self.calls == ["VST3", "AU"]

    def test_repeated_cross_format_match_must_still_be_deterministic(self) -> None:
        """Identical drift in both formats must not pass merely because each pair agrees."""

        def drifting_render(cli: str, preset: Path, work: Path, jobs: int) -> tuple[float, dict[str, Path]]:
            elapsed, outputs = self.render(cli, preset, work, jobs)
            if len(self.calls) > 2:
                for path in outputs.values():
                    path.write_bytes(audio(samples=(9, -2, 3, -4)))
            return elapsed, outputs

        with (
            patch.object(comparator, "render", side_effect=drifting_render),
            pytest.raises(ValueError, match="AU run 2"),
        ):
            comparator.benchmark_runs("cli", self.presets, self.root, 4, 1)

    def test_pcm_mismatch_has_unsuccessful_command_exit(self) -> None:
        """The public command entry point must exit unsuccessfully on real comparison mismatches."""
        first = self.root / "first.aif"
        second = self.root / "second.aif"
        first.write_bytes(audio())
        second.write_bytes(audio(samples=(2, -2, 3, -4)))
        with (
            patch.object(comparator, "main", side_effect=lambda: comparator.require_identical(first, second, "PCM")),
            pytest.raises(SystemExit) as raised,
        ):
            comparator.entry_point()
        assert raised.value.code != 0
        assert "samples differ" in str(raised.value.code)

    def test_cli_aliases_preserve_executable_and_typed_counts(self) -> None:
        """Parsing aliases does not require installed formats, generated inputs, or a preset writer."""
        parser = comparator.build_parser()
        short = parser.parse_args(["-e", "CLI with spaces", "-r", "6", "-j", "2"])
        long = parser.parse_args(["--cli", "CLI with spaces", "--runs", "6", "--jobs", "2"])
        assert short == long
        assert short == argparse.Namespace(cli="CLI with spaces", runs=6, jobs=2)

    def test_cli_rejects_invalid_run_and_job_counts(self) -> None:
        """No empty run, absent repeat check, unbalanced run order, or zero jobs may report success."""
        for arguments in [["--runs", "0"], ["--runs", "-1"], ["--runs", "1"], ["--runs", "3"], ["--jobs", "0"]]:
            with (
                self.subTest(arguments=arguments),
                patch.object(sys, "argv", ["compare_formats.py", *arguments]),
                contextlib.redirect_stderr(io.StringIO()),
                pytest.raises(SystemExit) as raised,
            ):
                comparator.main()
            assert raised.value.code != 0
        with pytest.raises(argparse.ArgumentTypeError):
            comparator.positive_integer("0")


if __name__ == "__main__":
    unittest.main()
