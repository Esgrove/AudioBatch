# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
"""Check repository-root Cargo ownership and the consolidated Rust plugin dependency boundary."""

import json
import shutil
import subprocess
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRIMARY_CRATES = ("dsp", "plugin", "xtask")
PLUGIN_ONLY = ("iced_baseview", "iced_program", "iced_widget", "baseview", "fragile")
REMOVED_PACKAGES = {"image-control-editor", "image-control-gpu", "image-control-vst3"}


def manifest(path: Path) -> dict:
    """Read a real Cargo manifest without invoking a build or updating dependency resolution."""
    return tomllib.loads(path.read_text(encoding="utf-8"))


def metadata(directory: Path) -> dict:
    """Obtain read-only package ownership and target information from Cargo itself."""
    cargo = shutil.which("cargo")
    assert cargo is not None, "Cargo is required to check workspace ownership"
    result = subprocess.run(  # noqa: S603
        [cargo, "metadata", "--no-deps", "--format-version", "1"],
        cwd=directory,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(result.stdout)


def has_primary_plugin() -> bool:
    """Keep migration-only checkouts valid while checking every registered plugin member."""
    return any(
        member.startswith("image-control-plugin/") for member in manifest(ROOT / "Cargo.toml")["workspace"]["members"]
    )


class RootWorkspaceTests(unittest.TestCase):
    """Pin Cargo-discovered ownership rather than strings in project-specific shell scripts."""

    def test_root_and_application_directory_share_one_workspace_and_target(self) -> None:
        """Root and existing application commands discover the same manifest and output directory."""
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
    def test_primary_packages_share_root_without_plugin_dependency_pollution(self) -> None:
        """The single plugin shares metadata but owns its GUI dependencies and independent version."""
        root = manifest(ROOT / "Cargo.toml")
        shared = root["workspace"]["dependencies"]
        for name in PLUGIN_ONLY:
            assert name not in shared
        for crate in PRIMARY_CRATES:
            package_manifest = ROOT / f"image-control-plugin/rust/crates/{crate}/Cargo.toml"
            local = manifest(package_manifest)
            package = local["package"]
            assert (package_manifest.parent / package["workspace"]).resolve() == ROOT
            assert isinstance(package["version"], str)
            assert package["edition"]["workspace"]
            for name, dependency in local.get("dependencies", {}).items():
                if isinstance(dependency, dict) and dependency.get("workspace"):
                    assert name in shared
        workspace = metadata(ROOT)
        names = {package["name"] for package in workspace["packages"]}
        assert {
            "audiobatch-cli",
            "audiobatch-gui",
            "image-control-dsp",
            "image-control-plugin-rs",
            "image-control-xtask",
        } <= names
        defaults = {
            package["name"]
            for package in workspace["packages"]
            if package["id"] in workspace["workspace_default_members"]
        }
        assert {"audiobatch-cli", "audiobatch-gui", "image-control-dsp", "image-control-plugin-rs"} <= defaults
        assert "image-control-xtask" not in defaults
        assert names.isdisjoint(REMOVED_PACKAGES)
        assert "patch" not in root

    def test_root_manifest_paths_use_the_shared_repository_root(self) -> None:
        """Scripts use REPO_ROOT rather than reconstructing manifest paths from project directories."""
        helpers = (ROOT / "rust/common.sh").read_text(encoding="utf-8")
        assert '"$REPO_ROOT/Cargo.toml"' in helpers
        assert '"$REPO/../Cargo.toml"' not in helpers
        for name in ("build.sh", "test.sh", "lint.sh", "format.sh", "rtsan.sh"):
            source = (ROOT / "image-control-plugin/rust" / name).read_text(encoding="utf-8")
            assert "$REPO_ROOT/" in source
            assert "../../Cargo.toml" not in source
            assert "$SCRIPT_DIR/../" not in source
            assert "--raw" not in source
            assert "crates/nice" not in source

    @unittest.skipUnless(has_primary_plugin(), "primary ImageControl is not registered in this checkout")
    def test_plugin_directory_uses_root_without_legacy_workspace_or_patches(self) -> None:
        """Cargo inside the plugin discovers the root and cannot load removed experimental packages."""
        plugin = ROOT / "image-control-plugin/rust"
        assert not (plugin / "Cargo.toml").exists()
        assert not (plugin / "Cargo.lock").exists()
        owned = metadata(plugin)
        assert Path(owned["workspace_root"]) == ROOT
        assert Path(owned["target_directory"]) == ROOT / "rust/target"
        for removed in ("editor", "gpu", "vst3", "nice"):
            directory = plugin / "crates" / removed
            assert not (directory / "Cargo.toml").exists()
            assert not list(directory.rglob("*.rs")), f"removed source remains under {directory}"
            assert not list(directory.rglob("*.wgsl"))
            assert not list(directory.rglob("*.metal"))
        assert not (plugin / "vendor/PATCHES.txt").exists()
        assert not list((plugin / "vendor").glob("*/Cargo.toml"))
        hook_config = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        assert "crates/(dsp|plugin|xtask)" in hook_config
        assert "crates/(dsp|nice|xtask)" not in hook_config
        names = {package["name"] for package in owned["packages"]}
        assert names.isdisjoint(REMOVED_PACKAGES)
        # The application keeps its real GPUI dependency. Only plugin-local experiments disappear.
        assert "gpui-kit" in manifest(ROOT / "Cargo.toml")["workspace"]["dependencies"]


if __name__ == "__main__":
    unittest.main()
