//! Shared core of `AudioBatch`, used by both the CLI and the GUI.
//!
//! Decodes audio files with symphonia, measures true peak with SIMD and EBU R128 loudness with ebur128,
//! builds the waveform overview and source overload history in the same decode pass, and caches the results in SQLite.
//! Normalizes analyzed files to a peak or loudness target as AIFF, carrying their tags and Serato data over.
//! `OperationResult` reports what normalization and plugin processing did to each file.

pub mod aiff;
pub mod analysis;
pub mod atomic_write;
pub mod cache;
pub mod coordinator;
pub mod decode;
pub mod discovery;
pub mod format;
#[cfg(test)]
mod golden;
#[cfg(test)]
mod golden_tests;
pub mod log_file;
#[doc(hidden)]
pub mod loudness;
pub mod metadata;
#[doc(hidden)]
pub mod meter;
#[cfg(test)]
mod meter_overload_tests;
pub mod normalize;
pub mod normalize_coordinator;
pub mod operation;
pub mod output_file;
pub mod overload;
pub mod record;
#[doc(hidden)]
pub mod signals;
pub mod sorting;
pub mod tags;
#[cfg(test)]
mod test_helpers;
pub mod waveform;

pub use analysis::analyze_file;
pub use cache::AnalysisCache;
pub use coordinator::{AnalysisCoordinator, AnalysisEvent};
pub use normalize_coordinator::{NormalizeCoordinator, NormalizeEvent};
pub use operation::{OperationOutcome, OperationResult};
pub use record::{AnalysisOptions, AnalysisRecord, AnalysisStatus, ShortTermLoudnessHistory};
pub use serato_tags;
pub use sorting::SortMode;
pub use tags::{FileTags, SeratoOverviewState, read_tags};
pub use waveform::WaveformOverview;
