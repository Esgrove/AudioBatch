//! Format-neutral UI thread dispatch and native windows for audio plugin hosts.
//!
//! Plugin lifecycle calls share one registered UI thread across formats.
//! The `editor` feature adds native editor windows without depending on a plugin SDK or GUI framework.

#[cfg(feature = "editor")]
pub mod editor;
pub mod platform_events;
pub mod thread;
pub mod ui_bound;

#[cfg(test)]
mod test_helpers;

#[cfg(feature = "editor")]
pub use editor::{EditorCallbacks, NativeWindow, WindowError, WindowPosition, WindowSize};
pub use platform_events::pump_platform_events;
pub use thread::{CallError, UiDispatcher, UiJob, UiJobs, UiThread, UiThreadError, ui_channel};
pub use ui_bound::UiBound;
