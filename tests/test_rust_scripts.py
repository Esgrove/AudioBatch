# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check Rust test selection, diagnostic pipelines, and coverage reporting without compiling Rust."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class RustScriptContracts(unittest.TestCase):
    """Exercise the production scripts with recorded cargo and uv commands in an isolated checkout."""

    def setUp(self) -> None:
        """Keep command recordings and generated artifacts out of the real workspace."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-rust-scripts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        self.project = self.root / "rust"
        self.project.mkdir(parents=True)
        for relative in ["common.sh", "rust/common.sh", "rust/test.sh", "rust/coverage.sh"]:
            destination = self.root / relative
            shutil.copyfile(ROOT / relative, destination)
            destination.chmod(0o755)
        self.tools = self.root / "bin"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        for name in ["cargo", "cargo-llvm-cov", "cargo-nextest", "uv"]:
            tool = self.tools / name
            tool.write_text(
                """#!/bin/bash
set -eo pipefail
tool=${0##*/}
printf '%s\\0' "$tool" "$PWD" "${CARGO_LLVM_COV_TARGET_DIR:-unset}" "${RUSTFLAGS:-unset}" \
    "${LLVM_PROFILE_FILE:-unset}" "$@" >> "$COMMAND_LOG"
printf '\\n' >> "$COMMAND_LOG"
if [ "$tool" = cargo ] && [ "$1" = build ]; then
    mkdir -p "$3/debug"
fi
if [ "$tool" = cargo ] && [ "$1" = llvm-cov ] && [ "$2" = show-env ]; then
    printf 'export RUSTFLAGS=instrumented\\n'
fi
if [ "$tool" = cargo ] && [ "$1" = llvm-cov ] && [ "$2" = report ]; then
    printf 'coverage summary\\n'
fi
printf 'tool stdout\\n' >&2
if [ -n "${FAIL_MATCH:-}" ] && [[ " $* " == *"$FAIL_MATCH"* ]]; then
    exit 7
fi
""",
                encoding="utf-8",
            )
            tool.chmod(0o755)
        uname = self.tools / "uname"
        uname.write_text("#!/bin/bash\nprintf '%s\\n' \"${TEST_PLATFORM:-Darwin}\"\n", encoding="utf-8")
        uname.chmod(0o755)

    def invoke(self, script: str, *arguments: str, **overrides: str) -> subprocess.CompletedProcess[str]:
        """Run from outside the Rust directory to verify script-relative workspace resolution."""
        environment = os.environ.copy()
        for name in ["CARGO_TARGET_DIR", "GITHUB_STEP_SUMMARY", "RUSTFLAGS", "LLVM_PROFILE_FILE"]:
            environment.pop(name, None)
        environment.update(
            PATH=os.pathsep.join([str(self.tools), environment.get("PATH", "")]),
            COMMAND_LOG=str(self.log),
            **overrides,
        )
        bash = shutil.which("bash")
        assert bash is not None, "Bash is required to test Rust shell entry points"
        return subprocess.run(  # noqa: S603
            [bash, str(self.project / script), *arguments],
            cwd=self.root,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def commands(self) -> list[list[str]]:
        """Read complete argv arrays without losing arguments containing spaces."""
        if not self.log.exists():
            return []
        return [record.removesuffix("\0").split("\0") for record in self.log.read_text().splitlines()]

    def test_unit_selection_matches_ci(self) -> None:
        """Keep GUI workflows out of the default unit and service tier."""
        result = self.invoke("test.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        assert self.commands()[0][5:] == [
            "nextest",
            "run",
            "--workspace",
            "--no-fail-fast",
            "--success-output",
            "final",
            "-E",
            "not test(integration_tests::)",
        ]
        assert self.commands()[0][1] == str(self.project)

    def test_gui_log_preserves_test_failure(self) -> None:
        """A successful tee must not mask a failed nextest process."""
        result = self.invoke("test.sh", "--gui-integration", FAIL_MATCH="nextest")
        assert result.returncode == 7, result.stdout + result.stderr
        assert self.commands()[0][5:] == [
            "nextest",
            "run",
            "-p",
            "audiobatch-gui",
            "-E",
            "test(integration_tests::)",
            "--no-fail-fast",
            "--failure-output",
            "immediate-final",
        ]
        assert "tool stdout" in (self.project / "target/integration-artifacts/gui.log").read_text()

    def test_optimized_selection_uses_shared_platform_detection(self) -> None:
        """Windows gets exactly the extra CLI subset even outside GitHub Actions."""
        result = self.invoke("test.sh", "--optimized", TEST_PLATFORM="MINGW64_NT", RUNNER_OS="macOS")
        assert result.returncode == 0, result.stdout + result.stderr
        assert self.commands()[0][5:] == [
            "nextest",
            "run",
            "--release",
            "-p",
            "audiobatch-core",
            "-p",
            "audiobatch-cli",
            "-E",
            "package(audiobatch-core) | (package(audiobatch-cli) & test(run::test_run_analysis::normalize_))",
            "--no-fail-fast",
        ]

    def test_optimized_nonwindows_selection_is_core_only(self) -> None:
        """Do not expand release test coverage on platforms that only need core baselines."""
        result = self.invoke("test.sh", "--optimized")
        assert result.returncode == 0, result.stdout + result.stderr
        assert self.commands()[0][5:] == [
            "nextest",
            "run",
            "--release",
            "-p",
            "audiobatch-core",
            "--no-fail-fast",
        ]

    def test_cli_uses_custom_target_directory_and_windows_suffix(self) -> None:
        """The subprocess suite must receive the executable just built, including under coverage."""
        target = self.project / "custom target"
        result = self.invoke(
            "test.sh",
            "--cli-integration",
            "--artifacts",
            "custom diagnostics",
            TEST_PLATFORM="MINGW64_NT",
            CARGO_TARGET_DIR=str(target),
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert self.commands()[0][5:] == ["build", "--target-dir", str(target), "--bin", "audiobatch-rs"]
        assert self.commands()[1][0] == "uv"
        assert self.commands()[1][5:] == [
            "run",
            "--script",
            "../tests/cli_integration.py",
            "--cli",
            str(target / "debug/audiobatch-rs.exe"),
            "--artifacts",
            "custom diagnostics",
        ]

    def test_build_failure_prevents_cli_contracts(self) -> None:
        """Never test an old executable when its build fails."""
        result = self.invoke("test.sh", "--cli-integration", FAIL_MATCH="build")
        assert result.returncode == 7, result.stdout + result.stderr
        assert len(self.commands()) == 1

    def test_tooling_stops_on_first_failure(self) -> None:
        """A second passing Python suite must not replace the first suite's failure."""
        result = self.invoke("test.sh", "--tooling", FAIL_MATCH="test_parity.py")
        assert result.returncode == 7, result.stdout + result.stderr
        assert self.commands()[0][5:] == ["run", "--script", "scripts/test_parity.py"]
        assert len(self.commands()) == 1

    def test_rejects_invalid_options_before_running_tools(self) -> None:
        """Invalid or contradictory tier and diagnostic options must fail explicitly."""
        for arguments in [
            ["--unit", "--optimized"],
            ["--artifacts"],
            ["--unit", "--artifacts", "somewhere"],
            ["--artifacts", "--gui-integration"],
            ["--unknown"],
        ]:
            with self.subTest(arguments=arguments):
                result = self.invoke("test.sh", *arguments)
                assert result.returncode != 0
                assert self.commands() == []

    def test_report_only_writes_all_reports_and_workflow_summary(self) -> None:
        """Reuse the same mapping directory without rebuilding or running tests."""
        summary = self.root / "summary.md"
        summary.write_text("Existing summary\n", encoding="utf-8")
        result = self.invoke("coverage.sh", "--report-only", GITHUB_STEP_SUMMARY=str(summary))
        assert result.returncode == 0, result.stdout + result.stderr
        assert [command[5:] for command in self.commands()] == [
            ["llvm-cov", "report", "--lcov", "--output-path", "target/llvm-cov/lcov.info"],
            ["llvm-cov", "report", "--html"],
            ["llvm-cov", "report", "--summary-only"],
        ]
        assert all(command[2] == str(self.project / "target/llvm-cov-target") for command in self.commands())
        assert summary.read_text() == "Existing summary\n## Rust code coverage\n```text\ncoverage summary\n```\n"

    def test_report_failure_is_not_hidden_by_markdown_output(self) -> None:
        """The failed summary command must not be replaced by a successful closing fence."""
        result = self.invoke("coverage.sh", "--report-only", FAIL_MATCH="--summary-only")
        assert result.returncode == 7, result.stdout + result.stderr

    def test_report_only_rejects_clean(self) -> None:
        """Do not delete the data a report-only invocation is meant to reuse."""
        result = self.invoke("coverage.sh", "--report-only", "--clean")
        assert result.returncode != 0
        assert self.commands() == []

    def test_coverage_reuses_instrumented_cli_tier_and_retains_test_failure(self) -> None:
        """CLI subprocesses keep instrumentation even when workspace tests fail first."""
        result = self.invoke("coverage.sh", FAIL_MATCH="nextest")
        assert result.returncode == 7, result.stdout + result.stderr
        commands = self.commands()
        assert commands[0][5:] == ["llvm-cov", "nextest", "--workspace", "--no-fail-fast", "--no-report"]
        assert commands[2][5:] == [
            "build",
            "--target-dir",
            str(self.project / "target/llvm-cov-target"),
            "--bin",
            "audiobatch-rs",
        ]
        assert commands[2][3] == "instrumented"
        assert commands[3][0] == "uv"
        assert commands[3][3] == "instrumented"
        assert commands[3][4].endswith("/rust-cli-integration-%m-%p.profraw")
        assert commands[3][-1] == "target/integration-artifacts/cli-coverage"
        assert commands[-1][5:] == ["llvm-cov", "report", "--summary-only"]
        assert commands[-1][3] == "unset"


if __name__ == "__main__":
    unittest.main(verbosity=2)
