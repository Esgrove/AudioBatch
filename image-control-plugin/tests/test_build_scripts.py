# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Test build/validation routing and failure propagation, plus safe installation and recovery.

Disposable bundles and recording tools keep real installations untouched.
Shared help and flag-documentation checks live in tests/test_shell_help.py at the repository root.

Run with `uv run --script tests/test_build_scripts.py` from image-control-plugin/.
No compiler, installed plugin, Audio Unit registry, or real validator is used.
"""

from __future__ import annotations

import base64
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which("bash")
SCRIPTS = ("build.sh", "install.sh")
ALL_SCRIPTS = (*SCRIPTS, "coverage.sh", "sanitize.sh", "format.sh", "lint.sh")


class BuildScriptContracts(unittest.TestCase):
    """Run unchanged production scripts in a miniature repository whose paths contain spaces."""

    def setUp(self) -> None:
        """Isolate both HOME and PATH so no installed plugin or real validator can be touched."""
        assert BASH is not None, "Bash is required for build script contracts"
        temporary = tempfile.TemporaryDirectory(prefix="image-control-scripts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "developer's repository with spaces [exact]"
        for relative in (
            "common.sh",
            "image-control-plugin/common.sh",
            *(f"image-control-plugin/{script}" for script in ALL_SCRIPTS),
            "cpp/cmake/Pluginval.cmake",
            "cpp/cmake/RunPluginval.cmake",
        ):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        self.plugin = self.root / "image-control-plugin"
        juce = self.root / "cpp/JUCE/CMakeLists.txt"
        juce.parent.mkdir(parents=True)
        juce.touch()
        self.home = self.root / "isolated home"
        self.home.mkdir()
        self.tools = self.root / "recording tools"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        self.environment = {
            "PATH": str(self.tools),
            "HOME": str(self.home),
            "COMMAND_LOG": str(self.log),
            "TEST_PLATFORM": "Darwin",
            "TEST_WINDOWS_PATH": r"C:\Contract Root\ImageControl.vst3",
            "LC_ALL": "C",
        }
        # Deliberately do not inherit PATH: a developer's pluginval, auval, or killall must never run.
        for command in ("dirname", "tr", "mkdir", "rm", "cp", "mv", "diff", "mktemp", "iconv", "base64", "seq", "grep"):
            executable = shutil.which(command)
            assert executable is not None, f"Required shell utility not found: {command}"
            (self.tools / command).symlink_to(executable)
        self.write_tool("uname", 'printf "%s\\n" "$TEST_PLATFORM"\n', record=False)
        self.write_tool(
            "cmake",
            """if [[ "$1" == -DSTRICTNESS=* ]]; then
    exit "${PLUGINVAL_SETUP_STATUS:-0}"
elif [ "$1" = --build ]; then
    build_dir="$2"
    shift 2
    config=Release
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --config) config="$2"; shift ;;
            ImageControl_VST3) build_vst3=true ;;
            ImageControl_AUSign) build_au=true ;;
            ImageControlTests) build_tests=true ;;
        esac
        shift
    done
    if [ "${CMAKE_BUILD_STATUS:-0}" -ne 0 ]; then
        exit "$CMAKE_BUILD_STATUS"
    fi
    artefacts="$build_dir/ImageControl_artefacts/$config"
    if [ "${build_vst3:-false}" = true ]; then
        mkdir -p "$artefacts/VST3/ImageControl.vst3/Contents/x86_64-win"
        printf 'built VST3' > "$artefacts/VST3/ImageControl.vst3/Contents/contract.txt"
        printf 'built DLL' > "$artefacts/VST3/ImageControl.vst3/Contents/x86_64-win/ImageControl.vst3"
    fi
    if [ "${build_au:-false}" = true ]; then
        mkdir -p "$artefacts/AU/ImageControl.component/Contents"
        printf 'built AU' > "$artefacts/AU/ImageControl.component/Contents/contract.txt"
    fi
    if [ "${build_tests:-false}" = true ]; then
        : > "$build_dir/tests-built"
    fi
else
    while [ "$#" -gt 0 ]; do
        if [ "$1" = -B ]; then
            mkdir -p "$2"
            : > "$2/CMakeCache.txt"
            printf '[]\\n' > "$2/compile_commands.json"
            exit 0
        fi
        shift
    done
    exit 91
fi
""",
        )
        self.write_tool("ctest", 'exit "${CTEST_STATUS:-0}"\n')
        self.write_tool(
            "pluginval",
            """if [ "$TEST_PLATFORM" = Darwin ]; then
    test -d "${@: -1}"
else
    test "${@: -1}" = "$TEST_WINDOWS_PATH"
fi
exit "${PLUGINVAL_STATUS:-0}"
""",
        )
        self.write_tool(
            "auval",
            """if [ "$1" = -a ]; then
    printf 'aufx Egic Esgr ImageControl\\n'
else
    test "$1" = -strict
    exit "${AUVAL_STATUS:-0}"
fi
""",
        )
        self.write_tool("killall", "exit 0\n")
        self.write_tool("codesign", "exit 0\n")
        self.write_tool(
            "cygpath",
            """test "$1" = -w
printf '%s\\n' "$TEST_WINDOWS_PATH"
""",
        )

    def write_tool(self, name: str, body: str, *, record: bool = True) -> None:
        """Record NUL-delimited argv without flattening paths containing spaces."""
        logging = 'printf "%s\\0" "${0##*/}" "$@" >> "$COMMAND_LOG"\nprintf "\\n" >> "$COMMAND_LOG"\n' if record else ""
        tool = self.tools / name
        tool.write_text(f"#!{BASH}\nset -eo pipefail\n{logging}{body}", encoding="utf-8")
        tool.chmod(0o755)

    def run_bash(self, *arguments: str, **overrides: str) -> subprocess.CompletedProcess[str]:
        """Bound every shell invocation and retain diagnostics for failed assertions."""
        assert BASH is not None
        return subprocess.run(  # noqa: S603
            [BASH, *arguments],
            cwd=self.plugin,
            env=self.environment | overrides,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def invoke(self, script: str, *arguments: str, **overrides: str) -> subprocess.CompletedProcess[str]:
        """Run a copied production entry point rather than a rewritten parser or installer."""
        return self.run_bash(str(self.plugin / script), *arguments, **overrides)

    def commands(self, name: str | None = None) -> list[list[str]]:
        """Return recorded commands, optionally selecting a tool and dropping its name."""
        if not self.log.exists():
            return []
        commands = [line.removesuffix("\0").split("\0") for line in self.log.read_text().splitlines()]
        return commands if name is None else [command[1:] for command in commands if command[0] == name]

    def clear_log(self) -> None:
        """Separate subcases while retaining the same disposable repository."""
        self.log.unlink(missing_ok=True)

    def built_vst3(self, build_type: str = "Release") -> Path:
        """Resolve the expected build artefact independently of build.sh --print."""
        return (
            self.plugin
            / f"cmake-build-{build_type.lower()}"
            / "ImageControl_artefacts"
            / build_type
            / "VST3/ImageControl.vst3"
        )

    def installed_vst3(self) -> Path:
        """Resolve only the sandboxed per-user macOS installation."""
        return self.home / "Library/Audio/Plug-Ins/VST3/ImageControl.vst3"

    def assert_success(self, result: subprocess.CompletedProcess[str]) -> None:
        """Include both output streams when a contract invocation fails."""
        assert result.returncode == 0, result.stdout + result.stderr

    def assert_validation(self, script: str, level: int, build_type: str = "Release") -> None:
        """Require exactly one validation against the built or installed bundle as appropriate."""
        expected = self.built_vst3(build_type) if script == "build.sh" else self.installed_vst3()
        assert self.commands("pluginval") == [["--strictness-level", str(level), "--validate", str(expected)]]
        assert (expected / "Contents/contract.txt").read_text() == "built VST3"
        if script == "install.sh":
            installed_au = self.home / "Library/Audio/Plug-Ins/Components/ImageControl.component"
            assert (installed_au / "Contents/contract.txt").read_text() == "built AU"
            assert self.commands("auval") == [["-a"], ["-strict", "-v", "aufx", "Egic", "Esgr"]]
            assert self.commands("killall") == [["-q", "AudioComponentRegistrar"]]

    def test_production_scripts_have_valid_bash_syntax(self) -> None:
        """Syntax-check both entry points and both sourced helpers without executing them."""
        for script in ("common.sh", *(f"image-control-plugin/{name}" for name in (*ALL_SCRIPTS, "common.sh"))):
            with self.subTest(script=script):
                self.assert_success(self.run_bash("-n", str(self.root / script)))
        assert not self.commands()

    def test_unknown_options_fail_before_side_effects(self) -> None:
        """Reject trailing unknown flags before tools, builds, cleaning, or installations."""
        for script in ALL_SCRIPTS:
            with self.subTest(script=script):
                result = self.invoke(script, "-v", "--unknown")
                assert result.returncode != 0, result.stdout + result.stderr
                assert not self.commands()
                assert not (self.home / "Library").exists()

    def test_build_and_installer_selection_aliases(self) -> None:
        """Short and long Debug, clean, reuse, verbose, and test flags keep identical behavior."""
        for script in SCRIPTS:
            for debug, clean, reuse, verbose, test in (
                ("-d", "-c", "-r", "-v", "-t"),
                ("--debug", "--clean", "--reuse", "--verbose", "--test"),
            ):
                with self.subTest(script=script, debug=debug):
                    marker = self.built_vst3("Debug") / "stale"
                    marker.parent.mkdir(parents=True, exist_ok=True)
                    marker.touch()
                    self.clear_log()
                    arguments = [debug, clean, verbose]
                    if script == "build.sh":
                        arguments.append(test)
                    self.assert_success(self.invoke(script, *arguments))
                    assert not marker.exists()
                    assert "Debug" in self.commands("cmake")[1]
                    assert bool(self.commands("ctest")) == (script == "build.sh")
                    self.clear_log()
                    self.assert_success(self.invoke(script, debug, reuse))
                    assert len(self.commands("cmake")) == 1
                    assert self.commands("cmake")[0][0] == "--build"

    def test_build_print_type_and_ninja_aliases(self) -> None:
        """Path-only selection honors both build-type and Windows Ninja aliases without building."""
        for print_flag, build_type, ninja in (("-p", "-b", "-n"), ("--print", "--build-type", "--ninja")):
            with self.subTest(print_flag=print_flag):
                result = self.invoke(
                    "build.sh", print_flag, build_type, "RelWithDebInfo", ninja, TEST_PLATFORM="MINGW64_NT"
                )
                self.assert_success(result)
                expected = (
                    self.plugin
                    / "cmake-build-ninja-relwithdebinfo/ImageControl_artefacts/RelWithDebInfo/VST3/ImageControl.vst3"
                )
                assert result.stdout.strip() == str(expected)
                assert not self.commands()

    def test_sanitizer_thread_and_realtime_aliases(self) -> None:
        """Both sanitizer mode aliases configure the same isolated instrumentation workspaces."""
        llvm = self.root / "fake LLVM"
        (llvm / "bin").mkdir(parents=True)
        for compiler in ("clang", "clang++"):
            executable = llvm / "bin" / compiler
            executable.write_text(f"#!{BASH}\nexit 0\n", encoding="utf-8")
            executable.chmod(0o755)
        self.write_tool("brew", 'printf "%s\\n" "$TEST_LLVM"\n')
        self.write_tool("uv", "exit 0\n")
        for mode, sanitizer in (
            ("-t", "thread"),
            ("--thread", "thread"),
            ("-R", "realtime"),
            ("--realtime", "realtime"),
        ):
            with self.subTest(mode=mode):
                self.clear_log()
                self.assert_success(self.invoke("sanitize.sh", mode, TEST_LLVM=str(llvm)))
                assert f"-DIMAGE_CONTROL_SANITIZE={sanitizer}" in self.commands("cmake")[0]
                assert len(self.commands("ctest")) == 1

    def test_coverage_report_aliases(self) -> None:
        """Both LCOV and HTML switches reach their report tools without touching real coverage tools."""
        self.write_tool("xcrun", 'printf "%s/%s\\n" "${0%/*}" "$2"\n')
        self.write_tool("llvm-profdata", "exit 0\n")
        self.write_tool("llvm-cov", "exit 0\n")
        self.write_tool("open", "exit 0\n")
        self.write_tool("git", "printf 'src/Contract.cpp\\0'\n")
        source = self.plugin / "src/Contract.cpp"
        source.parent.mkdir()
        source.touch()
        for lcov, open_flag in (("-l", "-o"), ("--lcov", "--open")):
            with self.subTest(lcov=lcov):
                self.clear_log()
                self.assert_success(self.invoke("coverage.sh", lcov, open_flag))
                assert [command[0] for command in self.commands("llvm-cov")] == ["report", "export", "show"]
                assert "-format=lcov" in self.commands("llvm-cov")[1]
                assert self.commands("open") == [
                    [str(self.plugin / "cmake-build-mac-coverage/coverage/html/index.html")]
                ]

    def test_lint_selection_and_check_aliases(self) -> None:
        """All lint selectors, database paths, job counts, and check aliases reach the production command."""
        for command in ("find", "sort", "xargs", "cat"):
            executable = shutil.which(command)
            assert executable is not None
            (self.tools / command).symlink_to(executable)
        self.write_tool("git", "printf 'image-control-plugin/tests/Contract.cpp\\n'\n")
        self.write_tool("clang-tidy", "printf 'contract.cpp:1:1: warning: contract warning\\n'\n")
        for directory in ("src", "tests", "benchmarks"):
            (self.plugin / directory).mkdir(exist_ok=True)
        (self.plugin / "tests/Contract.cpp").touch()
        (self.root / "cpp/JUCE/modules").mkdir()
        for config in (".clang-tidy", ".clang-format", "tests/.clang-tidy"):
            target = self.root / "cpp" / config
            target.parent.mkdir(parents=True, exist_ok=True)
            target.touch()
            (self.plugin / config).symlink_to(target)
        database = self.plugin / "selected lint workspace"
        for check, build_dir, jobs, selectors in (
            ("-c", "-p", "-j", ("-C", "-s")),
            ("--check", "--build-dir", "--jobs", ("--changed", "--staged")),
        ):
            for selector in selectors:
                with self.subTest(check=check, selector=selector):
                    self.clear_log()
                    result = self.invoke("lint.sh", check, selector, build_dir, str(database), jobs, "1")
                    assert result.returncode == 1, result.stdout + result.stderr
                    tidy = self.commands("clang-tidy")
                    assert len(tidy) == 1
                    assert tidy[0][:2] == ["-p", str(database)]
                    assert tidy[0][-1] == str(self.plugin / "tests/Contract.cpp")
                    git = self.commands("git")
                    assert any("--cached" in command for command in git) == (selector in ("-s", "--staged"))
        self.clear_log()
        self.write_tool("clang-tidy", "exit 0\n")
        self.assert_success(self.invoke("lint.sh", "--", "--help"))
        assert "--help" in self.commands("clang-tidy")[0]
        self.clear_log()
        self.write_tool("clang-tidy", "printf 'contract.cpp:1:1: warning: contract warning\\n'\n")
        result = self.invoke("lint.sh", "--changed", "--build-dir", str(database), "--jobs", "1")
        self.assert_success(result)

    def test_format_check_aliases(self) -> None:
        """Both check aliases call the nonmutating formatter mode rather than rewriting files."""
        for command in ("find", "sort"):
            executable = shutil.which(command)
            assert executable is not None
            (self.tools / command).symlink_to(executable)
        for directory in ("src", "tests", "benchmarks"):
            (self.plugin / directory).mkdir(exist_ok=True)
        self.write_tool("clang-format", "exit 37\n")
        for flag in ("-c", "--check"):
            with self.subTest(flag=flag):
                self.clear_log()
                result = self.invoke("format.sh", flag)
                assert result.returncode == 37, result.stdout + result.stderr
                assert "--dry-run" in self.commands("clang-format")[0]
                assert "-i" not in self.commands("clang-format")[0]

    def test_validation_is_opt_in(self) -> None:
        """Ordinary builds and installs never invoke either validator."""
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.clear_log()
                self.assert_success(self.invoke(script))
                assert len(self.commands("cmake")) == 2
                assert not self.commands("pluginval")
                assert not self.commands("auval")

    def test_validate_long_and_short_flags_default_to_five(self) -> None:
        """Both aliases use level five and validate exactly the intended artefact."""
        for script in SCRIPTS:
            for flag in ("--validate", "-V"):
                with self.subTest(script=script, flag=flag):
                    self.clear_log()
                    self.assert_success(self.invoke(script, flag))
                    self.assert_validation(script, 5)

    def test_validation_level_implies_validation(self) -> None:
        """Representative nondefault levels reach validation without a separate --validate flag."""
        for script in SCRIPTS:
            for flag, level in (("--validation-level", 8), ("-L", 10)):
                with self.subTest(script=script, flag=flag, level=level):
                    self.clear_log()
                    self.assert_success(self.invoke(script, flag, str(level)))
                    self.assert_validation(script, level)

    def test_invalid_levels_fail_before_build_or_install_side_effects(self) -> None:
        """Reject missing, malformed, and out-of-range levels before cleaning or replacing files."""
        built_marker = self.built_vst3() / "existing.txt"
        installed_marker = self.installed_vst3() / "existing.txt"
        for marker in (built_marker, installed_marker):
            marker.parent.mkdir(parents=True)
            marker.write_text("do not replace", encoding="utf-8")
        invalid = (None, "", "word", "5.0", "4", "11")
        for script in SCRIPTS:
            for flag in ("--validation-level", "-L"):
                for level in invalid:
                    with self.subTest(script=script, flag=flag, level=level):
                        arguments = ["--clean", flag]
                        if level is not None:
                            arguments.append(level)
                        result = self.invoke(script, *arguments)
                        assert result.returncode != 0, result.stdout + result.stderr
                        assert not self.commands(), self.commands()
                        for marker in (built_marker, installed_marker):
                            assert marker.read_text() == "do not replace"

    def test_build_validation_follows_build_and_unit_tests(self) -> None:
        """Validate the selected nondefault build only after CTest succeeds."""
        result = self.invoke("build.sh", "--build-type", "RelWithDebInfo", "--test", "--validation-level", "8")
        self.assert_success(result)
        self.assert_validation("build.sh", 8, "RelWithDebInfo")
        assert [command[0] for command in self.commands()] == ["cmake", "cmake", "ctest", "pluginval"]
        build_dir = self.plugin / "cmake-build-relwithdebinfo"
        assert (build_dir / "tests-built").is_file()
        assert "ImageControlTests" in self.commands("cmake")[1]
        ctest = self.commands("ctest")[0]
        assert ctest[ctest.index("--test-dir") + 1] == str(build_dir)
        assert ctest[ctest.index("--build-config") + 1] == "RelWithDebInfo"
        assert "--no-tests=error" in ctest

    def test_install_debug_validates_installed_bundle_only_once(self) -> None:
        """Forward build selection, but never forward installer validation options to build.sh."""
        self.assert_success(self.invoke("install.sh", "--debug", "--validate", "--validation-level", "9"))
        self.assert_validation("install.sh", 9, "Debug")
        assert self.built_vst3("Debug").is_dir()
        assert "Debug" in self.commands("cmake")[1]
        assert not self.built_vst3().exists()
        assert not self.commands("ctest")

    def test_print_bypasses_build_tests_clean_and_validation(self) -> None:
        """--print remains path-only regardless of validation flag position or test selection."""
        marker = self.built_vst3("Debug") / "keep.txt"
        marker.parent.mkdir(parents=True)
        marker.write_text("do not clean", encoding="utf-8")
        for arguments in (
            ("--print", "--validate", "--validation-level", "10", "--test", "--clean", "--debug"),
            ("-V", "--test", "--validation-level", "6", "--debug", "--clean", "-p"),
        ):
            with self.subTest(arguments=arguments):
                result = self.invoke("build.sh", *arguments)
                self.assert_success(result)
                assert result.stdout.strip() == str(self.built_vst3("Debug"))
                assert not self.commands()
                assert marker.read_text() == "do not clean"
                assert not (self.home / "Library").exists()

    def test_missing_pluginval_builds_and_validates_with_the_pinned_helper(self) -> None:
        """Missing PATH tools delegate setup and validation with the selected level and exact artefact."""
        (self.tools / "pluginval").unlink()
        for script in SCRIPTS:
            for arguments, level in ((["--validate"], 5), (["--validation-level", "10"], 10)):
                with self.subTest(script=script, level=level):
                    self.clear_log()
                    result = self.invoke(script, *arguments)
                    self.assert_success(result)
                    expected = self.built_vst3() if script == "build.sh" else self.installed_vst3()
                    assert self.commands("cmake")[-1] == [
                        f"-DSTRICTNESS={level}",
                        f"-DPLUGIN={expected}",
                        "-P",
                        str(self.root / "cpp/cmake/RunPluginval.cmake"),
                    ]
                    assert not self.commands("pluginval")
                    assert bool(self.commands("auval")) == (script == "install.sh")

    def test_pinned_pluginval_setup_or_validation_failure_propagates(self) -> None:
        """Never report success when automatic validator setup or its validation fails."""
        (self.tools / "pluginval").unlink()
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.clear_log()
                result = self.invoke(script, "--validate", PLUGINVAL_SETUP_STATUS="27")
                assert result.returncode == 27, result.stdout + result.stderr
                assert len(self.commands("cmake")) == 3

    def test_missing_pluginval_does_not_trigger_setup_without_validation(self) -> None:
        """Automatic downloads remain opt-in, including path-only invocations."""
        (self.tools / "pluginval").unlink()
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.clear_log()
                self.assert_success(self.invoke(script))
                assert len(self.commands("cmake")) == 2
        self.clear_log()
        self.assert_success(self.invoke("build.sh", "--print", "--validate"))
        assert not self.commands()

    def test_validator_failure_propagates(self) -> None:
        """Never turn a failed VST3 validation into a successful build or installation."""
        for script in SCRIPTS:
            with self.subTest(script=script):
                self.clear_log()
                result = self.invoke(script, "--validation-level", "7", PLUGINVAL_STATUS="23")
                assert result.returncode == 23, result.stdout + result.stderr
                self.assert_validation(script, 7)

    def test_audio_unit_validator_failure_propagates(self) -> None:
        """Preserve the existing strict auval failure instead of reporting installation success."""
        result = self.invoke("install.sh", "--validate", AUVAL_STATUS="24")
        assert result.returncode == 24, result.stdout + result.stderr
        assert self.commands("auval") == [["-a"], ["-strict", "-v", "aufx", "Egic", "Esgr"]]
        assert not self.commands("pluginval")

    def test_failed_build_or_tests_prevent_validation(self) -> None:
        """Validation cannot run on artefacts from an unsuccessful build or test invocation."""
        for overrides, status in (({"CMAKE_BUILD_STATUS": "25"}, 25), ({"CTEST_STATUS": "26"}, 26)):
            with self.subTest(overrides=overrides):
                self.clear_log()
                result = self.invoke("build.sh", "--test", "--validate", **overrides)
                assert result.returncode == status, result.stdout + result.stderr
                assert not self.commands("pluginval")

    def prepare_installed(self, *, audio_unit: bool = False, windows: bool = False) -> Path:
        """Seed a complete old installation with nested content that a partial copy cannot replace."""
        if windows:
            destination = self.root / "Common Files user's [exact]/VST3/ImageControl.vst3"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text("old DLL", encoding="utf-8")
        else:
            name = "Components/ImageControl.component" if audio_unit else "VST3/ImageControl.vst3"
            destination = self.home / "Library/Audio/Plug-Ins" / name
            (destination / "Contents/nested").mkdir(parents=True, exist_ok=True)
            (destination / "Contents/contract.txt").write_text("old bundle", encoding="utf-8")
            (destination / "Contents/nested/preserve.txt").write_text("old resource", encoding="utf-8")
        return destination

    def inject_install_failure(self, destination: Path, failure: str) -> None:
        """Wrap real disposable filesystem operations, failing only the requested transaction phase."""
        self.environment |= {"FAIL_DESTINATION": str(destination), "INSTALL_FAILURE": failure}
        for command in ("cp", "mv", "diff", "rm"):
            executable = shutil.which(command)
            assert executable is not None
            (self.tools / command).unlink()
            prefix = f"real_tool={str(executable)!r}\n"
            if command == "cp":
                body = """staged="${@: -1}"
if [[ "$staged" == "${FAIL_DESTINATION%/*}/.ImageControl-install."* ]]; then
    case "$INSTALL_FAILURE" in
        copy)
            if [[ "$FAIL_DESTINATION" == *.vst3 && "$TEST_PLATFORM" != Darwin ]]; then
                printf 'partial DLL' > "$staged"
            else
                mkdir -p "$staged/Contents"
                printf 'partial bundle' > "$staged/Contents/contract.txt"
            fi
            exit 31 ;;
        corrupt)
            "$real_tool" "$@"
            if [ -d "$staged" ]; then
                printf 'corrupted bundle' > "$staged/Contents/contract.txt"
            else
                printf 'corrupted DLL' > "$staged"
            fi
            exit 0 ;;
    esac
fi
"""
            elif command == "mv":
                body = """if [[ "$1" == "${FAIL_DESTINATION%/*}/.ImageControl-install."* ]]; then
    case "$1:$INSTALL_FAILURE" in
        */previous:recovery) exit 33 ;;
        */ImageControl.*:publish|*/ImageControl.*:recovery) exit 32 ;;
    esac
elif [ "$1" = "$FAIL_DESTINATION" ] && [ "$INSTALL_FAILURE" = backup ]; then
    exit 34
fi
"""
            elif command == "diff":
                body = """if [ "${@: -1}" = "$FAIL_DESTINATION" ] && [ "$INSTALL_FAILURE" = published-copy ]; then
    exit 35
fi
"""
            else:
                body = """if [ "${@: -1}" = "$FAIL_DESTINATION" ] && [ "$INSTALL_FAILURE" = recovery-remove ]; then
    exit 36
fi
"""
            self.write_tool(command, prefix + body + 'exec "$real_tool" "$@"\n')
        self.write_tool(
            "codesign",
            """target="${@: -1}"
if [[ "$FAIL_DESTINATION" == *.component ]]; then
    case "$INSTALL_FAILURE" in
        staged-signature) exit 37 ;;
        published-signature|recovery-remove)
            if [ "$target" = "$FAIL_DESTINATION" ]; then exit 38; fi ;;
    esac
fi
""",
        )

    def assert_old_installation(self, destination: Path) -> None:
        """Require the full old tree, not merely a destination that still exists."""
        if destination.is_file():
            assert destination.read_text() == "old DLL"
        else:
            assert (destination / "Contents/contract.txt").read_text() == "old bundle"
            assert (destination / "Contents/nested/preserve.txt").read_text() == "old resource"
        assert not list(destination.parent.glob(".ImageControl-install.*"))

    def configure_windows_install(self) -> Path:
        """Map the system-wide target into the disposable repository and keep paths exact."""
        destination = self.prepare_installed(windows=True)
        self.environment |= {
            "TEST_PLATFORM": "MINGW64_NT",
            "COMMONPROGRAMFILES": r"C:\Common Files user's [exact]",
            "TEST_COMMON_DIRECTORY": str(destination.parent.parent),
        }
        self.write_tool(
            "cygpath",
            """case "$1" in
    -u) test "$2" = "$COMMONPROGRAMFILES"; printf '%s\\n' "$TEST_COMMON_DIRECTORY" ;;
    -w) printf '%s\\n' "$2" ;;
    *) exit 91 ;;
esac
""",
        )
        return destination

    def test_install_replacement_preserves_old_bundle_on_failures(self) -> None:
        """Partial staging, corruption, backup/publication, and AU signing failures preserve the old tree."""
        for audio_unit in (False, True):
            failures = ["copy", "corrupt", "backup", "publish", "published-copy"]
            if audio_unit:
                failures.extend(("staged-signature", "published-signature"))
            for failure in failures:
                with self.subTest(audio_unit=audio_unit, failure=failure):
                    destination = self.prepare_installed(audio_unit=audio_unit)
                    self.inject_install_failure(destination, failure)
                    self.clear_log()
                    result = self.invoke("install.sh", "--validate")
                    assert result.returncode != 0, result.stdout + result.stderr
                    self.assert_old_installation(destination)
                    assert not self.commands("killall")
                    assert not self.commands("auval")
                    assert not self.commands("pluginval")
                    if failure in ("copy", "corrupt", "staged-signature"):
                        assert not any(command[0] == str(destination) for command in self.commands("mv"))
                    shutil.rmtree(destination)
                    for command in ("cp", "mv", "diff", "rm"):
                        (self.tools / command).unlink()
                        executable = shutil.which(command)
                        assert executable is not None
                        (self.tools / command).symlink_to(executable)

    def test_install_recovery_failure_retains_previous_bundle(self) -> None:
        """A failed rollback reports its recovery location and never cleans the only old copy."""
        for failure in ("recovery", "recovery-remove"):
            with self.subTest(failure=failure):
                destination = self.prepare_installed(audio_unit=True)
                self.inject_install_failure(destination, failure)
                self.clear_log()
                result = self.invoke("install.sh", "--validate")
                assert result.returncode != 0, result.stdout + result.stderr
                transactions = list(destination.parent.glob(".ImageControl-install.*"))
                assert len(transactions) == 1
                backup = transactions[0] / "previous"
                assert (backup / "Contents/contract.txt").read_text() == "old bundle"
                assert (backup / "Contents/nested/preserve.txt").read_text() == "old resource"
                assert str(backup) in result.stderr
                assert not self.commands("killall")
                assert not self.commands("auval")
                assert not self.commands("pluginval")
                shutil.rmtree(transactions[0])
                if destination.exists():
                    shutil.rmtree(destination)
                for command in ("cp", "mv", "diff", "rm"):
                    (self.tools / command).unlink()
                    executable = shutil.which(command)
                    assert executable is not None
                    (self.tools / command).symlink_to(executable)

    def test_successful_replacement_verifies_and_cleans_transactions(self) -> None:
        """Both formats publish exact complete bundles, verify AU signatures, and leave no old copy."""
        destinations = [self.prepare_installed(), self.prepare_installed(audio_unit=True)]
        self.inject_install_failure(destinations[1], "none")
        self.assert_success(self.invoke("install.sh", "--validate"))
        self.assert_validation("install.sh", 5)
        for destination, expected in zip(destinations, ("built VST3", "built AU"), strict=True):
            assert (destination / "Contents/contract.txt").read_text() == expected
            assert not (destination / "Contents/nested").exists()
            assert not list(destination.parent.glob(".ImageControl-install.*"))
        signatures = self.commands("codesign")
        assert ["--verify", "--deep", "--strict", str(destinations[1])] in signatures
        names = [command[0] for command in self.commands()]
        assert names.index("codesign") < names.index("killall") < names.index("auval") < names.index("pluginval")

    def test_windows_direct_install_publishes_only_the_dll(self) -> None:
        """A writable system directory installs the plain DLL without elevation or macOS operations."""
        destination = self.configure_windows_install()
        self.assert_success(self.invoke("install.sh"))
        assert destination.is_file()
        assert destination.read_text() == "built DLL"
        assert not list(destination.parent.glob(".ImageControl-install.*"))
        assert not self.commands("powershell.exe")
        assert not self.commands("killall")
        assert not self.commands("codesign")

    def test_windows_failed_publish_or_recovery_never_retries_elevated(self) -> None:
        """Successful rollback still fails the install, and recovery backups cannot be lost to a retry."""
        for failure in ("publish", "recovery"):
            with self.subTest(failure=failure):
                destination = self.configure_windows_install()
                self.inject_install_failure(destination, failure)
                self.write_tool("powershell.exe", "exit 0\n")
                self.clear_log()
                result = self.invoke("install.sh")
                assert result.returncode != 0, result.stdout + result.stderr
                assert not self.commands("powershell.exe")
                if failure == "publish":
                    self.assert_old_installation(destination)
                else:
                    transactions = list(destination.parent.glob(".ImageControl-install.*"))
                    assert len(transactions) == 1
                    assert (transactions[0] / "previous").read_text() == "old DLL"
                    assert str(transactions[0] / "previous") in result.stderr

    def test_windows_elevated_payload_uses_escaped_native_literal_paths(self) -> None:
        """Inspect the exact UTF-16 payload passed to UAC, including apostrophes and wildcard characters."""
        destination = self.configure_windows_install()
        self.inject_install_failure(destination, "copy")
        temporary = self.root / "user's temporary files [exact]"
        temporary.mkdir()
        self.environment["TMPDIR"] = str(temporary)
        mktemp = shutil.which("mktemp")
        assert mktemp is not None
        (self.tools / "mktemp").unlink()
        self.write_tool(
            "mktemp",
            f"real_tool={mktemp!r}\n"
            'if [ "$#" -eq 0 ]; then exec "$real_tool" "$TMPDIR/install-log.XXXXXXXX"; fi\n'
            'exec "$real_tool" "$@"\n',
        )
        self.write_tool(
            "cygpath",
            """case "$1" in
    -u) printf '%s\\n' "$TEST_COMMON_DIRECTORY" ;;
    -w) printf 'C:%s\\n' "${2//\\//\\\\}" ;;
    *) exit 91 ;;
esac
""",
        )
        self.write_tool("powershell.exe", "exit 39\n")
        result = self.invoke("install.sh")
        assert result.returncode != 0
        self.assert_old_installation(destination)
        commands = self.commands("powershell.exe")
        assert len(commands) == 1
        assert commands[0][:2] == ["-NoProfile", "-Command"]
        outer = commands[0][2]
        assert "-Verb RunAs -Wait -PassThru" in outer
        match = re.search(r"'-EncodedCommand','([A-Za-z0-9+/=]+)'", outer)
        assert match is not None, outer
        payload = base64.b64decode(match[1], validate=True).decode("utf-16-le")
        conversions = [command[1] for command in self.commands("cygpath") if command[0] == "-w"]
        assert len(conversions) == 3
        for variable, path in zip(("directory", "source", "log"), conversions, strict=True):
            assert "'" in path
            assert " " in path
            native = "C:" + path.replace("/", "\\")
            escaped = native.replace("'", "''")
            assert f"${variable} = '{escaped}';" in payload, payload
        for operation in ("Get-FileHash", "Test-Path", "Move-Item", "Remove-Item", "Out-File"):
            lines = [line for line in payload.splitlines() if operation in line]
            assert lines
            assert all(f"{operation} -LiteralPath" in line for line in lines), lines
        assert not list(temporary.iterdir())

    def test_native_powershell_transaction_success_and_failures(self) -> None:
        """Run the production elevated body without UAC against disposable paths when PowerShell exists."""
        powershell = shutil.which("powershell.exe") or shutil.which("pwsh")
        if powershell is None:
            self.skipTest("Native PowerShell is unavailable, encoded-payload contracts still run")
        destination = self.configure_windows_install()
        self.inject_install_failure(destination, "copy")
        self.write_tool("powershell.exe", "exit 39\n")
        result = self.invoke("install.sh")
        assert result.returncode != 0
        outer = self.commands("powershell.exe")[0][2]
        match = re.search(r"'-EncodedCommand','([A-Za-z0-9+/=]+)'", outer)
        assert match is not None
        payload = base64.b64decode(match[1], validate=True).decode("utf-16-le")
        body = payload.split("\n", 1)[1]

        def literal(path: Path) -> str:
            return "'" + str(path).replace("'", "''") + "'"

        for failure in ("none", "copy", "staged-hash", "publish", "published-hash", "recovery"):
            with self.subTest(failure=failure):
                sandbox = self.root / f"native user's [exact] {failure}"
                directory = sandbox / "Common Files user's [exact]/VST3"
                directory.mkdir(parents=True)
                installed = directory / "ImageControl.vst3"
                installed.write_text("old DLL", encoding="utf-8")
                source = sandbox / "source user's [exact]/ImageControl.vst3"
                source.parent.mkdir()
                if failure != "copy":
                    source.write_text("new DLL", encoding="utf-8")
                log = sandbox / "failure user's [exact].log"
                prelude = (
                    f"$directory = {literal(directory)}; $source = {literal(source)}; $log = {literal(log)};\n"
                    f"$contractFailure = '{failure}'\n"
                    """
function Move-Item {
    param([string]$LiteralPath, [string]$Destination)
    if (($contractFailure -eq 'publish' -or $contractFailure -eq 'recovery') -and
        $LiteralPath.EndsWith('ImageControl.vst3') -and $LiteralPath.Contains('.ImageControl-install.')) {
        throw 'Injected publication failure'
    }
    if ($contractFailure -eq 'recovery' -and $LiteralPath.EndsWith('previous')) {
        throw 'Injected recovery failure'
    }
    Microsoft.PowerShell.Management\\Move-Item -LiteralPath $LiteralPath -Destination $Destination
}
function Get-FileHash {
    param([string]$LiteralPath)
    if (($contractFailure -eq 'staged-hash' -and $LiteralPath.Contains('.ImageControl-install.')) -or
        ($contractFailure -eq 'published-hash' -and $LiteralPath -eq (Join-Path $directory 'ImageControl.vst3'))) {
        throw 'Injected verification failure'
    }
    Microsoft.PowerShell.Utility\\Get-FileHash -LiteralPath $LiteralPath
}
"""
                )
                encoded = base64.b64encode((prelude + body).encode("utf-16-le")).decode("ascii")
                native = subprocess.run(  # noqa: S603
                    [powershell, "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                    capture_output=True,
                    text=True,
                    timeout=15,
                    check=False,
                )
                transactions = list(directory.glob(".ImageControl-install.*"))
                if failure == "none":
                    assert native.returncode == 0, native.stdout + native.stderr
                    assert installed.read_text() == "new DLL"
                    assert not transactions
                    assert not log.exists()
                else:
                    assert native.returncode != 0, native.stdout + native.stderr
                    assert log.is_file()
                    if failure == "recovery":
                        assert len(transactions) == 1
                        backup = transactions[0] / "previous"
                        assert backup.read_text() == "old DLL"
                        assert str(backup) in log.read_text(encoding="utf-8-sig")
                    else:
                        assert installed.read_text() == "old DLL"
                        assert not transactions

    def test_windows_missing_pluginval_passes_native_path_to_pinned_helper(self) -> None:
        """The automatic setup path must preserve Windows paths and the requested strictness."""
        (self.tools / "pluginval").unlink()
        vst3 = str(self.root / "custom bundle path/ImageControl.vst3")
        result = self.run_bash(
            "-c",
            'source "$1/common.sh"; plugin_validate_vst3 "$2" "$3"',
            "validation-contract",
            str(self.plugin),
            vst3,
            "9",
            TEST_PLATFORM="MINGW64_NT",
        )
        self.assert_success(result)
        assert self.commands("cygpath") == [["-w", vst3]]
        assert self.commands("cmake") == [
            [
                "-DSTRICTNESS=9",
                f"-DPLUGIN={self.environment['TEST_WINDOWS_PATH']}",
                "-P",
                str(self.root / "cpp/cmake/RunPluginval.cmake"),
            ]
        ]

    def test_windows_helper_converts_the_supplied_vst3_path(self) -> None:
        """Exercise the shared Windows helper without a native compiler or elevated installer."""
        vst3 = str(self.root / "custom bundle path/ImageControl.vst3")
        result = self.run_bash(
            "-c",
            'source "$1/common.sh"; plugin_validate_vst3 "$2" "$3"',
            "validation-contract",
            str(self.plugin),
            vst3,
            "10",
            TEST_PLATFORM="MINGW64_NT",
        )
        self.assert_success(result)
        assert self.commands("cygpath") == [["-w", vst3]]
        assert self.commands("pluginval") == [
            ["--strictness-level", "10", "--validate", self.environment["TEST_WINDOWS_PATH"]]
        ]
        assert not self.commands("cmake")
        assert not self.commands("auval")


if __name__ == "__main__":
    unittest.main(verbosity=2)
