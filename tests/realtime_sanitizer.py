# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Run one bounded RealtimeSanitizer scenario and retain its exact diagnostics.

Clean scenarios reject every violation. Detector controls and blocked audits must match named unsafe calls and stacks.
Expected dependency findings are reported as BLOCKED, never as a clean real-time result.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
from pathlib import Path

ERROR_HEADER = re.compile(r"^.*ERROR: RealtimeSanitizer: ([\w-]+).*?$", re.MULTILINE)
UNSAFE_CALL = re.compile(
    r"(?:Intercepted call to real-time unsafe function|Call to blocking function) [`']([^`']+)[`']"
)
DEFAULT_TIMEOUT_SECONDS = 30.0
MAX_TIMEOUT_SECONDS = 180.0
SANITIZER_EXIT_CODE = 43


def run_bounded(command: list[str], environment: dict[str, str], timeout: float) -> tuple[int, str, str, bool]:
    """Reap a timed-out scenario and its process group so no orphan workers survive a failed contract."""
    # The caller explicitly chooses a local test executable and argv. No shell interprets these arguments.
    with subprocess.Popen(  # noqa: S603
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        env=environment,
        start_new_session=True,
    ) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            return process.returncode, stdout, stderr, True
        else:
            return process.returncode, stdout, stderr, False


def report_error(report: str, expected: list[str], stacks: list[str]) -> str | None:
    """Check each individual stack rather than matching names in unrelated process output."""
    header = ERROR_HEADER.match(report)
    if header is None or header.group(1) not in {"unsafe-library-call", "blocking-call"}:
        return "unexpected sanitizer failure"
    call = UNSAFE_CALL.search(report)
    if call is None or not any(re.fullmatch(pattern, call.group(1)) for pattern in expected):
        return "an unexpected unsafe function was reported"
    if any(re.search(pattern, report, re.MULTILINE) is None for pattern in stacks):
        return "a sanitizer report does not match the expected callback stack"
    return None


# Keyword-only policy switches intentionally mirror the command-line contract used by all three projects.
def classify(  # noqa: PLR0913
    returncode: int,
    output: str,
    *,
    expected: list[str],
    stacks: list[str],
    required: list[str],
    diagnostic: bool,
    control: bool,
) -> tuple[str, str]:
    """Reject unrelated crashes and changed findings rather than accepting a blanket expected failure."""
    if re.search(r"(?:\bRan 0 tests\b|\b0 tests from 0 test suites\b|\brunning 0 tests\b)", output):
        return "FAIL", "the selected scenario executed no tests"
    if any(re.search(pattern, output, re.MULTILINE) is None for pattern in required):
        return "FAIL", "required scenario output is missing"
    headers = list(ERROR_HEADER.finditer(output))
    if not expected:
        if returncode != 0 or "ERROR: RealtimeSanitizer:" in output or "SUMMARY: RealtimeSanitizer:" in output:
            return "FAIL", "a clean scenario failed or reported a real-time violation"
        return "CLEAN", "no detected real-time violation"
    error = expected_report_error(returncode, output, expected, stacks, diagnostic)
    if error is not None:
        return "FAIL", error
    return ("CONTROL" if control else "BLOCKED"), f"verified {len(headers)} expected sanitizer report(s)"


def expected_report_error(
    returncode: int, output: str, expected: list[str], stacks: list[str], diagnostic: bool
) -> str | None:
    """Accept only the sanitizer's configured exit code or a completed continuing diagnostic scenario."""
    headers = list(ERROR_HEADER.finditer(output))
    if not headers or output.count("ERROR: RealtimeSanitizer:") != len(headers):
        return "the expected sanitizer diagnostic is missing or malformed"
    if re.search(r"dyld\[.*(?:Library not loaded|Symbol not found)|The specified module could not be found", output):
        return "the process reported a loader failure"
    if returncode != (0 if diagnostic else SANITIZER_EXIT_CODE):
        return "scenario termination did not match the sanitizer policy"
    for index, header in enumerate(headers):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(output)
        error = report_error(output[header.start() : end], expected, stacks)
        if error is not None:
            return error
    reports = output[headers[0].start() :]
    for pattern in expected:
        if not any(re.fullmatch(pattern, match.group(1)) for match in UNSAFE_CALL.finditer(reports)):
            return f"expected unsafe call was not observed: {pattern}"
    return None


def build_parser() -> argparse.ArgumentParser:
    """Describe explicit execution and diagnostic policies without launching a scenario."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-e", "--executable", type=Path, required=True)
    parser.add_argument(
        "-n", "--case", required=True, help="Artifact label and default --case argument to the executable"
    )
    parser.add_argument("-a", "--argument", action="append", default=[], help="Exact argv entry, use --argument=--flag")
    parser.add_argument("-E", "--expect", action="append", default=[], help="Full-match unsafe function regex")
    parser.add_argument("-s", "--stack", action="append", default=[], help="Regex required in every sanitizer stack")
    parser.add_argument("-O", "--require-output", action="append", default=[])
    parser.add_argument("-A", "--artifacts", type=Path, required=True)
    parser.add_argument("-t", "--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument(
        "-c", "--control", action="store_true", help="An intentional detector self-test, not a blocked audit"
    )
    parser.add_argument(
        "-G", "--diagnostic", action="store_true", help="Continue after errors and verify all reported findings"
    )
    return parser


def parse_options(arguments: list[str] | None = None) -> argparse.Namespace:
    """Keep the invocation explicit and reject unbounded or ambiguous execution policies."""
    parser = build_parser()
    options = parser.parse_args(arguments)
    if not 0.0 < options.timeout <= MAX_TIMEOUT_SECONDS:
        parser.error("--timeout must be greater than zero and at most 180 seconds")
    if options.expect and not options.stack:
        parser.error("expected violations require at least one --stack constraint")
    if (options.control or options.diagnostic) and not options.expect:
        parser.error("--control and --diagnostic require --expect")
    for pattern in [*options.expect, *options.stack, *options.require_output]:
        try:
            re.compile(pattern)
        except re.error as error:
            parser.error(f"invalid diagnostic expression: {error}")
    if options.case in {".", ".."} or not re.fullmatch(r"[A-Za-z0-9_.-]+", options.case):
        parser.error("--case must contain only letters, digits, underscores, periods, or hyphens")
    return options


def main(arguments: list[str] | None = None) -> int:
    """Persist inputs, exit status and both output streams before deciding whether the contract passed."""
    options = parse_options(arguments)
    command = [str(options.executable.resolve()), *(options.argument or ["--case", options.case])]
    environment = os.environ.copy()
    # Explicit policies must not inherit suppressions or disabled interceptor verification from a developer shell.
    environment["RTSAN_OPTIONS"] = (
        f"halt_on_error={'false' if options.diagnostic else 'true'}:abort_on_error=false:"
        f"exitcode={SANITIZER_EXIT_CODE}:symbolize=true:color=never:verify_interceptors=true:print_stats_on_exit=true"
    )
    artifact = options.artifacts.resolve() / options.case
    artifact.mkdir(parents=True, exist_ok=True)
    try:
        returncode, stdout, stderr, timed_out = run_bounded(command, environment, options.timeout)
        if timed_out:
            outcome, reason = "FAIL", "scenario exceeded its deadline and was terminated"
        else:
            outcome, reason = classify(
                returncode,
                stdout + "\n" + stderr,
                expected=options.expect,
                stacks=options.stack,
                required=options.require_output,
                diagnostic=options.diagnostic,
                control=options.control,
            )
    except OSError as error:
        returncode, stdout, stderr, timed_out = None, "", str(error), False
        outcome, reason = "FAIL", "could not launch the scenario"
    (artifact / "stdout.txt").write_text(stdout, encoding="utf-8")
    (artifact / "stderr.txt").write_text(stderr, encoding="utf-8")
    (artifact / "result.json").write_text(
        json.dumps(
            {
                "command": command,
                "rtsan_options": environment["RTSAN_OPTIONS"],
                "returncode": returncode,
                "timed_out": timed_out,
                "outcome": outcome,
                "reason": reason,
                "expected_calls": options.expect,
                "expected_stacks": options.stack,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"{outcome}: {options.case}: {reason}. Artifacts: {artifact}")
    if outcome == "FAIL":
        print(stdout, file=sys.stderr)
        print(stderr, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
