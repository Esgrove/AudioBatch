# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Exercise the cross-plugin runner's fail-visible setup without builds or installed plugins."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "image-control-plugin/rust/parity.sh"


class ParityRunnerTests(unittest.TestCase):
    """Missing prerequisites fail before Cargo, and help has no external side effects."""

    def test_help_precedes_required_paths_and_cargo(self) -> None:
        """Both help aliases return successfully without a build, install, or plugin path."""
        bash = shutil.which("bash")
        assert bash is not None
        for flag in ("-h", "--help"):
            result = subprocess.run(  # noqa: S603
                [bash, str(RUNNER), "--cpp", flag],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            assert result.returncode == 0
            assert "--executable" in result.stdout
            assert "--rust" in result.stdout

    def test_missing_and_invalid_paths_never_succeed(self) -> None:
        """An absent counterpart is a hard error for the explicit parity entry point."""
        bash = shutil.which("bash")
        assert bash is not None
        environment = os.environ.copy()
        for key in ("IMAGE_CONTROL_CPP_BUNDLE", "IMAGE_CONTROL_OWN_BUNDLE", "IMAGE_CONTROL_PARITY_EXECUTABLE"):
            environment.pop(key, None)
        with tempfile.TemporaryDirectory(prefix="image-control-parity-") as directory:
            root = Path(directory)
            for arguments in ([], ["--cpp"], ["--unknown"], ["-C", str(root), "-e", str(root / "missing")]):
                result = subprocess.run(  # noqa: S603
                    [bash, str(RUNNER), *arguments],
                    capture_output=True,
                    text=True,
                    env=environment,
                    timeout=10,
                    check=False,
                )
                assert result.returncode != 0
                assert result.stdout or result.stderr


if __name__ == "__main__":
    unittest.main()
