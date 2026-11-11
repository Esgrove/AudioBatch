# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Verify the shared C++ test runner without invoking compilers or changing real build directories."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TestRunner(unittest.TestCase):
    """Check workspace selection, test tiers, and failure propagation using fake build tools."""

    def setUp(self) -> None:
        """Copy the real runner and helpers into isolated storage with logged tool substitutes."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-test-script-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        self.project = self.root / "cpp"
        self.project.mkdir(parents=True)
        for relative in [
            "common.sh",
            "cpp/common.sh",
            "cpp/build.sh",
            "cpp/completions.sh",
            "cpp/coverage.sh",
            "cpp/format.sh",
            "cpp/fuzz.sh",
            "cpp/install.sh",
            "cpp/lint.sh",
            "cpp/sanitize.sh",
            "cpp/test.sh",
            "test-plugin/build.sh",
        ]:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        self.tools = self.root / "bin"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        for name, contents in {
            "uname": "printf '%s\\n' \"${TEST_PLATFORM:-Darwin}\"\n",
            "cl": "exit 0\n",
            "clang-cl": "printf '%s\\n' '/fake LLVM/resource'\n",
            "lld-link": "exit 0\n",
            "cygpath": "printf '%s\\n' \"$2\"\n",
            "uv": "exit 0\n",
            "cmake": """printf '%s\\t' cmake "$CCACHE_DIR" "$@" >> "$TEST_LOG"
printf '\\n' >> "$TEST_LOG"
exit "${TEST_CMAKE_STATUS:-0}"
""",
            "ctest": """printf '%s\\t' ctest "$@" >> "$TEST_LOG"
printf '\\n' >> "$TEST_LOG"
exit "${TEST_CTEST_STATUS:-0}"
""",
        }.items():
            tool = self.tools / name
            tool.write_text(f"#!/bin/bash\n{contents}", encoding="utf-8")
            tool.chmod(0o755)

    def invoke(self, *arguments: str, **environment_overrides: str) -> subprocess.CompletedProcess[str]:
        """Run from outside the project with bounded execution and a controlled tool path."""
        script = environment_overrides.pop("TEST_SCRIPT", "test.sh")
        environment = os.environ.copy()
        environment.update(
            PATH=os.pathsep.join([str(self.tools), environment.get("PATH", "")]),
            TEST_LOG=str(self.log),
            TEST_PLATFORM="Darwin",
            TEST_CMAKE_STATUS="0",
            TEST_CTEST_STATUS="0",
        )
        environment.update(environment_overrides)
        bash = shutil.which("bash")
        assert bash is not None, "Bash is required to test the C++ test runner"
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
        """Return logged argument vectors without losing spaces inside workspace paths."""
        return [line.rstrip("\t").split("\t") for line in self.log.read_text(encoding="utf-8").splitlines()]

    def workspace(self, name: str, *, testing: bool = True, generator: str = "Ninja") -> Path:
        """Supply an existing CMake workspace without generating or building anything."""
        path = self.project / name
        path.mkdir()
        (path / "CMakeCache.txt").write_text(
            f"CMAKE_GENERATOR:INTERNAL={generator}\nCMAKE_BUILD_TYPE:STRING=Debug\n"
            f"BUILD_TESTING:BOOL={'ON' if testing else 'OFF'}\n",
            encoding="utf-8",
        )
        return path

    def test_help_is_handled_by_the_parser_before_tool_checks(self) -> None:
        """Help exits after bootstrap without starting tools, and malformed earlier options still fail."""
        for script in [
            "build.sh",
            "completions.sh",
            "coverage.sh",
            "format.sh",
            "fuzz.sh",
            "install.sh",
            "lint.sh",
            "sanitize.sh",
            "test.sh",
            "../test-plugin/build.sh",
        ]:
            for help_option in ["-h", "--help"]:
                with self.subTest(script=script, help_option=help_option):
                    result = self.invoke(help_option, TEST_SCRIPT=script)
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert "Usage:" in result.stdout
                    assert not self.log.exists()
                with self.subTest(script=script, invalid_before_help=help_option):
                    result = self.invoke("--unknown", help_option, TEST_SCRIPT=script)
                    assert result.returncode != 0, result.stdout + result.stderr
                    assert not self.log.exists()

    def test_short_test_options_select_canonical_tiers_and_values(self) -> None:
        """Short and long repeated selections retain tier names and whitespace in option values."""
        directory = self.workspace("custom workspace")
        for short, long, tier in [("-u", "--unit", "unit"), ("-i", "--integration", "integration")]:
            with self.subTest(tier=tier):
                result = self.invoke(short, long, "-p", directory.name, "-b", "Debug", "-N")
                assert result.returncode == 0, result.stdout + result.stderr
                [ctest] = self.commands()
                assert ctest[ctest.index("-L") + 1] == tier
                assert ctest[ctest.index("--test-dir") + 1] == str(directory)
                self.log.unlink()
        result = self.invoke("-u", "--unit", "-B")
        assert result.returncode == 0, result.stdout + result.stderr
        _, build = self.commands()
        assert build[build.index("--target") + 1 :] == ["AudioBatchTests", "--config", "Debug"]

    def test_short_visible_option_selects_integration(self) -> None:
        """Visible mode accepts its optional value and rejects the unit tier with either spelling."""
        directory = self.workspace("custom workspace")
        result = self.invoke("-W", "15", "-N", "-p", directory.name)
        assert result.returncode == 0, result.stdout + result.stderr
        [ctest] = self.commands()
        assert ctest[ctest.index("-L") + 1] == "integration"
        self.log.unlink()
        for arguments in [("-W", "-u"), ("--visible", "-u"), ("-W", "--unit")]:
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                assert result.returncode != 0
                assert "cannot be used with --unit" in result.stderr
                assert not self.log.exists()

    def test_short_modes_still_reject_mixed_spelling_conflicts(self) -> None:
        """Canonical mode comparisons reject conflicting short and long flags before builds."""
        for script, pairs in [
            ("test.sh", [("-u", "--integration"), ("--unit", "-i"), ("-B", "--no-build")]),
            ("fuzz.sh", [("-r", "--fuzz"), ("--replay", "-f"), ("-B", "--no-build")]),
        ]:
            for arguments in pairs:
                with self.subTest(script=script, arguments=arguments):
                    result = self.invoke(*arguments, TEST_SCRIPT=script)
                    assert result.returncode != 0
                    assert "cannot be used together" in result.stderr
                    assert not self.log.exists()

    def test_short_sanitizer_options_select_the_expected_presets(self) -> None:
        """Use fake CMake and CTest to verify thread and realtime alias routing."""
        for option, preset, target, label in [
            ("-t", "macos-tsan", "AudioBatchTests", "unit"),
            ("-R", "macos-rtsan", "AudioBatchRealtimeTests", "realtime"),
        ]:
            with self.subTest(option=option):
                result = self.invoke(option, TEST_SCRIPT="sanitize.sh")
                assert result.returncode == 0, result.stdout + result.stderr
                configure, build, ctest = self.commands()
                assert configure[-3:] == ["--preset", preset, "-DBUILD_TESTING=ON"]
                assert build[-4:] == ["--preset", preset, "--target", target]
                assert ctest[ctest.index("-L") + 1] == label
                self.log.unlink()

    def test_short_clang_sanitizer_selects_windows_llvm(self) -> None:
        """Route the compiler alias through the Windows LLVM preset without a physical toolchain."""
        result = self.invoke("-k", TEST_SCRIPT="sanitize.sh", TEST_PLATFORM="MINGW64_NT")
        assert result.returncode == 0, result.stdout + result.stderr
        configure, build, ctest = self.commands()
        assert configure[-3:] == ["--preset", "windows-clang-asan", "-DBUILD_TESTING=ON"]
        assert build[-4:] == ["--preset", "windows-clang-asan", "--target", "AudioBatchTests"]
        assert ctest[ctest.index("--build-config") + 1] == "RelWithDebInfo"

    def test_short_clang_build_preserves_existing_reuse_and_cli_aliases(self) -> None:
        """Package only an isolated fake CLI while selecting the Windows clang Debug workspace."""
        directory = self.workspace("cmake-build-windows-clang-debug")
        artifact = directory / "AudioBatchCli_artefacts/Debug/audiobatch.exe"
        artifact.parent.mkdir(parents=True)
        artifact.write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
        artifact.chmod(0o755)
        for name in ["ccache", "file"]:
            tool = self.tools / name
            tool.write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
            tool.chmod(0o755)
        result = self.invoke("-k", "-r", "-l", "-d", TEST_SCRIPT="build.sh", TEST_PLATFORM="MINGW64_NT")
        assert result.returncode == 0, result.stdout + result.stderr
        [build] = self.commands()
        assert build[2:] == ["--build", str(directory), "--target", "AudioBatchCli", "--config", "Debug"]
        assert (self.project / "audiobatch.exe").is_file()
        assert not (self.project / "AudioBatchApp.exe").exists()

    def test_test_plugin_keeps_print_and_debug_aliases(self) -> None:
        """Printing the Debug bundle stays side-effect free and retains the legacy -p meaning."""
        result = self.invoke("-p", "-d", TEST_SCRIPT="../test-plugin/build.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        assert "cmake-build-debug/AudioBatchTestPlugin_artefacts/Debug/VST3/AudioBatch Test MS.vst3" in result.stdout
        assert not self.log.exists()

    def test_default_builds_and_runs_both_tiers(self) -> None:
        """Use the debug preset, build every required executable, and leave CTest unfiltered."""
        result = self.invoke()
        assert result.returncode == 0, result.stdout + result.stderr
        configure, build, ctest = self.commands()
        directory = str(self.project / "cmake-build-mac-debug")
        assert configure == [
            "cmake",
            f"{directory}/ccache",
            "--preset",
            "macos-debug",
            "-B",
            directory,
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DBUILD_TESTING=ON",
        ]
        assert build == [
            "cmake",
            f"{directory}/ccache",
            "--build",
            directory,
            "--target",
            "AudioBatchTests",
            "AudioBatchIntegrationTests",
            "AudioBatchCli",
            "--config",
            "Debug",
        ]
        assert "-L" not in ctest
        assert "-LE" not in ctest
        assert "--no-tests=error" in ctest

    def test_unit_mode_reuses_existing_release_workspace(self) -> None:
        """Enable tests in the existing generator and retain build.sh's configuration and cache paths."""
        directory = self.workspace("cmake-build-mac-release", testing=False)
        result = self.invoke("--unit", "--build-type", "Release")
        assert result.returncode == 0, result.stdout + result.stderr
        configure, build, ctest = self.commands()
        assert "--preset" not in configure
        assert "-DBUILD_TESTING=ON" in configure
        assert str(directory) in configure
        assert f"{directory}/ccache" in configure
        assert build[build.index("--target") + 1 :] == ["AudioBatchTests", "--config", "Release"]
        assert ctest[ctest.index("-L") + 1] == "unit"

    def test_integration_mode_builds_only_its_executables(self) -> None:
        """Build both the production CLI and the nonvisual GUI target before running the integration label."""
        result = self.invoke("--integration")
        assert result.returncode == 0, result.stdout + result.stderr
        _, build, ctest = self.commands()
        assert build[build.index("--target") + 1 :] == [
            "AudioBatchIntegrationTests",
            "AudioBatchCli",
            "--config",
            "Debug",
        ]
        assert ctest[ctest.index("-L") + 1] == "integration"

    def test_build_only_and_no_build_support_separate_ci_steps(self) -> None:
        """Separate compilation from labelled runs without configuring or building twice."""
        result = self.invoke("--build-only")
        assert result.returncode == 0, result.stdout + result.stderr
        assert len(self.commands()) == 2
        self.workspace("cmake-build-mac-debug")
        result = self.invoke("--integration", "--no-build")
        assert result.returncode == 0, result.stdout + result.stderr
        assert [command[0] for command in self.commands()] == ["cmake", "cmake", "ctest"]

    def test_explicit_workspace_is_resolved_relative_to_project(self) -> None:
        """Keep custom toolchain workspaces usable without depending on the caller's working directory."""
        directory = self.workspace("custom workspace")
        result = self.invoke("--build-dir", directory.name, "--no-build", "--unit")
        assert result.returncode == 0, result.stdout + result.stderr
        [ctest] = self.commands()
        assert ctest[ctest.index("--test-dir") + 1] == str(directory)

    def test_windows_uses_visual_studio_workspace_convention(self) -> None:
        """Select the same Windows Release preset directory and configuration as build.sh."""
        result = self.invoke("--build-type", "Release", "--build-only", TEST_PLATFORM="MINGW64_NT")
        assert result.returncode == 0, result.stdout + result.stderr
        configure, build = self.commands()
        assert configure[configure.index("--preset") + 1] == "windows-debug"
        assert str(self.project / "cmake-build-windows-release") in configure
        assert build[-2:] == ["--config", "Release"]

    def test_short_fuzz_modes_retain_canonical_names(self) -> None:
        """Repeated alias spellings select replay without launching live fuzzers."""
        result = self.invoke("-B", TEST_SCRIPT="fuzz.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        assert len(self.commands()) == 2
        self.log.unlink()
        result = self.invoke("-N", "-r", "--replay", "-s", "2", TEST_SCRIPT="fuzz.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        [ctest] = self.commands()
        assert ctest[0] == "ctest"

    def test_fuzz_build_and_replay_use_shared_workspace(self) -> None:
        """Keep fuzz compilation and corpus replay independently reproducible without running live fuzzers."""
        result = self.invoke("--build-only", TEST_SCRIPT="fuzz.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        configure, build = self.commands()
        assert configure[-2:] == ["--preset", "macos-fuzz"]
        assert build[-3:] == ["--build", "--preset", "macos-fuzz"]
        result = self.invoke("--no-build", "--replay", TEST_SCRIPT="fuzz.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        assert [command[0] for command in self.commands()] == ["cmake", "cmake", "ctest"]

    def test_fuzz_uses_scratch_corpus_and_propagates_crashes(self) -> None:
        """Retain both harnesses, bound their runtime, and stop when a harness returns failure."""
        for name in ["SeratoTagFuzzer", "CliArgumentsFuzzer"]:
            harness = self.project / "cmake-build-mac-fuzz" / f"{name}_artefacts" / "RelWithDebInfo" / name
            harness.parent.mkdir(parents=True)
            harness.write_text(
                '#!/bin/bash\nprintf "%s\\t" "$@" >> "$TEST_LOG"\nprintf "\\n" >> "$TEST_LOG"\n'
                'exit "${TEST_CTEST_STATUS:-0}"\n',
                encoding="utf-8",
            )
            harness.chmod(0o755)
        result = self.invoke("-N", "-f", "--fuzz", "-s", "2", TEST_SCRIPT="fuzz.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        for name, command in zip(["SeratoTagFuzzer", "CliArgumentsFuzzer"], self.commands(), strict=True):
            assert command == [
                "-max_total_time=2",
                f"-artifact_prefix=fuzz-artifacts/{name}-",
                f"fuzz-scratch/{name}",
                f"tests/fuzz/corpus/{name}",
            ]
            assert (self.project / "fuzz-scratch" / name).is_dir()
        self.log.unlink()
        result = self.invoke("--no-build", "--fuzz", TEST_SCRIPT="fuzz.sh", TEST_CTEST_STATUS="9")
        assert result.returncode == 9
        assert len(self.commands()) == 1

    def test_fuzz_rejects_invalid_modes_and_durations(self) -> None:
        """Reject ambiguous modes and unbounded zero-second fuzzing before invoking tools."""
        for arguments in [
            ("--replay", "--fuzz"),
            ("--build-only", "--no-build"),
            ("--seconds", "0"),
            ("--seconds", "-1"),
            ("--seconds",),
        ]:
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments, TEST_SCRIPT="fuzz.sh")
                assert result.returncode != 0
                assert not self.log.exists()

    def test_invalid_options_fail_before_running_tools(self) -> None:
        """Reject incompatible modes, unsafe configuration names, missing values, and unknown flags."""
        for arguments in [
            ("--unit", "--integration"),
            ("--build-only", "--no-build"),
            ("--build-type", "../elsewhere"),
            ("--build-type",),
            ("--build-dir",),
            ("--build-dir", "--unit"),
            ("--build-type", "--integration"),
            ("--unknown",),
        ]:
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                assert result.returncode != 0, result.stdout + result.stderr
                assert not self.log.exists()

    def test_no_build_rejects_a_different_single_configuration(self) -> None:
        """Never silently run Debug binaries when Release was requested from a Ninja workspace."""
        directory = self.workspace("custom debug workspace")
        result = self.invoke("--no-build", "--build-dir", str(directory), "--build-type", "Release")
        assert result.returncode != 0
        assert "different build type" in result.stderr
        assert not self.log.exists()

    def test_no_build_requires_test_enabled_workspace(self) -> None:
        """Fail clearly when execution-only mode cannot discover the requested test targets."""
        self.workspace("cmake-build-mac-debug", testing=False)
        result = self.invoke("--no-build")
        assert result.returncode != 0
        assert "No test-enabled workspace" in result.stderr
        assert not self.log.exists()

    def test_build_and_test_failures_propagate(self) -> None:
        """Never run tests after failed configuration and never hide a failed CTest run."""
        result = self.invoke(TEST_CMAKE_STATUS="7")
        assert result.returncode == 7
        assert len(self.commands()) == 1
        self.log.unlink()
        result = self.invoke(TEST_CTEST_STATUS="8")
        assert result.returncode == 8
        assert len(self.commands()) == 3


class SharedHelperReporting(unittest.TestCase):
    """Keep progress paths portable across native macOS and Git Bash path tools."""

    def test_file_list_does_not_require_gnu_realpath_options(self) -> None:
        """Resolve spaced, parent-relative, external, and symlinked paths without GNU-only flags."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-file-list-")
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name).resolve()
        root = directory / "repository [with spaces]"
        project = root / "cpp"
        project.mkdir(parents=True)
        shutil.copyfile(ROOT / "common.sh", root / "common.sh")
        source = project / "Example.cpp"
        source.touch()
        external = directory / "external file.txt"
        external.touch()
        linked = directory / "linked repository"
        linked.symlink_to(root, target_is_directory=True)
        tools = directory / "tools"
        tools.mkdir()
        realpath = shutil.which("realpath")
        bash = shutil.which("bash")
        assert realpath is not None
        assert bash is not None
        wrapper = tools / "realpath"
        wrapper.write_text(
            '#!/bin/bash\ncase "$1" in -*) exit 97 ;; esac\nexec "$TEST_REALPATH" "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
        for working_directory, common, paths, expected in [
            (root, root / "common.sh", [source], ["cpp/Example.cpp"]),
            (project, root / "common.sh", [Path("../common.sh"), source], ["common.sh", "cpp/Example.cpp"]),
            (root, root / "common.sh", [external], ["../external file.txt"]),
            (linked, linked / "common.sh", [linked / "cpp/Example.cpp"], ["cpp/Example.cpp"]),
        ]:
            with self.subTest(working_directory=working_directory, paths=paths):
                result = subprocess.run(  # noqa: S603
                    [
                        bash,
                        "-c",
                        'source "$1"; shift; print_file_list Checking "$@"',
                        "test",
                        str(common),
                        *map(str, paths),
                    ],
                    cwd=working_directory,
                    env=os.environ
                    | {"PATH": os.pathsep.join([str(tools), os.environ["PATH"]]), "TEST_REALPATH": realpath},
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                assert result.returncode == 0, result.stdout + result.stderr
                assert result.stdout.splitlines() == [
                    f"Checking [{index}/{len(expected)}] {path}" for index, path in enumerate(expected, start=1)
                ]


if __name__ == "__main__":
    unittest.main(verbosity=2)
