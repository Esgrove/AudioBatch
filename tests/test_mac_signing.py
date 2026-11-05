# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""
Check local signing policy and final packaging without touching installed apps or the keychain.

Set AUDIOBATCH_TEST_NATIVE_SIGNING=1 on macOS to also build and inspect disposable real app bundles.
"""

from __future__ import annotations

import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASH = shutil.which("bash")
ENTITLEMENTS = {"com.apple.security.cs.disable-library-validation": True}


class SigningContracts(unittest.TestCase):
    """Record shell argv and inspect copied resources in a fully isolated miniature repository."""

    def setUp(self) -> None:
        """Copy production packaging code, never sign or remove a real workspace bundle."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-signing-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        for relative in [
            "common.sh",
            "rust/common.sh",
            "rust/Cargo.toml",
            "rust/crates/core/licenses/ebur128.txt",
            "cpp/cmake/MacSigning.cmake",
            "assets/macos.entitlements",
            "assets/icon_mac.icns",
        ]:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        self.tools = self.root / "bin"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        self.bundle = self.root / "test app.app"
        self.executable = self.root / "test executable"
        self.executable.write_text("placeholder", encoding="utf-8")
        self.environment = os.environ.copy()
        self.environment.pop("CODESIGN_IDENTITY", None)
        self.environment.update(COMMAND_LOG=str(self.log))
        for name, body in {
            "uname": 'printf "%s\\n" "${TEST_PLATFORM:-Darwin}"\n',
            "plutil": 'exit "${PLUTIL_STATUS:-0}"\n',
            "codesign": """if [ "$1" = --force ]; then
    bundle="${@: -1}"
    test -f "$bundle/Contents/Info.plist"
    test -f "$bundle/Contents/PkgInfo"
    test -f "$bundle/Contents/Resources/icon_mac.icns"
    test -f "$bundle/Contents/Resources/licenses/ebur128.txt"
    exit "${SIGN_STATUS:-0}"
fi
exit "${VERIFY_STATUS:-0}"
""",
            "xcrun": """if [ "$2" = --version ]; then
    printf '<key>short-bundle-version</key>\\n<string>27.0</string>\\n'
else
    printf 'compiled icon' > "$3/Assets.car"
fi
""",
            "security": "exit 99\n",
        }.items():
            tool = self.tools / name
            tool.write_text(
                '#!/bin/bash\nset -eo pipefail\nprintf "%s\\0" "${0##*/}" "$@" >> "$COMMAND_LOG"\n'
                'printf "\\n" >> "$COMMAND_LOG"\n' + body,
                encoding="utf-8",
            )
            tool.chmod(0o755)

    def invoke(self, *, native: bool = False, **overrides: str) -> subprocess.CompletedProcess[str]:
        """Package only the temporary bundle using the production Rust helper and bounded execution."""
        environment = self.environment | overrides
        if not native:
            environment["PATH"] = os.pathsep.join([str(self.tools), environment.get("PATH", "")])
        assert BASH is not None, "Bash is required for signing contracts"
        return subprocess.run(  # noqa: S603
            [
                BASH,
                "-c",
                'source "$1/rust/common.sh"; create_mac_app_bundle "$2" "$3"',
                "signing-test",
                str(self.root),
                str(self.executable),
                str(self.bundle),
            ],
            cwd=self.root,
            env=environment,
            capture_output=True,
            text=True,
            timeout=90,
            check=False,
        )

    def commands(self, name: str) -> list[list[str]]:
        """Keep identities and bundle paths containing spaces as single arguments."""
        return [
            command[1:]
            for line in self.log.read_text().splitlines()
            if (command := line.removesuffix("\0").split("\0"))[0] == name
        ]

    def test_default_empty_and_explicit_identities(self) -> None:
        """Use only the selected identity, with no implicit keychain discovery or automatic fallback."""
        for overrides, identity in [
            ({}, "-"),
            ({"CODESIGN_IDENTITY": ""}, "-"),
            ({"CODESIGN_IDENTITY": "Apple Development: Test Person (TEAM)"}, "Apple Development: Test Person (TEAM)"),
        ]:
            with self.subTest(identity=identity, overrides=overrides):
                result = self.invoke(**overrides)
                assert result.returncode == 0, result.stdout + result.stderr
                signing, verification = self.commands("codesign")[-2:]
                assert signing == [
                    "--force",
                    "--sign",
                    identity,
                    "--options",
                    "runtime",
                    "--timestamp=none",
                    "--entitlements",
                    str(self.root / "assets/macos.entitlements"),
                    str(self.bundle),
                ]
                assert verification == ["--verify", "--deep", "--strict", "--verbose=2", str(self.bundle)]
                assert not self.commands("security")
                assert (self.bundle / "Contents/Resources/Assets.car").is_file()

    def test_signing_failure_is_visible_and_never_falls_back(self) -> None:
        """A failed explicit signature stops packaging before verification or any retry."""
        result = self.invoke(CODESIGN_IDENTITY="missing identity", SIGN_STATUS="7")
        assert result.returncode == 7
        assert len(self.commands("codesign")) == 1
        assert "missing identity" in result.stdout

    def test_verification_failure_is_fatal(self) -> None:
        """Never report packaging success for an invalid signed bundle."""
        result = self.invoke(VERIFY_STATUS="8")
        assert result.returncode == 8
        assert len(self.commands("codesign")) == 2

    def test_invalid_plist_prevents_signing(self) -> None:
        """Malformed plist failures must propagate before invoking codesign."""
        result = self.invoke(PLUTIL_STATUS="9")
        assert result.returncode == 9
        assert not self.commands("codesign")

    def test_entitlements_are_plugin_hosting_only(self) -> None:
        """Hardened runtime is an option, not an entitlement, and playback needs no microphone access."""
        with (ROOT / "assets/macos.entitlements").open("rb") as source:
            assert plistlib.load(source) == ENTITLEMENTS
        result = self.invoke()
        assert result.returncode == 0, result.stdout + result.stderr
        with (self.bundle / "Contents/Info.plist").open("rb") as source:
            info = plistlib.load(source)
        assert "NSMicrophoneUsageDescription" not in info
        assert info["CFBundleExecutable"] == "AudioBatchRs"

    def test_cpp_registers_signing_after_all_bundle_steps(self) -> None:
        """Production CMake uses the tested helper last and never supplies a developer team implicitly."""
        configuration = (ROOT / "cpp/CMakeLists.txt").read_text()
        assert configuration.rstrip().endswith("include(cmake/MacSigning.cmake)\naudiobatch_sign_mac_app(AudioBatch)")
        assert "XCODE_ATTRIBUTE_DEVELOPMENT_TEAM" not in configuration
        assert "XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY" not in configuration
        module = (ROOT / "cpp/cmake/MacSigning.cmake").read_text()
        assert "if(NOT APPLE)\n        return()\n    endif()" in module
        assert "XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO" in module
        assert "XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO" in module
        assert "${target}Sign ALL" in module
        assert "DEPENDS ${target}" in module

    def test_nonmac_signing_is_rejected(self) -> None:
        """The shared helper never invokes macOS tools on Windows."""
        assert BASH is not None
        result = subprocess.run(  # noqa: S603
            [BASH, str(self.root / "common.sh"), "sign_mac_app_bundle", str(self.bundle)],
            env=self.environment
            | {"PATH": os.pathsep.join([str(self.tools), os.environ["PATH"]]), "TEST_PLATFORM": "MINGW64_NT"},
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        assert result.returncode != 0
        assert not self.commands("codesign")
        assert not self.commands("plutil")

    @unittest.skipUnless(
        sys.platform == "darwin" and os.environ.get("AUDIOBATCH_TEST_NATIVE_SIGNING") == "1",
        "opt-in real macOS packaging inspection",
    )
    def test_native_rust_and_cmake_bundles(self) -> None:
        """Sign real temporary Rust packages and Ninja/Xcode apps, after their final resources exist."""
        shutil.copytree(ROOT / "assets/icon_mac.icon", self.root / "assets/icon_mac.icon")
        source = self.root / "main.c"
        source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        self.run_native(["clang", str(source), "-o", str(self.executable)])
        result = self.invoke(native=True)
        assert result.returncode == 0, result.stdout + result.stderr
        self.inspect_signature(self.bundle)
        for generator in ["Ninja", "Xcode"]:
            with self.subTest(generator=generator):
                project = self.root / generator
                project.mkdir()
                (project / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.24)\nproject(SigningContract C)\n"
                    'add_executable(Contract MACOSX_BUNDLE "../main.c" resource.txt)\n'
                    "set_source_files_properties(resource.txt PROPERTIES MACOSX_PACKAGE_LOCATION Resources)\n"
                    "add_custom_command(TARGET Contract POST_BUILD\n"
                    ' COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:Contract>/Contents/Resources"\n'
                    ' COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../assets/icon_mac.icns"\n'
                    ' "$<TARGET_BUNDLE_DIR:Contract>/Contents/Resources/icon_mac.icns")\n'
                    'include("../cpp/cmake/MacSigning.cmake")\naudiobatch_sign_mac_app(Contract)\n',
                    encoding="utf-8",
                )
                resource = project / "resource.txt"
                resource.write_text("original resource", encoding="utf-8")
                build = project / "build"
                self.run_native(["cmake", "-S", str(project), "-B", str(build), "-G", generator])
                self.run_native(["cmake", "--build", str(build), "--config", "Debug"])
                bundle = build / ("Debug/Contract.app" if generator == "Xcode" else "Contract.app")
                self.inspect_signature(bundle)
                resource.write_text("rebuilt resource with different content", encoding="utf-8")
                rebuilt = self.run_native(["cmake", "--build", str(build), "--config", "Debug"])
                assert (bundle / "Contents/Resources/resource.txt").read_text() == resource.read_text()
                if generator == "Ninja":
                    assert "Linking" not in rebuilt.stdout, "Resource rebuild must not need a relink"
                self.inspect_signature(bundle)
                (bundle / "Contents/Resources/icon_mac.icns").write_bytes(b"changed resource")
                verification = self.run_native(["codesign", "--verify", "--deep", "--strict", str(bundle)], check=False)
                assert verification.returncode != 0

    def run_native(self, command: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
        """Run actual platform tools only against disposable files, always using ad-hoc signing."""
        result = subprocess.run(  # noqa: S603
            command,
            cwd=self.root,
            env=self.environment | {"CODESIGN_IDENTITY": "-"},
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if check:
            assert result.returncode == 0, result.stdout + result.stderr
        return result

    def inspect_signature(self, bundle: Path) -> None:
        """Require a strict signature, runtime flag, exact entitlements, and no microphone usage text."""
        self.run_native(["codesign", "--verify", "--deep", "--strict", "--verbose=2", str(bundle)])
        details = self.run_native(["codesign", "--display", "--verbose=4", str(bundle)])
        assert "Signature=adhoc" in details.stderr
        assert "runtime" in details.stderr
        entitlements = self.run_native(["codesign", "--display", "--entitlements", "-", "--xml", str(bundle)])
        assert plistlib.loads(entitlements.stdout.encode()) == ENTITLEMENTS
        with (bundle / "Contents/Info.plist").open("rb") as source:
            assert "NSMicrophoneUsageDescription" not in plistlib.load(source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
