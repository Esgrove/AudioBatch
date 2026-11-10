# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Check repository-root Cargo ownership, dependency boundaries, and isolated legacy patches."""

import json
import subprocess
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIMARY_CRATES = ("dsp", "nice", "xtask")
PLUGIN_ONLY = ("iced_baseview", "iced_program", "iced_widget", "baseview", "fragile")


def manifest(path: Path) -> dict:
    """Read a real Cargo manifest without invoking a build or updating dependency resolution."""
    return tomllib.loads(path.read_text(encoding="utf-8"))


def metadata(directory: Path, manifest_path: Path | None = None) -> dict:
    """Obtain bounded read-only package ownership and target information from Cargo itself."""
    arguments = ["cargo", "metadata", "--no-deps", "--format-version", "1"]
    if manifest_path is not None:
        arguments.extend(["--manifest-path", str(manifest_path)])
    # All argv values are fixed test-owned paths. No shell interprets the arguments.
    result = subprocess.run(  # noqa: S603
        arguments, cwd=directory, capture_output=True, text=True, timeout=60, check=True
    )
    return json.loads(result.stdout)


def has_primary_plugin() -> bool:
    """Allow migration-only checkouts while still requiring registered plugin members to be valid."""
    members = manifest(ROOT / "Cargo.toml")["workspace"]["members"]
    return any(member.startswith("image-control-plugin/") for member in members)


class RootWorkspaceTests(unittest.TestCase):
    """Pin Cargo-discovered ownership, not just strings in project-specific shell scripts."""

    def test_root_and_application_directory_share_one_workspace_and_target(self) -> None:
        """Root and existing application commands must discover the same manifest and output directory."""
        root = metadata(ROOT)
        application = metadata(ROOT / "rust")
        assert Path(root["workspace_root"]) == ROOT
        assert root["workspace_root"] == application["workspace_root"]
        assert root["target_directory"] == application["target_directory"]
        assert Path(root["target_directory"]) == ROOT / "rust/target"
        assert not (ROOT / "rust/Cargo.toml").exists()
        assert not (ROOT / "rust/Cargo.lock").exists()
        assert (ROOT / "Cargo.lock").is_file()

    @unittest.skipUnless(has_primary_plugin(), "primary ImageControl is not registered in this checkout")
    def test_primary_imagecontrol_packages_inherit_root_without_plugin_dependency_pollution(self) -> None:
        """The primary plugin shares metadata and versions but owns its framework dependencies."""
        root = manifest(ROOT / "Cargo.toml")
        shared = root["workspace"]["dependencies"]
        for name in PLUGIN_ONLY:
            assert name not in shared
        for crate in PRIMARY_CRATES:
            package_manifest = ROOT / f"image-control-plugin/rust/crates/{crate}/Cargo.toml"
            local = manifest(package_manifest)
            package = local["package"]
            assert (package_manifest.parent / package["workspace"]).resolve() == ROOT
            assert isinstance(package["version"], str), "ImageControl keeps an independent package version"
            assert package["edition"]["workspace"]
            for name, dependency in local.get("dependencies", {}).items():
                if isinstance(dependency, dict) and dependency.get("workspace"):
                    assert name in shared
        members = metadata(ROOT)
        names = {package["name"] for package in members["packages"]}
        assert {"audiobatch-cli", "audiobatch-gui", "image-control-dsp", "image-control-plugin-rs"} <= names
        assert "image-control-editor" not in names
        assert "image-control-gpu" not in names
        assert "image-control-vst3" not in names
        assert "patch" not in root

    def test_root_manifest_paths_use_the_shared_repository_root(self) -> None:
        """Scripts must not reconstruct root manifest paths by traversing above a project variable."""
        application_helpers = (ROOT / "rust/common.sh").read_text(encoding="utf-8")
        assert '"$REPO_ROOT/Cargo.toml"' in application_helpers
        assert '"$REPO/../Cargo.toml"' not in application_helpers
        for name in ("build.sh", "test.sh", "lint.sh", "format.sh", "rtsan.sh"):
            source = (ROOT / "image-control-plugin/rust" / name).read_text(encoding="utf-8")
            assert "$REPO_ROOT/" in source
            assert "../../Cargo.toml" not in source
            assert "$SCRIPT_DIR/../" not in source

    def test_legacy_gpui_patch_workspace_cannot_override_application_dependencies(self) -> None:
        """The legacy GUI retains a separate workspace root and cannot patch the application graph."""
        legacy_path = ROOT / "image-control-plugin/rust/Cargo.toml"
        if not legacy_path.exists():
            self.skipTest("legacy ImageControl experiment is not present in this checkout")
        legacy = manifest(legacy_path)
        assert "gpui-pre" in legacy["patch"]["crates-io"]
        owned = metadata(ROOT, legacy_path)
        assert Path(owned["workspace_root"]) == legacy_path.parent
        names = {package["name"] for package in owned["packages"]}
        assert names == {"image-control-editor", "image-control-gpu", "image-control-vst3"}


if __name__ == "__main__":
    unittest.main()
