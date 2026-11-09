# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Check bounded RealtimeSanitizer runner contracts without a sanitizer runtime or audio hardware.

Synthetic Python children exercise the real process runner and disposable result artifacts.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import realtime_sanitizer as runner

CALLBACK = "AudioBatchRealtimeCallback"
FATAL_EXIT_CODE = 43


def report(function: str = "malloc", *, stack: str = CALLBACK, kind: str = "unsafe-library-call") -> str:
    """Supply one representative sanitizer report, including the intercepted call and stack."""
    return (
        f"==123==ERROR: RealtimeSanitizer: {kind}\n"
        f"Intercepted call to real-time unsafe function `{function}` in real-time context!\n"
        f"    #0 {function} sanitizer_interceptors.cpp:10\n"
        f"    #1 {stack} callback.cpp:20\n"
        f"SUMMARY: RealtimeSanitizer: {kind}\n"
    )


class ClassificationContracts(unittest.TestCase):
    """Distinguish clean execution, intentional controls, blocked audits, and unrelated failures."""

    def check_outcome(self, outcome: str, returncode: int, output: str, **overrides: object) -> None:
        """Keep every test explicit about the execution result and the expected policy."""
        options = {"expected": [], "stacks": [], "required": [], "diagnostic": False, "control": False}
        options.update(overrides)
        actual, reason = runner.classify(returncode, output, **options)
        assert actual == outcome, f"{actual}: {reason}"
        assert reason

    def test_clean_requires_success_without_diagnostics(self) -> None:
        """An ordinary successful scenario is clean, but neither failure nor sanitizer text is."""
        self.check_outcome("CLEAN", 0, "Ran 1 test\nOK\n")
        for returncode, output in [(7, "scenario failed"), (0, report()), (1, report())]:
            with self.subTest(returncode=returncode, output=output):
                self.check_outcome("FAIL", returncode, output)

    def test_summary_or_truncated_header_is_not_clean(self) -> None:
        """A partial sanitizer diagnostic must not be mistaken for an absence of findings."""
        for output in ["SUMMARY: RealtimeSanitizer: unsafe-library-call\n", "ERROR: RealtimeSanitizer:\n"]:
            with self.subTest(output=output):
                self.check_outcome("FAIL", 0, output)

    def test_expected_fatal_report_is_blocked_or_control_never_clean(self) -> None:
        """Expected dependency findings and detector controls have distinct non-clean outcomes."""
        for control, outcome in [(False, "BLOCKED"), (True, "CONTROL")]:
            with self.subTest(control=control):
                self.check_outcome(
                    outcome, FATAL_EXIT_CODE, report(), expected=["malloc"], stacks=[CALLBACK], control=control
                )

    def test_fatal_expected_report_requires_the_configured_exit_code(self) -> None:
        """Fatal mode requires the sanitizer status, not success or an unrelated application failure."""
        for returncode in [0, 1, FATAL_EXIT_CODE - 1, FATAL_EXIT_CODE + 1]:
            with self.subTest(returncode=returncode):
                self.check_outcome("FAIL", returncode, report(), expected=["malloc"], stacks=[CALLBACK])

    def test_diagnostic_mode_requires_successful_completion(self) -> None:
        """Continuing diagnostics accept matching reports only when the scenario completes."""
        for returncode, outcome in [(0, "BLOCKED"), (1, "FAIL")]:
            with self.subTest(returncode=returncode):
                self.check_outcome(
                    outcome, returncode, report(), expected=["malloc"], stacks=[CALLBACK], diagnostic=True
                )
        self.check_outcome(
            "CONTROL", 0, report(), expected=["malloc"], stacks=[CALLBACK], diagnostic=True, control=True
        )

    def test_missing_expected_diagnostic_rejects_success_and_failure(self) -> None:
        """Neither exit status is sufficient proof that an expected detector actually ran."""
        for returncode in [0, 1]:
            with self.subTest(returncode=returncode):
                self.check_outcome("FAIL", returncode, "Ran 1 test\n", expected=["malloc"], stacks=[CALLBACK])

    def test_loader_failure_without_report_is_never_expected(self) -> None:
        """A loader failure cannot satisfy an expected sanitizer finding."""
        for output in [
            "dyld[123]: Library not loaded: @rpath/missing.dylib\n",
            "dyld[123]: Symbol not found: _missing_symbol\n",
            "The specified module could not be found.\n",
        ]:
            with self.subTest(output=output):
                self.check_outcome("FAIL", 1, output, expected=["malloc"], stacks=[CALLBACK])

    def test_loader_failure_cannot_hide_behind_expected_report(self) -> None:
        """A matching earlier finding must not mask a later loader failure."""
        for diagnostic, returncode in [(False, FATAL_EXIT_CODE), (True, 0)]:
            with self.subTest(diagnostic=diagnostic):
                self.check_outcome(
                    "FAIL",
                    returncode,
                    report() + "dyld[456]: Library not loaded: @rpath/missing.dylib\n",
                    expected=["malloc"],
                    stacks=[CALLBACK],
                    diagnostic=diagnostic,
                )

    def test_unrelated_signal_cannot_satisfy_expected_fatal_report(self) -> None:
        """An unrelated crash after a finding is not a successful fatal detector control."""
        self.check_outcome("FAIL", -signal.SIGSEGV, report(), expected=["malloc"], stacks=[CALLBACK], control=True)

    def test_expected_function_regex_is_a_full_match(self) -> None:
        """Patterns may intentionally name a family but cannot accept an accidental substring."""
        self.check_outcome(
            "BLOCKED", FATAL_EXIT_CODE, report("operator new"), expected=["operator (?:new|delete)"], stacks=[CALLBACK]
        )
        self.check_outcome("FAIL", FATAL_EXIT_CODE, report("malloc_extra"), expected=["malloc"], stacks=[CALLBACK])

    def test_every_expected_call_must_be_observed(self) -> None:
        """One expected function must not stand in for a second missing function."""
        self.check_outcome("FAIL", FATAL_EXIT_CODE, report(), expected=["malloc", "free"], stacks=[CALLBACK])
        self.check_outcome(
            "BLOCKED", 0, report() + report("free"), expected=["malloc", "free"], stacks=[CALLBACK], diagnostic=True
        )

    def test_expected_calls_cannot_be_satisfied_outside_reports(self) -> None:
        """Unrelated text before the first report must not supply a missing expected function."""
        output = "Intercepted call to real-time unsafe function `free`\n" + report()
        self.check_outcome("FAIL", FATAL_EXIT_CODE, output, expected=["malloc", "free"], stacks=[CALLBACK])

    def test_blocking_call_spelling_is_recognized(self) -> None:
        """Both supported sanitizer header and unsafe-call formats are recognized."""
        output = report("pthread_mutex_lock", kind="blocking-call").replace(
            "Intercepted call to real-time unsafe function `pthread_mutex_lock`",
            "Call to blocking function 'pthread_mutex_lock'",
        )
        self.check_outcome("BLOCKED", FATAL_EXIT_CODE, output, expected=["pthread_mutex_lock"], stacks=[CALLBACK])

    def test_every_report_requires_all_stack_constraints(self) -> None:
        """A valid first stack cannot authorize a report from a different callback."""
        valid = report(stack=f"{CALLBACK}\n    #2 ImageControl::processBlock")
        self.check_outcome(
            "BLOCKED", FATAL_EXIT_CODE, valid, expected=["malloc"], stacks=[CALLBACK, "ImageControl::processBlock"]
        )
        self.check_outcome(
            "FAIL", FATAL_EXIT_CODE, report(), expected=["malloc"], stacks=[CALLBACK, "ImageControl::processBlock"]
        )
        self.check_outcome(
            "FAIL",
            0,
            valid + report(stack="UnrelatedCallback"),
            expected=["malloc"],
            stacks=[CALLBACK],
            diagnostic=True,
        )

    def test_diagnostic_mode_rejects_every_unexpected_report(self) -> None:
        """Matching reports before and after a bad report never hide the unexpected finding."""
        for unexpected in [
            report("free"),
            report(kind="data-race"),
            report(stack="WrongCallback"),
            "ERROR: RealtimeSanitizer:\n",
            report().replace("Intercepted call to real-time unsafe function `malloc`", "Unknown unsafe call format"),
        ]:
            with self.subTest(unexpected=unexpected):
                self.check_outcome(
                    "FAIL", 0, report() + unexpected + report(), expected=["malloc"], stacks=[CALLBACK], diagnostic=True
                )

    def test_required_output_is_mandatory_and_multiline(self) -> None:
        """Explicit completion markers distinguish a silent clean child from an unexecuted scenario."""
        self.check_outcome("CLEAN", 0, "")
        self.check_outcome("FAIL", 0, "", required=["finished"])
        self.check_outcome("CLEAN", 0, "starting\nfinished 1 case\n", required=[r"^finished \d+ case$"])
        self.check_outcome("FAIL", 0, "starting\n", required=["finished"])
        self.check_outcome(
            "FAIL", FATAL_EXIT_CODE, report(), expected=["malloc"], stacks=[CALLBACK], required=["finished"]
        )

    def test_zero_selected_tests_is_never_a_pass(self) -> None:
        """Known executable test frameworks must execute at least one selected test."""
        for marker in ["Ran 0 tests in 0.000s", "[==========] 0 tests from 0 test suites ran.", "running 0 tests"]:
            for expected in [False, True]:
                with self.subTest(marker=marker, expected=expected):
                    self.check_outcome(
                        "FAIL",
                        int(expected),
                        marker + "\n" + (report() if expected else ""),
                        expected=["malloc"] if expected else [],
                        stacks=[CALLBACK] if expected else [],
                    )


class OptionContracts(unittest.TestCase):
    """Invalid or ambiguous policies fail before launching a child."""

    def arguments(self) -> list[str]:
        """Return a minimal invocation with no real executable or artifact access."""
        return ["--executable", sys.executable, "--case", "unit-case", "--artifacts", "unused-artifacts"]

    def test_defaults_and_exact_arguments(self) -> None:
        """Executable arguments are preserved verbatim, including flags and spaces."""
        options = runner.parse_options([*self.arguments(), "--argument=-c", "--argument=print('with spaces')"])
        assert options.timeout == runner.DEFAULT_TIMEOUT_SECONDS
        assert options.argument == ["-c", "print('with spaces')"]
        assert not options.expect
        assert not options.control
        assert not options.diagnostic
        assert runner.parse_options([*self.arguments(), "--timeout=180"]).timeout == 180

    def test_invalid_options_exit_with_usage_error(self) -> None:
        """Reject unbounded deadlines, incomplete expectations, malformed regexes, and path labels."""
        invalid = [
            ["--timeout=0"],
            ["--timeout=-1"],
            ["--timeout=180.1"],
            ["--timeout=nan"],
            ["--timeout=inf"],
            ["--timeout=not-a-number"],
            ["--expect=malloc"],
            ["--control"],
            ["--diagnostic"],
            ["--expect=[", "--stack=callback"],
            ["--expect=malloc", "--stack=["],
            ["--require-output=["],
            ["--case=../outside"],
            ["--case=."],
            ["--case=.."],
            ["--case=with spaces"],
            ["--case="],
            ["--unknown-option"],
        ]
        for arguments in invalid:
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()) as error:
                with self.assertRaises(SystemExit) as raised:  # noqa: PT027 - stdlib-only test executable
                    runner.parse_options(self.arguments() + arguments)
                assert raised.exception.code == 2
                assert "error:" in error.getvalue()

    def test_required_options_are_required(self) -> None:
        """The executable, label, and artifact root must be chosen explicitly."""
        for missing in ["--executable", "--case", "--artifacts"]:
            arguments = self.arguments()
            index = arguments.index(missing)
            del arguments[index : index + 2]
            with self.subTest(missing=missing), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:  # noqa: PT027 - stdlib-only test executable
                    runner.parse_options(arguments)
                assert raised.exception.code == 2


class BoundedProcessContracts(unittest.TestCase):
    """Exercise actual subprocess capture and process-group termination using Python only."""

    def test_exact_arguments_environment_streams_and_returncode(self) -> None:
        """The process runner preserves argv, environment, both streams, and a failing status."""
        source = (
            "import os, sys; print(repr(sys.argv[1:])); "
            "print(os.environ['AUDIOBATCH_TEST_RUNNER']); print('error stream', file=sys.stderr); sys.exit(7)"
        )
        result = runner.run_bounded(
            [sys.executable, "-c", source, "one argument with spaces", "--flag"],
            os.environ | {"AUDIOBATCH_TEST_RUNNER": "isolated value"},
            5,
        )
        assert result == (7, "['one argument with spaces', '--flag']\nisolated value\n", "error stream\n", False)

    @unittest.skipUnless(sys.platform == "darwin", "the sanitizer process-group runner targets macOS")
    def test_timeout_kills_the_entire_group_and_drains_streams(self) -> None:
        """An inherited child pipe must close promptly, not survive after only its parent dies."""
        child = "import time; print('child ready', flush=True); time.sleep(8); print('child survived', flush=True)"
        source = (
            "import subprocess, sys, time; "
            f"subprocess.Popen([sys.executable, '-u', '-c', {child!r}]); "
            "print('parent ready', flush=True); print('before timeout', file=sys.stderr, flush=True); time.sleep(8)"
        )
        started = time.monotonic()
        with mock.patch.object(runner.os, "killpg", wraps=os.killpg) as kill_group:
            returncode, stdout, stderr, timed_out = runner.run_bounded(
                [sys.executable, "-u", "-c", source], os.environ.copy(), 1
            )
        assert time.monotonic() - started < 5, "A descendant kept its inherited output pipe open past the deadline"
        assert timed_out
        assert returncode == -signal.SIGKILL
        assert "parent ready\n" in stdout
        assert "child ready\n" in stdout
        assert "child survived" not in stdout
        assert stderr == "before timeout\n"
        kill_group.assert_called_once()
        assert kill_group.call_args.args[1] == signal.SIGKILL


class ArtifactContracts(unittest.TestCase):
    """Run synthetic scenarios through the public entry point and inspect the retained evidence."""

    def setUp(self) -> None:
        """Allocate disposable paths with spaces, never production application storage."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-rtsan-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "artifacts with spaces"

    def invoke(self, source: str, *options: str) -> tuple[int, str, str, dict[str, object]]:
        """Use the current Python interpreter as an exact-argv scenario executable."""
        stdout, stderr = io.StringIO(), io.StringIO()
        arguments = [
            "--executable",
            sys.executable,
            "--case",
            "synthetic",
            "--argument=-c",
            f"--argument={source}",
            "--artifacts",
            str(self.root),
            *options,
        ]
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = runner.main(arguments)
        result = json.loads((self.root / "synthetic/result.json").read_text(encoding="utf-8"))
        assert result["command"] == [str(Path(sys.executable).resolve()), "-c", source]
        return status, stdout.getvalue(), stderr.getvalue(), result

    def test_clean_artifacts_and_environment_override(self) -> None:
        """The child receives a forced policy, not inherited suppressions or disabled checks."""
        source = "import os, sys; print(os.environ['RTSAN_OPTIONS']); print('ordinary stderr', file=sys.stderr)"
        with mock.patch.dict(os.environ, {"RTSAN_OPTIONS": "suppressions=unsafe.txt:verify_interceptors=false"}):
            status, stdout, stderr, result = self.invoke(source)
        assert status == 0
        assert stdout.startswith("CLEAN: synthetic:")
        assert stderr == ""
        assert result["outcome"] == "CLEAN"
        assert result["returncode"] == 0
        assert result["timed_out"] is False
        policy = result["rtsan_options"]
        assert policy == (
            "halt_on_error=true:abort_on_error=false:exitcode=43:"
            "symbolize=true:color=never:verify_interceptors=true:print_stats_on_exit=true"
        )
        assert (self.root / "synthetic/stdout.txt").read_text(encoding="utf-8") == policy + "\n"
        assert (self.root / "synthetic/stderr.txt").read_text(encoding="utf-8") == "ordinary stderr\n"

    def test_expected_control_blocked_and_diagnostic_artifacts(self) -> None:
        """Expected findings remain explicitly non-clean in console output and machine-readable artifacts."""
        for extra, returncode, outcome in [
            ([], FATAL_EXIT_CODE, "BLOCKED"),
            (["--control"], FATAL_EXIT_CODE, "CONTROL"),
            (["--diagnostic"], 0, "BLOCKED"),
        ]:
            with self.subTest(extra=extra):
                source = f"import sys; print({report()!r}, end='', file=sys.stderr); sys.exit({returncode})"
                status, stdout, stderr, result = self.invoke(source, "--expect=malloc", f"--stack={CALLBACK}", *extra)
                assert status == 0
                assert stdout.startswith(f"{outcome}: synthetic:")
                assert stderr == ""
                assert result["outcome"] == outcome
                assert result["returncode"] == returncode
                assert result["expected_calls"] == ["malloc"]
                assert result["expected_stacks"] == [CALLBACK]
                assert ("halt_on_error=false" in result["rtsan_options"]) == ("--diagnostic" in extra)
                assert (self.root / "synthetic/stderr.txt").read_text(encoding="utf-8") == report()

    def test_failure_retains_both_streams_and_nonzero_status(self) -> None:
        """A failed ordinary scenario leaves useful diagnostics and a failing wrapper status."""
        source = "import sys; print('scenario started'); print('loader failed', file=sys.stderr); sys.exit(9)"
        status, stdout, stderr, result = self.invoke(source)
        assert status == 1
        assert stdout.startswith("FAIL: synthetic:")
        assert "scenario started" in stderr
        assert "loader failed" in stderr
        assert result["outcome"] == "FAIL"
        assert result["returncode"] == 9
        assert (self.root / "synthetic/stdout.txt").read_text(encoding="utf-8") == "scenario started\n"
        assert (self.root / "synthetic/stderr.txt").read_text(encoding="utf-8") == "loader failed\n"

    @unittest.skipUnless(sys.platform == "darwin", "the sanitizer process-group runner targets macOS")
    def test_timeout_overrides_even_an_expected_diagnostic(self) -> None:
        """A hanging scenario never passes merely because it printed the expected finding first."""
        source = f"import time; print({report()!r}, end='', flush=True); time.sleep(8)"
        status, stdout, stderr, result = self.invoke(source, "--timeout=2", "--expect=malloc", f"--stack={CALLBACK}")
        assert status == 1
        assert stdout.startswith("FAIL: synthetic:")
        assert report() in stderr
        assert result["outcome"] == "FAIL"
        assert result["timed_out"] is True
        assert result["returncode"] == -signal.SIGKILL
        assert "deadline" in result["reason"]

    def test_launch_failure_still_writes_result_artifacts(self) -> None:
        """An absent executable is reported as a launch failure, never as an expected control."""
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            status = runner.main(
                [
                    "--executable",
                    str(self.root / "missing executable"),
                    "--case=missing",
                    "--artifacts",
                    str(self.root),
                    "--expect=malloc",
                    f"--stack={CALLBACK}",
                    "--control",
                ]
            )
        result = json.loads((self.root / "missing/result.json").read_text(encoding="utf-8"))
        assert status == 1
        assert result["outcome"] == "FAIL"
        assert result["returncode"] is None
        assert result["timed_out"] is False
        assert "launch" in result["reason"]
        assert (self.root / "missing/stdout.txt").read_text(encoding="utf-8") == ""
        assert "missing executable" in (self.root / "missing/stderr.txt").read_text(encoding="utf-8")

    def test_no_arguments_use_the_named_case(self) -> None:
        """Omitting explicit argv selects exactly the named scenario rather than every test."""
        with (
            mock.patch.object(runner, "run_bounded", return_value=(0, "one selected case\n", "", False)) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            status = runner.main(
                ["--executable", sys.executable, "--case=selected.case", "--artifacts", str(self.root)]
            )
        assert status == 0
        assert run.call_args.args[0] == [str(Path(sys.executable).resolve()), "--case", "selected.case"]


if __name__ == "__main__":
    unittest.main(verbosity=2)
