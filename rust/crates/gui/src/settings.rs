//! Preferences of the desktop app that persist between launches.
//!
//! They are the audio device and preview preferences of the C++ application's Audio Settings:
//! the output device with its sample rate and buffer size, continuing playback when changing files,
//! starting from the first cue point, and the loudness graph with its axis range.
//! They are stored as JSON next to the analysis cache in the `AudioBatch-rs` data directory.
//! A missing or unreadable file gives the defaults, so a broken file never stops the app from starting.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::Result;
use audiobatch_core::cache::data_directory;
use audiobatch_playback::OutputConfig;
use serde::{Deserialize, Serialize};

/// Lowest loudness the graph axis accepts, in LUFS.
pub const LOUDNESS_LIMIT_MINIMUM: i32 = -60;

/// Highest loudness the graph axis accepts, in LUFS.
pub const LOUDNESS_LIMIT_MAXIMUM: i32 = 6;

/// Bottom of the loudness graph axis by default, in LUFS.
pub const DEFAULT_LOUDNESS_MINIMUM: i32 = -24;

/// Top of the loudness graph axis by default, in LUFS.
pub const DEFAULT_LOUDNESS_MAXIMUM: i32 = -4;

/// File name of the settings in the data directory.
const SETTINGS_FILE_NAME: &str = "gui-settings.json";

/// Preferences that persist between launches.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct GuiSettings {
    /// Name of the output device to open, or the system default when unset.
    pub output_device: Option<String>,
    /// Output sample rate in hertz, or the device default when unset.
    pub sample_rate: Option<u32>,
    /// Output buffer size in frames, or the device default when unset.
    pub buffer_size: Option<u32>,
    /// Starts a newly selected file only if the previous one was playing.
    pub continue_playback: bool,
    /// Starts a loaded file at its first cue point when it is in the first half of the file.
    pub play_from_first_cue_point: bool,
    /// Draws the short-term loudness graph over the waveform.
    pub show_loudness_graph: bool,
    /// Bottom of the loudness graph axis in LUFS.
    pub loudness_minimum: i32,
    /// Top of the loudness graph axis in LUFS.
    pub loudness_maximum: i32,
}

impl Default for GuiSettings {
    fn default() -> Self {
        Self {
            output_device: None,
            sample_rate: None,
            buffer_size: None,
            continue_playback: false,
            play_from_first_cue_point: false,
            show_loudness_graph: false,
            loudness_minimum: DEFAULT_LOUDNESS_MINIMUM,
            loudness_maximum: DEFAULT_LOUDNESS_MAXIMUM,
        }
    }
}

impl GuiSettings {
    /// Reads the settings from the data directory.
    #[must_use]
    pub fn load() -> Self {
        Self::load_from(&settings_path())
    }

    /// Reads the settings from a file, with the defaults when it is missing or invalid.
    /// An invalid loudness range falls back to the default range on its own.
    #[must_use]
    pub fn load_from(path: &Path) -> Self {
        let mut settings: Self = fs::read_to_string(path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default();
        if !is_valid_loudness_range(settings.loudness_minimum, settings.loudness_maximum) {
            settings.loudness_minimum = DEFAULT_LOUDNESS_MINIMUM;
            settings.loudness_maximum = DEFAULT_LOUDNESS_MAXIMUM;
        }
        settings
    }

    /// Writes the settings to the data directory, reporting a failure on stderr.
    pub fn save(&self) {
        let path = settings_path();
        if let Err(error) = self.save_to(&path) {
            eprintln!("Failed to save the settings to {}: {error:#}", path.display());
        }
    }

    /// Writes the settings to a file, creating its directory when needed.
    ///
    /// # Errors
    /// Returns an error when the directory or the file cannot be written.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        if let Some(directory) = path.parent() {
            fs::create_dir_all(directory)?;
        }
        fs::write(path, serde_json::to_string_pretty(self)?)?;
        Ok(())
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

/// Returns the path of the settings file.
fn settings_path() -> PathBuf {
    data_directory().join(SETTINGS_FILE_NAME)
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
            continue_playback: true,
            play_from_first_cue_point: true,
            show_loudness_graph: true,
            loudness_minimum: -30,
            loudness_maximum: 0,
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
