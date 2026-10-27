//! The COM objects the host gives to plugins.
//!
//! `HostApplication` is the context plugins are initialized with, and it creates messages and attribute lists.
//! `MemoryStream` carries plugin state, `ComponentHandler` receives parameter edits and restart requests,
//! and `ParameterChanges` delivers parameter values to `process`.
//! None of them lock or allocate in the calls a plugin makes from its processing thread.

mod application;
mod attributes;
mod component_handler;
mod parameter_changes;
mod stream;

pub use application::HostApplication;
pub use component_handler::{ComponentHandler, EDIT_QUEUE_CAPACITY};
pub use component_handler::{ParameterEdit, RestartFlags};
pub use parameter_changes::ParameterChanges;
pub use stream::MemoryStream;
