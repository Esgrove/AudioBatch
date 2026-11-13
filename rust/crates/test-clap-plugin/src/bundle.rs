//! Wraps Cargo's uniquely named CLAP module example in isolated test bundles.

use std::io;
use std::path::{Path, PathBuf};

/// Distinct from the VST3 fixture example, which occupies the same Cargo examples directory.
const EXAMPLE_NAME: &str = "clap_module";

/// Returns the example library next to the current Cargo test executable's profile directory.
///
/// # Errors
/// Returns an error if the current executable cannot be resolved to a Cargo profile directory.
pub fn module_library_path() -> io::Result<PathBuf> {
    let executable = std::env::current_exe()?;
    let profile = profile_folder(&executable)
        .ok_or_else(|| io::Error::other("the test executable is not inside a Cargo target directory"))?;
    Ok(profile.join("examples").join(format!(
        "{}{EXAMPLE_NAME}{}",
        std::env::consts::DLL_PREFIX,
        std::env::consts::DLL_SUFFIX
    )))
}

/// Resolves the Cargo profile folder (`<target>/<profile>`) across both `<profile>/deps/<exe>`
/// and Cargo 1.100's `<profile>/build/<pkg>/<hash>/out/<exe>` build-dir layouts.
fn profile_folder(executable: &Path) -> Option<&Path> {
    let direct = executable.parent()?.parent()?;
    if direct
        .parent()
        .and_then(Path::parent)
        .and_then(Path::file_name)
        .is_some_and(|name| name == "build")
    {
        return direct.parent()?.parent()?.parent();
    }
    Some(direct)
}

/// Copies the module into `<folder>/<name>.clap`, never a system plugin directory.
///
/// On macOS this is a bundle directory, on Windows it is the library file itself.
/// The name accepts only ASCII letters, digits, underscores, and hyphens, preventing path or XML injection.
///
/// # Errors
/// Returns an error for invalid names, a missing module library, or filesystem failures.
pub fn create_bundle(folder: &Path, name: &str) -> io::Result<PathBuf> {
    if name.is_empty()
        || !name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
    {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid test bundle name"));
    }
    let library = module_library_path()?;
    if !library.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "{} is missing, build it with `cargo build -p audiobatch-test-clap-plugin --example {EXAMPLE_NAME}`",
                library.display()
            ),
        ));
    }
    let bundle = folder.join(format!("{name}.clap"));
    if cfg!(target_os = "macos") {
        let contents = bundle.join("Contents");
        let executables = contents.join("MacOS");
        std::fs::create_dir_all(&executables)?;
        std::fs::copy(library, executables.join(name))?;
        std::fs::write(contents.join("Info.plist"), info_plist(name))?;
    } else if cfg!(windows) {
        std::fs::create_dir_all(folder)?;
        std::fs::copy(library, &bundle)?;
    } else {
        return Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "test bundles require macOS or Windows",
        ));
    }
    Ok(bundle)
}

/// Provides the native executable name and stable test-only identifier required by macOS CLAP loaders.
fn info_plist(name: &str) -> String {
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>{name}</string>
    <key>CFBundleIdentifier</key>
    <string>com.audiobatch.tests.clap.{name}</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
"#
    )
}
