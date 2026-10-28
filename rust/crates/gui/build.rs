//! Build script of the `AudioBatchRs` app.
//!
//! It records the version, build time, and git commit that the About dialog and the `--version` flag show,
//! see the `build-info` crate.
//!
//! On Windows it embeds the app icon from the root `assets` directory as icon resource 1,
//! which GPUI loads for the window and the taskbar, and which Explorer shows for the executable.
//! It also fills in the version information that Explorer and Task Manager show.
//! macOS takes the icon from the app bundle that `build.sh` creates, so the script embeds nothing there.

#[cfg(windows)]
use std::path::Path;
use std::path::PathBuf;

/// The multi-size Windows icon, generated from `assets/icon_win.png`, relative to the crate directory.
const WINDOWS_ICON: &str = "../../../assets/icon_win.ico";

/// Records the build metadata, and embeds the Windows resources, rerunning when the icon changes.
#[cfg_attr(
    not(windows),
    expect(clippy::unnecessary_wraps, reason = "only Windows embeds resources")
)]
fn main() -> std::io::Result<()> {
    build_info::emit();
    let manifest_directory = std::env::var_os("CARGO_MANIFEST_DIR").map_or_else(PathBuf::new, PathBuf::from);
    let icon = manifest_directory.join(WINDOWS_ICON);
    println!("cargo::rerun-if-changed={}", icon.display());
    #[cfg(windows)]
    embed_windows_resources(&icon)?;
    Ok(())
}

/// Compiles the icon and the version information into a resource that is linked into the executable.
#[cfg(windows)]
fn embed_windows_resources(icon: &Path) -> std::io::Result<()> {
    let icon = icon
        .to_str()
        .ok_or_else(|| std::io::Error::other("The icon path is not valid UTF-8"))?;
    winresource::WindowsResource::new()
        .set_icon(icon)
        .set("FileDescription", "AudioBatchRs")
        .set("ProductName", "AudioBatchRs")
        .set("InternalName", "AudioBatchRs")
        .set("OriginalFilename", "AudioBatchRs.exe")
        .set("CompanyName", "Esgrove")
        .compile()
}
