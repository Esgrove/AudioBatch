//! Explicit source-artifact validation and clean, staged packages without executing plugin code.

use std::error::Error;
use std::path::Path;

use crate::{BUNDLE_ID, BuildContext, MINIMUM_MACOS_VERSION, PLUGIN_NAME, Platform, PluginFormat};

/// Checks every requested source before replacing any public package, including no-build invocations.
pub fn validate_inputs(context: &BuildContext, formats: &[PluginFormat]) -> Result<(), Box<dyn Error>> {
    if formats
        .iter()
        .any(|format| matches!(format, PluginFormat::Vst3 | PluginFormat::Clap))
    {
        require_file(&context.dynamic_library(), "matching-profile Rust dynamic library")?;
    }
    if formats.contains(&PluginFormat::AudioUnit) {
        if context.platform != Platform::MacOs {
            return Err("Audio Unit packaging requires macOS".into());
        }
        validate_audio_unit(context)?;
    }
    Ok(())
}

/// Requires a real, nonempty regular artifact rather than a directory, symlink, or missing profile output.
pub fn require_file(path: &Path, description: &str) -> Result<(), Box<dyn Error>> {
    let metadata = std::fs::symlink_metadata(path).map_err(|error| {
        format!(
            "{description} is missing at {}: {error}. Build the selected profile first",
            path.display()
        )
    })?;
    if !metadata.is_file() || metadata.len() == 0 {
        return Err(format!("{description} is not a nonempty regular file: {}", path.display()).into());
    }
    Ok(())
}

/// Confirms the sibling AU was configured for this exact archive, version, architecture, and profile.
fn validate_audio_unit(context: &BuildContext) -> Result<(), Box<dyn Error>> {
    let archive = context.static_library();
    require_file(&archive, "matching-profile Rust static library")?;
    let cache_path = context.au_build().join("CMakeCache.txt");
    require_file(&cache_path, "matching-profile AU CMake cache")?;
    let cache = std::fs::read_to_string(&cache_path)?;
    let archive_string = archive
        .to_str()
        .ok_or("AU archive path must be UTF-8 for CMake cache validation")?;
    for (key, expected) in [
        ("CMAKE_BUILD_TYPE", context.profile.cmake_name()),
        ("CMAKE_OSX_ARCHITECTURES", context.architecture.cmake_name()),
        ("CMAKE_OSX_DEPLOYMENT_TARGET", MINIMUM_MACOS_VERSION),
        ("IMAGE_CONTROL_RUST_LIBRARY", archive_string),
        ("IMAGE_CONTROL_VERSION", context.version.as_str()),
    ] {
        if cache_value(&cache, key) != Some(expected) {
            return Err(format!(
                "AU configuration mismatch for {key} in {}. Build the selected native profile first",
                cache_path.display()
            )
            .into());
        }
    }
    let bundle = context.au_bundle();
    require_file(&bundle.join("Contents/Info.plist"), "AU Info.plist")?;
    let executable = context.executable(&bundle, PluginFormat::AudioUnit)?;
    require_file(&executable, "matching-profile AU executable")?;
    if std::fs::metadata(&executable)?.modified()? < std::fs::metadata(&archive)?.modified()? {
        return Err(format!(
            "AU executable is older than its Rust archive: {}. Build the selected native profile first",
            executable.display()
        )
        .into());
    }
    Ok(())
}

/// Reads only exact `CMake` cache keys, ignoring comments and similarly named settings.
fn cache_value<'a>(cache: &'a str, key: &str) -> Option<&'a str> {
    cache.lines().find_map(|line| {
        let (name, entry) = line.split_once(':')?;
        if name != key {
            return None;
        }
        let (_, value) = entry.split_once('=')?;
        Some(value)
    })
}

/// Writes a clean temporary package then publishes it, keeping stale signatures and files out of new builds.
pub fn package(context: &BuildContext, format: PluginFormat) -> Result<std::path::PathBuf, Box<dyn Error>> {
    let destination = context.bundle(format);
    let parent = destination.parent().ok_or("bundle directory missing")?;
    std::fs::create_dir_all(parent)?;
    let staging = tempfile::Builder::new()
        .prefix(".image-control-package-")
        .tempdir_in(parent)?;
    let staged = staging
        .path()
        .join(destination.file_name().ok_or("bundle filename missing")?);
    if format == PluginFormat::AudioUnit {
        copy_bundle(&context.au_bundle(), &staged)?;
    } else {
        let executable = context.executable(&staged, format)?;
        std::fs::create_dir_all(executable.parent().ok_or("executable parent missing")?)?;
        std::fs::copy(context.dynamic_library(), &executable)?;
        if context.platform == Platform::MacOs {
            std::fs::write(
                staged.join("Contents/Info.plist"),
                info_plist(format, &context.version)?,
            )?;
        }
    }
    if let Ok(metadata) = std::fs::symlink_metadata(&destination) {
        if metadata.is_dir() && !metadata.file_type().is_symlink() {
            std::fs::remove_dir_all(&destination)?;
        } else {
            std::fs::remove_file(&destination)?;
        }
    }
    std::fs::rename(staged, &destination)?;
    Ok(destination)
}

/// Copies the native AU wrapper's metadata and resources without retaining obsolete bundle signatures.
fn copy_bundle(source: &Path, destination: &Path) -> Result<(), Box<dyn Error>> {
    std::fs::create_dir_all(destination)?;
    for entry in std::fs::read_dir(source)? {
        let entry = entry?;
        if entry.file_name() == "_CodeSignature" {
            continue;
        }
        let kind = entry.file_type()?;
        let output = destination.join(entry.file_name());
        if kind.is_dir() {
            copy_bundle(&entry.path(), &output)?;
        } else if kind.is_file() {
            std::fs::copy(entry.path(), output)?;
        } else {
            return Err(format!("unsupported special file in AU bundle: {}", entry.path().display()).into());
        }
    }
    Ok(())
}

/// Emits only wrapper metadata. The executable contains both exports and is never opened by packaging.
fn info_plist(format: PluginFormat, version: &str) -> Result<String, Box<dyn Error>> {
    let identifier = match format {
        PluginFormat::Vst3 => BUNDLE_ID.to_owned(),
        PluginFormat::Clap => format!("{BUNDLE_ID}.clap"),
        PluginFormat::AudioUnit => return Err("AU metadata must come from its native wrapper".into()),
    };
    let version = xml_text(version);
    Ok(format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
         <!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
         <plist version=\"1.0\"><dict>\n\
         <key>CFBundleExecutable</key><string>{PLUGIN_NAME}</string>\n\
         <key>CFBundleIdentifier</key><string>{identifier}</string>\n\
         <key>CFBundleName</key><string>{PLUGIN_NAME}</string>\n\
         <key>CFBundlePackageType</key><string>BNDL</string>\n\
         <key>CFBundleVersion</key><string>{version}</string>\n\
         <key>CFBundleShortVersionString</key><string>{version}</string>\n\
         <key>LSMinimumSystemVersion</key><string>{MINIMUM_MACOS_VERSION}</string>\n\
         </dict></plist>\n"
    ))
}

/// Escapes text values so Cargo prerelease metadata cannot produce invalid XML.
fn xml_text(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}
