# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Exercise lint-script reporting with a fake clang-tidy and an isolated repository."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class LintReporting(unittest.TestCase):
    """Keep display paths independent of clang-tidy inputs, diagnostics, and exit codes."""

    def setUp(self) -> None:
        """Create a minimal repository with literal shell-pattern characters in its root."""
        temporary = tempfile.TemporaryDirectory(prefix="audiobatch-lint-[test].")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve() / "repository with spaces"
        self.project = self.root / "cpp"
        self.project.mkdir(parents=True)
        for relative in ["common.sh", "cpp/common.sh", "cpp/lint.sh"]:
            shutil.copyfile(ROOT / relative, self.root / relative)
        self.sources = [
            self.project / "src" / "Example.cpp",
            self.project / "tests" / "ExampleTests.cpp",
            self.root / "test-plugin" / "src" / "Processor.cpp",
        ]
        for source in self.sources:
            source.parent.mkdir(parents=True, exist_ok=True)
            source.touch()
        self.build = self.project / "build"
        self.build.mkdir()
        (self.build / "compile_commands.json").write_text(
            json.dumps([{"file": str(source)} for source in self.sources], indent=2), encoding="utf-8"
        )
        self.tools = self.root / "bin"
        self.tools.mkdir()
        # Select a supported platform without depending on the host running this shell regression.
        uname = self.tools / "uname"
        uname.write_text("#!/bin/bash\nprintf '%s\\n' \"${LINT_TEST_PLATFORM:-Darwin}\"\n", encoding="utf-8")
        uname.chmod(0o755)
        cygpath = self.tools / "cygpath"
        cygpath.write_text(
            """#!/bin/bash
case "$1" in
    -u) printf '%s\\n' "$2" ;;
    -m) printf '%s\\n' "$LINT_TEST_MIXED_ROOT" ;;
    -w) printf '%s\\n' "$LINT_TEST_NATIVE_ROOT" ;;
esac
""",
            encoding="utf-8",
        )
        cygpath.chmod(0o755)
        clang_tidy = self.tools / "clang-tidy"
        clang_tidy.write_text(
            """#!/bin/bash
if [ "$1" = --version ]; then
    printf 'Fake clang-tidy\nReporting regression test\n'
    exit 0
fi
for argument in "$@"; do
    case "$argument" in
        *.cpp)
            if [[ "$argument" != /* ]] || [ ! -f "$argument" ]; then
                printf 'error: source argument must remain absolute\n'
                exit 7
            fi
            printf 'Processing file %s.\n' "$argument"
            if [ "$LINT_TEST_WARNING" = 1 ]; then
                printf '%s:4:2: warning: example finding [test-check]\n' "$argument"
            fi
            ;;
    esac
done
if [ "$LINT_TEST_PLATFORM" = MINGW64_NT ]; then
    printf '%s/cpp/src/Example.cpp:5:3: note: mixed Windows path\n' "$LINT_TEST_MIXED_ROOT"
    printf '%s%s\n' "$LINT_TEST_NATIVE_ROOT" '\\cpp\\src\\Example.cpp:6:4: note: native Windows path'
fi
printf '/external/vendor/Header.h:2:1: note: external location\n'
exit "${LINT_TEST_STATUS:-0}"
""",
            encoding="utf-8",
        )
        clang_tidy.chmod(0o755)

    def invoke(
        self, jobs: int, *, warning: bool = False, status: int = 0, windows: bool = False, prepare: bool = False
    ) -> subprocess.CompletedProcess[str]:
        """Run the real lint script with only the analyzer replaced and a bounded timeout."""
        environment = os.environ.copy()
        environment.update(
            PATH=os.pathsep.join([str(self.tools), environment.get("PATH", "")]),
            LINT_TEST_WARNING="1" if warning else "0",
            LINT_TEST_STATUS=str(status),
            LINT_TEST_PLATFORM="MINGW64_NT" if windows else "Darwin",
            LINT_TEST_MIXED_ROOT="C:/Users/Lint [test]/repository",
            LINT_TEST_NATIVE_ROOT=r"C:\Users\Lint [test]\repository",
        )
        bash = shutil.which("bash")
        assert bash is not None, "Bash is required to test the C++ lint script"
        arguments = [
            bash,
            str(self.project / "lint.sh"),
            "--check",
            "--build-dir",
            str(self.build),
            "--jobs",
            str(jobs),
        ]
        if prepare:
            arguments.append("--prepare")
        return subprocess.run(  # noqa: S603
            arguments,
            cwd=self.project,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def test_prepare_refreshes_only_generated_header_targets(self) -> None:
        """Refresh a restored Ninja workspace even when its compile database already exists."""
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Debug\n", encoding="utf-8"
        )
        log = self.root / "cmake.log"
        cmake = self.tools / "cmake"
        cmake.write_text(
            '#!/bin/bash\nprintf "%s\\t" "$@" >> "' + str(log) + '"\nprintf "\\n" >> "' + str(log) + '"\n',
            encoding="utf-8",
        )
        cmake.chmod(0o755)
        result = self.invoke(2, prepare=True)
        assert result.returncode == 0, result.stdout + result.stderr
        configure, build = [line.rstrip("\t").split("\t") for line in log.read_text(encoding="utf-8").splitlines()]
        assert "-DBUILD_TESTING=ON" in configure
        assert build[build.index("--target") + 1 :] == [
            "AudioBatch_artefacts/JuceLibraryCode/JuceHeader.h",
            "AudioBatchCli_artefacts/JuceLibraryCode/JuceHeader.h",
            "AudioBatchTests_artefacts/JuceLibraryCode/JuceHeader.h",
            "AudioBatchIntegrationTests_artefacts/JuceLibraryCode/JuceHeader.h",
            "juce_binarydata_Assets/JuceLibraryCode/BinaryData.h",
        ]
        assert "No clang-tidy issues found" in result.stdout

    def test_parallel_progress_and_diagnostics_use_repository_relative_paths(self) -> None:
        """Strip the literal root in progress and worker logs, leaving external locations intact."""
        result = self.invoke(2)
        assert result.returncode == 0, result.stdout + result.stderr
        for source in self.sources:
            relative = source.relative_to(self.root).as_posix()
            assert f"Linting {relative}" in result.stdout
            assert f"Processing file {relative}." in result.stdout
            assert f"Processing file {source}" not in result.stdout
        assert "/external/vendor/Header.h:2:1: note: external location" in result.stdout
        assert "No clang-tidy issues found" in result.stdout

    def test_serial_worker_logs_use_repository_relative_paths(self) -> None:
        """Apply the same path reporting when sources share a batched serial invocation."""
        result = self.invoke(1)
        assert result.returncode == 0, result.stdout + result.stderr
        for source in self.sources:
            assert f"Processing file {source.relative_to(self.root).as_posix()}." in result.stdout
            assert f"Processing file {source}" not in result.stdout

    def test_windows_diagnostics_accept_both_native_separator_styles(self) -> None:
        """Strip mixed and native Windows roots without treating spaces or brackets as patterns."""
        result = self.invoke(2, windows=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "cpp/src/Example.cpp:5:3: note: mixed Windows path" in result.stdout
        assert r"cpp\src\Example.cpp:6:4: note: native Windows path" in result.stdout
        assert "C:/Users/Lint" not in result.stdout
        assert r"C:\Users\Lint" not in result.stdout

    def test_check_still_rejects_relative_warning_diagnostics(self) -> None:
        """Count warnings after display rewriting without hiding their file, position, or check."""
        result = self.invoke(2, warning=True)
        assert result.returncode == 1, result.stdout + result.stderr
        assert "cpp/src/Example.cpp:4:2: warning: example finding [test-check]" in result.stdout
        assert "clang-tidy reported 3 issue(s)" in result.stderr

    def test_analyzer_failure_is_not_hidden_by_log_rewriting(self) -> None:
        """Preserve worker failures even when diagnostics contain no warning or error line."""
        result = self.invoke(2, status=7)
        assert result.returncode == 1, result.stdout + result.stderr
        assert "clang-tidy failed with exit code 7" in result.stderr


if __name__ == "__main__":
    unittest.main(verbosity=2)
