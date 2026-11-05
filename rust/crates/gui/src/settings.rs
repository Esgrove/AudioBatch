//! Preferences of the desktop app that persist between launches.
//!
//! They are the audio device and preview preferences of the Audio Settings window:
//! the output device with its sample rate and buffer size,
//! previewing through the plugin chain with or without latency compensation,
//! continuing playback when changing files, starting from the first cue point,
//! the source visualization and arrangement, independent overload indicators, and the loudness graph range.
//! The Options menu choices for the Serato waveform colours and the album artwork are kept here too.
//! The normalization mode and its peak and loudness targets are kept here too.
//! They are stored as JSON in the Rust-specific `AudioBatch-rs` data directory.
//! A missing or unreadable file gives the defaults, so a broken file never stops the app from starting.

use std::fs;
use std::path::Path;

use anyhow::Result;
use audiobatch_core::atomic_write::write_atomically;
use audiobatch_core::normalize::{
    DEFAULT_TARGET_LUFS, DEFAULT_TARGET_PEAK_DBFS, MAXIMUM_TARGET_LUFS, MAXIMUM_TARGET_PEAK_DBFS, MINIMUM_TARGET_LUFS,
    MINIMUM_TARGET_PEAK_DBFS, NormalizationMode,
};
use audiobatch_playback::OutputConfig;
use serde::{Deserialize, Deserializer, Serialize};

use crate::results::{ColumnKey, ResultsDelegate, SOURCE_STATISTICS_COLUMNS};

/// Lowest loudness the graph axis accepts, in LUFS.
pub const LOUDNESS_LIMIT_MINIMUM: i32 = -60;

/// Highest loudness the graph axis accepts, in LUFS.
pub const LOUDNESS_LIMIT_MAXIMUM: i32 = 6;

/// Bottom of the loudness graph axis by default, in LUFS.
pub const DEFAULT_LOUDNESS_MINIMUM: i32 = -24;

/// Top of the loudness graph axis by default, in LUFS.
pub const DEFAULT_LOUDNESS_MAXIMUM: i32 = -4;

/// Revision of the results table column defaults.
/// Revision 1 hides Path beside Folder. Revision 2 hides the optional source-statistics columns.
pub const COLUMNS_REVISION: u32 = 2;

/// File name of the settings in the data directory.
pub const SETTINGS_FILE_NAME: &str = "gui-settings.json";

/// Default controls width relative to its original responsive baseline.
pub const DEFAULT_CONTROLS_WIDTH_SCALE: f32 = 0.95;

/// Default information-panel width relative to its original responsive baseline.
pub const DEFAULT_INFORMATION_WIDTH_SCALE: f32 = 0.90;

/// Smallest controls width scale, retaining readable controls.
pub const MINIMUM_CONTROLS_WIDTH_SCALE: f32 = 0.855;

/// Smallest information-panel width scale.
pub const MINIMUM_INFORMATION_WIDTH_SCALE: f32 = 0.81;

/// Largest width scale of either independently resized preview column.
pub const MAXIMUM_PANEL_WIDTH_SCALE: f32 = 1.10;

/// The level normalization brings files to, as stored in the settings.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NormalizeMode {
    /// Bring the sample peak to the peak target.
    #[default]
    Peak,
    /// Bring the integrated loudness to the loudness target, limited by the true peak ceiling.
    Loudness,
}

/// Source visualization and arrangement shown in the preview.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PreviewViewMode {
    /// Shows the source waveform.
    #[default]
    Waveform,
    /// Shows the source spectrogram.
    Spectrogram,
    /// Shows the waveform and spectrogram beside each other.
    SideBySide,
    /// Shows the waveform above the spectrogram.
    Stacked,
}

/// Preferences that persist between launches.
#[non_exhaustive]
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct GuiSettings {
    /// Name of the output device to open, or the system default when unset.
    pub output_device: Option<String>,
    /// Output sample rate in hertz, or the device default when unset.
    pub sample_rate: Option<u32>,
    /// Output buffer size in frames, or the device default when unset.
    pub buffer_size: Option<u32>,
    /// Plays the preview through the enabled plugins of the chain.
    pub preview_plugin_chain: bool,
    /// Holds the playhead while the preview chain fills its latency, so what is heard lines up with the waveform.
    pub compensate_preview_latency: bool,
    /// Starts a newly selected file only if the previous one was playing.
    pub continue_playback: bool,
    /// Starts a loaded file at its first cue point when it is in the first half of the file.
    pub play_from_first_cue_point: bool,
    /// Source visualization and arrangement shown in the preview.
    #[serde(deserialize_with = "deserialize_preview_view_mode")]
    pub preview_view_mode: PreviewViewMode,
    /// Shows source sample-peak overload indicators independently of true peaks.
    pub show_sample_overloads: bool,
    /// Shows source true-peak overload indicators independently of sample peaks.
    pub show_true_peak_overloads: bool,
    /// Extends enabled overload indicators as vertical lines across the preview.
    pub show_overload_lines: bool,
    /// Draws the short-term loudness graph over the preview.
    pub show_loudness_graph: bool,
    /// Bottom of the loudness graph axis in LUFS.
    pub loudness_minimum: i32,
    /// Top of the loudness graph axis in LUFS.
    pub loudness_maximum: i32,
    /// Which level normalization brings files to.
    pub normalize_mode: NormalizeMode,
    /// Sample peak target of peak normalization in dBFS.
    pub normalize_target_peak_dbfs: f64,
    /// Integrated loudness target of loudness normalization in LUFS.
    pub normalize_target_lufs: f64,
    /// Identifiers of the hidden results table columns, or none for the columns hidden by default.
    pub hidden_columns: Option<Vec<String>>,
    /// Revision of the saved column defaults, which `load_from` migrates from.
    /// A file without it was saved before the Folder column, so it reads as zero.
    #[serde(default)]
    pub columns_revision: u32,
    /// Colour the waveform from the Serato Overview tag of a file that has one.
    pub serato_waveform_colours: bool,
    /// Shows the embedded album artwork of the selected file beside its name in the information panel.
    pub show_album_artwork: bool,
    /// Shows custom-gain peak predictions and loudness estimates beside the source metrics.
    pub show_gain_adjusted_metrics: bool,
    /// Controls width relative to the original responsive expanded-preview baseline.
    #[serde(deserialize_with = "deserialize_controls_width_scale")]
    pub controls_width_scale: f32,
    /// Information-panel width relative to the original responsive expanded-preview baseline.
    #[serde(deserialize_with = "deserialize_information_width_scale")]
    pub information_width_scale: f32,
}

impl NormalizeMode {
    /// Returns the normalization mode of the core library.
    #[must_use]
    pub const fn to_core(self) -> NormalizationMode {
        match self {
            Self::Peak => NormalizationMode::Peak,
            Self::Loudness => NormalizationMode::Loudness,
        }
    }
}

impl PreviewViewMode {
    /// Modes in the canonical display order shared by the selector and menus.
    pub const ALL: [Self; 4] = [Self::Waveform, Self::Spectrogram, Self::SideBySide, Self::Stacked];

    /// Returns the canonical display label, independent of its stable storage name.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Self::Waveform => "Waveform",
            Self::Spectrogram => "Spectrogram",
            Self::SideBySide => "Both: side by side",
            Self::Stacked => "Both: stacked",
        }
    }
}

impl Default for GuiSettings {
    fn default() -> Self {
        Self {
            output_device: None,
            sample_rate: None,
            buffer_size: None,
            preview_plugin_chain: false,
            compensate_preview_latency: true,
            continue_playback: false,
            play_from_first_cue_point: false,
            preview_view_mode: PreviewViewMode::Waveform,
            show_sample_overloads: true,
            show_true_peak_overloads: true,
            show_overload_lines: true,
            show_loudness_graph: false,
            loudness_minimum: DEFAULT_LOUDNESS_MINIMUM,
            loudness_maximum: DEFAULT_LOUDNESS_MAXIMUM,
            normalize_mode: NormalizeMode::Peak,
            normalize_target_peak_dbfs: DEFAULT_TARGET_PEAK_DBFS,
            normalize_target_lufs: DEFAULT_TARGET_LUFS,
            hidden_columns: None,
            columns_revision: COLUMNS_REVISION,
            serato_waveform_colours: true,
            show_album_artwork: true,
            show_gain_adjusted_metrics: true,
            controls_width_scale: DEFAULT_CONTROLS_WIDTH_SCALE,
            information_width_scale: DEFAULT_INFORMATION_WIDTH_SCALE,
        }
    }
}

impl GuiSettings {
    /// Reads the settings from a file, with the defaults when it is missing or invalid.
    /// An unknown preview mode falls back to waveform without changing the other preferences.
    /// An invalid loudness range falls back to the default range on its own,
    /// and a normalization target outside its range is clamped into it.
    /// A saved column choice from before the Folder column hides the Path column once,
    /// so existing users get the new default and can show Path again from the Columns menu.
    #[must_use]
    pub fn load_from(path: &Path) -> Self {
        let mut settings: Self = fs::read_to_string(path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default();
        if let Some(hidden) = &mut settings.hidden_columns {
            let mut hide = |key: ColumnKey| {
                let id = ResultsDelegate::column_identifier(key);
                if !hidden.iter().any(|value| value == id) {
                    hidden.push(id.to_string());
                }
            };
            if settings.columns_revision < 1 {
                hide(ColumnKey::Path);
            }
            if settings.columns_revision < 2 {
                SOURCE_STATISTICS_COLUMNS.into_iter().for_each(hide);
            }
        }
        settings.columns_revision = COLUMNS_REVISION;
        if !is_valid_loudness_range(settings.loudness_minimum, settings.loudness_maximum) {
            settings.loudness_minimum = DEFAULT_LOUDNESS_MINIMUM;
            settings.loudness_maximum = DEFAULT_LOUDNESS_MAXIMUM;
        }
        settings.normalize_target_peak_dbfs = clamp_target(
            settings.normalize_target_peak_dbfs,
            MINIMUM_TARGET_PEAK_DBFS,
            MAXIMUM_TARGET_PEAK_DBFS,
            DEFAULT_TARGET_PEAK_DBFS,
        );
        settings.normalize_target_lufs = clamp_target(
            settings.normalize_target_lufs,
            MINIMUM_TARGET_LUFS,
            MAXIMUM_TARGET_LUFS,
            DEFAULT_TARGET_LUFS,
        );
        settings
    }

    /// Writes the settings to a file, replacing it atomically and creating its directory when needed.
    ///
    /// # Errors
    /// Returns an error when the directory or the file cannot be written.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        Ok(write_atomically(path, serde_json::to_string_pretty(self)?.as_bytes())?)
    }

    /// Returns the output device configuration to open.
    #[must_use]
    pub fn output_config(&self) -> OutputConfig {
        OutputConfig {
            device: self.output_device.clone(),
            sample_rate: self.sample_rate,
            buffer_size: self.buffer_size,
        }
    }
}

/// Returns true when a loudness axis range is valid: whole LUFS within the limits, with the maximum above the minimum.
#[must_use]
pub fn is_valid_loudness_range(minimum: i32, maximum: i32) -> bool {
    let limits = LOUDNESS_LIMIT_MINIMUM..=LOUDNESS_LIMIT_MAXIMUM;
    limits.contains(&minimum) && limits.contains(&maximum) && maximum > minimum
}

/// Parses a loudness axis limit typed in the settings, a whole number of LUFS within the limits.
#[must_use]
pub fn parse_loudness_limit(text: &str) -> Option<i32> {
    text.trim()
        .parse::<i32>()
        .ok()
        .filter(|value| (LOUDNESS_LIMIT_MINIMUM..=LOUDNESS_LIMIT_MAXIMUM).contains(value))
}

/// Falls back only the preview mode when a saved value is unknown or has an invalid JSON type.
fn deserialize_preview_view_mode<'de, D: Deserializer<'de>>(deserializer: D) -> Result<PreviewViewMode, D::Error> {
    let value = serde_json::Value::deserialize(deserializer)?;
    Ok(serde_json::from_value(value).unwrap_or_default())
}

/// Reads a controls scale independently, so a malformed field does not reset unrelated preferences.
fn deserialize_controls_width_scale<'de, D: Deserializer<'de>>(deserializer: D) -> Result<f32, D::Error> {
    deserialize_width_scale(deserializer, MINIMUM_CONTROLS_WIDTH_SCALE, DEFAULT_CONTROLS_WIDTH_SCALE)
}

/// Reads an information scale independently, preserving other settings when this field has an invalid JSON type.
fn deserialize_information_width_scale<'de, D: Deserializer<'de>>(deserializer: D) -> Result<f32, D::Error> {
    deserialize_width_scale(
        deserializer,
        MINIMUM_INFORMATION_WIDTH_SCALE,
        DEFAULT_INFORMATION_WIDTH_SCALE,
    )
}

/// Validates a relative width at the storage boundary, not from temporarily constrained window geometry.
fn deserialize_width_scale<'de, D: Deserializer<'de>>(
    deserializer: D,
    minimum: f32,
    default: f32,
) -> Result<f32, D::Error> {
    let value = serde_json::Value::deserialize(deserializer)?;
    Ok(serde_json::from_value::<f32>(value)
        .ok()
        .filter(|value| value.is_finite())
        .map_or(default, |value| value.clamp(minimum, MAXIMUM_PANEL_WIDTH_SCALE)))
}

/// Clamps a stored normalization target into its range, with the default for a value that is not a number.
const fn clamp_target(value: f64, minimum: f64, maximum: f64, default: f64) -> f64 {
    if value.is_finite() {
        value.clamp(minimum, maximum)
    } else {
        default
    }
}

#[cfg(test)]
mod test_settings_file {
    use super::*;

    #[test]
    fn settings_round_trip() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join("nested").join(SETTINGS_FILE_NAME);
        let settings = GuiSettings {
            output_device: Some(String::from("Babyface Pro")),
            sample_rate: Some(96_000),
            buffer_size: Some(256),
            preview_plugin_chain: true,
            compensate_preview_latency: false,
            continue_playback: true,
            play_from_first_cue_point: true,
            preview_view_mode: PreviewViewMode::Stacked,
            show_sample_overloads: false,
            show_true_peak_overloads: false,
            show_overload_lines: false,
            show_loudness_graph: true,
            loudness_minimum: -30,
            loudness_maximum: 0,
            normalize_mode: NormalizeMode::Loudness,
            normalize_target_peak_dbfs: -1.0,
            normalize_target_lufs: -9.5,
            hidden_columns: Some(vec![String::from("path"), String::from("peak_left")]),
            columns_revision: COLUMNS_REVISION,
            serato_waveform_colours: false,
            show_album_artwork: false,
            show_gain_adjusted_metrics: false,
            controls_width_scale: 1.02,
            information_width_scale: 0.84,
        };
        settings.save_to(&path).expect("saves the settings");
        assert_eq!(GuiSettings::load_from(&path), settings);
    }

    #[test]
    fn missing_or_invalid_files_give_the_defaults() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        assert_eq!(GuiSettings::load_from(&path), GuiSettings::default());
        fs::write(&path, "not json").expect("writes the file");
        assert_eq!(GuiSettings::load_from(&path), GuiSettings::default());
    }

    #[test]
    fn missing_fields_keep_their_defaults() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        fs::write(&path, r#"{"show_loudness_graph": true}"#).expect("writes the file");
        let settings = GuiSettings::load_from(&path);
        assert!(settings.show_loudness_graph);
        assert_eq!(settings.output_device, None);
        assert_eq!(settings.loudness_minimum, DEFAULT_LOUDNESS_MINIMUM);
        assert!(!settings.preview_plugin_chain);
        assert!(settings.compensate_preview_latency);
        assert!(settings.show_album_artwork);
        assert!(settings.show_gain_adjusted_metrics);
        assert_eq!(settings.preview_view_mode, PreviewViewMode::Waveform);
        assert!(settings.show_sample_overloads);
        assert!(settings.show_true_peak_overloads);
        assert!(settings.show_overload_lines);
    }

    #[test]
    fn an_invalid_saved_range_falls_back_to_the_default_range() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        fs::write(&path, r#"{"loudness_minimum": -4, "loudness_maximum": -24}"#).expect("writes the file");
        let settings = GuiSettings::load_from(&path);
        assert_eq!(settings.loudness_minimum, DEFAULT_LOUDNESS_MINIMUM);
        assert_eq!(settings.loudness_maximum, DEFAULT_LOUDNESS_MAXIMUM);
    }

    #[test]
    fn normalization_targets_are_clamped_into_their_ranges() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        fs::write(
            &path,
            r#"{"normalize_mode": "loudness", "normalize_target_peak_dbfs": 3.0, "normalize_target_lufs": -40.0}"#,
        )
        .expect("writes the file");
        let settings = GuiSettings::load_from(&path);
        assert_eq!(settings.normalize_mode, NormalizeMode::Loudness);
        assert!((settings.normalize_target_peak_dbfs - MAXIMUM_TARGET_PEAK_DBFS).abs() < f64::EPSILON);
        assert!((settings.normalize_target_lufs - MINIMUM_TARGET_LUFS).abs() < f64::EPSILON);
    }
}

#[cfg(test)]
mod test_preview_preferences {
    use super::*;

    #[test]
    fn panel_scales_default_independently_for_old_or_malformed_settings() {
        for (value, controls, information) in [
            (
                serde_json::json!({}),
                DEFAULT_CONTROLS_WIDTH_SCALE,
                DEFAULT_INFORMATION_WIDTH_SCALE,
            ),
            (
                serde_json::json!({"controls_width_scale": null, "information_width_scale": 1.05}),
                DEFAULT_CONTROLS_WIDTH_SCALE,
                1.05,
            ),
            (
                serde_json::json!({"controls_width_scale": 1.01, "information_width_scale": "bad"}),
                1.01,
                DEFAULT_INFORMATION_WIDTH_SCALE,
            ),
            (
                serde_json::json!({"controls_width_scale": -10.0, "information_width_scale": 100.0}),
                MINIMUM_CONTROLS_WIDTH_SCALE,
                MAXIMUM_PANEL_WIDTH_SCALE,
            ),
            (
                serde_json::json!({"controls_width_scale": 100.0, "information_width_scale": -10.0}),
                MAXIMUM_PANEL_WIDTH_SCALE,
                MINIMUM_INFORMATION_WIDTH_SCALE,
            ),
        ] {
            let mut value = value;
            value
                .as_object_mut()
                .expect("settings object")
                .insert("continue_playback".into(), true.into());
            let settings: GuiSettings = serde_json::from_value(value).expect("loads valid fields independently");
            assert!(settings.continue_playback);
            assert!((settings.controls_width_scale - controls).abs() < f32::EPSILON);
            assert!((settings.information_width_scale - information).abs() < f32::EPSILON);
        }
    }

    #[test]
    fn modes_have_stable_storage_names_and_canonical_labels() {
        let expected = [
            (PreviewViewMode::Waveform, "waveform", "Waveform"),
            (PreviewViewMode::Spectrogram, "spectrogram", "Spectrogram"),
            (PreviewViewMode::SideBySide, "side_by_side", "Both: side by side"),
            (PreviewViewMode::Stacked, "stacked", "Both: stacked"),
        ];
        for (mode, name, label) in expected {
            assert_eq!(serde_json::to_value(mode).expect("serializes a mode"), name);
            assert_eq!(mode.label(), label);
            let settings = GuiSettings {
                preview_view_mode: mode,
                ..GuiSettings::default()
            };
            let json = serde_json::to_string(&settings).expect("serializes settings");
            assert_eq!(
                serde_json::from_str::<GuiSettings>(&json).expect("reads settings"),
                settings
            );
        }
    }

    #[test]
    fn unknown_modes_fall_back_without_resetting_other_preferences() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        let expected = GuiSettings {
            output_device: Some(String::from("Saved device")),
            sample_rate: Some(96_000),
            show_loudness_graph: true,
            normalize_mode: NormalizeMode::Loudness,
            normalize_target_lufs: -11.0,
            serato_waveform_colours: false,
            show_sample_overloads: false,
            show_overload_lines: false,
            ..GuiSettings::default()
        };
        for unknown in [
            serde_json::json!("future_mode"),
            serde_json::Value::Null,
            serde_json::json!(42),
        ] {
            let mut value = serde_json::to_value(&expected).expect("serializes settings");
            value
                .as_object_mut()
                .expect("settings serialize as an object")
                .insert(String::from("preview_view_mode"), unknown);
            fs::write(&path, serde_json::to_vec(&value).expect("serializes settings")).expect("writes settings");
            assert_eq!(GuiSettings::load_from(&path), expected);
        }
    }

    #[test]
    fn all_overload_combinations_round_trip_independently() {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        for sample in [false, true] {
            for true_peak in [false, true] {
                for lines in [false, true] {
                    let settings = GuiSettings {
                        show_sample_overloads: sample,
                        show_true_peak_overloads: true_peak,
                        show_overload_lines: lines,
                        ..GuiSettings::default()
                    };
                    settings.save_to(&path).expect("saves settings");
                    assert_eq!(GuiSettings::load_from(&path), settings);
                }
            }
        }
    }
}

#[cfg(test)]
mod test_column_migration {
    use super::*;

    /// Writes settings JSON and reads it back through the production loader.
    fn load(json: &serde_json::Value) -> GuiSettings {
        let directory = tempfile::tempdir().expect("creates a temporary directory");
        let path = directory.path().join(SETTINGS_FILE_NAME);
        fs::write(&path, json.to_string()).expect("writes the file");
        GuiSettings::load_from(&path)
    }

    #[test]
    fn columns_saved_before_the_folder_column_hide_path_once() {
        let settings = load(&serde_json::json!({"hidden_columns": ["peak_left", "custom_gain"]}));
        assert_eq!(
            settings.hidden_columns,
            Some(vec![
                String::from("peak_left"),
                String::from("custom_gain"),
                String::from("path"),
                String::from("plr"),
                String::from("psr"),
                String::from("crest"),
                String::from("dc"),
                String::from("clips"),
                String::from("correlation"),
            ])
        );
        assert_eq!(settings.columns_revision, COLUMNS_REVISION);
        let settings = load(&serde_json::json!({"hidden_columns": ["path"]}));
        let hidden = settings.hidden_columns.expect("explicit hidden columns");
        assert_eq!(hidden.iter().filter(|id| *id == "path").count(), 1);
        assert!(hidden.contains(&String::from("clips")));
    }

    #[test]
    fn current_column_choices_keep_a_shown_path() {
        let settings = load(&serde_json::json!({
            "hidden_columns": ["peak_left"],
            "columns_revision": COLUMNS_REVISION,
        }));
        assert_eq!(settings.hidden_columns, Some(vec![String::from("peak_left")]));
    }

    #[test]
    fn default_column_choices_need_no_migration() {
        let settings = load(&serde_json::json!({"show_loudness_graph": true}));
        assert_eq!(settings.hidden_columns, None);
        assert_eq!(settings.columns_revision, COLUMNS_REVISION);
        assert_eq!(GuiSettings::default().columns_revision, COLUMNS_REVISION);
    }
}

#[cfg(test)]
mod test_loudness_limits {
    use super::*;

    #[test]
    fn ranges_need_the_maximum_above_the_minimum_within_the_limits() {
        assert!(is_valid_loudness_range(-24, -4));
        assert!(is_valid_loudness_range(-60, 6));
        assert!(!is_valid_loudness_range(-10, -10));
        assert!(!is_valid_loudness_range(-61, -4));
        assert!(!is_valid_loudness_range(-24, 7));
    }

    #[test]
    fn limits_parse_as_whole_numbers_within_the_limits() {
        assert_eq!(parse_loudness_limit(" -24 "), Some(-24));
        assert_eq!(parse_loudness_limit("+6"), Some(6));
        assert_eq!(parse_loudness_limit("-24.5"), None);
        assert_eq!(parse_loudness_limit("-70"), None);
        assert_eq!(parse_loudness_limit("loud"), None);
    }
}
