//! Strict bundler arguments and native format selection, independent of build tool execution.

use std::error::Error;

use crate::{PLUGIN_PACKAGE, Platform, PluginFormat, Profile};

/// Help is processed before metadata, builds, or artifact access.
pub const USAGE: &str = "Usage: image-control-xtask bundle image-control-plugin-rs [OPTIONS]

Packages native formats without installing or loading the plugin.
Debug is the default. Packages are written to Cargo's target directory under bundled/.

OPTIONS:
    -h, --help                  Show help without running build tools.
    -r, --release               Build and package the Release profile.
    -d, --debug                 Build and package the Debug profile (default).
    -N, --no-build              Package existing matching-profile artifacts without compiling.
    -f, --format <FORMAT>       all, vst3, clap, or au (default: all).
                               all means VST3 + CLAP + AU on macOS, VST3 + CLAP on Windows.

Relative CARGO_TARGET_DIR paths are resolved from the invoking directory.
The bundler leaves final signing and validation to the project scripts.";

/// User-facing format selection, resolved against the native platform before builds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FormatSelection {
    All,
    One(PluginFormat),
}

impl FormatSelection {
    /// Rejects unsupported spellings rather than silently omitting a requested format.
    fn parse(value: &str) -> Result<Self, Box<dyn Error>> {
        match value {
            "all" => Ok(Self::All),
            "vst3" => Ok(Self::One(PluginFormat::Vst3)),
            "clap" => Ok(Self::One(PluginFormat::Clap)),
            "au" => Ok(Self::One(PluginFormat::AudioUnit)),
            _ => Err(format!("unsupported plugin format: {value}. Expected all, vst3, clap, or au").into()),
        }
    }

    /// Expands all to only the formats supported on the native host.
    pub fn select(self, platform: Platform) -> Result<Vec<PluginFormat>, Box<dyn Error>> {
        match (self, platform) {
            (Self::All, Platform::MacOs) => Ok(vec![PluginFormat::Vst3, PluginFormat::Clap, PluginFormat::AudioUnit]),
            (Self::All, Platform::Windows) => Ok(vec![PluginFormat::Vst3, PluginFormat::Clap]),
            (Self::One(PluginFormat::AudioUnit), Platform::Windows) => {
                Err("Audio Unit packaging requires macOS".into())
            }
            (Self::One(format), _) => Ok(vec![format]),
        }
    }
}

/// Compilation and artifact choices for one invocation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Options {
    /// Cargo and `CMake` configuration selected together.
    pub profile: Profile,
    /// Suppresses every compilation and `CMake` configuration command.
    pub no_build: bool,
    /// Native format subset selected by the caller.
    pub formats: FormatSelection,
}

impl Options {
    /// Parses the fixed command prefix and genuine short/long options before performing any work.
    pub fn parse(arguments: &[String]) -> Result<Option<Self>, Box<dyn Error>> {
        if arguments
            .iter()
            .any(|argument| argument == "-h" || argument == "--help")
        {
            return Ok(None);
        }
        let mut arguments = arguments.iter().map(String::as_str);
        if arguments.next() != Some("bundle") || arguments.next() != Some(PLUGIN_PACKAGE) {
            return Err(USAGE.into());
        }
        let mut options = Self {
            profile: Profile::Debug,
            no_build: false,
            formats: FormatSelection::All,
        };
        let mut selected_profile = None;
        let mut selected_format = false;
        while let Some(argument) = arguments.next() {
            match argument {
                "-r" | "--release" | "-d" | "--debug" => {
                    let profile = if matches!(argument, "-r" | "--release") {
                        Profile::Release
                    } else {
                        Profile::Debug
                    };
                    if selected_profile.is_some_and(|selected| selected != profile) {
                        return Err("--debug and --release cannot be combined".into());
                    }
                    selected_profile = Some(profile);
                    options.profile = profile;
                }
                "-N" | "--no-build" => options.no_build = true,
                "-f" | "--format" => {
                    let value = arguments.next().ok_or("--format requires all, vst3, clap, or au")?;
                    options.formats = parse_format(value, &mut selected_format)?;
                }
                _ => {
                    if let Some(value) = argument.strip_prefix("--format=") {
                        options.formats = parse_format(value, &mut selected_format)?;
                    } else {
                        return Err(format!("unsupported bundler argument: {argument}").into());
                    }
                }
            }
        }
        Ok(Some(options))
    }
}

/// Rejects repeated format selectors so conflicting arguments never package an unintended subset.
fn parse_format(value: &str, selected: &mut bool) -> Result<FormatSelection, Box<dyn Error>> {
    if *selected {
        return Err("specify --format only once".into());
    }
    *selected = true;
    FormatSelection::parse(value)
}
