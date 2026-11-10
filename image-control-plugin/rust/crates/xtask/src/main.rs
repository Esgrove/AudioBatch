//! Small VST3 bundler with no plugin framework or code loaded into the packaging process.

use std::error::Error;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Distinct product identity, independent of the C++ and abandoned framework experiments.
const PLUGIN_NAME: &str = "ImageControl RS";
/// macOS bundle identity must not collide with installed C++ `ImageControl` or Nice projects.
const BUNDLE_ID: &str = "com.esgrove.imagecontrol-rs-own";

/// Builds the primary plugin with Cargo and wraps the output without installing it.
fn main() -> Result<(), Box<dyn Error>> {
    let arguments = std::env::args().skip(1).collect::<Vec<_>>();
    if arguments
        .iter()
        .any(|argument| argument == "-h" || argument == "--help")
    {
        println!("Usage: image-control-xtask bundle image-control-plugin-rs [--release] [--no-build]");
        return Ok(());
    }
    let mut arguments = arguments.iter();
    if arguments.next().map(String::as_str) != Some("bundle")
        || arguments.next().map(String::as_str) != Some("image-control-plugin-rs")
    {
        return Err("Usage: image-control-xtask bundle image-control-plugin-rs [--release] [--no-build]".into());
    }
    let mut release = false;
    let mut no_build = false;
    for argument in arguments {
        match argument.as_str() {
            "--release" => release = true,
            "--no-build" => no_build = true,
            _ => return Err(format!("unsupported bundler argument: {argument}").into()),
        }
    }
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(4)
        .ok_or("repository root missing")?;
    let metadata = Command::new("cargo")
        .current_dir(workspace)
        .args(["metadata", "--no-deps", "--format-version", "1"])
        .output()?;
    if !metadata.status.success() {
        return Err("cargo metadata failed".into());
    }
    let metadata: serde_json::Value = serde_json::from_slice(&metadata.stdout)?;
    let root = PathBuf::from(
        metadata
            .get("workspace_root")
            .and_then(serde_json::Value::as_str)
            .ok_or("workspace root missing")?,
    );
    let target = PathBuf::from(
        metadata
            .get("target_directory")
            .and_then(serde_json::Value::as_str)
            .ok_or("target directory missing")?,
    );
    let package_version = metadata
        .get("packages")
        .and_then(serde_json::Value::as_array)
        .and_then(|packages| {
            packages.iter().find(|package| {
                package.get("name").and_then(serde_json::Value::as_str) == Some("image-control-plugin-rs")
            })
        })
        .and_then(|package| package.get("version"))
        .and_then(serde_json::Value::as_str)
        .ok_or("plugin version missing")?;
    if !no_build {
        let mut command = Command::new("cargo");
        command
            .current_dir(&root)
            .args(["build", "-p", "image-control-plugin-rs"]);
        if release {
            command.arg("--release");
        }
        if !command.status()?.success() {
            return Err("plugin build failed".into());
        }
    }
    let profile = if release { "release" } else { "debug" };
    let library = target.join(profile).join(format!(
        "{}image_control_plugin_rs{}",
        std::env::consts::DLL_PREFIX,
        std::env::consts::DLL_SUFFIX
    ));
    if !library.is_file() {
        return Err(format!("built plugin is missing: {}", library.display()).into());
    }
    let bundle = target.join("bundled").join(format!("{PLUGIN_NAME}.vst3"));
    let executable = executable_path(&bundle)?;
    std::fs::create_dir_all(executable.parent().ok_or("invalid bundle path")?)?;
    std::fs::copy(library, executable)?;
    if cfg!(target_os = "macos") {
        std::fs::write(
            bundle.join("Contents/Info.plist"),
            format!(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>\
             <key>CFBundleExecutable</key><string>{PLUGIN_NAME}</string>\
             <key>CFBundleIdentifier</key><string>{BUNDLE_ID}</string>\
             <key>CFBundleName</key><string>{PLUGIN_NAME}</string>\
             <key>CFBundlePackageType</key><string>BNDL</string>\
             <key>CFBundleVersion</key><string>{package_version}</string>\
             <key>CFBundleShortVersionString</key><string>{package_version}</string></dict></plist>"
            ),
        )?;
    }
    println!("{}", bundle.display());
    Ok(())
}

/// Chooses the standard executable location without linking any platform GUI dependency into the bundler.
fn executable_path(bundle: &Path) -> Result<PathBuf, Box<dyn Error>> {
    if cfg!(target_os = "macos") {
        Ok(bundle.join("Contents/MacOS").join(PLUGIN_NAME))
    } else if cfg!(windows) {
        let architecture = if cfg!(target_arch = "aarch64") {
            "arm64-win"
        } else {
            "x86_64-win"
        };
        Ok(bundle
            .join("Contents")
            .join(architecture)
            .join(format!("{PLUGIN_NAME}.vst3")))
    } else {
        Err("VST3 packaging supports macOS and Windows".into())
    }
}
