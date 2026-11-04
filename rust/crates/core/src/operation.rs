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

    /// Drops transient drawing payloads after cache persistence and before queuing a table result.
    /// Selected-file readers load final artifacts independently, keeping batch event retention bounded.
    pub fn clear_preview_artifacts(&mut self) {
        match &mut self.outcome {
            OperationOutcome::Failed { .. } => {}
            OperationOutcome::CommittedButUnanalyzed { output, .. } | OperationOutcome::Succeeded { output, .. } => {
                output.waveform = None;
                output.spectrogram = None;
            }
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
/// A converted source leaves the cache, and a newly analyzed output is stored.
/// Reused cached measurements do not overwrite newer measurements, provenance, gains, or spectral artifacts.
/// A failed cache write only costs a re-analysis later, so it is reported and otherwise ignored.
pub fn update_cache(cache: &AnalysisCache, source: &AnalysisRecord, result: &OperationResult) {
    let Some(output) = result.output() else {
        return;
    };
    if output.path != source.path
        && let Err(error) = cache.remove_analysis_for_record(source)
    {
        tracing::warn!("Could not update the cache: {}", format_error_chain(&error));
    }
    if !output.from_cache && matches!(result.outcome, OperationOutcome::Succeeded { .. }) {
        if let Err(error) = cache.store_analysis(output) {
            tracing::warn!("Could not update the cache: {}", format_error_chain(&error));
        }
        if let Some(spectrogram) = &output.spectrogram
            && let Err(error) = cache.store_spectrogram(output, spectrogram)
        {
            tracing::warn!("Could not cache the spectrogram: {}", format_error_chain(&error));
        }
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
    use crate::record::MeasurementProducer;
    use crate::test_helpers::{analyze, ready_fixture_copy, temporary_cache, temporary_cache_path};

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

    /// Preserves a replacement source's cache row when an older conversion completes after it.
    #[test]
    fn delayed_conversion_completion_does_not_remove_a_newer_source_row() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let old_source = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        cache.store_analysis(&old_source).expect("stores original");
        let replacement = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "source.wav");
        cache.store_analysis(&replacement).expect("stores replacement");
        let output = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "output.wav");
        update_cache(
            &cache,
            &old_source,
            &OperationResult::success(&old_source.path, output, None),
        );
        let retained = cache.get_analysis(&replacement.path).expect("newer source retained");
        assert_eq!(retained.file_size, replacement.file_size);
        assert_eq!(retained.sample_rate, replacement.sample_rate);
    }

    /// A delayed cached no-op has no authority over another connection's same-source measurement or gain clear.
    #[test]
    fn cached_no_op_preserves_newer_foreign_measurements_and_cleared_gain() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let writer = AnalysisCache::with_path(temporary_cache_path(&directory));
        let mut original = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        original.custom_gain_db = Some(-3.0);
        cache.store_analysis(&original).expect("stores native measurements");
        let source = cache.get_analysis(&original.path).expect("cached native source");
        assert!(source.from_cache);
        assert_eq!(source.measurement_producer, MeasurementProducer::Rust);
        let mut output = source.clone();
        output.spectrogram = original.spectrogram.clone();
        assert!(output.spectrogram.is_some());
        let result = OperationResult::success(&source.path, output, None);

        let mut newer = original;
        newer.measurement_producer = MeasurementProducer::Cpp;
        newer.overall_peak = 0.25;
        newer.overall_true_peak = 0.3;
        newer.integrated_lufs = -18.0;
        writer
            .store_analysis(&newer)
            .expect("stores newer foreign measurements");
        assert!(writer.store_custom_gain_for_record(&newer, None).expect("clears gain"));

        update_cache(&cache, &source, &result);

        let retained = cache
            .get_analysis(&source.path)
            .expect("retains newer cached measurements");
        assert_eq!(retained.measurement_producer, MeasurementProducer::Cpp);
        assert!((retained.overall_peak - newer.overall_peak).abs() < f32::EPSILON);
        assert!((retained.overall_true_peak - newer.overall_true_peak).abs() < f64::EPSILON);
        assert!((retained.integrated_lufs - newer.integrated_lufs).abs() < f64::EPSILON);
        assert!(retained.custom_gain_db.is_none());
        assert!(cache.get_spectrogram(&source.path).is_none());
    }

    /// A fresh native measurement remains publishable even when normalization does not rewrite the source.
    #[test]
    fn native_remeasurement_persists_same_path_measurements_and_spectrogram() {
        let directory = TempDir::new().expect("temporary directory");
        let cache = temporary_cache(&directory);
        let writer = AnalysisCache::with_path(temporary_cache_path(&directory));
        let mut foreign = ready_fixture_copy(&directory, "sine_mono_24000_16.wav", "source.wav");
        foreign.measurement_producer = MeasurementProducer::Cpp;
        foreign.overall_peak = 0.25;
        foreign.overall_true_peak = 0.3;
        foreign.integrated_lufs = -18.0;
        foreign.custom_gain_db = Some(-3.0);
        writer.store_analysis(&foreign).expect("stores foreign measurements");
        let source = cache.get_analysis(&foreign.path).expect("cached foreign source");
        let output = analyze(&source.path);
        assert!(output.is_ready(), "{}", output.error_message);
        assert!(!output.from_cache);
        assert_eq!(output.measurement_producer, MeasurementProducer::Rust);
        let spectrogram = output.spectrogram.clone().expect("native spectral artifact");
        assert!(writer.store_custom_gain_for_record(&source, None).expect("clears gain"));

        update_cache(
            &cache,
            &source,
            &OperationResult::success(&source.path, output.clone(), None),
        );

        let cached = cache.get_analysis(&source.path).expect("native measurements persisted");
        assert_eq!(cached.measurement_producer, MeasurementProducer::Rust);
        assert!((cached.overall_peak - output.overall_peak).abs() < f32::EPSILON);
        assert!((cached.overall_true_peak - output.overall_true_peak).abs() < f64::EPSILON);
        assert!((cached.integrated_lufs - output.integrated_lufs).abs() < f64::EPSILON);
        assert!(cached.custom_gain_db.is_none());
        assert_eq!(
            cache
                .get_spectrogram(&source.path)
                .expect("native spectral artifact persisted")
                .to_bytes(),
            spectrogram.to_bytes()
        );
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
