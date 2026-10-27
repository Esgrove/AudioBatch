//! Hosting VST3 audio effect plugins.
//!
//! This crate loads VST3 modules, lists their classes, creates plugin instances,
//! and runs their audio processing on preallocated buffers.
//! It wraps the raw `vst3` bindings in a safe interface, so the crates above it never touch COM pointers.
//!
//! VST3 requires that modules are loaded, and instances created, set up, and destroyed, on the thread
//! that runs the application's event loop.
//! The `UiThread` token proves that a call happens on that thread,
//! and `UiDispatcher` runs work there from other threads.
//! Audio processing itself runs on any thread through a `ProcessorHandle`.

pub mod discovery;
mod enums;
pub mod factory;
mod host_objects;
pub mod instance;
pub mod module;
pub mod module_info;
pub mod platform_events;
pub mod processor;
pub(crate) mod strings;
pub mod thread;

#[cfg(test)]
mod test_helpers;

pub use discovery::{default_search_folders, find_bundles, is_bundle_path};
pub use factory::{ClassId, ClassInfo, FactoryInfo};
pub use host_objects::{ParameterEdit, RestartFlags};
pub use instance::{BusLayout, PluginInstance, PluginState, ProcessMode};
pub use module::Module;
pub use module_info::read_module_info;
pub use platform_events::pump_platform_events;
pub use processor::{ProcessError, ProcessorHandle};
pub use thread::{CallError, UiDispatcher, UiJob, UiJobs, UiThread, ui_channel};
