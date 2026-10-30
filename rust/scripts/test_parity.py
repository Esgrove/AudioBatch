#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0", "numpy>=2"]
# ///
"""
Lightweight adversarial tests for the parity oracles, without invoking either audio application.

Run with uv run --script scripts/test_parity.py from rust/.
Synthetic files hold only a few samples and physical ID3 frames, so no decoder or LFS checkout is required.
"""

import io
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from collections import Counter
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

import cli_benchmark as benchmark
import compare_levels as levels
import compare_outputs as outputs
from mutagen.aiff import AIFF
from mutagen.id3 import ID3


def result(*, status: str = "analyzed", duration: float = 1.0, frames: int | None = None) -> dict:
    """Builds a CLI-shaped result with independently adjustable status and stream length."""
    row = {
        "status": status,
        "sampleRate": 44100,
        "channels": 1,
        "bitsPerSample": 16,
        "durationSeconds": duration,
        "peakDbfs": -1.0,
        "truePeakDbtp": -1.0,
        "integratedLufs": -10.0,
        "maxShortTermLufs": -10.0,
    }
    if frames is not None:
        row["frameCount"] = frames
    return row


def frame(frame_id: bytes, payload: bytes, version: int = 4) -> bytes:
    """Builds one physical frame without allowing an ID3 library to merge it with another."""
    size = (
        outputs.encoded_synchsafe(len(payload))
        if version == outputs.ID3_SYNCHSAFE_VERSION
        else struct.pack(">I", len(payload))
    )
    return frame_id + size + b"\x00\x00" + payload


def tag(*frames: bytes, version: int = 4) -> bytes:
    """Builds a padding-free tag containing exactly the supplied physical frames."""
    body = b"".join(frames)
    return b"ID3" + bytes([version, 0, 0]) + outputs.encoded_synchsafe(len(body)) + body


def write_aiff(
    path: Path,
    *,
    samples: bytes = b"\x00\x01\x00\x02",
    frames: int = 2,
    bits: int = 16,
    metadata: bytes | None = None,
) -> None:
    """Writes a tiny mono AIFF with controlled bit depth, frame count, audio length, and duplicate tags."""
    common = struct.pack(">HIH", 1, frames, bits) + bytes.fromhex("400eac44000000000000")
    chunks = {b"COMM": common, b"SSND": bytes(8) + samples}
    if metadata:
        chunks[b"ID3 "] = metadata
    body = b"AIFF" + b"".join(
        key + struct.pack(">I", len(value)) + value + (b"\x00" if len(value) & 1 else b"")
        for key, value in chunks.items()
    )
    path.write_bytes(b"FORM" + struct.pack(">I", len(body)) + body)


class TestLevelOracle(unittest.TestCase):
    """Checks that agreement cannot override known outcomes or source stream lengths."""

    def test_shared_failure_of_valid_fixture_is_rejected(self) -> None:
        """A shared failure must not hide the loss of support for valid input."""
        expected = {"tone.wav": {"status": "analyzed"}}
        rows = {"tone.wav": result(status="failed")}
        self.assertTrue(levels.compare_results(rows, rows, expected, 0.05))

    def test_known_failure_is_required_and_success_is_rejected(self) -> None:
        """A deliberately corrupt fixture must fail rather than producing fabricated audio."""
        expected = {"broken.wav": {"status": "failed"}}
        failed = {"broken.wav": result(status="failed")}
        self.assertEqual(levels.compare_results(failed, failed, expected, 0.05), [])
        succeeded = {"broken.wav": result()}
        self.assertTrue(levels.compare_results(succeeded, succeeded, expected, 0.05))

    def test_results_omitted_by_both_implementations_fail(self) -> None:
        """Input discovery is checked against a pre-operation manifest."""
        self.assertTrue(levels.compare_results({}, {}, {"tone.wav": {"status": "analyzed"}}, 0.05))

    def test_duration_and_exposed_frame_count_differences_fail(self) -> None:
        """Stream lengths are checked separately from level tolerance."""
        expected = {"tone.aif": {"status": "analyzed", "durationSeconds": 1.0, "frameCount": 44100}}
        correct = {"tone.aif": result(frames=44100)}
        shortened = {"tone.aif": result(duration=0.9, frames=44099)}
        self.assertTrue(levels.compare_results(correct, shortened, expected, 0.05))
        self.assertTrue(levels.compare_results(shortened, shortened, expected, 0.05))
        self.assertEqual(levels.compare_results(correct, correct, expected, 0.05), [])

    def test_unknown_shared_failure_is_not_implicitly_whitelisted(self) -> None:
        """Unrecognized input defaults to expected success rather than agreement-only validation."""
        expected = levels.expected_results([Path("/other/new.wav")])
        rows = {next(iter(expected)): result(status="failed")}
        self.assertTrue(levels.compare_results(rows, rows, expected, 0.05))

    def test_fixture_normalization_outcomes_depend_on_target(self) -> None:
        """Silence, excess gain, and output conflicts have independent per-fixture outcomes."""
        root = Path("/fixtures/normalization")
        sources = [
            root / "edge_silence_stereo_44100_16_3s.wav",
            root / "edge_gain_above_limit_stereo_44100_16_2s.wav",
            root / "edge_shared_output_44100_16_2s.wav",
            root / "edge_shared_output_44100_16_2s.flac",
        ]
        peak = levels.expected_results(sources, root, peak=0)
        loudness = levels.expected_results(sources, root, lufs=-23)
        self.assertEqual(peak[sources[0].name]["status"], "failed")
        self.assertEqual(peak[sources[1].name]["status"], "failed")
        self.assertEqual(loudness[sources[1].with_suffix(".aif").name]["status"], "analyzed")
        self.assertEqual(peak[sources[2].name]["status"], "failed")
        self.assertEqual(peak[sources[3].with_suffix(".aif").name]["status"], "analyzed")

    def test_duplicate_json_rows_are_rejected(self) -> None:
        """A repeated result must not overwrite a failed or otherwise differing result."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "results.json"
            row = {"path": "/music/tone.wav", **result()}
            path.write_text(json.dumps([row, row]), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate result"):
                levels.load(path, None)

    def test_non_finite_measurements_do_not_pass(self) -> None:
        """NaN cannot bypass numeric comparisons by making every inequality false."""
        row = result(duration=float("nan"))
        row["peakDbfs"] = float("nan")
        self.assertTrue(levels.compare_rows("tone", row, row, 0.05))


class TestOutputOracle(unittest.TestCase):
    """Checks physical frame payloads, altered sample lengths, and original-source preservation."""

    def test_altered_lengths_fail_with_or_without_tolerance(self) -> None:
        """Truncation fails even with a permissive audio-difference tolerance."""
        with tempfile.TemporaryDirectory() as temporary:
            cpp, rust = Path(temporary) / "cpp.aif", Path(temporary) / "rust.aif"
            write_aiff(cpp)
            write_aiff(rust, samples=b"\x00\x01", frames=1)
            for tolerance in (None, 0.0):
                self.assertTrue(outputs.compare_outputs(cpp, rust, tolerance))

    def test_identical_truncation_in_both_outputs_is_rejected(self) -> None:
        """A shared mismatch of COMM and SSND cannot pass by byte equality."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "short.aif"
            write_aiff(path, samples=b"\x00\x01")
            self.assertTrue(outputs.compare_outputs(path, path, None))

    def test_duplicate_same_identity_payload_is_compared_before_merging(self) -> None:
        """Mutagen's merged view and raw ID counts can agree while duplicate payload multiplicities differ."""
        for frame_id, prefix in ((b"TIT2", b"\x03"), (b"PRIV", b"same-owner\x00")):
            with self.subTest(frame_id=frame_id):
                first = frame(frame_id, prefix + b"alpha")
                second = frame(frame_id, prefix + b"beta")
                cpp_tag = tag(first, first, second)
                rust_tag = tag(first, second, second)
                merged_cpp = Counter(outputs.frame_fields(item) for item in ID3(io.BytesIO(cpp_tag)).values())
                merged_rust = Counter(outputs.frame_fields(item) for item in ID3(io.BytesIO(rust_tag)).values())
                self.assertEqual(merged_cpp, merged_rust)
                self.assertEqual(outputs.raw_frame_ids({b"ID3 ": cpp_tag}), outputs.raw_frame_ids({b"ID3 ": rust_tag}))
                with tempfile.TemporaryDirectory() as temporary:
                    cpp, rust = Path(temporary) / "cpp.aif", Path(temporary) / "rust.aif"
                    write_aiff(cpp, metadata=cpp_tag)
                    write_aiff(rust, metadata=rust_tag)
                    self.assertTrue(outputs.compare_outputs(cpp, rust, None))

    def test_equivalent_text_encodings_and_frame_order_pass(self) -> None:
        """Decoded comparison still permits harmless text encoding and physical ordering differences."""
        utf8 = tag(frame(b"TIT2", b"\x03title"), frame(b"TPE1", b"\x03artist"))
        latin1 = tag(frame(b"TPE1", b"\x00artist"), frame(b"TIT2", b"\x00title"))
        self.assertEqual(outputs.decoded_frames(utf8), outputs.decoded_frames(latin1))

    def test_unknown_duplicate_payloads_and_v23_frames_are_retained(self) -> None:
        """Opaque and old-version frames remain visible to the preservation oracle."""
        version = 3
        first = tag(frame(b"XABC", b"first", version), frame(b"XABC", b"second", version), version=version)
        second = tag(frame(b"XABC", b"first", version), frame(b"XABC", b"first", version), version=version)
        self.assertNotEqual(outputs.decoded_frames(first), outputs.decoded_frames(second))

    def test_legacy_date_snapshot_respects_v24_conversion(self) -> None:
        """A source date split across legacy frames is compared with its combined v2.4 date."""
        legacy = tag(
            frame(b"TYER", b"\x002026", 3),
            frame(b"TDAT", b"\x002710", 3),
            version=3,
        )
        current = tag(frame(b"TDRC", b"\x032026-10-27"))
        self.assertEqual(outputs.decoded_frames(legacy, processed=True), outputs.decoded_frames(current))

    def test_relocated_known_fixture_retains_its_outcome_and_length(self) -> None:
        """A copied fixture passed as a direct file still uses its independent golden properties."""
        source = Path("/temporary/1/sine_stereo_44100_16.wav")
        expected = levels.expected_results([source], peak=-1)
        row = next(iter(expected.values()))
        self.assertEqual(row["frameCount"], 44100)
        self.assertEqual(row["status"], "analyzed")
        failed = levels.expected_results([Path("/temporary/1/broken.wav")])
        self.assertEqual(next(iter(failed.values()))["status"], "failed")

    def test_failed_original_snapshot_checks_bytes_and_filesystem_metadata(self) -> None:
        """Two implementations identically corrupting a failed source must both be rejected."""
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source = folder / "broken.wav"
            source.write_bytes(b"original metadata and audio")
            expected = {"broken.wav": {"status": "failed", "source": "broken.wav"}}
            snapshot = outputs.snapshot_folder(folder, expected)
            self.assertEqual(outputs.check_preservation(folder, snapshot), [])
            original_stat = source.stat()
            source.write_bytes(b"changed metadata and audio!")
            os.utime(source, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
            self.assertTrue(outputs.check_preservation(folder, snapshot))
            source.write_bytes(b"original metadata and audio")
            os.utime(source, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns + 10000000))
            self.assertTrue(outputs.check_preservation(folder, snapshot))

    def test_source_length_and_metadata_snapshot_rejects_shared_loss(self) -> None:
        """Output agreement cannot hide loss of an original frame or shortening of valid audio."""
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source = folder / "source.aif"
            metadata = tag(frame(b"TIT2", b"\x03original"), frame(b"PRIV", b"owner\x00payload"))
            write_aiff(source, metadata=metadata)
            expected = {"source.aif": {"status": "analyzed", "source": "source.aif", "frameCount": 2}}
            snapshot = json.loads(json.dumps(outputs.snapshot_folder(folder, expected)))
            self.assertEqual(outputs.check_preservation(folder, snapshot), [])
            write_aiff(source, metadata=tag(frame(b"TIT2", b"\x03original")), frames=1, samples=b"\x00\x01")
            differences = outputs.check_preservation(folder, snapshot)
            self.assertTrue(any("frames" in difference for difference in differences))
            self.assertTrue(any("metadata frame" in difference for difference in differences))

    def test_overview_is_the_only_omitted_serato_frame(self) -> None:
        """Stale overviews are intentionally omitted while other opaque Serato data is preserved."""
        overview = frame(b"GEOB", b"\x03application/octet-stream\x00\x00Serato Overview\x00pixels")
        markers = frame(b"GEOB", b"\x03application/octet-stream\x00\x00Serato Markers2\x00cues")
        self.assertEqual(
            outputs.decoded_frames(tag(overview, markers), processed=True), outputs.decoded_frames(tag(markers))
        )

    def test_artwork_snapshot_detects_shared_picture_loss(self) -> None:
        """Artwork bytes are independently retained as well as participating in physical ID3 comparison."""
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            source = folder / "cover.aif"
            picture = frame(b"APIC", b"\x03image/png\x00\x03front\x00picture bytes")
            write_aiff(source, metadata=tag(picture))
            self.assertEqual(len(AIFF(source).tags.getall("APIC")), 1)
            expected = {"cover.aif": {"status": "analyzed", "source": "cover.aif"}}
            snapshot = outputs.snapshot_folder(folder, expected)
            write_aiff(source)
            self.assertTrue(any("artwork" in difference for difference in outputs.check_preservation(folder, snapshot)))


class TestNormalizationMode(unittest.TestCase):
    """Checks integer PCM limits and mode plumbing without running audio applications or benchmarks."""

    def test_lossless_zero_one_and_two_steps_at_both_bit_depths(self) -> None:
        """Loudness allows one step per sample, while peak normalization remains exact at 16 and 24 bits."""
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            cpp, rust = Path(temporary) / "cpp.aif", Path(temporary) / "rust.aif"
            for bits in (16, 24):
                width = bits // 8
                write_aiff(cpp, bits=bits, samples=bytes(width * 2))
                for steps in (0, 1, 2):
                    samples = steps.to_bytes(width, "big", signed=True) + (-steps).to_bytes(width, "big", signed=True)
                    write_aiff(rust, bits=bits, samples=samples)
                    for mode in ("peak", "lufs"):
                        with self.subTest(bits=bits, steps=steps, mode=mode):
                            differences = outputs.compare_outputs(cpp, rust, None, mode)
                            self.assertEqual(bool(differences), steps > (1 if mode == "lufs" else 0))
                    with self.subTest(bits=bits, steps=steps, mode="default"):
                        self.assertEqual(bool(outputs.compare_outputs(cpp, rust, None)), steps > 0)

    def test_lossy_tolerance_does_not_depend_on_normalization_mode(self) -> None:
        """Lossy differences keep their dBFS threshold rather than inheriting the one-step lossless limit."""
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            cpp, rust = Path(temporary) / "cpp.aif", Path(temporary) / "rust.aif"
            for bits in (16, 24):
                width = bits // 8
                write_aiff(cpp, bits=bits, samples=bytes(width * 2))
                for steps, rejected in ((2, False), (1 << (bits - 2), True)):
                    write_aiff(rust, bits=bits, samples=steps.to_bytes(width, "big", signed=True) + bytes(width))
                    for mode in ("peak", "lufs"):
                        with self.subTest(bits=bits, steps=steps, mode=mode):
                            self.assertEqual(bool(outputs.compare_outputs(cpp, rust, -72.0, mode)), rejected)

    def test_loudness_tolerance_does_not_relax_structure_or_tags(self) -> None:
        """The one-step allowance cannot mask malformed samples, changed metadata, or SSND alignment bytes."""
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            cpp, rust = Path(temporary) / "cpp.aif", Path(temporary) / "rust.aif"
            write_aiff(cpp, metadata=tag(frame(b"TIT2", b"\x03original")))
            write_aiff(rust, samples=b"\x00\x02\x00\x03", metadata=tag(frame(b"TIT2", b"\x03changed")))
            self.assertTrue(
                any("frame" in difference for difference in outputs.compare_outputs(cpp, rust, None, "lufs"))
            )
            write_aiff(rust, samples=b"\x00\x02")
            self.assertTrue(outputs.compare_outputs(cpp, rust, None, "lufs"))
            _, chunks = outputs.read_chunks(cpp)
            aligned = {**chunks, b"SSND": struct.pack(">II", 2, 0) + b"\x00\x00" + chunks[b"SSND"][8:]}
            changed = {**aligned, b"SSND": aligned[b"SSND"][:8] + b"\x00\x01" + aligned[b"SSND"][10:]}
            self.assertTrue(outputs.compare_samples("aligned", aligned, changed, None, 1))

    def test_output_cli_propagates_mode_and_keeps_peak_default(self) -> None:
        """The parsed mode reaches lossless folder comparisons, with exact comparison unless lufs is selected."""
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            root = Path(temporary)
            cpp, rust = root / "cpp", root / "rust"
            cpp.mkdir()
            rust.mkdir()
            write_aiff(cpp / "tone.aif")
            write_aiff(rust / "tone.aif", samples=b"\x00\x02\x00\x03")
            lossy = root / "lossy.txt"
            lossy.write_text("", encoding="utf-8")
            for mode in (None, "peak", "lufs"):
                arguments = ["compare_outputs.py", str(cpp), str(rust), "--lossy", str(lossy)]
                if mode is not None:
                    arguments += ["--mode", mode]
                with self.subTest(mode=mode), patch.object(outputs.sys, "argv", arguments):
                    if mode == "lufs":
                        outputs.main()
                    else:
                        with self.assertRaises(SystemExit) as raised:
                            outputs.main()
                        self.assertEqual(raised.exception.code, 1)

    def test_benchmark_propagates_normalization_mode_to_output_oracle(self) -> None:
        """Each normalized benchmark pair passes its mode, while analysis invokes no output oracle."""
        for mode in benchmark.TARGETS:
            with (
                self.subTest(mode=mode),
                patch.object(benchmark, "workspace"),
                patch.object(benchmark.subprocess, "run") as run,
            ):
                benchmark.compare(Path("/work"), Path("/results"), mode, 1)
                calls = [call.args[0] for call in run.call_args_list]
                output_calls = [
                    command for command in calls if any(part.endswith("compare_outputs.py") for part in command)
                ]
                self.assertEqual(len(output_calls), int(mode != "analyze"))
                if output_calls:
                    command = output_calls[0]
                    self.assertEqual(command[command.index("--mode") + 1], mode)
                    self.assertIn("--lossy", command)
                    self.assertIn("--snapshots", command)

    @unittest.skipIf(os.name == "nt", "Exercises the Bash parity entry point")
    def test_parity_shell_propagates_selected_target_mode(self) -> None:
        """The real shell passes the active target mode using recording shims instead of cargo or audio CLIs."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rust = root / "rust"
            scripts = rust / "scripts"
            scripts.mkdir(parents=True)
            shutil.copy2(Path(__file__).with_name("parity.sh"), scripts)
            (rust / "common.sh").write_text(
                'find_cpp_cli() { printf "%s\\n" "$RUST_DIR/target/debug/audiobatch-rs"; }\n'
                'cpp_cli_build_type() { printf "release\\n"; }\n'
                'executable_name() { printf "%s\\n" "$1"; }\n'
                'require_command() { command -v "$1" > /dev/null; }\n'
                "print_yellow() { :; }\n",
                encoding="utf-8",
            )
            binary = rust / "target/debug/audiobatch-rs"
            binary.parent.mkdir(parents=True)
            binary.write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
            binary.chmod(0o755)
            tools = root / "tools"
            tools.mkdir()
            for name in ("cargo", "uv"):
                tool = tools / name
                tool.write_text(
                    '#!/bin/bash\nprintf "%s\\0" "${0##*/}" "$@" >> "$PARITY_RECORD"\n'
                    'printf "\\n" >> "$PARITY_RECORD"\n',
                    encoding="utf-8",
                )
                tool.chmod(0o755)
            source = root / "tone.wav"
            source.write_bytes(b"original")
            record = root / "record"
            environment = {
                **os.environ,
                "PATH": f"{tools}{os.pathsep}{os.environ['PATH']}",
                "PARITY_RECORD": str(record),
            }
            for target, mode in (([], "peak"), (["--peak", "-1"], "peak"), (["--lufs", "-14"], "lufs")):
                record.write_text("", encoding="utf-8")
                with self.subTest(target=target):
                    subprocess.run(
                        ["bash", str(scripts / "parity.sh"), "--normalize", *target, str(source)],
                        check=True,
                        capture_output=True,
                        env=environment,
                    )
                    calls = [line.rstrip("\0").split("\0") for line in record.read_text(encoding="utf-8").splitlines()]
                    comparisons = [command for command in calls if "--mode" in command]
                    self.assertEqual(len(comparisons), 1)
                    command = comparisons[0]
                    self.assertTrue(command[4].endswith("compare_outputs.py"))
                    self.assertEqual(command[command.index("--mode") + 1], mode)
                    self.assertIn("--lossy", command)
                    self.assertIn("--snapshots", command)
            self.assertEqual(source.read_bytes(), b"original")


if __name__ == "__main__":
    unittest.main()
