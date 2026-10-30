//! The VST3 plugin features of `AudioBatch`: scanning, plugin chains, offline processing, and the live preview chain.
//!
//! Plugin hosting itself is in the `vst3-host` crate, so this crate contains no unsafe code.
//! Scans load plugins only in a separate worker process, so a crashing plugin cannot take the application down.
//! Fallible control APIs expose domain errors with concrete causes.
//! Scan replies, saved-data recovery warnings, and operation results convert errors to text at their reporting boundary.
//! The live audio callback only records the host's allocation-free process error for the UI thread to report.

pub mod chain;
mod chain_signal;
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
pub use library::{FORMAT_NAME, KnownPlugins, PluginDataError, PluginDataPaths, PluginDescription, VersionedJsonError};
pub use presets::{ChainPreset, PresetError, PresetInfo, PresetStore, sanitize_preset_name};
pub use preview_chain::{
    PreviewChain, PreviewChainError, PreviewFailure, chain_tail_frames, prepare_for_preview, reload_for_preview,
};
pub use processing::{ProcessingOptions, process_file};
pub use processing_chain::{ChainPlugin, ProcessingChain, ProcessingChainError};
pub use processing_coordinator::{
    ProcessingCoordinator, ProcessingCoordinatorError, ProcessingEvent, default_worker_count,
};
pub use scan_protocol::WORKER_FLAG;
pub use scan_worker::{is_scan_worker_invocation, run_scan_worker};
pub use scanner::{
    SCAN_BUSY_MESSAGE, SCAN_TIMEOUT, ScanError, ScanLock, ScanOptions, ScanProgress, ScanReport, WorkerCommand,
    scan_plugins,
};
