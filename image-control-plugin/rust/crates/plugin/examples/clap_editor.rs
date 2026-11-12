//! Bounded native editor lifecycle smoke against an explicitly selected packaged `ImageControl RS` CLAP.
//! No installed-plugin discovery, physical audio devices, screenshots, or synthetic editor models are used.

use std::path::PathBuf;

use anyhow::{Context as _, Result, bail};

#[cfg(all(target_os = "macos", feature = "clap", feature = "editor"))]
#[path = "support/clap_editor_native.rs"]
mod native;

#[cfg(all(windows, feature = "clap", feature = "editor"))]
#[path = "support/clap_editor_windows.rs"]
mod native;

/// Parses an explicit package selection before loading a plugin or initializing `AppKit`.
fn arguments() -> Result<Option<PathBuf>> {
    let mut arguments = std::env::args_os().skip(1);
    let mut bundle = None;
    while let Some(argument) = arguments.next() {
        match argument.to_str() {
            Some("-h" | "--help") => {
                println!(
                    "Usage: clap_editor -b|--bundle <absolute-build-tree.clap>\n\
Runs the actual packaged CLAP Iced editor on its native owner thread.\n\
IMAGE_CONTROL_CLAP_BUNDLE supplies the explicit package when --bundle is absent.\n\
Checks create, size, parent, show, hide, show, resize, destroy, and two reopens.\n\
No installation, audio hardware, screenshots, or installed-plugin discovery."
                );
                return Ok(None);
            }
            Some("-b" | "--bundle") => {
                bundle = Some(PathBuf::from(arguments.next().context("--bundle needs a path")?));
            }
            _ => bail!("unknown argument: {}", argument.to_string_lossy()),
        }
    }
    let bundle = bundle
        .or_else(|| std::env::var_os("IMAGE_CONTROL_CLAP_BUNDLE").map(PathBuf::from))
        .context("give -b/--bundle <absolute-build-tree.clap> or set IMAGE_CONTROL_CLAP_BUNDLE")?;
    Ok(Some(bundle))
}

/// Keeps every native GUI call on the executable's main thread and fails closed on unsupported configurations.
fn main() -> Result<()> {
    let Some(bundle) = arguments()? else {
        return Ok(());
    };
    #[cfg(all(any(target_os = "macos", windows), feature = "clap", feature = "editor"))]
    return native::run(&bundle);
    #[cfg(not(all(any(target_os = "macos", windows), feature = "clap", feature = "editor")))]
    {
        let _ = bundle;
        bail!("clap_editor requires macOS or Windows with the clap and editor features")
    }
}
