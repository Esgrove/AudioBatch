//! Process-isolated direct Clack host for packaged CLAP callback contracts and positive `RTSan` detector controls.

#![cfg_attr(audiobatch_rtsan, feature(sanitize))]

#[cfg(feature = "clap")]
#[path = "support/clap_host.rs"]
mod host;
#[cfg(feature = "clap")]
#[path = "support/clap_scenarios.rs"]
mod scenarios;

/// Runs one bounded named scenario without discovering installed plugins or opening physical audio devices.
fn main() -> anyhow::Result<()> {
    #[cfg(feature = "clap")]
    {
        scenarios::run()
    }
    #[cfg(not(feature = "clap"))]
    {
        anyhow::bail!("clap_host requires the clap feature")
    }
}
