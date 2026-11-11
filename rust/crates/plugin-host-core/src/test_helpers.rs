//! A shared dedicated UI thread for the format-neutral host tests.

use std::sync::OnceLock;

use crate::{UiDispatcher, UiThread};

/// Dispatcher of the UI thread shared by tests in this process.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Returns the dispatcher, starting the UI thread on first use.
pub fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("no other UI thread in the test process"))
}
