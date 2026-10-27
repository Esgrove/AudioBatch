//! Text the plugin windows and menus show: plugin and slot labels, latencies, the chain summary,
//! and the known effects grouped by vendor.

use audiobatch_core::sorting::compare_natural;
use audiobatch_plugins::{FORMAT_NAME, PluginDescription};

/// Vendor group of the plugins that report no vendor.
pub const UNKNOWN_VENDOR: &str = "Unknown Vendor";

/// The effects of one vendor, for the Add Plugin menu and the plugin lists.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VendorGroup {
    /// Vendor name, or `UNKNOWN_VENDOR`.
    pub vendor: String,
    /// The vendor's effects, in natural order of their names.
    pub plugins: Vec<PluginDescription>,
}

/// Returns the label of a plugin with its vendor and format, for example "Vendor Delay (VST3)".
#[must_use]
pub fn plugin_label(plugin: &PluginDescription) -> String {
    if plugin.vendor.is_empty() {
        format!("{} ({FORMAT_NAME})", plugin.name)
    } else {
        format!("{} {} ({FORMAT_NAME})", plugin.vendor, plugin.name)
    }
}

/// Returns the label of a chain slot with its one-based position, for example "1. Vendor Delay (VST3)".
#[must_use]
pub fn slot_label(position: usize, plugin: &PluginDescription) -> String {
    format!("{position}. {}", plugin_label(plugin))
}

/// Returns a latency in samples with its duration at a sample rate, for example "512 samples (10.7 ms)".
/// No latency is shown without a duration.
#[must_use]
pub fn latency_text(samples: u64, sample_rate: u32) -> String {
    if samples == 0 {
        return String::from("No latency");
    }
    let unit = if samples == 1 { "sample" } else { "samples" };
    if sample_rate > 0 {
        #[expect(
            clippy::cast_precision_loss,
            reason = "a latency is far below the f64 precision limit"
        )]
        let milliseconds = samples as f64 * 1000.0 / f64::from(sample_rate);
        format!("{samples} {unit} ({milliseconds:.1} ms)")
    } else {
        format!("{samples} {unit}")
    }
}

/// Returns the summary of a chain from its slot counts, for example "3 plugins, 1 bypassed".
#[must_use]
pub fn chain_summary(enabled: usize, total: usize) -> String {
    let plugins = if total == 1 { "plugin" } else { "plugins" };
    match (total, total.saturating_sub(enabled)) {
        (0, _) => String::from("Empty"),
        (_, 0) => format!("{total} {plugins}"),
        (_, bypassed) => format!("{total} {plugins}, {bypassed} bypassed"),
    }
}

/// Groups the effects among the plugins by vendor, in natural order of the vendors and of the names.
/// Instruments are left out, because the chain only processes audio.
#[must_use]
pub fn group_by_vendor(plugins: &[PluginDescription]) -> Vec<VendorGroup> {
    let mut effects: Vec<&PluginDescription> = plugins.iter().filter(|plugin| !plugin.is_instrument).collect();
    effects.sort_by(|first, second| {
        compare_natural(vendor_name(first), vendor_name(second))
            .then_with(|| compare_natural(&first.name, &second.name))
    });
    let mut groups: Vec<VendorGroup> = Vec::new();
    for plugin in effects {
        let vendor = vendor_name(plugin);
        match groups.last_mut() {
            Some(group) if group.vendor == vendor => group.plugins.push(plugin.clone()),
            _ => groups.push(VendorGroup {
                vendor: vendor.to_string(),
                plugins: vec![plugin.clone()],
            }),
        }
    }
    groups
}

/// Returns the vendor a plugin is grouped under.
fn vendor_name(plugin: &PluginDescription) -> &str {
    if plugin.vendor.is_empty() {
        UNKNOWN_VENDOR
    } else {
        &plugin.vendor
    }
}

#[cfg(test)]
mod test_labels {
    use super::*;
    use crate::test_helpers::effect as plugin;

    #[test]
    fn labels_name_the_vendor_and_format() {
        assert_eq!(plugin_label(&plugin("Acme", "Delay")), "Acme Delay (VST3)");
        assert_eq!(plugin_label(&plugin("", "Delay")), "Delay (VST3)");
        assert_eq!(slot_label(2, &plugin("Acme", "Delay")), "2. Acme Delay (VST3)");
    }

    #[test]
    fn latencies_show_their_duration() {
        assert_eq!(latency_text(0, 48_000), "No latency");
        assert_eq!(latency_text(1, 48_000), "1 sample (0.0 ms)");
        assert_eq!(latency_text(480, 48_000), "480 samples (10.0 ms)");
        assert_eq!(latency_text(480, 0), "480 samples");
    }

    #[test]
    fn the_summary_counts_bypassed_slots() {
        assert_eq!(chain_summary(0, 0), "Empty");
        assert_eq!(chain_summary(1, 1), "1 plugin");
        assert_eq!(chain_summary(2, 3), "3 plugins, 1 bypassed");
    }

    #[test]
    fn effects_are_grouped_by_vendor_in_natural_order() {
        let mut instrument = plugin("Acme", "Synth");
        instrument.is_instrument = true;
        let plugins = vec![
            plugin("acme", "Delay 10"),
            plugin("", "Mystery"),
            plugin("Beta", "Reverb"),
            plugin("acme", "Delay 2"),
            instrument,
        ];
        let groups = group_by_vendor(&plugins);
        let summary: Vec<(&str, Vec<&str>)> = groups
            .iter()
            .map(|group| {
                let names = group.plugins.iter().map(|plugin| plugin.name.as_str()).collect();
                (group.vendor.as_str(), names)
            })
            .collect();
        assert_eq!(
            summary,
            vec![
                ("acme", vec!["Delay 2", "Delay 10"]),
                ("Beta", vec!["Reverb"]),
                (UNKNOWN_VENDOR, vec!["Mystery"]),
            ]
        );
    }
}
