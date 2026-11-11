//! Safe CLAP effect hosting with UI-owned lifecycle and preallocated audio processing.
//!
//! Call `service_main_thread` regularly from the application's plugin tick and worker event loop.
//! Plugin requests only publish atomics, and never wake the event loop from an audio callback.

pub mod discovery;
#[cfg(feature = "editor")]
pub mod editor;
mod host;
#[cfg(test)]
mod host_tests;
mod instance;
mod module;
mod processor;

#[cfg(feature = "editor")]
pub use editor::{EditorError, EditorWindow, WindowPosition};
pub use host::{RestartAction, RestartFlags};
pub use instance::{BusLayout, InstanceError, PluginInstance, ProcessMode, service_main_thread};
pub use module::{ClassInfo, Module, ModuleError};
pub use plugin_host_core::pump_platform_events;
pub use plugin_host_core::{CallError, UiBound, UiDispatcher, UiJob, UiJobs, UiThread, UiThreadError, ui_channel};
pub use processor::{ProcessError, ProcessorHandle};
