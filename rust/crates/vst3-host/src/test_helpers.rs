//! Shared helpers for the unit tests.
//!
//! Only one thread per process can be the UI thread, and test threads come and go,
//! so the tests share one dedicated UI thread that runs for the rest of the test process.
//! Jobs on it must not panic, because a panic would end the thread for every later test,
//! so they return their results and the tests assert on the test thread.

use std::sync::{Arc, Mutex, MutexGuard, OnceLock, PoisonError};

use anyhow::Result;
use audiobatch_test_plugin::Kind;

use crate::factory::ClassId;
use crate::instance::{PluginInstance, ProcessMode};
use crate::module::Module;
use crate::thread::{UiDispatcher, UiThread};

/// Sample rate the tests process at.
pub const SAMPLE_RATE: f64 = 48_000.0;

/// Largest block the tests process.
pub const MAX_BLOCK: usize = 256;

/// The dispatcher of the shared test UI thread.
static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

/// Held by tests that read the call log of the recorder plugins,
/// which is shared by every test thread when the tests run in one process.
static RECORDER_LOG: Mutex<()> = Mutex::new(());

/// Returns exclusive use of the recorder call log until the guard is dropped.
pub fn lock_recorder_log() -> MutexGuard<'static, ()> {
    RECORDER_LOG.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Returns the dispatcher of the shared test UI thread, starting the thread on first use.
pub fn dispatcher() -> &'static UiDispatcher {
    DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("no other UI thread in the test process"))
}

/// Runs `work` on the shared test UI thread and returns its result.
pub fn on_ui<R: Send + 'static>(work: impl FnOnce(&UiThread) -> R + Send + 'static) -> R {
    dispatcher()
        .call(work, &|| false)
        .expect("the test UI thread runs the job")
}

/// Returns a module around the in-process factory of the fake plugins.
pub fn test_module(ui: &UiThread) -> Arc<Module> {
    Module::from_factory(ui, "test plugins", audiobatch_test_plugin::factory())
}

/// Returns the class id of a fake plugin.
pub fn class_id(kind: Kind) -> ClassId {
    ClassId::from_tuid(&kind.class_id())
}

/// Creates an instance of a fake plugin, configured for `channels` channels, set up and activated.
pub fn active_instance(ui: &UiThread, kind: Kind, channels: usize) -> Result<PluginInstance> {
    let module = test_module(ui);
    let mut instance = PluginInstance::create(ui, &module, class_id(kind), Some(dispatcher().clone()))?;
    instance.configure_main_buses(ui, channels)?;
    instance.setup(ui, ProcessMode::Offline, SAMPLE_RATE, MAX_BLOCK)?;
    instance.activate(ui)?;
    Ok(instance)
}
