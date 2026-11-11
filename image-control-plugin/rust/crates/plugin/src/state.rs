//! Format-independent bounded JSON sound, display, and logical editor-geometry persistence.

use image_control_dsp::{AnalysisTap, ColourMode, DisplayPreferences, SharedState};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::editor_geometry::{EditorGeometry, LogicalSize};
use crate::params::{Parameters, Snapshot};

/// Largest accepted state payload, independently of its host transport.
pub const MAXIMUM_STATE_BYTES: usize = 4096;
/// Logical geometry extends the sound and display-preference generations.
const STATE_VERSION: u32 = 3;

/// Failure to decode or transfer bounded state, always outside the audio callback.
#[derive(Debug, Error)]
pub enum StateError {
    /// A stream is missing, failed, or made no progress.
    #[error("The state stream could not transfer the requested bytes")]
    Stream,
    /// A length, version, or normalized value violates the state contract.
    #[error("The state does not satisfy the ImageControl RS format")]
    Invalid,
    /// The JSON payload is malformed.
    #[error("The state JSON is invalid")]
    Json(#[from] serde_json::Error),
}

/// Detached envelope that never retains a live editor or DSP reference.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct State {
    /// Independent schema generation.
    version: u32,
    /// Normalized sound values with fixed defaults for missing fields.
    parameters: Snapshot,
    /// Legacy states supply fixed preferences rather than retaining live choices.
    #[serde(default)]
    editor: EditorPreferences,
    /// Logical units do not depend on the state thread's DPI or an attached editor.
    #[serde(default)]
    geometry: LogicalSize,
}

/// Plain preferences keep serde and host state concerns outside the DSP crate.
#[derive(Clone, Copy, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
struct EditorPreferences {
    /// Selected observational source, unrelated to sound controls.
    tap: Tap,
    /// Exponential GPU persistence time constant in seconds.
    persistence_seconds: f32,
    /// Normalized worker contrast.
    contrast: f32,
    /// Worker palette selection.
    color_mode: Palette,
}

/// Stable state spellings independent of DSP enum representation.
#[derive(Clone, Copy, Default, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum Tap {
    Pre,
    #[default]
    Post,
}

/// Supported serialized spectral palettes.
#[derive(Clone, Copy, Default, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum Palette {
    #[default]
    Frequency,
    Phase,
}

impl EditorPreferences {
    /// Samples atomics without retaining or dispatching to an editor.
    fn capture(shared: &SharedState) -> Self {
        let display = shared.display_preferences();
        Self {
            tap: match shared.analysis_tap() {
                AnalysisTap::Pre => Tap::Pre,
                AnalysisTap::Post => Tap::Post,
            },
            persistence_seconds: display.persistence_seconds,
            contrast: display.contrast,
            color_mode: match display.color_mode {
                ColourMode::Frequency => Palette::Frequency,
                ColourMode::Phase => Palette::Phase,
            },
        }
    }

    /// Constructs only validated preferences before publishing live values.
    fn display(self) -> Option<DisplayPreferences> {
        let display = DisplayPreferences {
            persistence_seconds: self.persistence_seconds,
            contrast: self.contrast,
            color_mode: match self.color_mode {
                Palette::Frequency => ColourMode::Frequency,
                Palette::Phase => ColourMode::Phase,
            },
        };
        display.is_valid().then_some(display)
    }
}

impl Default for EditorPreferences {
    fn default() -> Self {
        let display = DisplayPreferences::default();
        Self {
            tap: Tap::default(),
            persistence_seconds: display.persistence_seconds,
            contrast: display.contrast,
            color_mode: Palette::default(),
        }
    }
}

/// Validates the entire detached document before publishing any live values.
pub fn restore_bytes(
    bytes: &[u8],
    parameters: &Parameters,
    shared: &SharedState,
    geometry: &EditorGeometry,
) -> Result<(), StateError> {
    if bytes.is_empty() || bytes.len() > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    let state: State = serde_json::from_slice(bytes)?;
    if ![1, 2, STATE_VERSION].contains(&state.version) || !state.parameters.is_valid() || !state.geometry.is_valid() {
        return Err(StateError::Invalid);
    }
    let display = state.editor.display().ok_or(StateError::Invalid)?;
    if !shared.set_display_preferences(display) {
        return Err(StateError::Invalid);
    }
    shared.set_analysis_tap(match state.editor.tap {
        Tap::Pre => AnalysisTap::Pre,
        Tap::Post => AnalysisTap::Post,
    });
    parameters.restore(state.parameters);
    geometry.publish(if state.version < STATE_VERSION {
        LogicalSize::default()
    } else {
        state.geometry
    });
    shared.invalidate_monitoring();
    Ok(())
}

/// Serializes detached authoritative values and coherent geometry without GUI work.
pub fn save_bytes(
    parameters: &Parameters,
    shared: &SharedState,
    geometry: &EditorGeometry,
) -> Result<Vec<u8>, StateError> {
    let bytes = serde_json::to_vec(&State {
        version: STATE_VERSION,
        parameters: parameters.snapshot(),
        editor: EditorPreferences::capture(shared),
        geometry: geometry.snapshot(),
    })?;
    if bytes.len() > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::{MAXIMUM_STATE_BYTES, restore_bytes, save_bytes};
    use crate::runtime::Instance;

    /// Every host transport rejects oversized documents before parsing or publishing values.
    #[test]
    fn oversized_and_empty_payloads_leave_the_instance_unchanged() {
        let instance = Instance::new();
        let before = save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("initial state");
        for bytes in [Vec::new(), vec![b' '; MAXIMUM_STATE_BYTES + 1]] {
            assert!(restore_bytes(&bytes, &instance.parameters, &instance.shared, &instance.geometry).is_err());
            assert_eq!(
                save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("unchanged state"),
                before
            );
        }
    }
}
