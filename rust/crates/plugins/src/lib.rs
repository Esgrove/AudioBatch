//! Plugin scanning, mixed-format chains, offline processing, and live preview for `AudioBatch`.
//!
//! Native VST3 and CLAP hosting lives in the format host crates, so this crate contains no unsafe code.
//! Scans load plugins only in a separate worker process, so a crashing plugin cannot take the application down.
//! Fallible control APIs expose domain errors with concrete causes.
//! Scan replies, saved-data recovery warnings, and operation results convert errors to text at their reporting boundary.
//! The live audio callback only records the host's allocation-free process error for the UI thread to report.

#![cfg_attr(audiobatch_rtsan, feature(sanitize))]

#[doc(hidden)]
pub mod benchmark;
pub mod chain;
mod chain_signal;
pub mod host;
pub mod identity;
pub mod library;
pub mod presets;
pub mod preview_chain;
pub mod processing;
pub mod processing_chain;
pub mod processing_coordinator;
mod scan_protocol;
pub mod scan_worker;
pub mod scanner;

#[cfg(test)]
mod test_helpers;

pub use chain::{ChainSlot, PluginChain};
pub use clap_host::service_main_thread as service_plugin_callbacks;
#[cfg(feature = "editor")]
pub use host::{HostEditor, HostEditorError};
pub use host::{HostError, HostInstance, HostProcessError, HostProcessor, HostState};
pub use identity::{PluginFormat, PluginId};
pub use library::{
    KnownPlugins, PluginDataError, PluginDataPaths, PluginDescription, RecoveredData, VersionedJsonError,
};
pub use presets::{ChainPreset, PresetError, PresetInfo, PresetStore, sanitize_preset_name};
pub use preview_chain::{
    PreviewChain, PreviewChainError, PreviewFailure, chain_tail_frames, prepare_for_preview, reload_for_preview,
};
pub use processing::{ProcessingOptions, process_file};
pub use processing_chain::{ChainPlugin, ProcessingChain, ProcessingChainError};
pub use processing_coordinator::{
    ProcessingCoordinator, ProcessingCoordinatorError, ProcessingEvent, ProcessingRun, default_worker_count,
};
pub use scan_protocol::WORKER_FLAG;
pub use scan_worker::{is_scan_worker_invocation, run_scan_worker};
pub use scanner::{
    SCAN_BUSY_MESSAGE, SCAN_TIMEOUT, ScanError, ScanLock, ScanOptions, ScanProgress, ScanReport, WorkerCommand,
    scan_plugins,
};
