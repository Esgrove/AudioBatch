//! Executable-level no-build and invocation-relative artifact-root checks without loading plugin binaries.

use std::path::{Path, PathBuf};
use std::process::Command;

/// Launches the built bundler directly so the no-build contract does not include an outer Cargo invocation.
fn bundler(invocation: &Path, target: &Path) -> Command {
    let mut command = Command::new(env!("CARGO_BIN_EXE_image-control-xtask"));
    command
        .current_dir(invocation)
        .env("CARGO_TARGET_DIR", target)
        // A missing CMake executable must not matter when packaging an existing cdylib.
        .env("CMAKE", invocation.join("CMake must not run"));
    command
}

/// Uses native filenames while keeping the fake artifact intentionally unloadable.
fn dynamic_library(target: &Path, profile: &str) -> PathBuf {
    let filename = if cfg!(target_os = "macos") {
        "libimage_control_plugin_rs.dylib"
    } else {
        "image_control_plugin_rs.dll"
    };
    target.join(profile).join(filename)
}

/// Writes isolated byte fixtures instead of constructing a plugin or using installed artifacts.
fn write_artifact(path: &Path, bytes: &[u8]) {
    std::fs::create_dir_all(path.parent().expect("artifact parent")).expect("create artifact parent");
    std::fs::write(path, bytes).expect("write artifact");
}

/// Actual no-build packaging respects a relative override even though metadata runs from the workspace.
#[test]
fn no_build_packages_relative_target_without_compiling_or_loading_the_artifact() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let relative_target = Path::new("custom artifacts with spaces");
    let target = directory.path().join(relative_target);
    write_artifact(&dynamic_library(&target, "release"), b"not a loadable plugin binary");
    let output = bundler(directory.path(), relative_target)
        .args([
            "bundle",
            "image-control-plugin-rs",
            "--release",
            "--no-build",
            "--format",
            "clap",
        ])
        .output()
        .expect("run bundler");
    assert!(
        output.status.success(),
        "bundler stderr: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let bundle = target.join("bundled/ImageControl RS.clap");
    let executable = if cfg!(target_os = "macos") {
        bundle.join("Contents/MacOS/ImageControl RS")
    } else {
        bundle.clone()
    };
    assert_eq!(
        std::fs::read(executable).expect("packaged executable"),
        b"not a loadable plugin binary"
    );
    let reported = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());
    assert_eq!(
        reported.canonicalize().expect("reported package path"),
        bundle.canonicalize().expect("expected package path")
    );
    assert!(!target.join("image-control-au").exists());
    assert!(!target.join("debug").exists());
    assert!(!target.join("bundled/ImageControl RS.vst3").exists());
}

/// A real no-build invocation fails for a missing Release cdylib rather than reusing Debug or public outputs.
#[test]
fn no_build_rejects_missing_profile_without_touching_old_bundle() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let target = directory.path().join("target");
    write_artifact(&dynamic_library(&target, "debug"), b"existing debug library");
    let previous = target.join("bundled/ImageControl RS.clap");
    write_artifact(&previous, b"previous public package");
    let output = bundler(directory.path(), &target)
        .args(["bundle", "image-control-plugin-rs", "-r", "-N", "-f", "clap"])
        .output()
        .expect("run bundler");
    assert!(!output.status.success());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("matching-profile Rust dynamic library"),
        "unexpected error: {stderr}"
    );
    assert_eq!(
        std::fs::read(previous).expect("previous package"),
        b"previous public package"
    );
    assert!(!target.join("release").exists());
}

/// Help must work with no build tools in PATH and without creating the requested target directory.
#[test]
fn help_does_not_run_cargo_or_cmake() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let target = directory.path().join("must not exist");
    let output = bundler(directory.path(), &target)
        .env("PATH", directory.path())
        .arg("--help")
        .output()
        .expect("run help");
    assert!(output.status.success());
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(stdout.contains("-f, --format"));
    assert!(stdout.contains("-N, --no-build"));
    assert!(!target.exists());
}
