//! Packages the native VST3, CLAP, and Audio Unit without loading or installing any plugin.

use std::error::Error;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;

mod arguments;
mod package;

use arguments::Options;

/// Distinct product identity, independent of the C++ plugin.
const PLUGIN_NAME: &str = "ImageControl RS";
/// Stable Rust plugin package selected by the bundler.
const PLUGIN_PACKAGE: &str = "image-control-plugin-rs";
/// macOS identity of the own-wrapper VST3.
const BUNDLE_ID: &str = "com.esgrove.imagecontrol-rs-own";
/// Matching minimum deployment target for the Rust archive and the native AU wrapper.
const MINIMUM_MACOS_VERSION: &str = "15.0";
/// The sibling `CMake` project exposes one native, single-configuration AU target.
const AU_TARGET: &str = "ImageControlRSAU";

/// Supported native packaging platforms, also explicit in path and command tests.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Platform {
    MacOs,
    Windows,
}

impl Platform {
    /// Rejects unsupported hosts before creating or replacing any packages.
    fn native() -> Result<Self, Box<dyn Error>> {
        if cfg!(target_os = "macos") {
            Ok(Self::MacOs)
        } else if cfg!(windows) {
            Ok(Self::Windows)
        } else {
            Err("ImageControl RS packaging supports macOS and Windows".into())
        }
    }
}

/// Native architectures shared by the Rust library and format wrappers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Architecture {
    Arm64,
    X86_64,
}

impl Architecture {
    /// Selects the running bundler's architecture rather than an unrelated `CMake` default.
    fn native() -> Result<Self, Box<dyn Error>> {
        if cfg!(target_arch = "aarch64") {
            Ok(Self::Arm64)
        } else if cfg!(target_arch = "x86_64") {
            Ok(Self::X86_64)
        } else {
            Err("ImageControl RS packaging requires arm64 or x86_64".into())
        }
    }

    /// Maps Rust's architecture to `CMake`'s macOS architecture spelling.
    const fn cmake_name(self) -> &'static str {
        match self {
            Self::Arm64 => "arm64",
            Self::X86_64 => "x86_64",
        }
    }
}

/// Cargo and `CMake` profiles must select the same Rust archive and native AU wrapper.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Profile {
    Debug,
    Release,
}

impl Profile {
    /// Returns Cargo's on-disk profile directory.
    const fn cargo_name(self) -> &'static str {
        match self {
            Self::Debug => "debug",
            Self::Release => "release",
        }
    }

    /// Returns the matching single-configuration `CMake` build type.
    const fn cmake_name(self) -> &'static str {
        match self {
            Self::Debug => "Debug",
            Self::Release => "Release",
        }
    }
}

/// Independently selectable packages sharing the same Rust implementation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PluginFormat {
    Vst3,
    Clap,
    AudioUnit,
}

impl PluginFormat {
    /// Returns the conventional artifact extension without a leading dot.
    const fn extension(self) -> &'static str {
        match self {
            Self::Vst3 => "vst3",
            Self::Clap => "clap",
            Self::AudioUnit => "component",
        }
    }
}

/// Authoritative workspace paths and native configuration for one bundling operation.
struct BuildContext {
    /// Workspace root returned by Cargo metadata.
    workspace: PathBuf,
    /// Invocation-resolved Cargo artifact root, shared by all formats.
    target: PathBuf,
    /// Plugin package version returned by Cargo metadata.
    version: String,
    /// Native host platform, never a cross-compilation destination.
    platform: Platform,
    /// Native architecture passed explicitly to the AU build.
    architecture: Architecture,
    /// Matching Rust and native wrapper configuration.
    profile: Profile,
}

impl BuildContext {
    /// Selects the sibling `CMake` source without depending on the invoking directory.
    fn au_source(&self) -> PathBuf {
        self.workspace.join("image-control-plugin/rust/au")
    }

    /// Separates native AU configurations so a previous profile cannot satisfy another invocation.
    fn au_build(&self) -> PathBuf {
        self.target
            .join("image-control-au")
            .join(self.architecture.cmake_name())
            .join(self.profile.cargo_name())
    }

    /// Uses the sibling `CMake` project's stable, explicitly documented bundle output.
    fn au_bundle(&self) -> PathBuf {
        self.au_build().join(format!("{PLUGIN_NAME}.component"))
    }

    /// Locates only the requested profile's Cargo cdylib, never a previously bundled copy.
    fn dynamic_library(&self) -> PathBuf {
        let filename = match self.platform {
            Platform::MacOs => "libimage_control_plugin_rs.dylib",
            Platform::Windows => "image_control_plugin_rs.dll",
        };
        self.target.join(self.profile.cargo_name()).join(filename)
    }

    /// Selects the matching-profile static archive consumed by the native macOS AU target.
    fn static_library(&self) -> PathBuf {
        self.target
            .join(self.profile.cargo_name())
            .join("libimage_control_plugin_rs.a")
    }

    /// Keeps all public format artifacts under the same Cargo target root.
    fn bundle(&self, format: PluginFormat) -> PathBuf {
        self.target
            .join("bundled")
            .join(format!("{PLUGIN_NAME}.{}", format.extension()))
    }

    /// Selects the ABI-prescribed executable path without loading any platform or GUI dependency.
    fn executable(&self, bundle: &Path, format: PluginFormat) -> Result<PathBuf, Box<dyn Error>> {
        match (self.platform, format) {
            (Platform::MacOs, _) => Ok(bundle.join("Contents/MacOS").join(PLUGIN_NAME)),
            (Platform::Windows, PluginFormat::Clap) => Ok(bundle.to_path_buf()),
            (Platform::Windows, PluginFormat::Vst3) => {
                let architecture = match self.architecture {
                    Architecture::Arm64 => "arm64-win",
                    Architecture::X86_64 => "x86_64-win",
                };
                Ok(bundle
                    .join("Contents")
                    .join(architecture)
                    .join(format!("{PLUGIN_NAME}.vst3")))
            }
            (Platform::Windows, PluginFormat::AudioUnit) => Err("Audio Unit packaging requires macOS".into()),
        }
    }
}

/// Builds the selected native formats and packages their explicit artifacts without installing them.
fn main() -> Result<(), Box<dyn Error>> {
    let arguments = std::env::args().skip(1).collect::<Vec<_>>();
    let Some(options) = Options::parse(&arguments)? else {
        println!("{}", arguments::USAGE);
        return Ok(());
    };
    let platform = Platform::native()?;
    let architecture = Architecture::native()?;
    let formats = options.formats.select(platform)?;
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(4)
        .ok_or("repository root missing")?;
    let invocation_directory = std::env::current_dir()?;
    let target_override = std::env::var_os("CARGO_TARGET_DIR")
        .filter(|value| !value.is_empty())
        .map(|value| absolute_target_directory(Path::new(&value), &invocation_directory));
    let metadata = workspace_metadata(workspace, target_override.as_deref())?;
    let context = context_from_metadata(&metadata, platform, architecture, options.profile)?;
    for mut command in build_commands(&context, options, &formats) {
        if command.get_program() == "cmake" {
            package::require_file(&context.static_library(), "matching-profile Rust static library")?;
        }
        if !command.status()?.success() {
            return Err(format!("build command failed: {command:?}").into());
        }
    }
    package::validate_inputs(&context, &formats)?;
    for format in formats {
        let bundle = package::package(&context, format)?;
        println!("{}", bundle.display());
    }
    Ok(())
}

/// Extracts paths and package identity from Cargo rather than guessing the workspace artifact root.
fn context_from_metadata(
    metadata: &serde_json::Value,
    platform: Platform,
    architecture: Architecture,
    profile: Profile,
) -> Result<BuildContext, Box<dyn Error>> {
    let workspace = metadata
        .get("workspace_root")
        .and_then(serde_json::Value::as_str)
        .ok_or("workspace root missing")?;
    let target = metadata
        .get("target_directory")
        .and_then(serde_json::Value::as_str)
        .ok_or("target directory missing")?;
    let version = metadata
        .get("packages")
        .and_then(serde_json::Value::as_array)
        .and_then(|packages| {
            packages
                .iter()
                .find(|package| package.get("name").and_then(serde_json::Value::as_str) == Some(PLUGIN_PACKAGE))
        })
        .and_then(|package| package.get("version"))
        .and_then(serde_json::Value::as_str)
        .ok_or("plugin version missing")?;
    if !Path::new(workspace).is_absolute() || !Path::new(target).is_absolute() {
        return Err("Cargo metadata must provide absolute workspace and target paths".into());
    }
    Ok(BuildContext {
        workspace: workspace.into(),
        target: target.into(),
        version: version.into(),
        platform,
        architecture,
        profile,
    })
}

/// Plans every compilation command in one place so no-build cannot accidentally configure or compile the AU.
fn build_commands(context: &BuildContext, options: Options, formats: &[PluginFormat]) -> Vec<Command> {
    if options.no_build {
        return Vec::new();
    }
    let mut cargo = Command::new("cargo");
    cargo
        .current_dir(&context.workspace)
        .env("CARGO_TARGET_DIR", &context.target)
        .args(["build", "-p", PLUGIN_PACKAGE, "--lib"]);
    if context.profile == Profile::Release {
        cargo.arg("--release");
    }
    if context.platform == Platform::MacOs {
        cargo.env("MACOSX_DEPLOYMENT_TARGET", MINIMUM_MACOS_VERSION);
    }
    let mut commands = vec![cargo];
    if formats.contains(&PluginFormat::AudioUnit) {
        let mut configure = Command::new("cmake");
        configure
            .current_dir(&context.workspace)
            .arg("-S")
            .arg(context.au_source())
            .arg("-B")
            .arg(context.au_build())
            // Reuse the configured single-configuration generator instead of conflicting with a native test build.
            .arg(format!("-DCMAKE_BUILD_TYPE={}", context.profile.cmake_name()))
            .arg(format!("-DCMAKE_OSX_ARCHITECTURES={}", context.architecture.cmake_name()))
            .arg(format!("-DCMAKE_OSX_DEPLOYMENT_TARGET={MINIMUM_MACOS_VERSION}"))
            .arg(cmake_path_definition("IMAGE_CONTROL_RUST_LIBRARY", &context.static_library()))
            .arg(format!("-DIMAGE_CONTROL_VERSION={}", context.version))
            // Configure the isolated host without building or running it during packaging.
            .arg("-DIMAGE_CONTROL_AU_BUILD_TESTS=ON");
        commands.push(configure);
        let mut build = Command::new("cmake");
        build
            .current_dir(&context.workspace)
            .arg("--build")
            .arg(context.au_build())
            .args(["--config", context.profile.cmake_name(), "--target", AU_TARGET]);
        commands.push(build);
    }
    commands
}

/// Preserves native paths and spaces as one `CMake` argument without requiring UTF-8 path conversion.
fn cmake_path_definition(name: &str, path: &Path) -> OsString {
    let mut definition = OsString::from(format!("-D{name}="));
    definition.push(path);
    definition
}

/// Reads Cargo's authoritative workspace paths without resolving or compiling dependencies.
fn workspace_metadata(workspace: &Path, target_override: Option<&Path>) -> Result<serde_json::Value, Box<dyn Error>> {
    let mut command = Command::new("cargo");
    command
        .current_dir(workspace)
        .args(["metadata", "--no-deps", "--format-version", "1"]);
    if let Some(target) = target_override {
        command.env("CARGO_TARGET_DIR", target);
    }
    let metadata = command.output()?;
    if !metadata.status.success() {
        return Err(format!("cargo metadata failed: {}", String::from_utf8_lossy(&metadata.stderr)).into());
    }
    Ok(serde_json::from_slice(&metadata.stdout)?)
}

/// Anchors an explicit Cargo override before nested commands change the working directory.
fn absolute_target_directory(target: &Path, invocation_directory: &Path) -> PathBuf {
    if target.is_absolute() {
        target.to_path_buf()
    } else {
        invocation_directory.join(target)
    }
}

#[cfg(test)]
mod tests;
