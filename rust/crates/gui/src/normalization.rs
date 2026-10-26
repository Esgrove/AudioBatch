//! Normalization from the desktop app: the target, the options of a run, and the progress of a run.
//!
//! Follows `NormalizationTargetControl.cpp` and the normalization pass of the C++ application.
//! The target is a sample peak in dBFS or an integrated loudness in LUFS,
//! limited to the ranges of the core library and stepped like the C++ slider.
//! A run counts its finished files and collects the failures,
//! so the status bar can show the progress and the end of the run can list what went wrong.

use std::path::Path;
use std::time::Instant;

use audiobatch_core::normalize::{
    DEFAULT_TARGET_LUFS, DEFAULT_TARGET_PEAK_DBFS, MAXIMUM_TARGET_LUFS, MAXIMUM_TARGET_PEAK_DBFS, MINIMUM_TARGET_LUFS,
    MINIMUM_TARGET_PEAK_DBFS, NormalizationOptions, NormalizationResult,
};

use crate::settings::{GuiSettings, NormalizeMode};

/// Status text when normalization is asked for while files are being analyzed.
pub const BLOCKED_BY_ANALYSIS: &str = "Wait for analysis to finish before normalizing";

/// Status text when normalization is asked for while a run is already in progress.
pub const ALREADY_NORMALIZING: &str = "Normalization already in progress";

/// Status text when the list is changed while files are being normalized.
pub const NORMALIZATION_IN_PROGRESS: &str = "Normalization in progress";

/// Most failures listed by name at the end of a run, like the C++ failure summary.
const MAXIMUM_LISTED_FAILURES: usize = 8;

/// Range and step of the target slider for one normalization mode.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TargetRange {
    /// Lowest target.
    pub minimum: f32,
    /// Highest target.
    pub maximum: f32,
    /// Slider step, 0.1 dB for peaks and 0.5 LU for loudness.
    pub step: f32,
    /// Target at startup and after a reset.
    pub default: f32,
}

/// Progress of one normalization run.
#[derive(Debug, Clone)]
pub struct NormalizeRun {
    /// Coordinator run the events belong to.
    pub run_id: u64,
    /// Number of files in the run.
    pub total: usize,
    /// Number of files with a result.
    pub finished: usize,
    /// One line per failed file, its name and the reason.
    pub failures: Vec<String>,
    /// True once every file has a result.
    pub completed: bool,
    started: Instant,
}

impl NormalizeRun {
    /// Starts tracking a run of the given number of files.
    #[must_use]
    pub fn new(run_id: u64, total: usize) -> Self {
        Self {
            run_id,
            total,
            finished: 0,
            failures: Vec::new(),
            completed: false,
            started: Instant::now(),
        }
    }

    /// Counts the result of one file and remembers its failure.
    pub fn record(&mut self, result: &NormalizationResult) {
        self.finished += 1;
        if let Some(error) = result.error() {
            self.failures.push(format!("{}: {error}", file_name(&result.source)));
        }
    }

    /// Returns the status bar text while the run is in progress, as in the C++ application.
    #[must_use]
    pub fn progress_text(&self) -> String {
        format!("Normalizing {}/{}", self.finished, self.total)
    }

    /// Returns the status bar text once the run is complete.
    #[must_use]
    pub fn summary(&self) -> String {
        let files = if self.total == 1 { "file" } else { "files" };
        match self.failures.len() {
            0 => format!("Normalized {} {files}", self.total),
            failed => format!("Normalized {} {files}, {failed} failed", self.total),
        }
    }

    /// Returns the log line of a completed run, with its duration and counts like the C++ log.
    #[must_use]
    pub fn log_line(&self) -> String {
        format!(
            "Normalization complete: {:.2} s, {} files ({} succeeded, {} failed)",
            self.started.elapsed().as_secs_f64(),
            self.total,
            self.total.saturating_sub(self.failures.len()),
            self.failures.len()
        )
    }

    /// Returns the text of the failure dialog, or none when every file succeeded.
    /// A single failure is shown on its own, and a longer list names the first few files.
    #[must_use]
    pub fn failure_message(&self) -> Option<String> {
        match self.failures.as_slice() {
            [] => None,
            [single] => Some(single.clone()),
            failures => {
                let mut lines: Vec<String> = failures
                    .iter()
                    .take(MAXIMUM_LISTED_FAILURES)
                    .map(|failure| format!("- {failure}"))
                    .collect();
                if failures.len() > MAXIMUM_LISTED_FAILURES {
                    lines.push(format!("- ...and {} more", failures.len() - MAXIMUM_LISTED_FAILURES));
                }
                Some(format!("Some files could not be normalized:\n\n{}", lines.join("\n")))
            }
        }
    }
}

/// Returns the slider range of a normalization mode.
#[must_use]
pub const fn target_range(mode: NormalizeMode) -> TargetRange {
    match mode {
        NormalizeMode::Peak => TargetRange {
            minimum: MINIMUM_TARGET_PEAK_DBFS as f32,
            maximum: MAXIMUM_TARGET_PEAK_DBFS as f32,
            step: 0.1,
            default: DEFAULT_TARGET_PEAK_DBFS as f32,
        },
        NormalizeMode::Loudness => TargetRange {
            minimum: MINIMUM_TARGET_LUFS as f32,
            maximum: MAXIMUM_TARGET_LUFS as f32,
            step: 0.5,
            default: DEFAULT_TARGET_LUFS as f32,
        },
    }
}

/// Describes the target for the menu, for example "-14.0 LUFS" or "0.0 dBFS peak", like the C++ control.
#[must_use]
pub fn describe_target(settings: &GuiSettings) -> String {
    match settings.normalize_mode {
        NormalizeMode::Peak => format!("{:.1} dBFS peak", settings.normalize_target_peak_dbfs),
        NormalizeMode::Loudness => format!("{:.1} LUFS", settings.normalize_target_lufs),
    }
}

/// Returns the short target text next to the slider caption, for example "-14.0 LUFS" or "-1.0 dBFS".
#[must_use]
pub fn target_value_text(settings: &GuiSettings) -> String {
    match settings.normalize_mode {
        NormalizeMode::Peak => format!("{:.1} dBFS", settings.normalize_target_peak_dbfs),
        NormalizeMode::Loudness => format!("{:.1} LUFS", settings.normalize_target_lufs),
    }
}

/// Stores a target from the slider of a mode, rounded to the slider step and clamped into the range.
/// Returns true when the stored target changed.
pub fn set_target(settings: &mut GuiSettings, mode: NormalizeMode, value: f32) -> bool {
    let range = target_range(mode);
    let stepped = ((value / range.step).round() * range.step).clamp(range.minimum, range.maximum);
    let stepped = f64::from((stepped * 10.0).round()) / 10.0;
    let target = match mode {
        NormalizeMode::Peak => &mut settings.normalize_target_peak_dbfs,
        NormalizeMode::Loudness => &mut settings.normalize_target_lufs,
    };
    let changed = (*target - stepped).abs() > f64::EPSILON;
    *target = stepped;
    changed
}

/// Returns the options of a run with the targets of the settings,
/// moving replaced and converted files to the system trash.
#[must_use]
pub fn normalization_options(settings: &GuiSettings, overwrite_existing: bool) -> NormalizationOptions {
    NormalizationOptions {
        mode: settings.normalize_mode.to_core(),
        target_peak_dbfs: settings.normalize_target_peak_dbfs,
        target_lufs: settings.normalize_target_lufs,
        overwrite_existing,
        ..NormalizationOptions::default()
    }
}

/// Returns the file name of a path for messages.
fn file_name(path: &Path) -> String {
    path.file_name().map_or_else(
        || path.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

#[cfg(test)]
mod test_target {
    use audiobatch_core::normalize::NormalizationMode;

    use super::*;

    #[test]
    fn describes_the_target_of_the_current_mode() {
        let mut settings = GuiSettings::default();
        assert_eq!(describe_target(&settings), "0.0 dBFS peak");
        assert_eq!(target_value_text(&settings), "0.0 dBFS");
        settings.normalize_mode = NormalizeMode::Loudness;
        assert_eq!(describe_target(&settings), "-14.0 LUFS");
        assert_eq!(target_value_text(&settings), "-14.0 LUFS");
    }

    #[test]
    fn slider_values_are_stepped_and_clamped() {
        let mut settings = GuiSettings::default();
        assert!(set_target(&mut settings, NormalizeMode::Peak, -1.04));
        assert!((settings.normalize_target_peak_dbfs + 1.0).abs() < 1e-9);
        assert!(!set_target(&mut settings, NormalizeMode::Peak, -0.98));
        set_target(&mut settings, NormalizeMode::Peak, 4.0);
        assert!(settings.normalize_target_peak_dbfs.abs() < 1e-9);
        set_target(&mut settings, NormalizeMode::Loudness, -9.3);
        assert!((settings.normalize_target_lufs + 9.5).abs() < 1e-9);
        assert!(settings.normalize_target_peak_dbfs.abs() < 1e-9);
    }

    #[test]
    fn options_take_the_mode_and_both_targets() {
        let settings = GuiSettings {
            normalize_mode: NormalizeMode::Loudness,
            normalize_target_peak_dbfs: -2.0,
            normalize_target_lufs: -10.0,
            ..GuiSettings::default()
        };
        let options = normalization_options(&settings, true);
        assert_eq!(options.mode, NormalizationMode::Loudness);
        assert!((options.target_peak_dbfs + 2.0).abs() < f64::EPSILON);
        assert!((options.target_lufs + 10.0).abs() < f64::EPSILON);
        assert!(options.overwrite_existing);
    }

    #[test]
    fn ranges_follow_the_core_limits() {
        let peak = target_range(NormalizeMode::Peak);
        assert!((peak.minimum + 12.0).abs() < f32::EPSILON && peak.maximum.abs() < f32::EPSILON);
        let loudness = target_range(NormalizeMode::Loudness);
        assert!((loudness.minimum + 24.0).abs() < f32::EPSILON && (loudness.maximum + 6.0).abs() < f32::EPSILON);
        assert!((loudness.step - 0.5).abs() < f32::EPSILON);
    }
}

#[cfg(test)]
mod test_run {
    use std::path::PathBuf;

    use super::*;

    fn failure(name: &str) -> NormalizationResult {
        NormalizationResult::failure(&PathBuf::from("/music").join(name), "Could not read")
    }

    #[test]
    fn progress_and_summary_count_the_results() {
        let mut run = NormalizeRun::new(1, 3);
        assert_eq!(run.progress_text(), "Normalizing 0/3");
        run.record(&failure("a.wav"));
        assert_eq!(run.progress_text(), "Normalizing 1/3");
        assert_eq!(run.summary(), "Normalized 3 files, 1 failed");
        assert!(run.log_line().contains("3 files (2 succeeded, 1 failed)"));
    }

    #[test]
    fn summary_of_a_clean_run_has_no_failures() {
        let run = NormalizeRun::new(1, 1);
        assert_eq!(run.summary(), "Normalized 1 file");
        assert_eq!(run.failure_message(), None);
    }

    #[test]
    fn single_failure_is_shown_on_its_own() {
        let mut run = NormalizeRun::new(1, 2);
        run.record(&failure("a.wav"));
        assert_eq!(run.failure_message().as_deref(), Some("a.wav: Could not read"));
    }

    #[test]
    fn long_failure_list_is_shortened() {
        let mut run = NormalizeRun::new(1, 10);
        for index in 0..10 {
            run.record(&failure(&format!("{index}.wav")));
        }
        let message = run.failure_message().expect("has failures");
        assert!(message.starts_with("Some files could not be normalized:\n\n- 0.wav: Could not read"));
        assert!(message.ends_with("- ...and 2 more"));
        assert_eq!(message.lines().filter(|line| line.starts_with("- ")).count(), 9);
    }
}
