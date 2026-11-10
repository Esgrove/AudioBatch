# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Verify first-party shell entry points expose help without invoking tools or changing files."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASH = shutil.which("bash")
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
            executable = shutil.which(name)
            assert executable is not None
            (self.tools / name).symlink_to(executable)
        (self.tools / "uname").write_text(f'#!{BASH}\nprintf "Darwin\\n"\n', encoding="utf-8")
        (self.tools / "uname").chmod(0o755)
        for name in SIDE_EFFECT_TOOLS:
            tool = self.tools / name
            tool.write_text(
                f'#!{BASH}\nprintf "%s\\n" "${{0##*/}} $*" >> "$HELP_TOOL_LOG"\nexit 93\n', encoding="utf-8"
            )
            tool.chmod(0o755)
        self.environment = {
            "PATH": str(self.tools),
            "HOME": str(self.root / "home"),
            "LC_ALL": "C",
            "HELP_TOOL_LOG": str(self.log),
        }

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
