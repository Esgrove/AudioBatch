//! Bounded hardware-free native Iced editor close/reopen run against the actual build-tree VST3.

#![cfg_attr(audiobatch_rtsan, feature(sanitize))]

#[path = "support/packaged_host.rs"]
mod packaged_host;

/// Defaults to the two-instance editor lifecycle, retaining `AppKit` ownership on the macOS main thread.
fn main() -> anyhow::Result<()> {
    if let Some(options) = packaged_host::arguments(Some(packaged_host::Case::NativeEditorLifecycle))? {
        packaged_host::run(&options)?;
    }
    Ok(())
}
