//! Normalization from the desktop app: the target, the options of a run, and the status texts of a blocked run.
//!
//! The target is a sample peak in dBFS or an integrated loudness in LUFS,
//! limited to the ranges of the core library, in steps of 0.1 dB for peaks and 0.5 LU for loudness.
//! The controls column edits it in a number input, which accepts partly typed numbers such as "-" and "-1.".
//! The progress of a run is tracked by `OperationRun`, which plugin processing shares.

use audiobatch_core::normalize::{
    MAXIMUM_TARGET_LUFS, MAXIMUM_TARGET_PEAK_DBFS, MINIMUM_TARGET_LUFS, MINIMUM_TARGET_PEAK_DBFS, NormalizationOptions,
};

use crate::settings::{GuiSettings, NormalizeMode};

/// Status text when normalization is asked for while files are being analyzed.
pub const BLOCKED_BY_ANALYSIS: &str = "Wait for analysis to finish before normalizing";

/// Status text when normalization is asked for while files are being processed with plugins.
pub const BLOCKED_BY_PROCESSING: &str = "Wait for plugin processing to finish before normalizing";

/// Status text when normalization is asked for while a run is already in progress.
pub const ALREADY_NORMALIZING: &str = "Normalization already in progress";

/// Status text when the list is changed while files are being normalized.
pub const NORMALIZATION_IN_PROGRESS: &str = "Normalization in progress";

/// Longest integer part of a typed target, enough for every target in the ranges.
const MAXIMUM_INTEGER_DIGITS: usize = 3;

/// Range and step of the target input for one normalization mode.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TargetRange {
    /// Lowest target.
    pub minimum: f64,
    /// Highest target.
    pub maximum: f64,
    /// Step of the input's buttons and arrow keys, 0.1 dB for peaks and 0.5 LU for loudness.
    pub step: f64,
}

/// Returns the target range of a normalization mode.
#[must_use]
pub const fn target_range(mode: NormalizeMode) -> TargetRange {
    match mode {
        NormalizeMode::Peak => TargetRange {
            minimum: MINIMUM_TARGET_PEAK_DBFS,
            maximum: MAXIMUM_TARGET_PEAK_DBFS,
            step: 0.1,
        },
        NormalizeMode::Loudness => TargetRange {
            minimum: MINIMUM_TARGET_LUFS,
            maximum: MAXIMUM_TARGET_LUFS,
            step: 0.5,
        },
    }
}

/// Describes the target for the menu, for example "-14.0 LUFS" or "0.0 dBFS peak".
#[must_use]
pub fn describe_target(settings: &GuiSettings) -> String {
    match settings.normalize_mode {
        NormalizeMode::Peak => format!("{:.1} dBFS peak", settings.normalize_target_peak_dbfs),
        NormalizeMode::Loudness => format!("{:.1} LUFS", settings.normalize_target_lufs),
    }
}

/// Returns the unit of a mode's target, shown next to the target caption.
#[must_use]
pub const fn target_unit(mode: NormalizeMode) -> &'static str {
    match mode {
        NormalizeMode::Peak => "dBFS",
        NormalizeMode::Loudness => "LUFS",
    }
}

/// Returns the stored target of a mode.
#[must_use]
pub const fn target(settings: &GuiSettings, mode: NormalizeMode) -> f64 {
    match mode {
        NormalizeMode::Peak => settings.normalize_target_peak_dbfs,
        NormalizeMode::Loudness => settings.normalize_target_lufs,
    }
}

/// Returns a target as the target input shows it, with one decimal, for example "-14.0".
#[must_use]
pub fn format_target(value: f64) -> String {
    format!("{value:.1}")
}

/// Returns true when the text is a target being typed:
/// an optional minus sign, up to three digits, and an optional point followed by at most one digit.
/// Partial numbers such as "-" and "-1." are accepted, so they can be typed one character at a time.
#[must_use]
pub fn is_partial_target(text: &str) -> bool {
    let unsigned = text.strip_prefix('-').unwrap_or(text);
    let (integer, fraction) = unsigned.split_once('.').unwrap_or((unsigned, ""));
    integer.len() <= MAXIMUM_INTEGER_DIGITS
        && fraction.len() <= 1
        && integer
            .chars()
            .chain(fraction.chars())
            .all(|character| character.is_ascii_digit())
}

/// Stores a target typed for a mode, rounded to the step of the mode and clamped into its range.
/// Returns true when the stored target changed.
pub fn set_target(settings: &mut GuiSettings, mode: NormalizeMode, value: f64) -> bool {
    let range = target_range(mode);
    let stepped = ((value / range.step).round() * range.step).clamp(range.minimum, range.maximum);
    let stepped = (stepped * 10.0).round() / 10.0;
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

#[cfg(test)]
mod test_target {
    use audiobatch_core::normalize::NormalizationMode;

    use super::*;

    #[test]
    fn describes_the_target_of_the_current_mode() {
        let mut settings = GuiSettings::default();
        assert_eq!(describe_target(&settings), "0.0 dBFS peak");
        assert_eq!(target_unit(settings.normalize_mode), "dBFS");
        assert_eq!(format_target(target(&settings, NormalizeMode::Peak)), "0.0");
        settings.normalize_mode = NormalizeMode::Loudness;
        assert_eq!(describe_target(&settings), "-14.0 LUFS");
        assert_eq!(target_unit(settings.normalize_mode), "LUFS");
        assert_eq!(format_target(target(&settings, NormalizeMode::Loudness)), "-14.0");
    }

    #[test]
    fn partly_typed_targets_are_accepted() {
        for text in ["", "-", "-1", "-1.", "-1.5", ".5", "-12.0", "0"] {
            assert!(is_partial_target(text), "{text:?} should be accepted");
        }
        for text in ["--1", "1-", "-1.25", "1.2.3", "abc", "-1e3", "1000", " 1"] {
            assert!(!is_partial_target(text), "{text:?} should be rejected");
        }
    }

    #[test]
    fn typed_values_are_stepped_and_clamped() {
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
        assert!((peak.minimum + 12.0).abs() < f64::EPSILON && peak.maximum.abs() < f64::EPSILON);
        let loudness = target_range(NormalizeMode::Loudness);
        assert!((loudness.minimum + 24.0).abs() < f64::EPSILON && (loudness.maximum + 6.0).abs() < f64::EPSILON);
        assert!((loudness.step - 0.5).abs() < f64::EPSILON);
    }
}
