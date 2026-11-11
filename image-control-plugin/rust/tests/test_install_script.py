# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Exercise the Rust installer using temporary bundles and fake build/signing/validation tools."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASH = shutil.which("bash")


class RustInstallContracts(unittest.TestCase):
    """Install only inside a disposable repository and home, never into real plugin locations."""

    def setUp(self) -> None:
        """Copy production helpers and replace build/signing/validation with deterministic tools."""
        assert BASH is not None
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-rust-installer-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "repository with spaces"
        for relative in (
            "common.sh",
            "image-control-plugin/common.sh",
            "image-control-plugin/rust/common.sh",
            "image-control-plugin/rust/install.sh",
        ):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        self.script = self.root / "image-control-plugin/rust/install.sh"
        self.home = self.root / "isolated home"
        self.home.mkdir()
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        for command in ("dirname", "mkdir", "cp", "mv", "rm", "mktemp"):
            executable = shutil.which(command)
            assert executable is not None
            (self.tools / command).symlink_to(executable)
        self.tool("uname", 'printf "%s\\n" "${TEST_PLATFORM:-Darwin}"\n', record=False)
        self.tool("codesign", 'exit "${SIGN_STATUS:-0}"\n')
        self.tool("pluginval", 'exit "${VALIDATOR_STATUS:-0}"\n')
        self.tool("cygpath", 'printf "%s\\n" "$2"\n')
        self.bundle = self.root / "rust/target/bundled/ImageControl RS.vst3"
        executable = self.bundle / "Contents/MacOS/ImageControl RS"
        executable.parent.mkdir(parents=True)
        executable.write_text("new Rust plugin", encoding="utf-8")
        build = self.script.parent / "build.sh"
        build.write_text(
            f'#!{BASH}\nprintf "build %s\\n" "$*" >> "$INSTALL_LOG"\nexit "${{BUILD_STATUS:-0}}"\n',
            encoding="utf-8",
        )
        build.chmod(0o755)
        self.environment = {
            "PATH": str(self.tools),
            "HOME": str(self.home),
            "INSTALL_LOG": str(self.log),
            "LC_ALL": "C",
        }

    def tool(self, name: str, body: str, *, record: bool = True) -> None:
        """Create a shell tool which cannot escape the test-owned install tree."""
        recording = 'printf "%s %s\\n" "${0##*/}" "$*" >> "$INSTALL_LOG"\n' if record else ""
        path = self.tools / name
        path.write_text(f"#!{BASH}\n{recording}{body}", encoding="utf-8")
        path.chmod(0o755)

    def invoke(self, *arguments: str, **overrides: str) -> subprocess.CompletedProcess[str]:
        """Run the unchanged installer with bounded execution and captured diagnostics."""
        assert BASH is not None
        return subprocess.run(  # noqa: S603
            [BASH, str(self.script), *arguments],
            cwd=self.root,
            env=self.environment | overrides,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def test_help_never_builds_signs_or_installs(self) -> None:
        """Both help aliases exit before touching build tools or installation folders."""
        for flag in ("-h", "--help"):
            for prefix in ((), ("--debug",)):
                result = self.invoke(*prefix, flag, "--unknown")
                assert result.returncode == 0, result.stderr
                for option in ("-N | --no-build", "-D | --destination", "-L | --validation-level"):
                    assert option in result.stdout
                assert not self.log.exists()
                assert not (self.home / "Library").exists()

    def test_invalid_arguments_before_help_are_not_overridden(self) -> None:
        """Sequential parsing rejects earlier errors and treats value-position help as an operand."""
        for flag in ("-h", "--help"):
            for arguments in (
                ("--unknown", flag),
                ("--validation-level", flag),
                ("--validation-level", "4", flag),
                ("--destination", "", flag),
                ("--", flag),
            ):
                with self.subTest(arguments=arguments):
                    result = self.invoke(*arguments)
                    assert result.returncode != 0, result.stdout + result.stderr
                    assert "Usage:" not in result.stdout
                    assert not self.log.exists()
                    assert not (self.home / "Library").exists()

    def test_default_mac_install_is_separate_and_replaces_only_rust_bundle(self) -> None:
        """A staged replacement preserves the separate C++ plugin and forwards build/validation options."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3"
        cpp = destination / "ImageControl.vst3"
        cpp.mkdir(parents=True)
        (cpp / "keep").write_text("C++", encoding="utf-8")
        installed = destination / "ImageControl RS.vst3"
        installed.mkdir()
        (installed / "stale").write_text("old", encoding="utf-8")
        result = self.invoke("--debug", "--validate")
        assert result.returncode == 0, result.stdout + result.stderr
        assert (installed / "Contents/MacOS/ImageControl RS").read_text() == "new Rust plugin"
        assert not (installed / "stale").exists()
        assert (cpp / "keep").read_text() == "C++"
        assert "build --debug" in self.log.read_text()
        assert "pluginval --strictness-level 5 --validate" in self.log.read_text()
        assert not list(destination.glob(".imagecontrol-rs-install.*"))

    def test_short_debug_and_validate_aliases(self) -> None:
        """The short build and validation switches forward the same options as their long forms."""
        result = self.invoke("-d", "-V")
        assert result.returncode == 0, result.stdout + result.stderr
        assert "build --debug" in self.log.read_text()
        assert "pluginval --strictness-level 5 --validate" in self.log.read_text()

    def test_validation_level_destination_and_no_build_aliases(self) -> None:
        """Both complete alias sets install existing output and accept every strictness from five to ten."""
        for no_build, destination_flag, level_flag in (
            ("-N", "-D", "-L"),
            ("--no-build", "--destination", "--validation-level"),
        ):
            for level in range(5, 11):
                with self.subTest(no_build=no_build, level=level):
                    self.log.unlink(missing_ok=True)
                    destination = self.root / f"custom VST3 {level}"
                    result = self.invoke(no_build, destination_flag, str(destination), level_flag, str(level))
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert (destination / "ImageControl RS.vst3/Contents").is_dir()
                    assert "build " not in self.log.read_text()
                    assert f"pluginval --strictness-level {level} --validate {destination}" in self.log.read_text()

    def test_invalid_validation_alias_values_fail_before_side_effects(self) -> None:
        """Reject missing, noncanonical, and out-of-range strictness before build or destination creation."""
        for flag in ("-L", "--validation-level"):
            for value in (None, "", "4", "11", "05", "+5", "5.0", "word"):
                with self.subTest(flag=flag, value=value):
                    arguments = [flag] if value is None else [flag, value]
                    result = self.invoke(*arguments)
                    assert result.returncode != 0
                    assert not self.log.exists()
                    assert not (self.home / "Library").exists()
        for flag in ("-D", "--destination"):
            result = self.invoke(flag)
            assert result.returncode != 0
            assert not self.log.exists()

    def test_build_failure_leaves_installed_plugin_untouched(self) -> None:
        """Never replace an installed bundle with stale output after a failed build."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        destination.mkdir(parents=True)
        (destination / "old").write_text("keep", encoding="utf-8")
        result = self.invoke(BUILD_STATUS="7")
        assert result.returncode == 7
        assert (destination / "old").read_text() == "keep"
        assert "codesign" not in self.log.read_text()

    def test_staged_copy_failure_preserves_previous_plugin(self) -> None:
        """A partially copied staging bundle is disposable, and publication never touches the old plugin."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        destination.mkdir(parents=True)
        (destination / "old").write_text("keep", encoding="utf-8")
        (self.tools / "cp").unlink()
        self.tool("cp", 'mkdir -p "${@: -1}"\nexit 31\n')
        result = self.invoke("--no-build")
        assert result.returncode == 31, result.stdout + result.stderr
        assert (destination / "old").read_text() == "keep"
        assert not list(destination.parent.glob(".imagecontrol-rs-install.*"))
        assert "Installed:" not in result.stdout

    def test_staged_signature_failure_preserves_previous_plugin(self) -> None:
        """A source signature can pass while a bad staged copy fails before moving the previous installation."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        destination.mkdir(parents=True)
        (destination / "old").write_text("keep", encoding="utf-8")
        self.tool("codesign", 'case "$*" in *".imagecontrol-rs-install."*) exit 32 ;; esac\n')
        result = self.invoke("--no-build")
        assert result.returncode == 32, result.stdout + result.stderr
        assert (destination / "old").read_text() == "keep"
        assert not list(destination.parent.glob(".imagecontrol-rs-install.*"))
        assert "Installed:" not in result.stdout

    def test_publication_failure_rolls_back_previous_plugin(self) -> None:
        """A failed publish restores the old installation and cleans the rejected replacement."""
        self.check_publication_failure(rollback_fails=False)

    def test_failed_rollback_retains_recovery_copy(self) -> None:
        """If a host or filesystem also blocks rollback, cleanup never destroys the only old bundle."""
        self.check_publication_failure(rollback_fails=True)

    def check_publication_failure(self, *, rollback_fails: bool) -> None:
        """Inject failures only in staged publication and optional recovery, forwarding all other moves."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        destination.mkdir(parents=True)
        (destination / "old").write_text("keep", encoding="utf-8")
        move = shutil.which("mv")
        assert move is not None
        (self.tools / "mv").unlink()
        self.tool(
            "mv",
            'case "$1" in\n'
            '  *".imagecontrol-rs-install."*/"ImageControl RS.vst3") exit 33 ;;\n'
            '  *".imagecontrol-rs-install."*/previous.vst3)\n'
            '    [ "${ROLLBACK_FAILS:-0}" = 0 ] || exit 34 ;;\n'
            f'esac\nexec "{move}" "$@"\n',
        )
        result = self.invoke("--no-build", ROLLBACK_FAILS="1" if rollback_fails else "0")
        assert result.returncode != 0, result.stdout + result.stderr
        assert "Installed:" not in result.stdout
        recovery = list(destination.parent.glob(".imagecontrol-rs-install.*/previous.vst3"))
        if rollback_fails:
            assert not destination.exists()
            assert len(recovery) == 1
            assert (recovery[0] / "old").read_text() == "keep"
            assert str(recovery[0]) in result.stderr
        else:
            assert (destination / "old").read_text() == "keep"
            assert not recovery
            assert not list(destination.parent.glob(".imagecontrol-rs-install.*"))

    def test_unsigned_source_preserves_previous_plugin(self) -> None:
        """Reject source signature errors before creating a staging tree or changing the installed bundle."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        destination.mkdir(parents=True)
        (destination / "old").write_text("keep", encoding="utf-8")
        result = self.invoke("--no-build", SIGN_STATUS="35")
        assert result.returncode == 35
        assert (destination / "old").read_text() == "keep"
        assert not list(destination.parent.glob(".imagecontrol-rs-install.*"))
        assert "Installed:" not in result.stdout

    def test_relative_external_target_is_resolved_from_invocation_directory(self) -> None:
        """The installer accepts an explicit outside build tree without interpreting it from the script directory."""
        target = self.root.parent / "external artifacts"
        shutil.copytree(self.bundle, target / "bundled/ImageControl RS.vst3")
        result = self.invoke("--no-build", CARGO_TARGET_DIR="../external artifacts")
        assert result.returncode == 0, result.stdout + result.stderr
        installed = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        assert (installed / "Contents/MacOS/ImageControl RS").read_text() == "new Rust plugin"
        assert "build " not in self.log.read_text()

    def test_existing_build_custom_directory_and_validation_failure(self) -> None:
        """The no-build path uses supplied artifacts and propagates installed validator failures."""
        destination = self.root / "custom VST3"
        result = self.invoke(
            "--no-build", "--destination", str(destination), "--validation-level", "7", VALIDATOR_STATUS="9"
        )
        assert result.returncode == 9
        assert (destination / "ImageControl RS.vst3/Contents").is_dir()
        assert "build " not in self.log.read_text()
        assert "--strictness-level 7" in self.log.read_text()

    def test_windows_uses_user_bundle_directory_without_elevation(self) -> None:
        """Preserve the complete VST3 bundle in the standard per-user Windows directory."""
        local = self.root / "Local AppData"
        result = self.invoke("--no-build", TEST_PLATFORM="MINGW64_NT", LOCALAPPDATA=str(local))
        assert result.returncode == 0, result.stdout + result.stderr
        assert (local / "Programs/Common/VST3/ImageControl RS.vst3/Contents").is_dir()
        assert "codesign" not in self.log.read_text()

    def test_invalid_options_missing_bundles_and_symlink_targets_are_rejected(self) -> None:
        """Reject unsafe or malformed requests before replacing any existing plugin."""
        for arguments in (("--destination",), ("--validation-level", "4"), ("--unknown",)):
            result = self.invoke(*arguments)
            assert result.returncode != 0
            assert not self.log.exists()
        missing = self.invoke("--no-build", CARGO_TARGET_DIR=str(self.root / "missing"))
        assert missing.returncode != 0
        destination = self.root / "linked VST3"
        destination.mkdir()
        (destination / "ImageControl RS.vst3").symlink_to(self.bundle, target_is_directory=True)
        result = self.invoke("--no-build", "--destination", str(destination))
        assert result.returncode != 0
        assert (destination / "ImageControl RS.vst3").is_symlink()


class RustEntryPointContracts(unittest.TestCase):
    """Exercise every other production Rust script against recording tools in a disposable workspace."""

    scripts = ("build.sh", "test.sh", "lint.sh", "format.sh", "parity.sh", "rtsan.sh")

    def setUp(self) -> None:
        """Keep all Cargo, formatting, signing, and native host execution inside fake tools."""
        assert BASH is not None
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-rust-scripts-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "repository with spaces"
        for relative in (
            "common.sh",
            "image-control-plugin/common.sh",
            "image-control-plugin/rust/common.sh",
            *(f"image-control-plugin/rust/{script}" for script in self.scripts),
        ):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        self.directory = self.root / "image-control-plugin/rust"
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        for command in ("dirname", "mkdir"):
            executable = shutil.which(command)
            assert executable is not None
            (self.tools / command).symlink_to(executable)
        uname = self.tools / "uname"
        uname.write_text(f'#!{BASH}\nprintf "%s\\n" "${{TEST_PLATFORM:-Darwin}}"\n', encoding="utf-8")
        uname.chmod(0o755)
        for name in ("cargo", "codesign", "pluginval", "shfmt", "slb"):
            executable = self.tools / name
            executable.write_text(
                f'#!{BASH}\nprintf "%s\\0" "${{0##*/}}" "$@" >> "$COMMAND_LOG"\nprintf "\\n" >> "$COMMAND_LOG"\n'
                'if [ "${FAIL_TOOL:-}" = "${0##*/}" ]; then exit 37; fi\n',
                encoding="utf-8",
            )
            executable.chmod(0o755)
        self.bundle = self.root / "rust/target/bundled/ImageControl RS.vst3"
        (self.bundle / "Contents").mkdir(parents=True)
        self.environment = {"PATH": str(self.tools), "COMMAND_LOG": str(self.log), "LC_ALL": "C"}

    def invoke(
        self, script: str, *arguments: str, cwd: Path | None = None, **overrides: str
    ) -> subprocess.CompletedProcess[str]:
        """Run the copied script with a bounded timeout and no inherited executable search path."""
        assert BASH is not None
        return subprocess.run(  # noqa: S603
            [BASH, str(self.directory / script), *arguments],
            cwd=cwd or self.directory,
            env=self.environment | overrides,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def commands(self, name: str | None = None) -> list[list[str]]:
        """Read exact argument boundaries so spaces in manifests and bundle paths remain meaningful."""
        if not self.log.exists():
            return []
        commands = [line.removesuffix("\0").split("\0") for line in self.log.read_text().splitlines()]
        return commands if name is None else [command[1:] for command in commands if command[0] == name]

    def test_help_aliases_precede_tools_but_not_earlier_errors(self) -> None:
        """Help exits when reached, while earlier invalid arguments remain errors without invoking tools."""
        selections = {
            "build.sh": ("-d",),
            "test.sh": ("-l",),
            "lint.sh": ("-c",),
            "format.sh": ("-c",),
            "parity.sh": ("-C", "missing"),
            "rtsan.sh": ("-N",),
        }
        for script in self.scripts:
            for flag in ("-h", "--help"):
                for prefix in ((), selections[script]):
                    with self.subTest(script=script, flag=flag, prefix=prefix):
                        result = self.invoke(script, *prefix, flag, "--unknown")
                        assert result.returncode == 0, result.stdout + result.stderr
                        assert "Usage:" in result.stdout
                        assert not self.commands()
                for prefix in (("--unknown",), ("--",)):
                    with self.subTest(script=script, flag=flag, prefix=prefix):
                        result = self.invoke(script, *prefix, flag)
                        assert result.returncode != 0, result.stdout + result.stderr
                        assert "Usage:" not in result.stdout
                        assert not self.commands()
        result = self.invoke("test.sh", "-m", "-l", "--help")
        assert result.returncode != 0
        assert "Choose at most one verification mode" in result.stderr
        assert not self.commands()

    def test_unknown_and_trailing_options_fail_before_tools(self) -> None:
        """No entry point silently ignores extra arguments or performs work before rejecting them."""
        selections = {
            "build.sh": "-d",
            "test.sh": "-l",
            "lint.sh": "-c",
            "format.sh": "-c",
            "parity.sh": "-C",
            "rtsan.sh": "-N",
        }
        for script, selection in selections.items():
            for arguments in (("--unknown",), (selection, "--unknown")):
                with self.subTest(script=script, arguments=arguments):
                    result = self.invoke(script, *arguments)
                    assert result.returncode != 0, result.stdout + result.stderr
                    assert not self.commands()
        for arguments in (("-m", "-l"), ("--licenses", "--licenses"), ("--miri", "extra")):
            result = self.invoke("test.sh", *arguments)
            assert result.returncode != 0
            assert not self.commands()

    def test_definitions_load_before_workflow_execution(self) -> None:
        """Bootstrap libraries first, but defer every workflow until all functions have been parsed."""
        for name in (*self.scripts, "install.sh", "benchmark.sh"):
            with self.subTest(script=name):
                source = (ROOT / "image-control-plugin/rust" / name).read_text()
                assert source.count("parse_arguments() {") == 1
                assert "main()" not in source
                assert "show_help_if_requested" not in source
                assert source.index('source "$') < source.index("parse_arguments() {")
                lines = source.rstrip().splitlines()
                workflow, arguments = lines[-1].split(" ", 1)
                assert arguments == '"$@"'
                assert f"{workflow}() {{" in source
                definitions = self.directory / "definitions.sh"
                definitions.write_text("\n".join(lines[:-1]) + "\n")
                result = self.invoke(definitions.name)
                assert result.returncode == 0, result.stdout + result.stderr
                assert not self.commands()
                assert not (self.directory / "target").exists()

    def test_failures_stop_nested_workflows_with_original_status(self) -> None:
        """A descriptive workflow call must not disable errexit or continue with stale output."""
        for script, arguments, tool, count in (
            ("build.sh", ("-t", "-V"), "cargo", 1),
            ("build.sh", ("-t", "-V"), "codesign", 2),
            ("test.sh", (), "cargo", 1),
            ("test.sh", ("-m",), "cargo", 1),
            ("lint.sh", ("-c",), "cargo", 1),
            ("format.sh", ("-c",), "cargo", 1),
            ("format.sh", ("-c",), "shfmt", 2),
        ):
            with self.subTest(script=script, tool=tool):
                self.log.unlink(missing_ok=True)
                result = self.invoke(script, *arguments, FAIL_TOOL=tool)
                assert result.returncode == 37, result.stdout + result.stderr
                assert len(self.commands()) == count

    def test_parity_aliases_preserve_explicit_paths_and_failure_status(self) -> None:
        """Only existing explicit build-tree artifacts authorize the cross-implementation test process."""
        cpp = self.root / "C++ build.vst3"
        (cpp / "Contents").mkdir(parents=True)
        executable = self.root / "ImageControlParity"
        executable.touch()
        environment_log = self.root / "parity-environment.log"
        cargo = self.tools / "cargo"
        cargo.write_text(
            cargo.read_text() + 'printf "%s\\n" "$IMAGE_CONTROL_CPP_BUNDLE" "$IMAGE_CONTROL_OWN_BUNDLE" '
            '"$IMAGE_CONTROL_PARITY_EXECUTABLE" "$CARGO_TARGET_DIR" > "$PARITY_ENV_LOG"\n'
        )
        expected = None
        for cpp_flag, rust_flag, executable_flag in (
            ("-C", "-r", "-e"),
            ("--cpp", "--rust", "--executable"),
        ):
            self.log.unlink(missing_ok=True)
            result = self.invoke(
                "parity.sh",
                cpp_flag,
                str(cpp),
                rust_flag,
                str(self.bundle),
                executable_flag,
                str(executable),
                PARITY_ENV_LOG=str(environment_log),
            )
            assert result.returncode == 0, result.stdout + result.stderr
            assert environment_log.read_text().splitlines() == [
                str(cpp),
                str(self.bundle),
                str(executable),
                str(self.root / "rust/target"),
            ]
            commands = self.commands("cargo")
            assert commands == [
                [
                    "test",
                    "--manifest-path",
                    str(self.root / "Cargo.toml"),
                    "-p",
                    "image-control-plugin-rs",
                    "--test",
                    "cross_implementation",
                    "--",
                    "--nocapture",
                ]
            ]
            if expected is not None:
                assert commands == expected
            expected = commands
        self.log.unlink()
        result = self.invoke(
            "parity.sh",
            IMAGE_CONTROL_CPP_BUNDLE=str(cpp),
            IMAGE_CONTROL_OWN_BUNDLE=str(self.bundle),
            IMAGE_CONTROL_PARITY_EXECUTABLE=str(executable),
            FAIL_TOOL="cargo",
        )
        assert result.returncode == 37, result.stdout + result.stderr
        assert len(self.commands()) == 1

    def test_parity_rejects_missing_paths_before_cargo(self) -> None:
        """Missing operands, required selections, and artifacts cannot launch a native parity process."""
        for arguments in (("-C",), ("-r",), ("-e",), (), ("-C", "missing", "-e", "missing")):
            with self.subTest(arguments=arguments):
                result = self.invoke("parity.sh", *arguments)
                assert result.returncode != 0, result.stdout + result.stderr
                assert not self.commands()

    def test_rtsan_cases_keep_bundle_target_and_fail_fast(self) -> None:
        """Native audits use isolated instrumented output while each bounded case receives the original bundle root."""
        for name in ("cut", "rg"):
            executable = shutil.which(name)
            assert executable is not None
            (self.tools / name).symlink_to(executable)
        runtime = self.root / "rtsan-runtime.dylib"
        runtime.touch()
        clang = self.tools / "clang"
        clang.write_text(
            f'#!{BASH}\ncase "$1" in\n'
            '  --version) printf "clang version 22.1.0\\n" ;;\n'
            f'  --print-file-name=*) printf "%s\\n" "{runtime}" ;;\nesac\n'
        )
        clang.chmod(0o755)
        rustc = self.tools / "rustc"
        rustc.write_text(f'#!{BASH}\nprintf "host: aarch64-apple-darwin\\nLLVM version: 22.1.0\\n"\n')
        rustc.chmod(0o755)
        jq = self.tools / "jq"
        jq.write_text(f'#!{BASH}\nprintf "%s\\n" "{self.tools / "cargo"}"\n')
        jq.chmod(0o755)
        uv = self.tools / "uv"
        uv.write_text(
            f'#!{BASH}\nprintf "%s\\0" uv "$@" >> "$COMMAND_LOG"\nprintf "\\n" >> "$COMMAND_LOG"\n'
            'printf "%s\\n" "$CARGO_TARGET_DIR" >> "$TARGET_LOG"\n'
            'case " $* " in *" $FAIL_CASE "*) exit 38 ;; esac\n'
        )
        uv.chmod(0o755)
        target_log = self.root / "realtime-targets.log"
        cases = [
            "clean-control",
            "allocation-control",
            "mutex-control",
            "native-cold-automation",
            "native-reset",
            "native-state-interaction",
            "native-editor-lifecycle",
        ]
        for failed_case, count, status in (("none", 7, 0), ("own-vst3-native-reset", 5, 38)):
            self.log.unlink(missing_ok=True)
            target_log.unlink(missing_ok=True)
            result = self.invoke(
                "rtsan.sh",
                "-N",
                AUDIOBATCH_RTSAN_CLANG=str(clang),
                TARGET_LOG=str(target_log),
                FAIL_CASE=failed_case,
            )
            assert result.returncode == status, result.stdout + result.stderr
            observed = self.commands("uv")
            assert len(observed) == count
            assert [command[command.index("--case") + 1] for command in observed] == [
                f"own-vst3-{case}" for case in cases[:count]
            ]
            assert target_log.read_text().splitlines() == [str(self.root / "rust/target")] * count
            for command in observed:
                assert f"--argument={self.bundle}" in command
                assert command[command.index("--timeout") + 1] == "45"
                assert command[command.index("--artifacts") + 1] == str(self.directory / "target/rtsan-own/artifacts")
        self.log.unlink()
        result = self.invoke("rtsan.sh", "-N", "-b", "")
        assert result.returncode != 0
        assert "Build-tree VST3 is missing: " in result.stderr
        assert not self.commands()

    def test_build_aliases_preserve_primary_workspace_packaging(self) -> None:
        """Both alias sets package the primary root-workspace plugin, test it, and validate its bundle."""
        for arguments in (("-d", "-t", "-V"), ("--debug", "--test", "--validate")):
            with self.subTest(arguments=arguments):
                self.log.unlink(missing_ok=True)
                result = self.invoke("build.sh", *arguments)
                assert result.returncode == 0, result.stdout + result.stderr
                cargo = self.commands("cargo")
                assert cargo[0] == [
                    "run",
                    "--manifest-path",
                    str(self.root / "Cargo.toml"),
                    "-p",
                    "image-control-xtask",
                    "--",
                    "bundle",
                    "image-control-plugin-rs",
                ]
                assert cargo[1] == [
                    "test",
                    "--manifest-path",
                    str(self.root / "Cargo.toml"),
                    "-p",
                    "image-control-dsp",
                    "-p",
                    "image-control-plugin-rs",
                    "-p",
                    "image-control-xtask",
                    "--all-features",
                ]
                assert self.commands("pluginval") == [["--strictness-level", "5", "--validate", str(self.bundle)]]
                assert self.commands("codesign") == [
                    ["--force", "--sign", "-", str(self.bundle)],
                    ["--verify", "--strict", str(self.bundle)],
                ]
        self.log.unlink()
        result = self.invoke("build.sh")
        assert result.returncode == 0, result.stdout + result.stderr
        assert self.commands("cargo")[0][-1] == "--release"
        assert not self.commands("pluginval")

    def test_relative_and_absolute_targets_reach_all_nested_cargo_commands(self) -> None:
        """Build and test preserve invocation-relative targets even after their own working-directory changes."""
        target = self.root.parent / "external artifacts"
        bundle = target / "bundled/ImageControl RS.vst3"
        (bundle / "Contents").mkdir(parents=True)
        target_log = self.root / "cargo-targets.log"
        cargo = self.tools / "cargo"
        cargo.write_text(
            f'#!{BASH}\nprintf "%s\\n" "$CARGO_TARGET_DIR" >> "$TARGET_LOG"\n',
            encoding="utf-8",
        )
        for selection in ("external artifacts", str(target)):
            for script in ("build.sh", "test.sh"):
                with self.subTest(script=script, selection=selection):
                    target_log.unlink(missing_ok=True)
                    result = self.invoke(
                        script,
                        cwd=self.root.parent,
                        CARGO_TARGET_DIR=selection,
                        TARGET_LOG=str(target_log),
                    )
                    assert result.returncode == 0, result.stdout + result.stderr
                    observed = target_log.read_text().splitlines()
                    assert len(observed) == (1 if script == "build.sh" else 2)
                    assert all(
                        Path(path).is_absolute() and Path(path).resolve() == target.resolve() for path in observed
                    )
                    if script == "build.sh":
                        assert Path(result.stdout.strip()).resolve() == bundle.resolve()

    def test_miri_and_licenses_aliases(self) -> None:
        """Both modes retain their existing package scope and cannot invoke native hosts."""
        for short, long, count in (("-m", "--miri", 3), ("-l", "--licenses", 1)):
            self.log.unlink(missing_ok=True)
            result = self.invoke("test.sh", short)
            assert result.returncode == 0, result.stdout + result.stderr
            expected = self.commands()
            assert len(self.commands("cargo")) == count
            if short == "-l":
                assert self.commands("cargo") == [
                    ["deny", "--manifest-path", str(self.directory / "crates/plugin/Cargo.toml"), "check", "licenses"]
                ]
            else:
                assert all(command[:3] == ["+nightly", "miri", "test"] for command in self.commands("cargo"))
            self.log.unlink()
            result = self.invoke("test.sh", long)
            assert result.returncode == 0, result.stdout + result.stderr
            assert self.commands() == expected

    def test_check_aliases_preserve_nonmutating_tools(self) -> None:
        """Short and long check flags reach Cargo and every optional formatter without fix arguments."""
        for script in ("lint.sh", "format.sh"):
            expected = None
            for flag in ("-c", "--check"):
                with self.subTest(script=script, flag=flag):
                    self.log.unlink(missing_ok=True)
                    result = self.invoke(script, flag)
                    assert result.returncode == 0, result.stdout + result.stderr
                    commands = self.commands()
                    if expected is not None:
                        assert commands == expected
                    expected = commands
                    assert "--fix" not in self.commands("cargo")[0]
                    if script == "format.sh":
                        assert self.commands("cargo")[0][-1] == "--check"
                        assert self.commands("shfmt")[0][0] == "--diff"
                        assert self.commands("slb")[0][0] == "crates"
                    else:
                        assert self.commands("cargo")[0][-3:] == ["--", "-D", "warnings"]

    def test_rtsan_bundle_and_no_build_aliases_skip_packaging(self) -> None:
        """All no-build aliases reject the supplied missing bundle without builds or native dependencies."""
        missing = self.root / "missing bundle.vst3"
        for bundle in ("-b", "--bundle"):
            for no_build in ("-N", "--no-build", "--skip-build"):
                with self.subTest(bundle=bundle, no_build=no_build):
                    result = self.invoke("rtsan.sh", bundle, str(missing), no_build)
                    assert result.returncode != 0
                    assert f"Build-tree VST3 is missing: {missing}" in result.stderr
                    assert not self.commands()
            result = self.invoke("rtsan.sh", bundle)
            assert result.returncode != 0
            assert "requires a path" in result.stderr
            assert not self.commands()
        help_result = self.invoke("rtsan.sh", "-h")
        assert "-N | --no-build" in help_result.stdout
        assert "--skip-build is a deprecated alias" in help_result.stdout


if __name__ == "__main__":
    unittest.main(verbosity=2)
