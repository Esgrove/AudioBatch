//! The VST3 plugin features of `AudioBatch`: scanning, plugin chains, offline processing, and the live preview chain.
//!
//! Plugin hosting itself is in the `vst3-host` crate, so this crate contains no unsafe code.
//! Scans load plugins only in a separate worker process, so a crashing plugin cannot take the application down.

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
pub use library::{FORMAT_NAME, KnownPlugins, PluginDataPaths, PluginDescription};
pub use presets::{
    ChainPreset, PRESET_FORMAT_VERSION, PresetInfo, PresetStore, load_preset_file, sanitize_preset_name,
};
pub use preview_chain::{PreviewChain, PreviewFailure, chain_tail_frames};
pub use processing::{ProcessingOptions, process_file, processing_output_path};
pub use processing_chain::{ChainPlugin, MAXIMUM_TAIL_SECONDS, PROCESSING_BLOCK_FRAMES, ProcessingChain};
pub use processing_coordinator::{ProcessingCoordinator, ProcessingEvent, default_worker_count};
pub use scan_protocol::WORKER_FLAG;
pub use scan_worker::{is_scan_worker_invocation, run_scan_worker};
pub use scanner::{
    SCAN_BUSY_MESSAGE, SCAN_TIMEOUT, ScanLock, ScanOptions, ScanProgress, ScanReport, WorkerCommand, scan_plugins,
};
