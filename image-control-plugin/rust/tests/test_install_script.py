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
        for relative in ("common.sh", "image-control-plugin/common.sh", "image-control-plugin/rust/install.sh"):
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
        self.tool("cygpath", 'printf "%s\\n" "$LOCALAPPDATA"\n')
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
            result = self.invoke(flag)
            assert result.returncode == 0, result.stderr
            assert "Usage:" in result.stdout
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

    def test_build_failure_leaves_installed_plugin_untouched(self) -> None:
        """Never replace an installed bundle with stale output after a failed build."""
        destination = self.home / "Library/Audio/Plug-Ins/VST3/ImageControl RS.vst3"
        destination.mkdir(parents=True)
        (destination / "old").write_text("keep", encoding="utf-8")
        result = self.invoke(BUILD_STATUS="7")
        assert result.returncode == 7
        assert (destination / "old").read_text() == "keep"
        assert "codesign" not in self.log.read_text()

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


if __name__ == "__main__":
    unittest.main()
