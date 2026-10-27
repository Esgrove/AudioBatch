//! Wrapping the `module` example library into a `.vst3` bundle for tests.
//!
//! Cargo builds the example whenever the workspace tests are built,
//! into the `examples` folder next to the `deps` folder that holds the test executables.

use std::io;
use std::path::{Path, PathBuf};

/// Name of the example that builds the plugin library.
const EXAMPLE_NAME: &str = "module";

/// Returns where Cargo puts the `module` example library for the running test executable.
///
/// # Errors
///
/// Returns an error when the path of the running executable is unknown.
pub fn module_library_path() -> io::Result<PathBuf> {
    let executable = std::env::current_exe()?;
    let profile_folder = executable
        .parent()
        .and_then(Path::parent)
        .ok_or_else(|| io::Error::other("the test executable is not inside a Cargo target folder"))?;
    let file_name = format!(
        "{}{EXAMPLE_NAME}{}",
        std::env::consts::DLL_PREFIX,
        std::env::consts::DLL_SUFFIX
    );
    Ok(profile_folder.join("examples").join(file_name))
}

/// Creates the bundle `<folder>/<name>.vst3` around a copy of the `module` example library and returns its path.
///
/// Each bundle holds its own copy, so bundles with different names load as separate modules.
///
/// # Errors
///
/// Returns an error when the library has not been built or the bundle cannot be written.
pub fn create_bundle(folder: &Path, name: &str) -> io::Result<PathBuf> {
    let library = module_library_path()?;
    if !library.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "{} is missing. Build it with `cargo build -p audiobatch-test-plugin --example {EXAMPLE_NAME}`, \
                 or run the tests of the whole workspace",
                library.display()
            ),
        ));
    }
    let bundle = folder.join(format!("{name}.vst3"));
    let contents = bundle.join("Contents");
    let library_target = library_location(&contents, name);
    if let Some(parent) = library_target.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::copy(&library, &library_target)?;
    if cfg!(target_os = "macos") {
        std::fs::write(contents.join("Info.plist"), info_plist(name))?;
    }
    Ok(bundle)
}

/// Returns where the library goes inside the bundle contents.
fn library_location(contents: &Path, name: &str) -> PathBuf {
    if cfg!(target_os = "macos") {
        contents.join("MacOS").join(name)
    } else if cfg!(windows) {
        let architecture = if cfg!(target_arch = "aarch64") {
            "arm64-win"
        } else {
            "x86_64-win"
        };
        contents.join(architecture).join(format!("{name}.vst3"))
    } else {
        contents
            .join(format!("{}-linux", std::env::consts::ARCH))
            .join(format!("{name}.so"))
    }
}

/// Returns the `Info.plist` of a macOS bundle.
fn info_plist(name: &str) -> String {
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>{name}</string>
    <key>CFBundleIdentifier</key>
    <string>com.audiobatch.tests.{name}</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
</dict>
</plist>
"#
    )
}
