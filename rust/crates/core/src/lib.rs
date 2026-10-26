//! Shared core of the `AudioBatch` Rust port, used by both the CLI and the GUI.
//!
//! Decodes audio files with symphonia, measures sample peak, true peak, and EBU R128 loudness with ebur128,
//! builds the waveform overview in the same decode pass, and caches the results in SQLite.
//! Normalizes analyzed files to a peak or loudness target as AIFF, carrying their tags and Serato data over.

pub mod aiff;
pub mod analysis;
pub mod cache;
pub mod coordinator;
pub mod decode;
pub mod discovery;
pub mod format;
pub mod metadata;
pub mod normalize;
pub mod output_file;
pub mod record;
pub mod sorting;
pub mod tags;
pub mod waveform;

pub use analysis::analyze_file;
pub use cache::AnalysisCache;
pub use coordinator::{AnalysisCoordinator, AnalysisEvent};
pub use record::{AnalysisOptions, AnalysisRecord, AnalysisStatus, ShortTermLoudnessHistory};
pub use serato_tags;
pub use sorting::SortMode;
pub use tags::{FileTags, read_tags};
pub use waveform::WaveformOverview;
