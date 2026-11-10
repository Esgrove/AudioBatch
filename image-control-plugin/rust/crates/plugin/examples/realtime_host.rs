//! Process-isolated `RTSan` host for the actual build-tree VST3, without audio hardware or installed plugins.

#![cfg_attr(audiobatch_rtsan, feature(sanitize))]

#[path = "support/packaged_host.rs"]
mod packaged_host;

/// Keeps native module and editor ownership on the executable's main thread.
fn main() -> anyhow::Result<()> {
    if let Some(options) = packaged_host::arguments(None)? {
        packaged_host::run(&options)?;
    }
    Ok(())
}
