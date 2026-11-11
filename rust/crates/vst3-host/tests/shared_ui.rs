//! Public compatibility contracts for the shared plugin UI thread types.

use plugin_host_core::{UiBound, UiThread};
use vst3_host::thread::{UiDispatcher, UiJobs};

/// Accepts the shared token through the historical VST3 module path without conversion.
fn accepts_vst3_token(ui: &vst3_host::thread::UiThread) -> bool {
    let _: &vst3_host::UiThread = ui;
    UiThread::is_current()
}

#[test]
fn shared_and_vst3_paths_use_one_token_queue_and_bound_value() {
    let dispatcher = UiThread::spawn_dedicated().expect("register shared UI thread");
    let historical: UiDispatcher = dispatcher.clone();
    let matches = historical
        .call(
            move |ui| {
                let bound: vst3_host::ui_bound::UiBound<i32> = UiBound::new(ui, dispatcher, 42);
                accepts_vst3_token(ui) && *bound.get(ui) == 42
            },
            &|| false,
        )
        .expect("shared UI thread runs VST3 job");
    assert!(matches);
    let (_dispatcher, jobs): (plugin_host_core::UiDispatcher, UiJobs) = vst3_host::ui_channel();
    drop(jobs);
}
