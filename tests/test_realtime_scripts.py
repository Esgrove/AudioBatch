# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Check realtime sanitizer routing and early failures without native builds or installed toolchains."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = ("cpp", "image-control-plugin", "rust")


class RealtimeScripts(unittest.TestCase):
    """Run copied production scripts with isolated homes and logged, noncompiling tool substitutes."""

    def setUp(self) -> None:
        """Copy only shell entry points and helpers into a temporary repository with spaced paths."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-realtime-scripts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        self.root.mkdir()
        shutil.copyfile(ROOT / "common.sh", self.root / "common.sh")
        for project in PROJECTS:
            (self.root / project).mkdir()
            script = "rtsan.sh" if project == "rust" else "sanitize.sh"
            for name in ("common.sh", script):
                shutil.copyfile(ROOT / project / name, self.root / project / name)
        plugin_rust = self.root / "image-control-plugin/rust"
        plugin_rust.mkdir()
        for name in ("common.sh", "rtsan.sh"):
            shutil.copyfile(ROOT / "image-control-plugin/rust" / name, plugin_rust / name)
        self.tools = self.root / "bin"
        self.llvm = self.root / "fake LLVM"
        self.log = self.root / "commands.log"
        self.home = self.root / "home"
        self.home.mkdir()
        self.tools.mkdir()
        (self.llvm / "bin").mkdir(parents=True)
        scripts = {
            "uname": 'printf "%s\\n" "${TEST_PLATFORM:-Darwin}"\n',
            "brew": 'printf "%s\\n" "$TEST_LLVM"\n',
            "cmake": "exit 0\n",
            "ctest": "exit 0\n",
            "rustc": 'printf "%s\\n" "rustc 1.99.0-nightly" "host: aarch64-apple-darwin" "LLVM version: 22.1.0"\n',
            "rg": "prefix=${1#^}; while IFS= read -r line; do\n"
            '  case "$line" in "$prefix"*) printf "%s\\n" "$line";; esac\ndone\n',
            "clang": 'case "$1" in\n'
            '  --version) printf "clang version %s\\n" "${TEST_CLANG_VERSION:-22.1.0}";;\n'
            '  --print-file-name=*) printf "%s/missing-runtime.dylib\\n" "$TEST_LLVM";;\n'
            "  *) exit 91;;\nesac\n",
        }
        for name in ("uv", "rustup", "cargo", "jq", "clang++", "llvm-symbolizer"):
            scripts[name] = "exit 91\n"
        for name, body in scripts.items():
            directory = self.llvm / "bin" if name in ("clang", "clang++", "llvm-symbolizer") else self.tools
            tool = directory / name
            logging = (
                ""
                if name in ("uname", "rg")
                else ('printf "%s\\t" "${0##*/}" "$@" >> "$TEST_LOG"\nprintf "\\n" >> "$TEST_LOG"\n')
            )
            # Bash runs executable text stubs itself, without needing another interpreter process.
            tool.write_text(logging + body, encoding="utf-8")
            tool.chmod(0o755)

    def invoke(self, project: str, *arguments: str, **overrides: str) -> subprocess.CompletedProcess[str]:
        """Run outside the project without inherited compiler, startup, sanitizer, or user-home settings."""
        self.log.unlink(missing_ok=True)
        environment = {
            "PATH": os.pathsep.join((str(self.tools), os.defpath)),
            "HOME": str(self.home),
            "CARGO_HOME": str(self.home / "cargo"),
            "RUSTUP_HOME": str(self.home / "rustup"),
            "TMPDIR": str(self.home),
            "LC_ALL": "C",
            "TEST_LOG": str(self.log),
            "TEST_LLVM": str(self.llvm),
        }
        environment.update(overrides)
        bash = shutil.which("bash")
        assert bash is not None, "Bash is required for sanitizer script contracts"
        script = "rtsan.sh" if project in ("rust", "image-control-plugin/rust") else "sanitize.sh"
        return subprocess.run(  # noqa: S603
            [bash, str(self.root / project / script), *arguments],
            cwd=self.root,
            env=environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def commands(self) -> list[list[str]]:
        """Preserve spaced arguments and return an empty log when validation stopped before tool execution."""
        if not self.log.exists():
            return []
        return [line.rstrip("\t").split("\t") for line in self.log.read_text(encoding="utf-8").splitlines()]

    def prepare_plugin_rtsan_tools(self, *, au_markers: bool = True) -> tuple[Path, Path]:
        """Model builds and exact package outputs while recording instrumentation across compiler boundaries."""
        runtime = self.llvm / "rtsan-runtime.dylib"
        runtime.touch()
        target = self.root / "image-control-plugin/rust/target/rtsan-own"
        instrumented = target / "aarch64-apple-darwin"
        original = self.root / "rust/target"
        for directory in (original, instrumented):
            for extension in ("vst3", "clap", "component"):
                (directory / f"bundled/ImageControl RS.{extension}/Contents").mkdir(parents=True)
        native = target / "au/arm64/release"
        (native / "ImageControl RS.component/Contents").mkdir(parents=True)
        host = self.tools / "native-probe"
        host.write_text("exit 0\n", encoding="utf-8")
        host.chmod(0o755)
        au_host = native / "ImageControlRSAURealtime"
        markers = (
            "clean-control allocation-control mutex-control au-cold-automation au-reset"
            if au_markers
            else "Usage: qualification host --bundle path"
        )
        au_host.write_text(f"printf '%s\\n' '{markers}'\n", encoding="utf-8")
        au_host.chmod(0o755)
        logging = 'printf "%s\\t" "${0##*/}" "$@" >> "$TEST_LOG"\nprintf "\\n" >> "$TEST_LOG"\n'
        bodies = {
            self.llvm / "bin/clang": 'case "$1" in\n'
            '  --version) printf "clang version 22.1.0\\n";;\n'
            f'  --print-file-name=*) printf "%s\\n" "{runtime}";;\nesac\n',
            self.tools / "cargo": 'printf "%s\\t%s\\t%s\\n" "$CARGO_TARGET_DIR" '
            '"${CARGO_ENCODED_RUSTFLAGS:-}" "${RUSTFLAGS:-}" >> "$HOME/cargo-flags.log"\n'
            'if [ "${FAIL_PLUGIN_BUILD:-}" = 1 ] && [[ " $* " == *" --lib "* ]]; then exit 37; fi\n',
            self.tools / "jq": 'case "$*" in\n'
            '  *packages*) printf "0.1.0\\n";;\n'
            f'  *) printf "%s\\n" "{host}";;\nesac\n',
            self.tools / "uv": 'printf "%s\\n" "$CARGO_TARGET_DIR" >> "$HOME/runner-targets.log"\n'
            'case " $* " in *" $FAIL_CASE "*) exit 38;; esac\n',
        }
        for path, body in bodies.items():
            path.write_text(logging + body, encoding="utf-8")
            path.chmod(0o755)
        return target, original

    def test_rust_plugin_formats_validate_before_tools(self) -> None:
        """Malformed selections and missing reused artifacts never compile, package, or inspect toolchains."""
        project = "image-control-plugin/rust"
        for arguments in (
            ("--format",),
            ("-f", ""),
            ("-f", "other"),
            ("-f", "all", "-b", "bundle"),
            ("-f", "clap", "-b", "bundle"),
        ):
            with self.subTest(arguments=arguments):
                result = self.invoke(project, *arguments)
                assert result.returncode != 0
                assert self.commands() == []
        for format_name, description in (("vst3", "VST3"), ("clap", "CLAP"), ("au", "Audio Unit")):
            result = self.invoke(project, "-N", "-f", format_name, "-b", "missing bundle")
            assert result.returncode != 0
            assert f"Build-tree {description} is missing" in result.stderr
            assert self.commands() == []
        result = self.invoke(project, "-f", "all", "--help")
        assert result.returncode == 0
        assert "-f | --format" in result.stdout
        assert self.commands() == []

    def test_rust_plugin_clap_builds_exact_instrumented_library_and_host(self) -> None:
        """Build the actual plugin before packaging, retaining spaced compiler paths and direct CLAP cases."""
        target, original = self.prepare_plugin_rtsan_tools()
        result = self.invoke("image-control-plugin/rust", "--format", "clap", FAIL_CASE="none")
        assert result.returncode == 0, result.stdout + result.stderr
        cargo = [command for command in self.commands() if command[0] == "cargo"]
        assert len(cargo) == 3
        library, package, host = cargo
        assert "--lib" in library
        assert "--release" in library
        assert library[library.index("--target") + 1] == "aarch64-apple-darwin"
        assert package[-4:] == ["--release", "--no-build", "--format", "clap"]
        assert host[host.index("--example") + 1] == "clap_host"
        flags = (self.home / "cargo-flags.log").read_text().splitlines()
        library_flags, package_flags, host_flags = [line.split("\t") for line in flags]
        assert library_flags == host_flags
        assert library_flags[0] == str(target)
        assert "-Zsanitizer=realtime" in library_flags[1].split("\x1f")
        assert f"-Clinker={self.llvm}/bin/clang" in library_flags[1].split("\x1f")
        assert package_flags == [str(target / "aarch64-apple-darwin"), "", ""]
        cases = [command for command in self.commands() if command[0] == "uv"]
        names = [command[command.index("--case") + 1] for command in cases]
        assert names == [
            f"own-clap-{name}"
            for name in (
                "clean-control",
                "allocation-control",
                "mutex-control",
                "clap-cold-automation",
                "clap-reset",
                "clap-state-interaction",
            )
        ]
        for command in cases:
            assert f"--argument={target}/aarch64-apple-darwin/bundled/ImageControl RS.clap" in command
            assert "--diagnostic" not in command
        assert (self.home / "runner-targets.log").read_text().splitlines() == [str(original)] * 6

    def test_rust_plugin_clap_no_build_keeps_explicit_package_and_aliases(self) -> None:
        """Reused CLAP packages stay explicit, while both format selectors build only their instrumented host."""
        target, original = self.prepare_plugin_rtsan_tools()
        bundle = original / "bundled/ImageControl RS.clap"
        for format_flag in ("-f", "--format"):
            result = self.invoke(
                "image-control-plugin/rust", format_flag, "clap", "-N", "-b", str(bundle), FAIL_CASE="none"
            )
            assert result.returncode == 0, result.stdout + result.stderr
            commands = self.commands()
            cargo = [command for command in commands if command[0] == "cargo"]
            assert len(cargo) == 1
            assert cargo[0][cargo[0].index("--example") + 1] == "clap_host"
            assert "--lib" not in cargo[0]
            for command in commands:
                if command[0] == "uv":
                    assert f"--argument={bundle}" in command
                    assert command[command.index("--artifacts") + 1] == str(target / "artifacts")

    def test_rust_plugin_build_failure_never_audits_stale_package(self) -> None:
        """Fail before packaging or native cases when instrumented plugin compilation fails."""
        self.prepare_plugin_rtsan_tools()
        result = self.invoke("image-control-plugin/rust", "-f", "clap", FAIL_PLUGIN_BUILD="1")
        assert result.returncode == 37, result.stdout + result.stderr
        commands = self.commands()
        assert len([command for command in commands if command[0] == "cargo"]) == 1
        assert not any(command[0] == "uv" for command in commands)

    def test_rust_plugin_au_builds_native_and_static_rust_instrumentation(self) -> None:
        """Use matching LLVM for every native language and refuse qualification-only hosts as realtime evidence."""
        target, _ = self.prepare_plugin_rtsan_tools(au_markers=False)
        result = self.invoke("image-control-plugin/rust", "-f", "au", FAIL_CASE="none")
        assert result.returncode != 0
        assert "No AU clean result is available" in result.stderr
        commands = self.commands()
        configure = next(command for command in commands if command[:2] == ["cmake", "-S"])
        for option in (
            f"-DIMAGE_CONTROL_RUST_LIBRARY={target}/aarch64-apple-darwin/release/libimage_control_plugin_rs.a",
            "-DIMAGE_CONTROL_AU_RTSAN=ON",
            "-DIMAGE_CONTROL_AU_BUILD_TESTS=ON",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0",
        ):
            assert option in configure
        for language, compiler in (("C", "clang"), ("CXX", "clang++"), ("OBJC", "clang"), ("OBJCXX", "clang++")):
            assert f"-DCMAKE_{language}_COMPILER={self.llvm}/bin/{compiler}" in configure
            assert f"-DCMAKE_{language}_FLAGS=-fsanitize=realtime -g -fno-omit-frame-pointer" in configure
        assert not any(command[0] == "uv" for command in commands)
        assert not any("auval" in argument or "killall" in argument for command in commands for argument in command)

    def test_rust_plugin_all_formats_and_clean_failures_are_independent(self) -> None:
        """Run every format's own controls and callbacks, never suppressing an unexpected native finding."""
        self.prepare_plugin_rtsan_tools()
        result = self.invoke("image-control-plugin/rust", "-f", "all", FAIL_CASE="own-au-au-reset")
        assert result.returncode == 38, result.stdout + result.stderr
        cases = [command for command in self.commands() if command[0] == "uv"]
        names = [command[command.index("--case") + 1] for command in cases]
        assert len(names) == 18
        assert names[0] == "own-vst3-clean-control"
        assert names[7] == "own-clap-clean-control"
        assert names[13] == "own-au-clean-control"
        assert names[-1] == "own-au-au-reset"
        assert all("--control" in command for command in cases if "--expect" in command)
        assert all("--diagnostic" not in command for command in cases)

    def test_rust_plugin_au_diagnostics_match_only_verified_dependency_stacks(self) -> None:
        """Keep explicit BLOCKED dependency audits separate from the default required-clean AU callbacks."""
        self.prepare_plugin_rtsan_tools()
        result = self.invoke("image-control-plugin/rust", "-f", "au", "-G", FAIL_CASE="none")
        assert result.returncode == 0, result.stdout + result.stderr
        cases = [command for command in self.commands() if command[0] == "uv"]
        assert len(cases) == 5
        cold, reset = cases[-2:]
        assert cold[cold.index("--expect") + 1] == "malloc"
        assert cold[cold.index("--stack") + 1] == "Clap::Plugin::AlwaysAudioThread"
        assert reset[reset.index("--expect") + 1] == "pthread_mutex_lock"
        assert "ausdk::AUMethodReset" in reset
        assert "au_reset_region" in reset
        assert all("--control" not in command for command in (cold, reset))
        assert all("--require-output" not in command for command in (cold, reset))

    def test_cpp_presets_targets_and_labels(self) -> None:
        """Keep realtime callback probes separate from the default and thread sanitizer unit suites."""
        for arguments, suffix, target, label in [
            (("--realtime",), "rtsan", "AudioBatchRealtimeTests", "realtime"),
            (("--thread",), "tsan", "AudioBatchTests", "unit"),
            ((), "asan", "AudioBatchTests", "unit"),
        ]:
            with self.subTest(arguments=arguments):
                result = self.invoke("cpp", *arguments)
                assert result.returncode == 0, result.stdout + result.stderr
                configure, build, ctest = self.commands()
                assert configure == ["cmake", "--preset", f"macos-{suffix}", "-DBUILD_TESTING=ON"]
                assert build == ["cmake", "--build", "--preset", f"macos-{suffix}", "--target", target]
                assert ctest == [
                    "ctest",
                    "--test-dir",
                    f"cmake-build-mac-{suffix}",
                    "--build-config",
                    "Debug",
                    "-L",
                    label,
                    "--output-on-failure",
                    "--no-tests=error",
                ]

    def test_plugin_realtime_uses_only_callback_target_and_label(self) -> None:
        """Select the isolated realtime workspace and fake Homebrew compilers without scheduling unit tests."""
        result = self.invoke("image-control-plugin", "--realtime")
        assert result.returncode == 0, result.stdout + result.stderr
        brew, configure, build, ctest = self.commands()
        directory = str(self.root / "image-control-plugin" / "cmake-build-mac-rtsan")
        assert brew == ["brew", "--prefix", "llvm"]
        assert configure[:5] == ["cmake", "-S", str(self.root / "image-control-plugin"), "-B", directory]
        for option in [
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DIMAGE_CONTROL_BUILD_TESTS=ON",
            "-DIMAGE_CONTROL_SANITIZE=realtime",
            "-DIMAGE_CONTROL_COVERAGE=OFF",
            f"-DCMAKE_C_COMPILER={self.llvm}/bin/clang",
            f"-DCMAKE_CXX_COMPILER={self.llvm}/bin/clang++",
        ]:
            assert option in configure
        assert build == [
            "cmake",
            "--build",
            directory,
            "--config",
            "Debug",
            "--target",
            "ImageControlRealtimeTests",
            "--parallel",
            "4",
        ]
        assert ctest == [
            "ctest",
            "--test-dir",
            directory,
            "--build-config",
            "Debug",
            "--output-on-failure",
            "--no-tests=error",
            "-L",
            "realtime",
        ]

    def test_sanitizer_short_aliases_preserve_command_routing(self) -> None:
        """Short selection aliases run the same isolated target and flags as their canonical long forms."""
        for project in PROJECTS[:2]:
            for short, long in (("-R", "--realtime"), ("-t", "--thread")):
                with self.subTest(project=project, short=short):
                    expected = self.invoke(project, long)
                    expected_commands = self.commands()
                    actual = self.invoke(project, short)
                    assert actual.returncode == expected.returncode, actual.stdout + actual.stderr
                    assert self.commands() == expected_commands

    def test_realtime_conflicts_fail_in_both_argument_orders(self) -> None:
        """Reject thread and realtime combinations before configuration, including the short thread flag."""
        for project in PROJECTS[:2]:
            for flag in ("--thread", "-t", *(("--clang",) if project == "cpp" else ())):
                for arguments in (("--realtime", flag), (flag, "--realtime")):
                    with self.subTest(project=project, arguments=arguments):
                        result = self.invoke(project, *arguments)
                        assert result.returncode != 0
                        assert "--realtime" in result.stderr
                        assert self.commands() == []

    def test_unsupported_platforms_and_cpp_clang_fail_before_configure(self) -> None:
        """Reject unsupported realtime hosts and macOS clang mode without invoking build tools."""
        for project in PROJECTS[:2]:
            for platform in ("MINGW64_NT", "unsupported-host"):
                with self.subTest(project=project, platform=platform):
                    result = self.invoke(project, "--realtime", TEST_PLATFORM=platform)
                    assert result.returncode != 0
                    assert "macOS" in result.stderr
                    assert self.commands() == []
        result = self.invoke("cpp", "--clang")
        assert result.returncode != 0
        assert "only supported on Windows" in result.stderr
        assert self.commands() == []

    def test_rust_workflow_requires_strict_first_party_checks(self) -> None:
        """Require clean first-party callbacks separately from expected native dependency findings."""
        workflow = (ROOT / ".github/workflows/rust.yml").read_text(encoding="utf-8")
        strict = "./rtsan.sh --skip-native --artifacts target/rtsan/artifacts/first-party"
        diagnostic = "./rtsan.sh --diagnostic"
        assert strict in workflow
        assert diagnostic in workflow
        assert workflow.index(strict) < workflow.index(diagnostic)

    def test_owned_error_disposal_audits_are_distinct_from_clean_audio(self) -> None:
        """Preserve explicit free/owner matching for bounded incoming error ownership limitations."""
        script = (ROOT / "rust/rtsan.sh").read_text(encoding="utf-8")
        assert "rtsan_full_callback_mutex_baseline" not in script
        assert "rtsan_full_callback_clean_all_formats" in script
        for scenario in (
            "rtsan_owned_ignored_is_blocked",
            "rtsan_owned_full_is_blocked",
            "rtsan_owned_disconnected_is_blocked",
        ):
            assert scenario in script
        assert "--expect free --stack" in script
        assert "StreamErrors.*report" in script

    def test_miri_explicitly_checks_playback_callback_admission(self) -> None:
        """The unsafe renderer access primitive must be selected, not merely assumed covered by host tests."""
        script = (ROOT / "rust/miri.sh").read_text(encoding="utf-8")
        assert "-p audiobatch-playback --lib" in script
        assert "callback_access::" in script

    def test_cpp_first_party_callback_no_longer_expects_native_unlock(self) -> None:
        """Keep idle/full first-party probes clean without weakening dependency owner matching."""
        configuration = (ROOT / "cpp/CMakeLists.txt").read_text(encoding="utf-8")
        assert "realtime_try_unlock_stack" not in configuration
        assert "GenericScopedTryLock" not in configuration
        assert "player-idle" in configuration
        assert "ResamplingAudioSource::getNextAudioBlock" in configuration
        assert "VST3PluginInstance" in configuration

    def test_help_and_invalid_flags_do_not_invoke_tools(self) -> None:
        """Keep help and malformed options independent of native toolchain availability."""
        for project in PROJECTS:
            with self.subTest(project=project):
                result = self.invoke(project, "--help")
                assert result.returncode == 0
                assert "Usage:" in result.stdout
                assert self.commands() == []
                for arguments in [("--unknown",)] + ([("--artifacts",)] if project == "rust" else []):
                    result = self.invoke(project, *arguments)
                    assert result.returncode != 0
                    assert self.commands() == []

    def test_rust_toolchain_mismatch_and_missing_runtime_fail_early(self) -> None:
        """Stop before Cargo, artifact creation, or native bundle lookup when LLVM prerequisites disagree."""
        for version, error, expected in [
            ("21.1.0", "Rust LLVM 22.1.0 and clang must match", ["rustc", "brew", "clang"]),
            ("22.1.0", "Matching native RTSan startup runtime is missing", ["rustc", "brew", "clang", "clang"]),
        ]:
            with self.subTest(version=version):
                artifacts = self.root / "diagnostic artifacts"
                result = self.invoke(
                    "rust",
                    "--skip-native",
                    "--diagnostic",
                    "--artifacts",
                    str(artifacts),
                    TEST_CLANG_VERSION=version,
                )
                assert result.returncode != 0
                assert error in result.stderr
                assert [command[0] for command in self.commands()] == expected
                assert self.commands()[0][1:] == ["+nightly", "--version", "--verbose"]
                assert not artifacts.exists()
                assert not (self.root / "rust" / "target").exists()


if __name__ == "__main__":
    unittest.main(verbosity=2)
