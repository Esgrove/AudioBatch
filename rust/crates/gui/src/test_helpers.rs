//! Builders shared by the unit tests of several modules.

use std::path::PathBuf;
use std::sync::OnceLock;

use audiobatch_plugins::PluginDescription;
use vst3_host::{ClassId, UiDispatcher, UiThread};

/// Shares the one legal VST3 lifecycle thread between unit tests and nonvisual GUI workflows.
pub fn plugin_dispatcher() -> UiDispatcher {
    static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();
    DISPATCHER
        .get_or_init(|| UiThread::spawn_dedicated().expect("test VST3 UI thread"))
        .clone()
}

/// Returns a stereo effect of a vendor, in a bundle named after it.
pub fn effect(vendor: &str, name: &str) -> PluginDescription {
    PluginDescription {
        name: name.to_string(),
        vendor: vendor.to_string(),
        version: String::from("1.0"),
        category: String::from("Fx"),
        class_id: ClassId([1; 16]),
        sdk_version: String::new(),
        bundle: PathBuf::from(format!("/plugins/{name}.vst3")),
        bundle_modified_ms: 0,
        is_instrument: false,
        main_input_channels: Some(2),
        main_output_channels: Some(2),
    }
}
