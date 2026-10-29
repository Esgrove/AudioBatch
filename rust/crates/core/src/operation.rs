//! The result of an operation that rewrites one file, such as normalization or plugin processing.
//!
//! Both write a new output, commit it in place of the source, and analyze it,
//! so the front ends apply their results the same way.

use std::path::{Path, PathBuf};

use crate::cache::AnalysisCache;
use crate::format::format_error_chain;
use crate::record::AnalysisRecord;

/// What an operation on one file produced.
#[derive(Debug, Clone)]
pub enum OperationOutcome {
    /// Nothing was changed on disk.
    Failed {
        /// User-facing reason.
        error: String,
    },
    /// The output is in place, but analyzing it failed.
    CommittedButUnanalyzed {
        /// The failed analysis of the output file.
        output: AnalysisRecord,
        /// User-facing reason.
        error: String,
    },
    /// The output is in place and analyzed.
    Succeeded {
        /// Analysis of the output file.
        output: AnalysisRecord,
        /// Something the user should know about the output, such as source packets that failed to decode.
        notice: Option<String>,
    },
}

/// The result of an operation on one file.
#[derive(Debug, Clone)]
pub struct OperationResult {
    /// The source file.
    pub source: PathBuf,
    /// What happened.
    pub outcome: OperationOutcome,
}

impl OperationResult {
    /// Builds a failed result for the source.
    #[must_use]
    pub fn failure(source: &Path, error: impl Into<String>) -> Self {
        Self {
            source: source.to_path_buf(),
            outcome: OperationOutcome::Failed { error: error.into() },
        }
    }

    /// Builds a successful result.
    #[must_use]
    pub fn success(source: &Path, output: AnalysisRecord, notice: Option<String>) -> Self {
        Self {
            source: source.to_path_buf(),
            outcome: OperationOutcome::Succeeded { output, notice },
        }
    }

    /// Returns the user-facing error, when the operation failed in any way.
    #[must_use]
    pub fn error(&self) -> Option<&str> {
        match &self.outcome {
            OperationOutcome::Failed { error } | OperationOutcome::CommittedButUnanalyzed { error, .. } => Some(error),
            OperationOutcome::Succeeded { .. } => None,
        }
    }

    /// Returns the analysis of the output file, when the output was committed.
    #[must_use]
    pub const fn output(&self) -> Option<&AnalysisRecord> {
        match &self.outcome {
            OperationOutcome::Failed { .. } => None,
            OperationOutcome::CommittedButUnanalyzed { output, .. } | OperationOutcome::Succeeded { output, .. } => {
                Some(output)
            }
        }
    }
}

/// Keeps the cache in step with a committed output.
///
/// A converted source leaves the cache, and an analyzed output is stored.
/// A failed cache write only costs a re-analysis later, so it is reported and otherwise ignored.
pub fn update_cache(cache: &AnalysisCache, source: &AnalysisRecord, result: &OperationResult) {
    let Some(output) = result.output() else {
        return;
    };
    if output.path != source.path
        && let Err(error) = cache.remove_analysis(&source.path)
    {
        tracing::warn!("Could not update the cache: {}", format_error_chain(&error));
    }
    if matches!(result.outcome, OperationOutcome::Succeeded { .. })
        && let Err(error) = cache.store_analysis(output)
    {
        tracing::warn!("Could not update the cache: {}", format_error_chain(&error));
    }
}

/// Maps progress from 0 to 1 onto the range from `start` to `end` of the caller's progress.
pub fn progress_range(on_progress: &mut dyn FnMut(f32), start: f32, end: f32) -> impl FnMut(f32) + '_ {
    move |fraction| on_progress((end - start).mul_add(fraction.clamp(0.0, 1.0), start))
}
