//! The plugin instances one processing worker renders files with.
//!
//! VST3 restricts bus arrangement, `setupProcessing`, `setActive`, and `setState` to the UI thread,
//! so a worker hands its chain to the UI thread to prepare it for each file and to release it afterwards,
//! and only runs the returned processor handles itself.
//! The instances live in a `UiBound`, so the worker can own the chain without ever touching them.
//! Every UI thread call takes the worker's cancellation check,
//! so a worker never blocks on a UI thread that is waiting for the workers to finish.

use std::cell::RefCell;
use std::sync::Arc;

use anyhow::Result;
use vst3_host::{
    CallError, Module, PluginInstance, PluginState, ProcessMode, ProcessorHandle, UiBound, UiDispatcher, UiThread,
};

use crate::chain::{ChainSlot, PluginChain};

/// Frames per block the chain renders.
/// Every block has this length, a short final block is padded with silence, because many plugins expect that.
pub(crate) const PROCESSING_BLOCK_FRAMES: usize = 1024;

/// Longest tail rendered per plugin, so an infinite or very long tail cannot keep a render going.
pub(crate) const MAXIMUM_TAIL_SECONDS: f64 = 20.0;

/// User-facing message of a cancelled processing call.
pub const CANCELLED_MESSAGE: &str = "Processing was cancelled";

/// One plugin of a processing chain, with the settings it is restored to for every file.
pub struct ChainPlugin {
    /// Display name, for error messages.
    name: String,
    /// The instance.
    instance: PluginInstance,
    /// The settings restored before each file, so every file starts from the same state.
    state: PluginState,
}

/// The plugin instances one worker processes files with, in processing order.
#[derive(Clone)]
pub struct ProcessingChain {
    /// The plugins, reached only on the UI thread.
    plugins: Arc<UiBound<RefCell<Vec<ChainPlugin>>>>,
    /// Number of plugins, readable on any thread.
    plugin_count: usize,
}

/// A chain prepared for one file.
pub(crate) struct PreparedChain {
    /// One processor handle per plugin, in processing order.
    pub(crate) processors: Vec<ProcessorHandle>,
    /// Sum of the plugins' latencies, in samples.
    pub(crate) latency_samples: u64,
    /// Sum of the plugins' tails, each limited to `MAXIMUM_TAIL_SECONDS`, in samples.
    pub(crate) tail_samples: u64,
    /// Display names of the plugins, in processing order, for a failure message.
    pub(crate) names: Vec<String>,
}

impl ChainPlugin {
    /// Wraps an instance and the settings it is restored to.
    #[must_use]
    pub const fn new(name: String, instance: PluginInstance, state: PluginState) -> Self {
        Self { name, instance, state }
    }

    /// Loads the plugin of a slot and creates an instance with the slot's settings.
    /// A slot without saved settings gets the defaults of the new instance,
    /// because restoring an empty state would leave each file with whatever the previous file left behind.
    ///
    /// # Errors
    ///
    /// Returns an error when the bundle cannot be loaded or the instance cannot be created.
    pub(crate) fn from_slot(ui: &UiThread, dispatcher: &UiDispatcher, slot: &ChainSlot) -> Result<Self> {
        let module = Module::load(ui, &slot.plugin.bundle)?;
        let instance = PluginInstance::create(ui, &module, slot.plugin.class_id, Some(dispatcher.clone()))?;
        let saved = slot.state();
        let state = if saved == PluginState::default() {
            instance.state(ui).unwrap_or(saved)
        } else {
            saved
        };
        Ok(Self::new(slot.plugin.name.clone(), instance, state))
    }

    /// Returns the display name.
    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Restores the settings, configures the buses for the file, sets up offline processing, and activates.
    fn prepare(&mut self, ui: &UiThread, channels: usize, sample_rate: f64) -> Result<(), String> {
        let name = &self.name;
        self.instance
            .deactivate(ui)
            .map_err(|error| format!("Plugin \"{name}\" could not be stopped: {error}"))?;
        self.instance
            .set_state(ui, &self.state)
            .map_err(|error| format!("Plugin \"{name}\" could not restore its settings: {error}"))?;
        self.instance.configure_main_buses(ui, channels).map_err(|_| {
            format!("Plugin \"{name}\" does not support the file's channel layout ({channels} channels)")
        })?;
        self.instance
            .setup(ui, ProcessMode::Offline, sample_rate, PROCESSING_BLOCK_FRAMES)
            .map_err(|error| format!("Plugin \"{name}\" could not be prepared: {error}"))?;
        self.instance
            .activate(ui)
            .map_err(|error| format!("Plugin \"{name}\" could not be prepared: {error}"))
    }
}

impl ProcessingChain {
    /// Wraps plugin instances created on the UI thread.
    #[must_use]
    pub fn new(ui: &UiThread, dispatcher: UiDispatcher, plugins: Vec<ChainPlugin>) -> Self {
        let plugin_count = plugins.len();
        Self {
            plugins: Arc::new(UiBound::new(ui, dispatcher, RefCell::new(plugins))),
            plugin_count,
        }
    }

    /// Creates instances of the enabled plugins of a saved chain.
    ///
    /// # Errors
    ///
    /// Returns the user-facing error of the first plugin that cannot be loaded.
    pub fn instantiate(ui: &UiThread, dispatcher: &UiDispatcher, chain: &PluginChain) -> Result<Self, String> {
        let plugins = chain
            .enabled_slots()
            .map(|slot| {
                ChainPlugin::from_slot(ui, dispatcher, slot)
                    .map_err(|error| format!("Could not load plugin \"{}\": {error}", slot.plugin.name))
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Self::new(ui, dispatcher.clone(), plugins))
    }

    /// Returns the number of plugins.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.plugin_count
    }

    /// Returns true when the chain has no plugins.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.plugin_count == 0
    }

    /// Prepares every plugin for a file on the UI thread and returns their processor handles,
    /// the chain latency, and the chain tail.
    /// A plugin that fails releases the plugins prepared before it, so the chain stays usable for the next file.
    pub(crate) fn prepare(
        &self,
        channels: usize,
        sample_rate: f64,
        is_cancelled: &dyn Fn() -> bool,
    ) -> Result<PreparedChain, String> {
        let plugins = Arc::clone(&self.plugins);
        self.call_on_ui(
            move |ui| {
                let Ok(mut plugins) = plugins.get(ui).try_borrow_mut() else {
                    return Err("The plugin chain is already in use".to_string());
                };
                prepare_plugins(ui, &mut plugins, channels, sample_rate)
            },
            is_cancelled,
        )?
    }

    /// Deactivates every plugin on the UI thread, after the caller dropped the processor handles.
    /// A cancelled call leaves the plugins active, and the next `prepare` or the teardown deactivates them.
    pub(crate) fn release(&self, is_cancelled: &dyn Fn() -> bool) {
        let plugins = Arc::clone(&self.plugins);
        let _ = self.call_on_ui(
            move |ui| {
                if let Ok(mut plugins) = plugins.get(ui).try_borrow_mut() {
                    release_plugins(ui, &mut plugins);
                }
            },
            is_cancelled,
        );
    }

    /// Runs work on the UI thread and returns its result, or the cancellation message.
    fn call_on_ui<R: Send + 'static>(
        &self,
        work: impl FnOnce(&UiThread) -> R + Send + 'static,
        is_cancelled: &dyn Fn() -> bool,
    ) -> Result<R, String> {
        if let Some(ui) = UiThread::current() {
            return Ok(work(&ui));
        }
        self.plugins
            .dispatcher()
            .call(work, is_cancelled)
            .map_err(|error| match error {
                CallError::Cancelled => CANCELLED_MESSAGE.to_string(),
                CallError::Closed => "The application is shutting down".to_string(),
            })
    }
}

/// Prepares the plugins in order and collects their processor handles, latency, and tail.
fn prepare_plugins(
    ui: &UiThread,
    plugins: &mut [ChainPlugin],
    channels: usize,
    sample_rate: f64,
) -> Result<PreparedChain, String> {
    let mut prepared = PreparedChain {
        processors: Vec::with_capacity(plugins.len()),
        latency_samples: 0,
        tail_samples: 0,
        names: plugins.iter().map(|plugin| plugin.name.clone()).collect(),
    };
    let mut failure = None;
    for plugin in plugins.iter_mut() {
        if let Err(error) = plugin.prepare(ui, channels, sample_rate) {
            failure = Some(error);
            break;
        }
        match plugin.instance.processor(ui) {
            Ok(processor) => prepared.processors.push(processor),
            Err(error) => {
                failure = Some(format!("Plugin \"{}\" could not be prepared: {error}", plugin.name));
                break;
            }
        }
        // Every stage delays the signal it passes on, and each stage's tail rings through the stages after it,
        // so both add up along the chain.
        prepared.latency_samples += u64::from(plugin.instance.latency_samples(ui));
        prepared.tail_samples += limited_tail(plugin.instance.tail_samples(ui), sample_rate);
    }
    if let Some(error) = failure {
        drop(prepared);
        release_plugins(ui, plugins);
        return Err(error);
    }
    Ok(prepared)
}

/// Deactivates every plugin. A plugin that cannot be deactivated yet is deactivated by the next preparation.
fn release_plugins(ui: &UiThread, plugins: &mut [ChainPlugin]) {
    for plugin in plugins {
        let _ = plugin.instance.deactivate(ui);
    }
}

/// Limits a plugin's reported tail to `MAXIMUM_TAIL_SECONDS`, which also bounds the infinite tail `u32::MAX`.
pub(crate) fn limited_tail(tail_samples: u32, sample_rate: f64) -> u64 {
    let maximum = (MAXIMUM_TAIL_SECONDS * sample_rate.max(0.0)).ceil() as u64;
    u64::from(tail_samples).min(maximum)
}

#[cfg(test)]
mod test_limited_tail {
    use super::*;

    #[test]
    fn short_tails_are_kept_and_long_or_infinite_tails_are_limited() {
        assert_eq!(limited_tail(0, 48_000.0), 0);
        assert_eq!(limited_tail(100, 48_000.0), 100);
        assert_eq!(limited_tail(u32::MAX, 48_000.0), 960_000);
        assert_eq!(limited_tail(u32::MAX, 44_100.5), 882_010);
    }
}
