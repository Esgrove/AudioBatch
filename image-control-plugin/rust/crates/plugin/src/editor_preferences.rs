//! Detached, validated section layout and interaction preferences, independent of sound and native editor lifetime.

use serde::{Deserialize, Serialize};

/// Expanded band-controls height for new instances and states without an explicit preference.
pub const DEFAULT_BAND_HEIGHT: f32 = 304.0;
/// Minimum expanded band-controls height that retains the complete controls.
pub const MIN_BAND_HEIGHT: f32 = 280.0;
/// Maximum retained expanded band-controls height, independent of the current window size.
pub const MAX_BAND_HEIGHT: f32 = 4096.0;

/// Complete non-sound editor choices. Missing serialized fields use fixed defaults rather than live editor values.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct LayoutPreferences {
    /// Per-band linking of local Mid and Side source-trim gestures.
    pub linked: [bool; 4],
    /// User preference for showing the processing toolbar, independent of automatic inline placement.
    pub toolbar_shown: bool,
    /// User preference for showing the per-band correlation row.
    pub correlation_shown: bool,
    /// Compact band-controls presentation without changing the retained expanded height.
    pub compact_bands: bool,
    /// Retained expanded band-controls height in logical units.
    pub band_height: f32,
    /// User preference for hiding the spectrogram section.
    pub spectrogram_hidden: bool,
    /// User choice to stop analysis, independent of temporary editor or renderer availability.
    pub analysis_stopped: bool,
    /// Expands the plot to the panel inset and overlays its axes without hiding controls.
    pub maximize_spectrogram: bool,
}

impl LayoutPreferences {
    /// Validates detached values before state or UI publication, without clamping or mutating them.
    #[must_use]
    pub fn is_valid(self) -> bool {
        self.band_height.is_finite() && (MIN_BAND_HEIGHT..=MAX_BAND_HEIGHT).contains(&self.band_height)
    }
}

impl Default for LayoutPreferences {
    fn default() -> Self {
        Self {
            linked: [true; 4],
            toolbar_shown: true,
            correlation_shown: true,
            compact_bands: false,
            band_height: DEFAULT_BAND_HEIGHT,
            spectrogram_hidden: false,
            analysis_stopped: false,
            maximize_spectrogram: false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_fixed_and_valid() {
        let preferences = LayoutPreferences::default();
        assert_eq!(preferences.linked, [true; 4]);
        assert!(preferences.toolbar_shown);
        assert!(preferences.correlation_shown);
        assert!(!preferences.compact_bands);
        assert_eq!(preferences.band_height.to_bits(), 304.0_f32.to_bits());
        assert!(!preferences.spectrogram_hidden);
        assert!(!preferences.analysis_stopped);
        assert!(!preferences.maximize_spectrogram);
        assert!(preferences.is_valid());
    }

    #[test]
    fn missing_fields_use_fixed_defaults() {
        assert_eq!(
            serde_json::from_str::<LayoutPreferences>("{}").expect("empty layout preferences"),
            LayoutPreferences::default()
        );
        let partial = serde_json::from_str::<LayoutPreferences>(
            r#"{"linked":[false,true,false,true],"compact_bands":true,"band_height":512.25}"#,
        )
        .expect("partial layout preferences");
        assert_eq!(
            partial,
            LayoutPreferences {
                linked: [false, true, false, true],
                compact_bands: true,
                band_height: 512.25,
                ..LayoutPreferences::default()
            }
        );
    }

    #[test]
    fn complete_preferences_round_trip_without_losing_choices() {
        let preferences = LayoutPreferences {
            linked: [false, true, true, false],
            toolbar_shown: false,
            correlation_shown: false,
            compact_bands: true,
            band_height: 304.125,
            spectrogram_hidden: true,
            analysis_stopped: true,
            maximize_spectrogram: true,
        };
        let bytes = serde_json::to_vec(&preferences).expect("serialize layout preferences");
        assert_eq!(
            serde_json::from_slice::<LayoutPreferences>(&bytes).expect("deserialize layout preferences"),
            preferences
        );
    }

    #[test]
    fn malformed_or_unknown_fields_are_rejected() {
        for document in [
            r#"{"unknown":true}"#,
            r#"{"linked":[true,false,true]}"#,
            r#"{"linked":[true,false,true,false,true]}"#,
            r#"{"linked":[true,false,true,1]}"#,
            r#"{"toolbar_shown":1}"#,
            r#"{"correlation_shown":"true"}"#,
            r#"{"compact_bands":null}"#,
            r#"{"band_height":"304"}"#,
            r#"{"spectrogram_hidden":0}"#,
            r#"{"analysis_stopped":[]}"#,
            r#"{"maximize_spectrogram":null}"#,
            r#"{"maximize_spectrogram":1}"#,
        ] {
            assert!(
                serde_json::from_str::<LayoutPreferences>(document).is_err(),
                "{document}"
            );
        }
    }

    #[test]
    fn finite_heights_include_both_boundaries_and_fractional_values() {
        for band_height in [MIN_BAND_HEIGHT, 280.125, DEFAULT_BAND_HEIGHT, 4095.75, MAX_BAND_HEIGHT] {
            assert!(
                LayoutPreferences {
                    band_height,
                    ..LayoutPreferences::default()
                }
                .is_valid(),
                "{band_height}"
            );
        }
    }

    #[test]
    fn nonfinite_and_out_of_range_heights_are_invalid() {
        for band_height in [
            f32::NAN,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::MIN,
            f32::MAX,
            -1.0,
            0.0,
            MIN_BAND_HEIGHT.next_down(),
            MAX_BAND_HEIGHT.next_up(),
        ] {
            assert!(
                !LayoutPreferences {
                    band_height,
                    ..LayoutPreferences::default()
                }
                .is_valid(),
                "{band_height}"
            );
        }
    }
}
