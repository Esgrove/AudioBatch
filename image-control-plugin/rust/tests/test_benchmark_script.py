# /// script
# requires-python = ">=3.14"
# dependencies = ["pytest>=8.0.0"]
# ///
"""Exercise the hardware-free benchmark script and independently validate its evidence helper without builds."""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import pytest

ROOT = Path(__file__).resolve().parents[3]
RUST = ROOT / "image-control-plugin/rust"
BASH = shutil.which("bash")
if os.name == "nt" and BASH is not None:
    native_bash = Path(BASH).parent.parent / "usr/bin/bash.exe"
    if native_bash.is_file():
        BASH = str(native_bash)
SHEBANG = "/bin/bash" if os.name == "nt" else BASH
SPEC = importlib.util.spec_from_file_location("benchmark_artifacts", RUST / "benchmarks/artifacts.py")
assert SPEC is not None
assert SPEC.loader is not None
ARTIFACTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARTIFACTS)


def evidence() -> dict:
    """Supply independently specified geometry rather than output captured from the production plugin."""
    return {
        "schema": 1,
        "plugin": "ImageControl RS",
        "case": "recover",
        "frames": 4096,
        "channels": 2,
        "finite_samples": 8192,
        "mode": "realtime-warm-callbacks",
        "sample_rate": 48_000,
        "blocks": [1, 17, 257, 511],
        "callbacks": 23,
        "energy": 15.0,
        "peak": 0.2,
        "latency_samples": 4352,
        "checksum": "0123456789abcdef",
        "callback_seconds": 0.001,
    }


class BenchmarkEvidenceContracts(unittest.TestCase):
    """Signal observability is not a self-recorded checksum correctness oracle."""

    def setUp(self) -> None:
        """Keep source paths, bundles, and JSON evidence entirely disposable."""
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-benchmark-evidence-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("CARGO_TARGET_DIR", None)
        self.first = self.root / "first.json"
        self.second = self.root / "second.json"

    def write(self, first: dict, second: dict | None = None) -> None:
        """Publish two test-owned reports without touching benchmark artifacts."""
        self.first.write_text(json.dumps(first), encoding="utf-8")
        self.second.write_text(json.dumps(first if second is None else second), encoding="utf-8")

    def test_independent_geometry_finite_energy_and_determinism(self) -> None:
        """Different timings are valid, but truncated, silent, non-finite, and nondeterministic work is not."""
        value = evidence()
        self.write(value, value | {"callback_seconds": 0.002})
        ARTIFACTS.compare(self.first, self.second, 4096, "recover")
        for field, malformed in (
            ("finite_samples", 8191),
            ("frames", 4095),
            ("channels", 1),
            ("energy", 0.0),
            ("energy", float("inf")),
            ("peak", float("nan")),
            ("callback_seconds", -0.1),
            ("latency_samples", 4351),
            ("checksum", "arbitrary"),
            ("blocks", []),
        ):
            with self.subTest(field=field, malformed=malformed):
                self.write(value | {field: malformed})
                with pytest.raises(ValueError, match=r"bundle|build-tree|first.json|invalid|reported"):
                    ARTIFACTS.compare(self.first, self.second, 4096, "recover")
        self.write(value, value | {"checksum": "fedcba9876543210"})
        with pytest.raises(ValueError, match="nondeterministic"):
            ARTIFACTS.compare(self.first, self.second, 4096, "recover")

    def test_preparation_timings_are_finite_paired_and_excluded_from_determinism(self) -> None:
        """Preserve legacy evidence while validating both independently timed preparation boundaries."""
        value = evidence() | {"module_load_seconds": 0.02, "preparation_seconds": 0.03}
        self.write(value, value | {"module_load_seconds": 0.04, "preparation_seconds": 0.05})
        ARTIFACTS.compare(self.first, self.second, 4096, "recover")
        for field in ARTIFACTS.PREPARATION_TIMINGS:
            for malformed in (None, 0.0, -0.1, float("nan"), float("inf")):
                with self.subTest(field=field, malformed=malformed):
                    self.write(value | {field: malformed})
                    with pytest.raises(ValueError, match="finite and positive"):
                        ARTIFACTS.report(self.first, 4096, "recover")
            incomplete = value.copy()
            del incomplete[field]
            self.write(incomplete)
            with pytest.raises(ValueError, match="finite and positive"):
                ARTIFACTS.report(self.first, 4096, "recover")

    def test_explicit_build_tree_paths_and_symlink_escapes(self) -> None:
        """Never permit installed bundles, relative requests, or symlinks leaving the repository."""
        repository = self.root / "repository"
        repository.mkdir()
        bundle = repository / "rust/target/bundled/ImageControl RS.vst3"
        ARTIFACTS.checked_bundle(repository, bundle, "building")
        with pytest.raises(ValueError, match=r"bundle|build-tree|first.json|invalid|reported"):
            ARTIFACTS.checked_bundle(repository, bundle, "existing")
        bundle.mkdir(parents=True)
        ARTIFACTS.checked_bundle(repository, bundle, "existing")
        outside = self.root / "installed.vst3"
        outside.mkdir()
        link = repository / "escape.vst3"
        link.symlink_to(outside, target_is_directory=True)
        for invalid in (outside, link, Path("relative.vst3"), repository / "missing.vst3"):
            with (
                self.subTest(invalid=invalid),
                pytest.raises(ValueError, match=r"bundle|build-tree|first.json|invalid|reported"),
            ):
                ARTIFACTS.checked_bundle(repository, invalid, "existing")

    def test_external_targets_require_explicit_canonical_authorization(self) -> None:
        """An explicit build root authorizes only its real descendants, including before the normal bundle is built."""
        repository = self.root / "repository"
        repository.mkdir()
        target = self.root / "external build target"
        target.mkdir()
        bundle = target / "bundled/ImageControl RS.vst3"
        with pytest.raises(ValueError, match="explicit CARGO_TARGET_DIR"):
            ARTIFACTS.checked_bundle(repository, bundle, "building")
        with patch.dict(os.environ, {"CARGO_TARGET_DIR": str(target)}):
            ARTIFACTS.checked_bundle(repository, bundle, "building")
            assert not bundle.exists()
            with pytest.raises(ValueError, match="missing"):
                ARTIFACTS.checked_bundle(repository, bundle, "existing")
            bundle.mkdir(parents=True)
            ARTIFACTS.checked_bundle(repository, bundle, "existing")
            outside = self.root / "installed.vst3"
            outside.mkdir()
            for allowed in (repository, target):
                link = allowed / "escape.vst3"
                link.symlink_to(outside, target_is_directory=True)
                with pytest.raises(ValueError, match="explicit CARGO_TARGET_DIR"):
                    ARTIFACTS.checked_bundle(repository, link, "existing")
            with pytest.raises(ValueError, match="explicit CARGO_TARGET_DIR"):
                ARTIFACTS.checked_bundle(repository, outside, "existing")
        root_alias = self.root / "target alias"
        root_alias.symlink_to(target, target_is_directory=True)
        with patch.dict(os.environ, {"CARGO_TARGET_DIR": str(root_alias)}):
            ARTIFACTS.checked_bundle(repository, bundle, "existing")

    def test_repository_artifacts_ignore_missing_unrelated_target(self) -> None:
        """An absent unrelated target neither invalidates repository artifacts nor creates directories."""
        repository = self.root / "repository"
        bundle = repository / "existing.vst3"
        bundle.mkdir(parents=True)
        missing = self.root / "missing target"
        with patch.dict(os.environ, {"CARGO_TARGET_DIR": str(missing)}):
            ARTIFACTS.checked_bundle(repository, bundle, "existing")
        assert not missing.exists()

    def test_independent_rate_geometry_and_mono_count(self) -> None:
        """All four FIR rates and mono sample counts are checked without consulting the plugin implementation."""
        for rate, latency in ((44_100, 4352), (48_000, 4352), (96_000, 8448), (192_000, 16_640)):
            value = evidence() | {"sample_rate": rate, "latency_samples": latency}
            self.write(value)
            ARTIFACTS.report(self.first, 4096, "recover")
        value = evidence() | {"case": "mono-input", "channels": 1, "finite_samples": 4096}
        self.write(value)
        ARTIFACTS.report(self.first, 4096, "mono-input")

    def test_metadata_fingerprints_actual_bytes_without_claiming_existing_profile(self) -> None:
        """Existing artifacts retain exact identity and explicit unknown-build provenance instead of guessed flags."""
        bundle = self.root / "rust/target/bundled/ImageControl RS.vst3"
        library = bundle / "Contents/MacOS/ImageControl RS"
        library.parent.mkdir(parents=True)
        library.write_bytes(b"explicit native library")
        host = self.root / "processing_benchmark"
        host.write_bytes(b"explicit benchmark executable")
        directory = self.root / "artifacts"
        directory.mkdir()
        options = argparse.Namespace(
            root=self.root,
            directory=directory,
            host=host,
            bundle=bundle,
            runs=2,
            warmup=0,
            built="false",
            custom_bundle="false",
        )
        with patch.object(ARTIFACTS, "command", return_value={"status": 0, "stdout": "test", "stderr": ""}):
            ARTIFACTS.metadata(options)
        value = json.loads((directory / "metadata.json").read_text())
        assert value["host"] == ARTIFACTS.identity(host.resolve())
        assert value["bundle"] == [ARTIFACTS.identity(library.resolve())]
        assert value["built_this_run"] is False
        assert value["requested_profile"] == "release"
        assert "cannot establish" in value["profile_note"]
        assert "checks" in value["wall_time_scope"]
        assert "timer overhead" in value["callback_time_scope"]
        options.built = "true"
        options.custom_bundle = "true"
        with patch.object(ARTIFACTS, "command", return_value={"status": 0, "stdout": "test", "stderr": ""}):
            ARTIFACTS.metadata(options)
        value = json.loads((directory / "metadata.json").read_text())
        assert value["host_built_this_run"] is True
        assert value["bundle_built_this_run"] is False
        assert value["built_this_run"] is False
        assert value["custom_bundle_selected"] is True


@unittest.skipUnless(BASH, "Bash is needed to exercise the supported-platform shell entry point")
class BenchmarkScriptContracts(unittest.TestCase):
    """Replace all build/native measurement tools, preserving production argument parsing and shell quoting."""

    def setUp(self) -> None:
        """Create a fake build tree with spaces and command logging, never using installed plugin locations."""
        assert BASH is not None
        temporary = tempfile.TemporaryDirectory(prefix="imagecontrol-benchmark-script-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "repository with spaces"
        for relative in (
            "common.sh",
            "image-control-plugin/common.sh",
            "image-control-plugin/rust/common.sh",
            "image-control-plugin/rust/benchmark.sh",
        ):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        self.script = self.root / "image-control-plugin/rust/benchmark.sh"
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.log = self.root / "commands.log"
        for name in ("dirname", "mkdir", "date", "mktemp"):
            if os.name == "nt":
                executable = Path(BASH).with_name(f"{name}.exe")
                self.tool(name, f'exec "{executable.as_posix()}" "$@"\n', record=False)
            else:
                executable = shutil.which(name)
                assert executable is not None
                (self.tools / name).symlink_to(executable)
        self.tool("uname", 'printf "%s\\n" "${TEST_PLATFORM:-Darwin}"\n', record=False)
        self.tool("uv", 'exit "${CHECK_STATUS:-0}"\n')
        self.tool("git", "exit 0\n")
        self.tool("rustc", "exit 0\n")
        self.tool("cargo", 'exit "${BUILD_STATUS:-0}"\n')
        self.tool("bash.exe", f'exec "{Path(BASH).as_posix()}" "$@"\n', record=False)
        cygpath = Path(BASH).with_name("cygpath.exe")
        self.tool(
            "cygpath",
            f'exec "{cygpath.as_posix()}" "$@"\n' if os.name == "nt" else 'printf "%s\\n" "$2"\n',
        )
        self.tool(
            "hyperfine",
            """
if [ "${HYPERFINE_STATUS:-0}" != 0 ]; then exit "$HYPERFINE_STATUS"; fi
for argument in "$@"; do last="$argument"; done
"$REAL_BASH" -c "$last"
""",
        )
        build = self.script.parent / "build.sh"
        build.write_text(
            f'#!{SHEBANG}\nprintf "build %s\\n" "$*" >> "$BENCHMARK_LOG"\nexit "${{BUILD_STATUS:-0}}"\n',
            encoding="utf-8",
            newline="\n",
        )
        build.chmod(0o755)
        self.target = self.root / "rust/target"
        self.artifact_tree(self.target)
        fixture = self.root / "evidence.json"
        fixture.write_text(json.dumps(evidence()), encoding="utf-8")
        self.environment = {
            "PATH": str(self.tools),
            "HOME": str(self.root / "isolated home"),
            "LC_ALL": "C",
            "BENCHMARK_LOG": str(self.log),
            "EVIDENCE": str(fixture),
            "REAL_BASH": Path(BASH).as_posix(),
            "TEST_PLATFORM": "MINGW64_NT" if os.name == "nt" else "Darwin",
        }

    def artifact_tree(self, target: Path) -> None:
        """Populate an explicitly selected fake Cargo target without any actual build or native plugin load."""
        (target / "bundled/ImageControl RS.vst3").mkdir(parents=True)
        host = target / "release/examples/processing_benchmark"
        host.parent.mkdir(parents=True)
        host.write_text(
            f'#!{SHEBANG}\nprintf "host %s\\n" "$*" >> "$BENCHMARK_LOG"\n'
            'printf "host-target %s\\n" "$CARGO_TARGET_DIR" >> "$BENCHMARK_LOG"\n'
            'while IFS= read -r line || [ -n "$line" ]; do printf "%s\\n" "$line"; done < "$EVIDENCE"\n',
            encoding="utf-8",
            newline="\n",
        )
        host.chmod(0o755)
        shutil.copy2(host, host.with_suffix(".exe"))

    def tool(self, name: str, body: str, *, record: bool = True) -> None:
        """Write an isolated shell stand-in with optional invocation evidence."""
        prefix = 'printf "%s %s\\n" "${0##*/}" "$*" >> "$BENCHMARK_LOG"\n' if record else ""
        path = self.tools / name
        path.write_text(f"#!{SHEBANG}\n{prefix}{body}", encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def native_path(self, value: str) -> Path:
        """Resolve observed MSYS paths through the same native conversion as the shell under test."""
        if os.name == "nt":
            assert BASH is not None
            result = subprocess.run(  # noqa: S603
                [str(Path(BASH).with_name("cygpath.exe")), "-m", value],
                capture_output=True,
                text=True,
                timeout=5,
                check=True,
            )
            value = result.stdout.strip()
        return Path(value)

    def invoke(
        self, *arguments: str, working_directory: Path | None = None, **overrides: str
    ) -> subprocess.CompletedProcess[str]:
        """Bound shell execution and capture failures while all plugin/build tools remain fake."""
        return subprocess.run(  # noqa: S603
            [BASH, self.script.as_posix(), *arguments],
            cwd=self.root if working_directory is None else working_directory,
            env=self.environment | overrides,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def test_help_precedes_tools_builds_and_artifacts(self) -> None:
        """Help aliases exit when reached without querying build tools or creating benchmark artifacts."""
        for flag in ("-h", "--help"):
            for prefix in ((), ("--no-build",)):
                result = self.invoke(*prefix, flag, "--frames")
                assert result.returncode == 0, result.stderr
                for option in ("-N | --no-build", "-A | --artifacts", "-C | --case", "-r | --runs"):
                    assert option in result.stdout
                assert not self.log.exists()
                assert not (self.script.parent / "target").exists()

    def test_earlier_errors_and_value_operands_do_not_trigger_help(self) -> None:
        """Sequential options reject earlier errors and consume help in value positions as ordinary operands."""
        for flag in ("-h", "--help"):
            for arguments in (("--unknown", flag), ("--frames", flag), ("--frames", "", flag), ("--", flag)):
                with self.subTest(arguments=arguments):
                    result = self.invoke(*arguments)
                    assert result.returncode != 0, result.stdout + result.stderr
                    assert "Usage:" not in result.stdout
                    assert not self.log.exists()
                    assert not (self.script.parent / "target").exists()

    def test_invalid_options_fail_before_builds_and_artifacts(self) -> None:
        """Bounds and value-taking options cannot start a build or create a result directory on malformed input."""
        for arguments in (
            ("--frames",),
            ("--frames", "0"),
            ("--frames", "04096"),
            ("--frames", "4000001"),
            ("--runs", "0"),
            ("--runs", "101"),
            ("--warmup", "21"),
            ("--block", "4097"),
            ("--sample-rate", "88200"),
            ("--case", "unknown"),
            ("--unknown",),
        ):
            with self.subTest(arguments=arguments):
                result = self.invoke(*arguments)
                assert result.returncode != 0
                assert not self.log.exists()
                assert not (self.script.parent / "target").exists()

    def test_no_build_long_and_short_options_quote_exact_paths(self) -> None:
        """Both complete alias sets preserve explicit paths and run preflight checks outside hyperfine."""
        for flags in (
            ("--no-build", "--case", "--sample-rate", "--block", "--frames", "--runs", "--warmup", "--artifacts"),
            ("-N", "-C", "-S", "-k", "-f", "-r", "-w", "-A"),
        ):
            no_build, case, rate, block, frames, runs, warmup, artifacts = flags
            result = self.invoke(
                no_build,
                case,
                "recover",
                rate,
                "48000",
                block,
                "irregular",
                frames,
                "4096",
                runs,
                "2",
                warmup,
                "0",
                artifacts,
                str(self.root / "custom artifacts"),
            )
            assert result.returncode == 0, result.stdout + result.stderr
            log = self.log.read_text()
            assert "build " not in log
            assert "cargo " not in log
            assert log.count("host --bundle") >= 3
            assert "--frames 4096" in log
            assert "--warmup 0 --runs 2" in log
            assert "metadata " in log
            assert " check " in log

    def test_builds_release_and_propagates_failures(self) -> None:
        """Default build is a build-tree package, never installation, and every failure remains fatal."""
        result = self.invoke("--frames", "4096", "--runs", "1", "--warmup", "0")
        assert result.returncode == 0, result.stdout + result.stderr
        log = self.log.read_text()
        assert "build " in log
        assert "--release -p image-control-plugin-rs --example processing_benchmark" in log
        assert "install" not in log
        for overrides, status in (
            ({"BUILD_STATUS": "7"}, 7),
            ({"CHECK_STATUS": "8"}, 8),
            ({"HYPERFINE_STATUS": "9"}, 9),
        ):
            result = self.invoke("--frames", "4096", **overrides)
            assert result.returncode == status, result.stdout + result.stderr

    def test_target_resolution_is_invocation_relative_before_directory_change(self) -> None:
        """Relative Cargo targets follow the caller, not the repository root or the script's working directory."""
        caller = self.root / "caller with spaces"
        caller.mkdir()
        relative = "relative target"
        target = (caller / relative).resolve()
        self.artifact_tree(target)
        result = self.invoke(
            "-N", "-f", "4096", "-r", "1", "-w", "0", working_directory=caller, CARGO_TARGET_DIR=relative
        )
        assert result.returncode == 0, result.stdout + result.stderr
        log = self.log.read_text()
        assert f"host-target {target.as_posix()}" in log
        assert f"host --bundle {target.as_posix()}/bundled/ImageControl RS.vst3" in log
        assert (self.root / relative).as_posix() not in log

    def test_external_absolute_target_is_preserved(self) -> None:
        """An explicit external build tree supplies both native host and selected bundle, with no default fallback."""
        target = self.root.parent / "external target"
        self.artifact_tree(target)
        result = self.invoke("-N", "-f", "4096", "-r", "1", "-w", "0", CARGO_TARGET_DIR=str(target))
        assert result.returncode == 0, result.stdout + result.stderr
        log = self.log.read_text()
        assert f"host-target {target.as_posix()}" in log
        assert f"host --bundle {target.as_posix()}/bundled/ImageControl RS.vst3" in log
        assert f"host --bundle {self.target.as_posix()}" not in log

    def test_windows_relative_target_converts_after_invocation_resolution(self) -> None:
        """Windows receives a native target after caller-relative resolution and selects the native host executable."""
        caller = self.root / "windows caller"
        caller.mkdir()
        target = (caller / "target with spaces").resolve()
        self.artifact_tree(target)
        result = self.invoke(
            "-N",
            "-f",
            "4096",
            "-r",
            "1",
            "-w",
            "0",
            working_directory=caller,
            TEST_PLATFORM="MINGW64_NT",
            CARGO_TARGET_DIR="target with spaces",
        )
        assert result.returncode == 0, result.stdout + result.stderr
        log = self.log.read_text()
        converted = [
            self.native_path(line.removeprefix("cygpath -m "))
            for line in log.splitlines()
            if line.startswith("cygpath -m ")
        ]
        assert any(path.is_absolute() and path.resolve() == target for path in converted), log
        assert f"host-target {target.as_posix()}" in log
        assert "processing_benchmark.exe" in log

    def test_windows_selects_native_git_bash(self) -> None:
        """A native hyperfine process uses explicit Git Bash rather than an unrelated shell resolver."""
        result = self.invoke("-N", "-f", "4096", "-r", "1", "-w", "0", TEST_PLATFORM="MINGW64_NT")
        assert result.returncode == 0, result.stdout + result.stderr
        log = self.log.read_text()
        assert "bash.exe" in log
        assert "processing_benchmark.exe" in log


if __name__ == "__main__":
    unittest.main()
