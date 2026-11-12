# /// script
# requires-python = ">=3.14"
# dependencies = ["PyYAML>=6.0"]
# ///
"""Keep ImageControl CI on functional contracts rather than local performance entry points."""

import re
import shlex
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/image-control.yml"
LOCAL_ONLY = {
    "benchmark.sh",
    "render_benchmark.py",
    "compare_formats.py",
    "render_compare.py",
    "hyperfine",
    "$benchmark",
    "${benchmark}",
}
BENCHMARK_BINARY = re.compile(
    r"(?:ImageControl(?:Render)?Benchmark|processing_benchmark|render_benchmark|editor_benchmark)(?:\.exe)?$"
)


def performance_commands(script: str) -> list[str]:
    """Identify performance execution while permitting explicit compile-only benchmark targets."""
    violations = []
    for line in script.replace("\\\n", " ").splitlines():
        tokens = shlex.split(line, comments=True)
        if not tokens:
            continue
        names = [token.replace("\\", "/").rsplit("/", 1)[-1] for token in tokens]
        compile_only = ("cmake" in names and "--build" in tokens) or (
            "cargo" in names
            and "run" not in tokens
            and ("build" in tokens or ("bench" in tokens and "--no-run" in tokens))
        )
        if any(name in LOCAL_ONLY for name in names) or (
            not compile_only
            and (
                any(BENCHMARK_BINARY.fullmatch(name) for name in names)
                or ("cargo" in names and any(token in tokens for token in ("bench", "--benches", "--all-targets")))
            )
        ):
            violations.append(line.strip())
    return violations


class CorrectnessCiContracts(unittest.TestCase):
    """Check actual workflow commands and the compile-only distinction independently."""

    def test_performance_entry_points_are_local_only(self) -> None:
        """GPU and packaged correctness checks must not invoke benchmark tools, including help smokes."""
        workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        for job_name, job in workflow["jobs"].items():
            for step in job["steps"]:
                if "run" in step:
                    with self.subTest(job=job_name, step=step.get("name", step["run"])):
                        assert not performance_commands(step["run"]), step["run"]

    def test_compile_only_detection_does_not_allow_execution(self) -> None:
        """Known execution routes fail even when called help, smoke tests, or workload validation."""
        for script in (
            "cargo bench -p image-control-dsp",
            "cargo test --all-targets",
            "cargo run -p image-control-plugin-rs --example processing_benchmark -- --help",
            "cargo run --example render_benchmark -- --frames 1",
            "cargo run --example render_benchmark -- --no-run",
            '"$root/ImageControlRenderBenchmark.exe" --help',
            '"$benchmark" --help',
            "uv run --script ../benchmarks/render_benchmark.py --runs 2",
            "uv run --script benchmarks/compare_formats.py",
            "./benchmark.sh --no-build",
            "hyperfine --runs 5 command",
        ):
            with self.subTest(script=script):
                assert performance_commands(script)
        for script in (
            "cargo bench -p image-control-dsp --no-run",
            "cargo build --example render_benchmark",
            'cmake --build "$build_dir" --target ImageControlRenderBenchmark',
            "uv run --script benchmarks/test_render_benchmark.py",
            "cargo test --lib spectrogram:: -- --test-threads=1",
            './parity.sh --cpp "$bundle" --executable "$parity"',
        ):
            with self.subTest(script=script):
                assert not performance_commands(script)

    def test_correctness_routes_remain_enabled(self) -> None:
        """Require direct package, parity, native editor, and GPU assertions without performance sampling."""
        workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        jobs = workflow["jobs"]
        contracts = "\n".join(step.get("run", "") for step in jobs["rust-contracts"]["steps"])
        native = "\n".join(step.get("run", "") for step in jobs["windows-native"]["steps"])
        for command in ("./test.sh", "./parity.sh", "--strictness-level 10"):
            assert command in contracts, command
        for command in ("--lib spectrogram::", "--example editor_reentry", "--example clap_editor"):
            assert command in native, command


if __name__ == "__main__":
    unittest.main()
