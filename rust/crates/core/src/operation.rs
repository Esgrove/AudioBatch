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

#[cfg(test)]
mod test_operation_results {
    use super::*;

    #[test]
    fn failed_and_committed_outcomes_keep_distinct_output_authority() {
        let source = Path::new("source.wav");
        let failed = OperationResult::failure(source, "untouched");
        assert_eq!(failed.source, source);
        assert_eq!(failed.error(), Some("untouched"));
        assert!(failed.output().is_none());

        let output = AnalysisRecord {
            path: PathBuf::from("output.aif"),
            ..AnalysisRecord::default()
        };
        let committed = OperationResult {
            source: source.to_path_buf(),
            outcome: OperationOutcome::CommittedButUnanalyzed {
                output: output.clone(),
                error: "readback failed".to_owned(),
            },
        };
        assert_eq!(committed.error(), Some("readback failed"));
        assert_eq!(committed.output().map(|record| &record.path), Some(&output.path));

        let succeeded = OperationResult::success(source, output.clone(), Some("decode notice".to_owned()));
        assert_eq!(succeeded.error(), None);
        assert_eq!(succeeded.output().map(|record| &record.path), Some(&output.path));
        assert!(matches!(
            succeeded.outcome,
            OperationOutcome::Succeeded { notice: Some(notice), .. } if notice == "decode notice"
        ));
    }

    #[test]
    fn progress_is_clamped_and_mapped_to_the_callers_range() {
        let mut reported = Vec::new();
        {
            let mut report = |fraction| reported.push(fraction);
            let mut progress = progress_range(&mut report, 0.2, 0.8);
            for fraction in [-1.0, 0.0, 0.5, 1.0, 2.0] {
                progress(fraction);
            }
        }
        for (actual, expected) in reported.iter().zip([0.2, 0.2, 0.5, 0.8, 0.8]) {
            assert!((actual - expected).abs() < 1e-6);
        }
        assert_eq!(reported.len(), 5);
    }
}

#[cfg(test)]
mod test_operation_cache {
    use tempfile::TempDir;

    use super::*;
    use crate::test_helpers::{ready_fixture_copy, temporary_cache};

    #[test]
    fn failed_operations_leave_the_cached_source_untouched() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let source = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        cache.store_analysis(&source).expect("stores source");

        update_cache(
            &cache,
            &source,
            &OperationResult::failure(&source.path, "not committed"),
        );

        assert!(cache.get_analysis(&source.path).is_some());
    }

    #[test]
    fn successful_conversion_removes_only_the_source_and_stores_the_output() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let source = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        let output = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "output.wav");
        let unrelated = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "unrelated.wav");
        for record in [&source, &unrelated] {
            cache.store_analysis(record).expect("stores record");
        }

        update_cache(
            &cache,
            &source,
            &OperationResult::success(&source.path, output.clone(), None),
        );

        assert!(cache.get_analysis(&source.path).is_none());
        assert!(cache.get_analysis(&output.path).is_some());
        assert!(cache.get_analysis(&unrelated.path).is_some());
    }

    #[test]
    fn unanalyzed_conversion_removes_the_source_without_storing_a_failed_readback() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let source = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        cache.store_analysis(&source).expect("stores source");
        let output = AnalysisRecord {
            path: directory.path().join("output.aif"),
            ..AnalysisRecord::default()
        };
        let result = OperationResult {
            source: source.path.clone(),
            outcome: OperationOutcome::CommittedButUnanalyzed {
                output: output.clone(),
                error: "readback failed".to_owned(),
            },
        };

        update_cache(&cache, &source, &result);

        assert!(!cache.remove_analysis(&source.path).expect("source row removed"));
        assert!(!cache.remove_analysis(&output.path).expect("output row absent"));
    }

    #[test]
    fn unavailable_cache_cannot_turn_a_committed_output_into_a_failure() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = AnalysisCache::with_path(directory.path());
        let source = AnalysisRecord {
            path: directory.path().join("source.wav"),
            ..AnalysisRecord::default()
        };
        let output = AnalysisRecord {
            path: directory.path().join("output.aif"),
            ..AnalysisRecord::default()
        };
        let result = OperationResult::success(&source.path, output.clone(), None);

        update_cache(&cache, &source, &result);

        assert_eq!(result.error(), None);
        assert_eq!(result.output().map(|record| &record.path), Some(&output.path));
    }

    #[test]
    fn successful_in_place_output_replaces_the_cached_measurements() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let source = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        cache.store_analysis(&source).expect("stores source");
        let mut output = source.clone();
        output.overall_peak = 0.25;

        update_cache(&cache, &source, &OperationResult::success(&source.path, output, None));

        let cached = cache.get_analysis(&source.path).expect("updated cache row");
        assert!((cached.overall_peak - 0.25).abs() < f32::EPSILON);
    }
}
