#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Checks that Rust build and install outputs carry the shared ebur128 notice.

Runs the real shell entry points with fake build tools in temporary directories.
No compilation, application installation, signing, or system configuration changes occur.
Run with uv run --script scripts/test_distribution_licenses.py from rust/.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

RUST_DIRECTORY = Path(__file__).resolve().parents[1]
NOTICE_PATH = Path("crates/core/licenses/ebur128.txt")


class TestDistributionLicenses(unittest.TestCase):
    """Exercises loose binaries, CLI-only installs, and platform-specific GUI packaging."""

    def setUp(self) -> None:
        """Creates a private workspace with production scripts and fake external tools."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch license test ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.rust = self.root / "rust"
        self.rust.mkdir()
        shutil.copy2(RUST_DIRECTORY.parent / "common.sh", self.root / "common.sh")
        for name in ("common.sh", "build.sh", "install.sh"):
            shutil.copy2(RUST_DIRECTORY / name, self.rust / name)
        notice = self.rust / NOTICE_PATH
        notice.parent.mkdir(parents=True)
        shutil.copy2(RUST_DIRECTORY / NOTICE_PATH, notice)
        self.write_executable(self.rust / "completions.sh", "exit 0\n")
        (self.rust / "Cargo.toml").write_text('[workspace.package]\nversion = "1.2.3"\n', encoding="utf-8")
        for name, binary in (("cli", "audiobatch-rs"), ("gui", "AudioBatchRs")):
            manifest = self.rust / "crates" / name / "Cargo.toml"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(f'[[bin]]\nname = "{binary}"\n', encoding="utf-8")
        assets = self.root / "assets"
        assets.mkdir()
        (assets / "icon_mac.icns").write_bytes(b"test icon")
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.bin = self.root / "cargo install" / "bin"
        self.bin.mkdir(parents=True)
        executable = self.root / "fake executable"
        self.write_executable(executable, 'printf "test executable\\n"\n')
        self.write_executable(self.tools / "uname", 'printf "%s\\n" "$TEST_UNAME"\n')
        self.write_executable(
            self.tools / "cargo",
            'if [ "$1" = install ]; then\n'
            '    cp "$FAKE_EXECUTABLE" "$TEST_BIN/audiobatch-rs$TEST_SUFFIX"\n'
            "else\n"
            "    mkdir -p target/release\n"
            '    cp "$FAKE_EXECUTABLE" "target/release/audiobatch-rs$TEST_SUFFIX"\n'
            '    cp "$FAKE_EXECUTABLE" "target/release/AudioBatchRs$TEST_SUFFIX"\n'
            "fi\n",
        )
        for tool in ("plutil", "codesign", "powershell.exe"):
            self.write_executable(self.tools / tool, "exit 0\n")
        self.write_executable(self.tools / "xcrun", "exit 1\n")
        self.write_executable(self.tools / "cygpath", 'printf "%s\\n" "$2"\n')
        self.environment = {
            **os.environ,
            "PATH": f"{self.bin}{os.pathsep}{self.tools}{os.pathsep}{os.environ['PATH']}",
            "TEST_UNAME": "Linux",
            "TEST_SUFFIX": "",
            "TEST_BIN": str(self.bin),
            "FAKE_EXECUTABLE": str(executable),
            "LOCALAPPDATA": str(self.root / "local app data"),
            "APPDATA": str(self.root / "roaming app data"),
        }

    @staticmethod
    def write_executable(path: Path, body: str) -> None:
        """Writes a Bash tool shim that stops on command errors."""
        path.write_text("#!/bin/bash\nset -eu\n" + body, encoding="utf-8")
        path.chmod(0o755)

    def run_script(self, name: str, *arguments: str) -> None:
        """Runs a production entry point without changing files outside the temporary workspace."""
        result = subprocess.run(
            ["bash", str(self.rust / name), *arguments],
            cwd=self.rust,
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_notice(self, directory: Path) -> None:
        """Requires the packaged notice to match the complete canonical text byte for byte."""
        self.assertEqual((directory / "ebur128.txt").read_bytes(), (RUST_DIRECTORY / NOTICE_PATH).read_bytes())

    def test_cli_only_build_carries_notice(self) -> None:
        """Skipping the GUI still creates a notice beside the distributable CLI."""
        self.run_script("build.sh", "--cli-only")
        self.assertTrue((self.rust / "audiobatch-rs").is_file())
        self.assert_notice(self.rust / "audiobatch-rs-licenses")

    def test_cli_only_install_carries_notice(self) -> None:
        """The installed CLI receives its notice even with an installation path containing spaces."""
        self.run_script("install.sh", "--cli-only")
        self.assertTrue((self.bin / "audiobatch-rs").is_file())
        self.assert_notice(self.bin / "audiobatch-rs-licenses")

    def test_loose_gui_build_carries_notice(self) -> None:
        """Both loose binaries share one packaged notice directory."""
        self.run_script("build.sh")
        self.assertTrue((self.rust / "AudioBatchRs").is_file())
        self.assert_notice(self.rust / "audiobatch-rs-licenses")

    def test_macos_bundle_carries_notice_before_signing(self) -> None:
        """App resources include the notice before the bundle receives its signature."""
        self.environment["TEST_UNAME"] = "Darwin"
        self.write_executable(
            self.tools / "codesign",
            'test -f "$4/Contents/Resources/licenses/ebur128.txt"\n',
        )
        self.run_script("build.sh")
        self.assert_notice(self.rust / "AudioBatchRs.app/Contents/Resources/licenses")
        self.assert_notice(self.rust / "audiobatch-rs-licenses")

    def test_windows_gui_install_carries_notice(self) -> None:
        """The Windows GUI and the separately installed CLI each receive the notice."""
        self.environment.update(TEST_UNAME="MINGW64_NT", TEST_SUFFIX=".exe")
        # Git Bash resolves .exe commands, while the host Bash used for simulation may not.
        self.write_executable(self.bin / "audiobatch-rs", 'exec "$TEST_BIN/audiobatch-rs.exe" "$@"\n')
        self.run_script("install.sh")
        app = self.root / "local app data/Programs/AudioBatchRs"
        self.assertTrue((app / "AudioBatchRs.exe").is_file())
        self.assert_notice(app / "licenses")
        self.assert_notice(self.bin / "audiobatch-rs-licenses")


if __name__ == "__main__":
    unittest.main()
