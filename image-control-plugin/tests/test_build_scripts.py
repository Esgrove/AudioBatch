# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Exercise ImageControl's production shell scripts with disposable bundles and recording tools.

Run with `uv run --script tests/test_build_scripts.py` from image-control-plugin/.
No compiler, installed plugin, Audio Unit registry, or real validator is used.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which("bash")
SCRIPTS = ("build.sh", "install.sh")


class BuildScriptContracts(unittest.TestCase):
    """Run unchanged production scripts in a miniature repository whose paths contain spaces."""

    def setUp(self) -> None:
        """Isolate both HOME and PATH so no installed plugin or real validator can be touched."""
        assert BASH is not None, "Bash is required for build script contracts"
        temporary = tempfile.TemporaryDirectory(prefix="image-control-scripts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        for relative in (
            "common.sh",
            "image-control-plugin/common.sh",
            "image-control-plugin/build.sh",
            "image-control-plugin/install.sh",
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
        for command in ("dirname", "tr", "mkdir", "rm", "cp", "seq", "grep"):
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
        for script in ("common.sh", *(f"image-control-plugin/{name}" for name in (*SCRIPTS, "common.sh"))):
            with self.subTest(script=script):
                self.assert_success(self.run_bash("-n", str(self.root / script)))
        assert not self.commands()

    def test_help_describes_optional_validation_and_levels(self) -> None:
        """Document validator lookup, both validation flags, and the default and accepted range."""
        for script in SCRIPTS:
            with self.subTest(script=script):
                result = self.invoke(script, "--help")
                self.assert_success(result)
                for text in ("--validate", "-V", "--validation-level", "pluginval", "PATH", "5", "10"):
                    assert text in result.stdout, result.stdout
                assert ("built" if script == "build.sh" else "installed") in result.stdout.lower()
                if script == "install.sh":
                    assert "auval" in result.stdout
                assert not self.commands()

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

    def test_each_valid_level_implies_validation(self) -> None:
        """Every accepted value works without a separate --validate flag."""
        for script in SCRIPTS:
            for level in range(5, 11):
                with self.subTest(script=script, level=level):
                    self.clear_log()
                    self.assert_success(self.invoke(script, "--validation-level", str(level)))
                    self.assert_validation(script, level)

    def test_invalid_levels_fail_before_build_or_install_side_effects(self) -> None:
        """Reject missing, empty, noncanonical, noninteger, and out-of-range arguments before --clean."""
        built_marker = self.built_vst3() / "existing.txt"
        installed_marker = self.installed_vst3() / "existing.txt"
        for marker in (built_marker, installed_marker):
            marker.parent.mkdir(parents=True)
            marker.write_text("do not replace", encoding="utf-8")
        invalid = (None, "", "word", "5.0", "5e0", "-1", "4", "11", "+5", "05", " 5", "5 ", "99999999999999999999")
        for script in SCRIPTS:
            for level in invalid:
                with self.subTest(script=script, level=level):
                    arguments = ["--clean", "--validation-level"]
                    if level is not None:
                        arguments.append(level)
                    result = self.invoke(script, *arguments)
                    assert result.returncode != 0, result.stdout + result.stderr
                    assert "validation" in (result.stdout + result.stderr).lower()
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
        assert self.commands("ctest") == [
            [
                "--test-dir",
                str(build_dir),
                "--build-config",
                "RelWithDebInfo",
                "--output-on-failure",
                "--no-tests=error",
                "--timeout",
                "180",
            ]
        ]

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
                        str(self.plugin / "../cpp/cmake/RunPluginval.cmake"),
                    ]
                    assert "building the pinned version" in result.stdout
                    assert "skipping" not in result.stdout
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
                assert "Installation complete" not in result.stdout

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
                assert "Installation complete" not in result.stdout

    def test_audio_unit_validator_failure_propagates(self) -> None:
        """Preserve the existing strict auval failure instead of reporting installation success."""
        result = self.invoke("install.sh", "--validate", AUVAL_STATUS="24")
        assert result.returncode == 24, result.stdout + result.stderr
        assert self.commands("auval") == [["-a"], ["-strict", "-v", "aufx", "Egic", "Esgr"]]
        assert not self.commands("pluginval")
        assert "Installation complete" not in result.stdout

    def test_failed_build_or_tests_prevent_validation(self) -> None:
        """Validation cannot run on artefacts from an unsuccessful build or test invocation."""
        for overrides, status in (({"CMAKE_BUILD_STATUS": "25"}, 25), ({"CTEST_STATUS": "26"}, 26)):
            with self.subTest(overrides=overrides):
                self.clear_log()
                result = self.invoke("build.sh", "--test", "--validate", **overrides)
                assert result.returncode == status, result.stdout + result.stderr
                assert not self.commands("pluginval")

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
                str(self.plugin / "../cpp/cmake/RunPluginval.cmake"),
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
