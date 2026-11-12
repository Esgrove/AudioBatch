# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Verify first-party shell entry points expose help without invoking tools or changing files."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASH = shutil.which("bash")
if os.name == "nt" and BASH is not None:
    native_bash = Path(BASH).parent.parent / "usr/bin/bash.exe"
    if native_bash.is_file():
        BASH = str(native_bash)
SHEBANG = "/bin/bash" if os.name == "nt" else BASH
SCRIPT_DIRECTORIES = (
    ".",
    "cpp",
    "rust",
    "rust/scripts",
    "image-control-plugin",
    "image-control-plugin/rust",
    "test-plugin",
)
# Concepts share a canonical pair. Different concepts may reuse a character in different entry points.
CANONICAL_SHORT_FLAGS = {
    "--help": "-h",
    "--verbose": "-v",
    "--debug": "-d",
    "--check": "-c",
    "--clean": "-c",
    "--jobs": "-j",
    "--test": "-t",
    "--validate": "-V",
    "--validation-level": "-L",
    "--no-build": "-N",
    "--build-only": "-B",
    "--artifacts": "-A",
    "--realtime": "-R",
    "--build-type": "-b",
    "--build-dir": "-p",
    "--clang": "-k",
    "--prepare": "-P",
    "--ninja": "-n",
    "--xcode": "-x",
    "--reuse": "-r",
    "--open": "-o",
    "--print": "-p",
    "--cli-only": "-l",
    "--app-only": "-a",
    "--thread": "-t",
    "--lcov": "-l",
    "--unit": "-u",
    "--gui-integration": "-g",
    "--cli-integration": "-i",
    "--integration": "-i",
    "--optimized": "-O",
    "--tooling": "-T",
    "--miri": "-m",
    "--licenses": "-l",
    "--diagnostic": "-G",
    "--skip-native": "-S",
    "--destination": "-D",
    "--bundle": "-b",
    "--report-only": "-r",
    "--replay": "-r",
    "--fuzz": "-f",
    "--seconds": "-s",
    "--targets": "-T",
    "--install": "-I",
    "--release": "-R",
    "--iterations": "-i",
    "--runs": "-r",
    "--warmup": "-w",
    "--corpus": "-c",
    "--cpp": "-C",
    "--visible": "-W",
    "--silent": "-s",
    "--changed": "-C",
    "--staged": "-s",
    "--recurse": "-r",
    "--normalize": "-n",
    "--tolerance": "-t",
    "--peak": "-p",
    "--lufs": "-l",
}
SIDE_EFFECT_TOOLS = (
    "cargo",
    "rustup",
    "rustc",
    "cmake",
    "ctest",
    "ninja",
    "uv",
    "uvx",
    "brew",
    "ffmpeg",
    "pluginval",
    "auval",
    "codesign",
    "killall",
    "sudo",
    "powershell.exe",
    "shfmt",
    "clang-format",
    "clang-tidy",
    "slb",
    "jq",
    "git",
    "cp",
    "mv",
    "rm",
    "mkdir",
    "touch",
    "tee",
    "xcrun",
    "hyperfine",
)


def entry_points() -> list[Path]:
    """Exclude sourced helper libraries and argument-free fixture generation, not executable entry points."""
    return sorted(
        path.relative_to(ROOT)
        for directory in SCRIPT_DIRECTORIES
        for path in (ROOT / directory).glob("*.sh")
        if path.name != "common.sh"
    )


class ShellHelpContracts(unittest.TestCase):
    """Exercise copied real scripts under a tool-denying PATH and disposable HOME."""

    def setUp(self) -> None:
        """Build a disposable checkout with no access to installed build or packaging programs."""
        assert BASH is not None
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-shell-help-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "repository with spaces"
        self.root.mkdir()
        for directory in SCRIPT_DIRECTORIES:
            for source in (ROOT / directory).glob("*.sh"):
                destination = self.root / source.relative_to(ROOT)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.log = self.root / "unexpected-tools.log"
        for name in ("dirname", "basename", "realpath", "tr", "sed", "awk", "cut", "readlink"):
            self.allow_tool(name)
        self.tool("uname", 'printf "%s\\n" "${TEST_PLATFORM:-Darwin}"\n')
        for name in SIDE_EFFECT_TOOLS:
            self.tool(name, 'printf "%s\\n" "${0##*/} $*" >> "$HELP_TOOL_LOG"\nexit 93\n')
        if os.name == "nt":
            self.allow_tool("cygpath")
        self.environment = {
            "PATH": str(self.tools),
            "HOME": (self.root / "home").as_posix(),
            "LC_ALL": "C",
            "HELP_TOOL_LOG": self.log.as_posix(),
            "TEST_PLATFORM": "MINGW64_NT" if os.name == "nt" else "Darwin",
        }

    def tool(self, name: str, body: str) -> None:
        """Create executable POSIX fixtures with LF endings and a shell-resolvable interpreter."""
        path = self.tools / name
        path.write_text(f"#!{SHEBANG}\n{body}", encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def allow_tool(self, name: str) -> None:
        """Keep Windows runtime tools at their original paths instead of relocating Scoop shims."""
        assert BASH is not None
        if os.name == "nt":
            executable = Path(BASH).with_name(f"{name}.exe")
            assert executable.is_file(), executable
            self.tool(name, f'exec "{executable.as_posix()}" "$@"\n')
        else:
            executable = shutil.which(name)
            assert executable is not None
            (self.tools / name).symlink_to(executable)

    def test_denied_tools_execute_only_recording_fixtures(self) -> None:
        """A usable fixture shell must reject every side-effect tool rather than finding real programs."""
        assert BASH is not None
        for name in SIDE_EFFECT_TOOLS:
            with self.subTest(tool=name):
                self.log.unlink(missing_ok=True)
                result = subprocess.run(  # noqa: S603
                    [BASH, "-c", '"$1" "argument with spaces"', "fixture", name],
                    cwd=self.root,
                    env=self.environment,
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                assert result.returncode == 93, result.stdout + result.stderr
                assert self.log.read_text() == f"{name} argument with spaces\n"

    def test_all_entry_points_support_both_help_aliases_without_side_effects(self) -> None:
        """Each alias exits successfully, documents itself, and avoids build/install/format tools."""
        assert BASH is not None
        for relative in entry_points():
            for flag in ("-h", "--help"):
                with self.subTest(script=str(relative), flag=flag):
                    result = subprocess.run(  # noqa: S603
                        [BASH, str(self.root / relative), flag],
                        cwd=self.root,
                        env=self.environment,
                        capture_output=True,
                        text=True,
                        timeout=10,
                        check=False,
                    )
                    assert result.returncode == 0, f"{relative} {flag}: {result.stdout}\n{result.stderr}"
                    assert "usage:" in result.stdout.lower(), f"{relative}: missing usage output"
                    assert "--help" in result.stdout, f"{relative}: long help alias undocumented"
                    assert "-h" in result.stdout, f"{relative}: short help alias undocumented"
                    assert not self.log.exists(), self.log.read_text() if self.log.exists() else ""
                    assert not (self.root / "home").exists(), f"{relative}: help created user storage"

    def test_entry_points_load_common_before_their_single_argument_parser(self) -> None:
        """Keep bootstrap at file scope, parsing together with help, and descriptive workflow calls last."""
        for relative in entry_points():
            with self.subTest(script=str(relative)):
                source = (ROOT / relative).read_text(encoding="utf-8")
                assert re.search(r"^main\s*\(", source, re.MULTILINE) is None
                bootstrap = re.search(r"^source .*/common\.sh\"?$", source, re.MULTILINE)
                assert bootstrap is not None, f"{relative}: common must load at top level"
                first_function = re.search(r"^[a-zA-Z_][a-zA-Z_0-9]*\(\)\s*\{", source, re.MULTILINE)
                assert first_function is not None
                assert bootstrap.start() < first_function.start(), f"{relative}: bootstrap follows a function"
                parsers = re.findall(r"^parse_arguments\(\)", source, re.MULTILINE)
                assert len(parsers) == 1, f"{relative}: expected exactly one argument parser"
                assert first_function.group().startswith("parse_arguments()"), f"{relative}: parser must come first"
                assert source.index("USAGE=") < first_function.start(), f"{relative}: usage must precede its parser"
                assert re.search(r'^[a-zA-Z_][a-zA-Z_0-9]* "\$@"$', source.rstrip().splitlines()[-1])

    def test_root_dispatch_preserves_scope_arguments_and_fail_fast_status(self) -> None:
        """Named scopes retain exact argv and a failed child prevents every later project from running."""
        assert BASH is not None
        for project in ("cpp", "rust", "image-control-plugin", "image-control-plugin/rust"):
            for name in ("build.sh", "install.sh", "format.sh", "lint.sh"):
                script = self.root / project / name
                script.write_text(
                    f'#!{SHEBANG}\nprintf "%s\\0" "{project}/{name}" "$@" >> "$HELP_TOOL_LOG"\n'
                    f'printf "\\n" >> "$HELP_TOOL_LOG"\n'
                    f'if [ "${{FAIL_PROJECT:-}}" = "{project}" ]; then exit 47; fi\n',
                    encoding="utf-8",
                    newline="\n",
                )
                script.chmod(0o755)
        for name in ("build.sh", "install.sh", "format.sh", "lint.sh"):
            cases = [
                (["cpp", "--unknown", "a value"], [(f"cpp/{name}", "--unknown", "a value")]),
                (["rust", "--", "a value"], [(f"rust/{name}", "--", "a value")]),
            ]
            if name != "build.sh":
                forwarded = () if name == "install.sh" else ("--check",)
                expected_plugins = [(f"image-control-plugin/{name}", *forwarded)]
                if name == "install.sh":
                    expected_plugins.append(("image-control-plugin/rust/install.sh",))
                    cases.extend(
                        (
                            arguments,
                            [(f"{project}/{name}", *arguments) for project in ("cpp", "rust")] + expected_plugins,
                        )
                        for arguments in ([], ["--cli-only", "--verbose"])
                    )
                cases.append((["image-control", "--check"], expected_plugins))
            for arguments, expected in cases:
                with self.subTest(script=name, arguments=arguments):
                    self.log.unlink(missing_ok=True)
                    result = subprocess.run(  # noqa: S603
                        [BASH, str(self.root / name), *arguments],
                        cwd=self.root,
                        env=self.environment,
                        capture_output=True,
                        text=True,
                        timeout=10,
                        check=False,
                    )
                    assert result.returncode == 0, result.stdout + result.stderr
                    actual = [tuple(line.removesuffix("\0").split("\0")) for line in self.log.read_text().splitlines()]
                    assert actual == expected
            failure_projects = ("cpp", "rust") if name == "build.sh" else ("cpp", "rust", "image-control-plugin")
            if name == "install.sh":
                failure_projects += ("image-control-plugin/rust",)
            for failed_project in failure_projects:
                with self.subTest(script=name, fail=failed_project):
                    self.log.unlink(missing_ok=True)
                    result = subprocess.run(  # noqa: S603
                        [BASH, str(self.root / name)],
                        cwd=self.root,
                        env={**self.environment, "FAIL_PROJECT": failed_project},
                        capture_output=True,
                        text=True,
                        timeout=10,
                        check=False,
                    )
                    assert result.returncode == 47, result.stdout + result.stderr
                    actual = [line.split("\0")[0] for line in self.log.read_text().splitlines()]
                    projects = ["cpp", "rust", "image-control-plugin", "image-control-plugin/rust"]
                    expected_projects = projects[: projects.index(failed_project) + 1]
                    assert actual == [f"{project}/{name}" for project in expected_projects]

    def test_version_workflow_updates_only_coordinated_declarations_and_propagates_lock_failure(self) -> None:
        """Exercise the real version editor in disposable files with a recording Cargo command."""
        assert BASH is not None
        self.allow_tool("perl")
        declarations = {
            "cpp/CMakeLists.txt": 'project(\n  "AudioBatch"\n  VERSION 1.2.3\n)\n',
            "Cargo.toml": '[workspace.package]\nversion = "1.2.3"\n',
            "test-plugin/CMakeLists.txt": 'project(\n  "AudioBatchTestPlugin"\n  VERSION 1.2.3\n)\n',
        }
        self.tool("cargo", 'printf "%s\\n" "$*" >> "$HELP_TOOL_LOG"\nexit "${CARGO_STATUS:-0}"\n')
        for arguments, expected, cargo_status in [([], "1.2.4", 0), (["--minor"], "1.3.0", 0), (["-m"], "2.0.0", 48)]:
            with self.subTest(arguments=arguments, cargo_status=cargo_status):
                self.log.unlink(missing_ok=True)
                for relative, content in declarations.items():
                    (self.root / relative).write_text(content, encoding="utf-8", newline="\n")
                result = subprocess.run(  # noqa: S603
                    [BASH, str(self.root / "bump-version.sh"), *arguments],
                    cwd=self.root,
                    env={**self.environment, "CARGO_STATUS": str(cargo_status)},
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                assert (result.returncode == 0) == (cargo_status == 0), result.stdout + result.stderr
                assert self.log.read_text() == "update --workspace\n"
                for relative, content in declarations.items():
                    assert (self.root / relative).read_text() == content.replace("1.2.3", expected)
                if cargo_status:
                    assert "Cargo.lock could not be refreshed" in result.stderr

    def test_rust_plugin_rejects_removed_frontend_flags_before_building(self) -> None:
        """Removed prototype choices fail explicitly instead of silently selecting another implementation."""
        assert BASH is not None
        for script, flag in (("build.sh", "--raw"), ("build.sh", "--editor"), ("test.sh", "--raw")):
            result = subprocess.run(  # noqa: S603
                [BASH, str(self.root / "image-control-plugin/rust" / script), flag],
                cwd=self.root,
                env=self.environment,
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            assert result.returncode != 0, result.stdout + result.stderr
            output = (result.stdout + result.stderr).lower()
            assert "usage:" in output or "unknown option" in output, result.stdout + result.stderr
            assert not self.log.exists(), self.log.read_text() if self.log.exists() else ""

    def test_option_help_documents_short_and_long_forms(self) -> None:
        """Every shell option declaration has an explicit usable short spelling, not just help."""
        assert BASH is not None
        declaration = re.compile(
            r"^\s+(?P<flags>(?:-[A-Za-z]|--[a-z][a-z0-9-]*)(?:\s*\|\s*(?:-[A-Za-z]|--[a-z][a-z0-9-]*))*)"
        )
        for relative in entry_points():
            result = subprocess.run(  # noqa: S603
                [BASH, str(self.root / relative), "--help"],
                cwd=self.root,
                env=self.environment,
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            assert result.returncode == 0, result.stdout + result.stderr
            for line in result.stdout.splitlines():
                match = declaration.match(line)
                if match is None:
                    continue
                if re.search(r"\bis a (?:deprecated |compatibility )?alias\b", line):
                    continue
                flags = match.group("flags")
                assert re.search(r"(?:^|\|)\s*-[A-Za-z](?:\s|$|\|)", flags), f"{relative}: missing short form: {line}"
                long_flags = re.findall(r"--[a-z][a-z0-9-]*", flags)
                assert long_flags, f"{relative}: missing long form: {line}"
                for option in long_flags:
                    expected = CANONICAL_SHORT_FLAGS.get(option)
                    if expected is not None:
                        assert expected in re.split(r"\s*\|\s*", flags), (
                            f"{relative}: {option} must use {expected}: {line}"
                        )

    def test_project_dispatch_preserves_side_effect_free_help(self) -> None:
        """Root wrappers delegate project help without running the real project command."""
        assert BASH is not None
        for script, project in (
            ("build.sh", "cpp"),
            ("build.sh", "rust"),
            ("install.sh", "image-control"),
            ("format.sh", "image-control"),
            ("lint.sh", "image-control"),
        ):
            result = subprocess.run(  # noqa: S603
                [BASH, str(self.root / script), project, "--help"],
                cwd=self.root,
                env=self.environment,
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            assert result.returncode == 0, f"{script} {project}: {result.stdout}\n{result.stderr}"
            assert "usage:" in result.stdout.lower()
            assert not self.log.exists(), self.log.read_text() if self.log.exists() else ""


if __name__ == "__main__":
    unittest.main()
