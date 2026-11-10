# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml==6.0.3"]
# ///
"""Check Rust test and fuzz selection, diagnostic pipelines, and coverage reporting without compiling Rust."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
APPLICATION_PACKAGES = sorted(
    tomllib.loads(manifest.read_text())["package"]["name"] for manifest in (ROOT / "rust/crates").glob("*/Cargo.toml")
)
APPLICATION_PACKAGE_ARGS = [argument for package in APPLICATION_PACKAGES for argument in ("-p", package)]


class RustScriptContracts(unittest.TestCase):
    """Exercise the production scripts with recorded cargo and uv commands in an isolated checkout."""

    def setUp(self) -> None:
        """Keep command recordings and generated artifacts out of the real workspace."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-rust-scripts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        self.project = self.root / "rust"
        self.project.mkdir(parents=True)
        for relative in [
            "common.sh",
            "rust/common.sh",
            "rust/build.sh",
            "rust/lint.sh",
            "rust/format.sh",
            "rust/test.sh",
            "rust/coverage.sh",
            "rust/fuzz.sh",
        ]:
            destination = self.root / relative
            shutil.copyfile(ROOT / relative, destination)
            destination.chmod(0o755)
        self.tools = self.root / "bin"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        self.fuzz = self.project / "fuzz"
        self.fuzz.mkdir()
        for name in ["cargo", "cargo-llvm-cov", "cargo-nextest", "cargo-fuzz", "rustup", "uv"]:
            tool = self.tools / name
            tool.write_text(
                """#!/bin/bash
set -eo pipefail
tool=${0##*/}
printf '%s\\0' "$tool" "$PWD" "${CARGO_LLVM_COV_TARGET_DIR:-unset}" "${RUSTFLAGS:-unset}" \
    "${LLVM_PROFILE_FILE:-unset}" "$@" >> "$COMMAND_LOG"
printf '\\n' >> "$COMMAND_LOG"
if [ -n "${CARGO_TARGET_ENV_LOG:-}" ]; then
    printf '%s\\n' "${CARGO_TARGET_DIR:-unset}" >> "$CARGO_TARGET_ENV_LOG"
fi
if [ "$tool" = cargo ] && [ "$1" = build ] && [ "$2" = --target-dir ]; then
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
        rustc = self.tools / "rustc"
        rustc.write_text("#!/bin/bash\nprintf 'host: test-host\\n'\n", encoding="utf-8")
        rustc.chmod(0o755)
        for target in ["serato_tags", "cli_arguments", "cache_blobs", "decode_audio"]:
            binary = self.fuzz / "target/test-host/release" / target
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_text(
                """#!/bin/bash
set -eo pipefail
target=${0##*/}
if [ -n "${ASAN_ENV_LOG:-}" ]; then
    printf '%s\\n' "${ASAN_OPTIONS:-unset}" >> "$ASAN_ENV_LOG"
fi
printf '%s\\0' "$target" "$PWD" unset "${RUSTFLAGS-unset}" unset "$@" >> "$COMMAND_LOG"
printf '\\n' >> "$COMMAND_LOG"
if [ "${FAIL_MATCH:-}" = "$target" ]; then
    printf 'decoder or parser finding\\n' >&2
    printf 'reproducer\\n' > "artifacts/$target-test-finding"
    exit 77
fi
""",
                encoding="utf-8",
            )
            binary.chmod(0o755)

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

    def test_build_selects_only_application_binaries(self) -> None:
        """Packaging must not build or distribute plugin workspace binaries."""
        result = self.invoke("build.sh", FAIL_MATCH="build")
        assert result.returncode == 7, result.stdout + result.stderr
        assert self.commands()[0][5:] == [
            "build",
            "--release",
            "-p",
            "audiobatch-cli",
            "-p",
            "audiobatch-gui",
            "--bins",
        ]
        assert self.commands()[0][1] == str(self.project)

    def test_lint_selection_excludes_plugin_packages(self) -> None:
        """Application linting includes every app crate and keeps fuzz in its independent workspace."""
        result = self.invoke("lint.sh", "--check")
        assert result.returncode == 0, result.stdout + result.stderr
        assert [command[5:] for command in self.commands()] == [
            ["clippy", *APPLICATION_PACKAGE_ARGS, "--all-targets", "--", "-D", "warnings"],
            ["clippy", "--manifest-path", "fuzz/Cargo.toml", "--all-targets", "--", "-D", "warnings"],
        ]

    def test_format_selection_excludes_plugin_packages(self) -> None:
        """Application-only formatting must stop before unrelated shell and prose formatters."""
        result = self.invoke("format.sh", "--check", FAIL_MATCH="fmt")
        assert result.returncode == 7, result.stdout + result.stderr
        assert self.commands()[0][5:] == ["fmt", *APPLICATION_PACKAGE_ARGS, "--check"]

    def test_fuzz_default_build_and_replay_include_every_target(self) -> None:
        """Required replay retains the decoder seeds and both shared C++ corpora."""
        target_log = self.root / "fuzz-target-directory.log"
        result = self.invoke("fuzz.sh", "--replay", CARGO_TARGET_ENV_LOG=str(target_log))
        assert result.returncode == 0, result.stdout + result.stderr
        commands = self.commands()
        assert commands[0][5:] == ["+nightly", "fuzz", "build"]
        assert target_log.read_text().splitlines() == [str(self.fuzz / "target")]
        assert [command[0] for command in commands[1:]] == [
            "serato_tags",
            "cli_arguments",
            "cache_blobs",
            "decode_audio",
        ]
        for command in commands[1:]:
            assert command[1] == str(self.fuzz)
            assert command[3] == ""
            assert command[5:10] == [
                "-timeout=5",
                "-malloc_limit_mb=128",
                "-rss_limit_mb=512",
                "-max_len=262145",
                "-runs=0",
            ]
        assert commands[1][-2:] == ["corpus/serato_tags", "../../cpp/tests/fuzz/corpus/SeratoTagFuzzer"]
        assert commands[2][-2:] == ["corpus/cli_arguments", "../../cpp/tests/fuzz/corpus/CliArgumentsFuzzer"]
        assert not (self.fuzz / "scratch").exists()

    def test_fuzz_bounds_quarantine_without_disabling_sanitizer_diagnostics(self) -> None:
        """Every target uses bounded quarantine and retains other explicitly requested ASan diagnostics."""
        log = self.root / "asan-options.log"
        result = self.invoke(
            "fuzz.sh",
            "--no-build",
            "--replay",
            ASAN_OPTIONS="detect_leaks=1:quarantine_size_mb=256",
            ASAN_ENV_LOG=str(log),
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert (
            log.read_text().splitlines()
            == ["detect_leaks=1:quarantine_size_mb=256:quarantine_size_mb=64:thread_local_quarantine_size_kb=64"] * 4
        )

    def test_fuzz_selectors_limit_builds_and_runs(self) -> None:
        """Every mode uses exactly the selected group, with no decoder in parser mutations."""
        for group, targets in [
            ("parsers", ["serato_tags", "cli_arguments", "cache_blobs"]),
            ("decoder", ["decode_audio"]),
        ]:
            for mode in ["--build-only", "--replay", "--fuzz"]:
                with self.subTest(group=group, mode=mode):
                    self.log.unlink(missing_ok=True)
                    result = self.invoke("fuzz.sh", mode, "--targets", group, "--seconds", "3")
                    assert result.returncode == 0, result.stdout + result.stderr
                    commands = self.commands()
                    assert [command[5:] for command in commands[: len(targets)]] == [
                        ["+nightly", "fuzz", "build", target] for target in targets
                    ]
                    runs = commands[len(targets) :]
                    assert [command[0] for command in runs] == ([] if mode == "--build-only" else targets)
                    for command in runs:
                        assert command[9] == ("-runs=0" if mode == "--replay" else "-max_total_time=3")
                        if mode == "--fuzz":
                            assert command[11] == f"scratch/{command[0]}"
                            assert command[12] == f"corpus/{command[0]}"

    def test_fuzz_no_build_does_not_require_unselected_binaries(self) -> None:
        """Decoder-only runs do not depend on cached parser executables."""
        for target in ["serato_tags", "cli_arguments", "cache_blobs"]:
            (self.fuzz / "target/test-host/release" / target).unlink()
        result = self.invoke("fuzz.sh", "--no-build", "--replay", "--targets", "decoder")
        assert result.returncode == 0, result.stdout + result.stderr
        assert [command[0] for command in self.commands()] == ["decode_audio"]

    def test_fuzz_failures_and_artifacts_remain_visible(self) -> None:
        """Non-blocking is a CI job policy, never success conversion inside the script."""
        for group, target in [("decoder", "decode_audio"), ("parsers", "serato_tags")]:
            for mode in ["--fuzz", "--replay"]:
                with self.subTest(group=group, mode=mode):
                    self.log.unlink(missing_ok=True)
                    result = self.invoke("fuzz.sh", "--no-build", mode, "--targets", group, FAIL_MATCH=target)
                    assert result.returncode == 77, result.stdout + result.stderr
                    assert "decoder or parser finding" in result.stderr
                    assert [command[0] for command in self.commands()] == [target]
                    assert (self.fuzz / f"artifacts/{target}-test-finding").read_text() == "reproducer\n"

    def test_fuzz_rejects_invalid_selectors_before_running_tools(self) -> None:
        """Typos must not quietly expand diagnostics or drop required targets."""
        for arguments in [["--targets"], ["--targets", "unknown"], ["--targets", "--replay"]]:
            with self.subTest(arguments=arguments):
                result = self.invoke("fuzz.sh", *arguments)
                assert result.returncode != 0
                assert self.commands() == []

    def workflow_jobs(self) -> dict:
        """Read the actual CI policy so routing contracts cannot pass against a separate test-only job."""
        return yaml.safe_load((ROOT / ".github/workflows/rust.yml").read_text())["jobs"]

    def test_workflow_only_decoder_diagnostics_are_nonblocking(self) -> None:
        """Miri, checked decoder regressions, every replay, and parser/cache mutations remain gating."""
        jobs = self.workflow_jobs()
        assert {name for name, job in jobs.items() if job.get("continue-on-error")} == {"decoder-fuzz-diagnostics"}
        assert not any(step.get("continue-on-error") for job in jobs.values() for step in job["steps"])
        assert any(step.get("run") == "./miri.sh" for step in jobs["miri"]["steps"])
        required = [step["run"] for step in jobs["fuzz"]["steps"] if "run" in step]
        assert required == [
            "cargo test --manifest-path fuzz/Cargo.toml --target-dir fuzz/target --test decoder_regressions",
            "./fuzz.sh --build-only",
            "./fuzz.sh --no-build --replay",
            "./fuzz.sh --no-build --fuzz --targets parsers --seconds 60",
        ]
        diagnostics = jobs["decoder-fuzz-diagnostics"]
        assert "non-blocking diagnostics" in diagnostics["name"]
        assert not diagnostics.get("needs"), "Required check failures must not hide decoder diagnostics"
        artifacts = next(
            step for step in diagnostics["steps"] if step.get("uses", "").startswith("actions/upload-artifact@")
        )
        assert artifacts["if"] == "always()"
        assert artifacts["with"]["path"] == "rust/fuzz/artifacts"
        assert artifacts["with"]["name"] == "rust-decoder-fuzz-diagnostics"
        assert artifacts["with"]["if-no-files-found"] == "warn"

    def test_workflow_decoder_pipeline_retains_failed_status_log_and_input(self) -> None:
        """Execute the real CI pipeline with an injected finding, including Bash pipefail and tee."""
        steps = self.workflow_jobs()["decoder-fuzz-diagnostics"]["steps"]
        mutation = next(step for step in steps if step.get("id") == "decoder-mutations")
        script = self.project / "diagnostic-pipeline.sh"
        script.write_text('#!/bin/bash\nset -eo pipefail\ncd "$PIPELINE_DIRECTORY"\n' + mutation["run"])
        result = self.invoke(script.name, PIPELINE_DIRECTORY=str(self.project), FAIL_MATCH="decode_audio")
        assert result.returncode == 77, result.stdout + result.stderr
        assert "decoder or parser finding" in (self.fuzz / "artifacts/decoder-mutations.log").read_text()
        assert (self.fuzz / "artifacts/decode_audio-test-finding").is_file()
        assert [command[0] for command in self.commands()] == ["decode_audio"]

    def test_workflow_summary_reports_failure_success_and_incomplete_runs(self) -> None:
        """The always-run report must state the raw outcome rather than turn findings into a clean result."""
        steps = self.workflow_jobs()["decoder-fuzz-diagnostics"]["steps"]
        report = next(step for step in steps if step["name"] == "Report decoder diagnostic outcome")
        assert report["if"] == "always()"
        assert report["env"]["MUTATION_OUTCOME"] == "${{ steps.decoder-mutations.outcome }}"
        script = self.project / "diagnostic-summary.sh"
        script.write_text("#!/bin/bash\nset -eo pipefail\n" + report["run"])
        for status, outcome in [("failure", "failure"), ("success", "success"), ("failure", "skipped")]:
            with self.subTest(status=status, outcome=outcome):
                summary = self.root / f"summary-{outcome}.md"
                result = self.invoke(
                    script.name,
                    GITHUB_STEP_SUMMARY=str(summary),
                    DIAGNOSTIC_STATUS=status,
                    MUTATION_OUTCOME=outcome,
                )
                assert result.returncode == 0, result.stdout + result.stderr
                text = summary.read_text()
                assert f"Job status before reporting: {status}. Mutation step outcome: {outcome}." in text
                assert "remain unfixed" in text
                assert ("::warning::" in result.stdout) == (outcome != "success")

    def test_unit_selection_matches_ci(self) -> None:
        """Keep GUI workflows out of the default unit and service tier."""
        result = self.invoke("test.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        assert self.commands()[0][5:] == [
            "nextest",
            "run",
            *APPLICATION_PACKAGE_ARGS,
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

    def test_tooling_includes_callback_benchmark_contracts(self) -> None:
        """Require callback comparison safety checks alongside parity and CLI benchmark contracts."""
        result = self.invoke("test.sh", "--tooling")
        assert result.returncode == 0, result.stdout + result.stderr
        assert [command[5:] for command in self.commands()] == [
            ["run", "--script", "scripts/test_parity.py"],
            ["run", "--script", "scripts/test_cli_benchmark.py"],
            ["run", "--script", "../tests/test_callback_benchmark.py"],
        ]

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
            ["llvm-cov", "report", *APPLICATION_PACKAGE_ARGS, "--lcov", "--output-path", "target/llvm-cov/lcov.info"],
            ["llvm-cov", "report", *APPLICATION_PACKAGE_ARGS, "--html", "--output-dir", "target/llvm-cov"],
            ["llvm-cov", "report", *APPLICATION_PACKAGE_ARGS, "--summary-only"],
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
        assert commands[0][5:] == ["llvm-cov", "nextest", *APPLICATION_PACKAGE_ARGS, "--no-fail-fast", "--no-report"]
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
        assert commands[-1][5:] == ["llvm-cov", "report", *APPLICATION_PACKAGE_ARGS, "--summary-only"]
        assert commands[-1][3] == "unset"


if __name__ == "__main__":
    unittest.main(verbosity=2)
