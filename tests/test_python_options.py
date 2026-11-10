# /// script
# requires-python = ">=3.11"
# dependencies = ["mutagen==1.47.0", "numpy>=2.1", "psutil", "scipy>=1.14", "soundfile>=0.12"]
# ///
"""Verify first-party Python CLI aliases using real parsers without running audio tools or writing fixtures."""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import subprocess
import sys
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[1]
# Values deliberately include spaces, repeated options, negative numbers, and child flags encoded as JSON.
PARSER_CASES = {
    "tests/realtime_sanitizer.py": (
        [],
        [
            ("--executable", "-e", "CLI with spaces"),
            ("--case", "-n", "case-name"),
            ("--argument", "-a", "--child-flag"),
            ("--argument", "-a", "-1"),
            ("--expect", "-E", "malloc"),
            ("--stack", "-s", "Callback"),
            ("--require-output", "-O", "finished"),
            ("--artifacts", "-A", "artifacts with spaces"),
            ("--timeout", "-t", "12.5"),
            ("--control", "-c", None),
            ("--diagnostic", "-G", None),
        ],
    ),
    "tests/callback_benchmark.py": (
        [],
        [
            ("--baseline-command", "-b", '["CLI with spaces", "--flag", "-1"]'),
            ("--candidate-command", "-k", '["other CLI", "--flag"]'),
            ("--pairs", "-P", "2"),
            ("--artifacts", "-A", "artifacts with spaces"),
            ("--cwd", "-d", "working directory"),
            ("--timeout", "-t", "12.5"),
            ("--capture-only", "-q", None),
            ("--report-key", "-K", "benchmark.cases"),
            ("--native-schema", "-N", "rust"),
            ("--field-map", "-F", '{"case": "native.case"}'),
            ("--env", "-e", '{"SHARED": "with spaces"}'),
            ("--baseline-env", "-u", '{"ROLE": "baseline"}'),
            ("--candidate-env", "-U", '{"ROLE": "candidate"}'),
            ("--baseline-metadata", "-M", '{"flags": ["-O3"]}'),
            ("--candidate-metadata", "-m", '{"flags": ["-O2"]}'),
        ],
    ),
    "tests/cli_integration.py": (
        [],
        [("--cli", "-e", "CLI path"), ("--test-plugin", "-p", "plugin path"), ("--artifacts", "-A", "artifacts")],
    ),
    "fixtures/limiter_model.py": ([], [("--output", "-o", "fit report.json")]),
    "fixtures/spectrogram_expected.py": ([], [("--check", "-c", None)]),
    "fixtures/metrics_expected.py": ([], [("--check", "-c", None)]),
    "fixtures/metadata/inspect_metadata.py": (
        [],
        [("--directory", "-d", "fixture directory"), ("--output", "-o", "capture.json"), ("--check", "-c", None)],
    ),
    "fixtures/benchmark/generate.py": ([], [("--force", "-f", None)]),
    "rust/scripts/cli_benchmark.py": (
        ["init", "work directory", "results directory"],
        [
            ("--cpp", "-C", "C++ CLI"),
            ("--rust", "-R", "Rust CLI"),
            ("--jobs", "-j", "2"),
            ("--runs", "-r", "3"),
            ("--warmup", "-w", "1"),
            ("--corpus", "-c", "input corpus"),
        ],
    ),
    "rust/scripts/compare_levels.py": (
        ["cpp.json", "rust.json"],
        [
            ("--tolerance", "-t", "0.01"),
            ("--folders", "-f", ["C++ folder", "Rust folder"]),
            ("--expectations", "-E", "expected.json"),
            ("--write-expectations", "-W", "capture.json"),
            ("--inputs", "-i", ["first input", "second input"]),
            ("--base", "-b", "base directory"),
            ("--recurse", "-r", None),
            ("--lufs", "-l", "-14"),
        ],
    ),
    "rust/scripts/compare_outputs.py": (
        ["C++ folder", "Rust folder"],
        [
            ("--tolerance", "-t", "-72"),
            ("--lossy", "-l", "lossy.txt"),
            ("--list-lossy", "-L", "input directory"),
            ("--mode", "-m", "lufs"),
            ("--snapshot", "-s", "source directory"),
            ("--expectations", "-E", "expected.json"),
            ("--snapshots", "-S", ["cpp-before.json", "rust-before.json"]),
        ],
    ),
    "rust/scripts/test_aac_reconstruction.py": (
        [],
        [
            ("--cpp-cli", "-C", "C++ CLI"),
            ("--rust-cli", "-R", "Rust CLI"),
            ("--input", "-i", "first.m4a"),
            ("--input", "-i", "second.m4a"),
        ],
    ),
    "image-control-plugin/benchmarks/compare_formats.py": (
        [],
        [("--cli", "-e", "CLI with spaces"), ("--runs", "-r", "6"), ("--jobs", "-j", "2")],
    ),
    "image-control-plugin/test-signals/generate.py": (
        [],
        [("--force", "-f", None), ("--group", "-g", "listening"), ("--output", "-o", "signal directory")],
    ),
}


def load_script(relative: str) -> ModuleType:
    """Import a standalone script with its local imports available, without invoking its command entry point."""
    path = ROOT / relative
    name = "python_option_contract_" + relative.replace("/", "_").replace(".", "_")
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        message = f"Cannot import {path}"
        raise ImportError(message)
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    sys.path.insert(0, str(path.parent))
    try:
        specification.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module


def option_tokens(option: str, value: str | list[str] | None) -> list[str]:
    """Attach flag-shaped values explicitly while preserving multi-value argparse actions."""
    if value is None:
        return [option]
    if isinstance(value, list):
        return [option, *value]
    return [f"{option}={value}"]


class PythonOptionContracts(unittest.TestCase):
    """Keep short aliases, typed values, help, and positional boundaries consistent across standalone tools."""

    @classmethod
    def setUpClass(cls) -> None:
        """Load actual parser builders once, without any fixture generation or native processing."""
        cls.modules = {relative: load_script(relative) for relative in PARSER_CASES}

    def test_short_and_long_options_produce_identical_namespaces(self) -> None:
        """Every listed option preserves destination names, types, repeats, and Boolean values."""
        for relative, (positionals, options) in PARSER_CASES.items():
            with self.subTest(script=relative):
                parser = self.modules[relative].build_parser()
                long = [
                    *positionals,
                    *(token for option, _, value in options for token in option_tokens(option, value)),
                ]
                short = [
                    *positionals,
                    *(token for _, option, value in options for token in option_tokens(option, value)),
                ]
                expected = parser.parse_args(long)
                actual = parser.parse_args(short)
                assert actual == expected
                for field in ("output", "directory", "artifacts", "input", "inputs", "cpp_cli", "rust_cli"):
                    value = getattr(actual, field, None)
                    if value is not None:
                        assert all(isinstance(path, Path) for path in (value if isinstance(value, list) else [value]))

    def test_every_optional_action_has_a_single_letter_alias(self) -> None:
        """Even dynamically generated callback role options fit the available alphabet without long-only exceptions."""
        for relative, module in self.modules.items():
            with self.subTest(script=relative):
                parsers = [module.build_parser()]
                while parsers:
                    parser = parsers.pop()
                    for action in parser._actions:
                        if isinstance(action, argparse._SubParsersAction):
                            parsers.extend(action.choices.values())
                        if action.option_strings:
                            assert any(option.startswith("--") for option in action.option_strings)
                            assert any(len(option) == 2 and option.startswith("-") for option in action.option_strings)
                            if "-v" in action.option_strings:
                                assert "--verbose" in action.option_strings, f"{relative}: -v is reserved for verbosity"
                            if "-h" in action.option_strings:
                                assert "--help" in action.option_strings

    def test_shared_option_concepts_use_the_same_short_spelling(self) -> None:
        """Cross-script concepts retain one alias even when each command has its own parser."""
        aliases = {
            "--help": "-h",
            "--output": "-o",
            "--check": "-c",
            "--force": "-f",
            "--jobs": "-j",
            "--runs": "-r",
            "--warmup": "-w",
            "--corpus": "-c",
            "--cpp": "-C",
            "--cpp-cli": "-C",
            "--rust": "-R",
            "--rust-cli": "-R",
            "--cli": "-e",
            "--executable": "-e",
            "--artifacts": "-A",
            "--timeout": "-t",
            "--diagnostic": "-G",
            "--input": "-i",
            "--inputs": "-i",
            "--expectations": "-E",
            "--tolerance": "-t",
            "--recurse": "-r",
            "--peak": "-p",
            "--lufs": "-l",
        }
        for relative, module in self.modules.items():
            parsers = [module.build_parser()]
            while parsers:
                parser = parsers.pop()
                for action in parser._actions:
                    if isinstance(action, argparse._SubParsersAction):
                        parsers.extend(action.choices.values())
                    for long, short in aliases.items():
                        if long in action.option_strings:
                            assert short in action.option_strings, f"{relative}: {long} must use {short}"

    def test_help_exits_before_side_effects_and_advertises_aliases(self) -> None:
        """Both help forms work as real processes without executables, installed plugins, or generated inputs."""
        for relative, (_, options) in PARSER_CASES.items():
            subcommand = ["init"] if relative == "rust/scripts/cli_benchmark.py" else []
            outputs = []
            for help_option in ("-h", "--help"):
                with self.subTest(script=relative, help=help_option):
                    completed = subprocess.run(  # noqa: S603 - owned script, help only, exact argv without a shell
                        [sys.executable, str(ROOT / relative), *subcommand, help_option],
                        capture_output=True,
                        text=True,
                        check=False,
                        timeout=30,
                    )
                    assert completed.returncode == 0, completed.stderr
                    assert "usage:" in completed.stdout
                    for long, short, _ in options:
                        assert long in completed.stdout
                        assert short in completed.stdout
                    outputs.append(completed.stdout)
            assert outputs[0] == outputs[1]

    def test_negative_targets_and_terminated_positional_names_are_unchanged(self) -> None:
        """Adding aliases does not reinterpret negative numerical targets or paths protected by a terminator."""
        levels = self.modules["rust/scripts/compare_levels.py"].build_parser()
        short = levels.parse_args(["-p", "-1", "--", "-cpp.json", "-rust.json"])
        long = levels.parse_args(["--peak", "-1", "--", "-cpp.json", "-rust.json"])
        assert short == long
        assert short.peak == -1.0
        assert short.cpp == Path("-cpp.json")
        assert short.rust == Path("-rust.json")
        outputs = self.modules["rust/scripts/compare_outputs.py"].build_parser()
        assert outputs.parse_args(["-t", "-72", "--", "-cpp", "-rust"]) == outputs.parse_args(
            [
                "--tolerance",
                "-72",
                "--",
                "-cpp",
                "-rust",
            ]
        )

    def test_parser_help_does_not_run_main(self) -> None:
        """Parser-only help exits before required values or runtime validation are needed."""
        for relative, module in self.modules.items():
            with self.subTest(script=relative), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:  # noqa: PT027 - stdlib unittest runner
                    module.build_parser().parse_args(["--help"])
                assert raised.exception.code == 0


if __name__ == "__main__":
    unittest.main(verbosity=2)
