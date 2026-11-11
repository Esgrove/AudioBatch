//! Bounded length-prefixed JSON persistence with complete detached validation before publication.

#![expect(unsafe_code, reason = "transfers bounded bytes through the host's VST3 stream ABI")]

use image_control_dsp::{AnalysisTap, ColourMode, DisplayPreferences, SharedState};
use serde::{Deserialize, Serialize};
use thiserror::Error;
use vst3::ComRef;
use vst3::Steinberg::{IBStream, IBStreamTrait as _, kResultOk};

use crate::params::{Parameters, Snapshot};

/// Largest accepted payload regardless of a hostile host's length prefix.
const MAXIMUM_STATE_BYTES: usize = 4096;
/// Editor preferences extend this component identity's sound-only first generation.
const STATE_VERSION: u32 = 2;

/// Failure to decode or transfer a bounded state, always outside the audio callback.
#[derive(Debug, Error)]
pub(super) enum StateError {
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
    /// Detached preferences. Legacy states supply fixed defaults rather than retaining live choices.
    #[serde(default)]
    editor: EditorPreferences,
}

/// Versioned plain preferences keep serde and host state concerns outside the DSP crate.
#[derive(Clone, Copy, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
struct EditorPreferences {
    /// The selected observational source, unrelated to sound controls.
    tap: Tap,
    /// Exponential GPU persistence time constant in seconds.
    persistence_seconds: f32,
    /// Normalized worker contrast.
    contrast: f32,
    /// Worker palette selection.
    color_mode: Palette,
}

/// Stable state spellings do not depend on the DSP enum's representation.
#[derive(Clone, Copy, Default, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum Tap {
    Pre,
    #[default]
    Post,
}

/// The persistence codec admits only the supported spectral palettes.
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

    /// Constructs only validated preferences before any live sound parameter is published.
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

/// Validates the complete detached document before publishing either parameters or preferences.
fn restore_bytes(bytes: &[u8], parameters: &Parameters, shared: &SharedState) -> Result<(), StateError> {
    let state: State = serde_json::from_slice(bytes)?;
    if ![1, STATE_VERSION].contains(&state.version) || !state.parameters.is_valid() {
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
    shared.invalidate_monitoring();
    Ok(())
}

/// Serializes detached authoritative atomics without waiting for GUI work.
fn save_bytes(parameters: &Parameters, shared: &SharedState) -> Result<Vec<u8>, StateError> {
    Ok(serde_json::to_vec(&State {
        version: STATE_VERSION,
        parameters: parameters.snapshot(),
        editor: EditorPreferences::capture(shared),
    })?)
}

/// Restores parameters without entering audio-owned history or waiting for the GUI.
///
/// # Safety
///
/// A non-null stream must reference a live host `IBStream` throughout this call.
pub(super) unsafe fn restore(
    stream: *mut IBStream,
    parameters: &Parameters,
    shared: &SharedState,
) -> Result<(), StateError> {
    // SAFETY: caller guarantees a valid stream or null, rejected here.
    let stream = unsafe { ComRef::from_raw(stream) }.ok_or(StateError::Stream)?;
    let mut length = [0_u8; 4];
    transfer_read(&stream, &mut length)?;
    let length = usize::try_from(u32::from_le_bytes(length)).map_err(|_| StateError::Invalid)?;
    if length == 0 || length > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    let mut bytes = vec![0; length];
    transfer_read(&stream, &mut bytes)?;
    restore_bytes(&bytes, parameters, shared)
}

/// Serializes an independent atomic snapshot on a non-audio state thread.
///
/// # Safety
///
/// A non-null stream must reference a live writable host `IBStream` throughout this call.
pub(super) unsafe fn save(
    stream: *mut IBStream,
    parameters: &Parameters,
    shared: &SharedState,
) -> Result<(), StateError> {
    // SAFETY: caller guarantees a valid stream or null, rejected here.
    let stream = unsafe { ComRef::from_raw(stream) }.ok_or(StateError::Stream)?;
    let mut bytes = save_bytes(parameters, shared)?;
    if bytes.len() > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    let mut length = u32::try_from(bytes.len())
        .map_err(|_| StateError::Invalid)?
        .to_le_bytes();
    transfer_write(&stream, &mut length)?;
    transfer_write(&stream, &mut bytes)
}

/// Reads exactly the requested bounded bytes, rejecting EOF and impossible progress reports.
fn transfer_read(stream: &ComRef<'_, IBStream>, mut bytes: &mut [u8]) -> Result<(), StateError> {
    while !bytes.is_empty() {
        let count = i32::try_from(bytes.len()).map_err(|_| StateError::Invalid)?;
        let mut transferred = 0;
        // SAFETY: the bounded writable buffer covers count bytes, and the stream remains alive.
        let result = unsafe { stream.read(bytes.as_mut_ptr().cast(), count, &raw mut transferred) };
        if result != kResultOk || transferred <= 0 || transferred > count {
            return Err(StateError::Stream);
        }
        bytes = bytes.get_mut(transferred as usize..).ok_or(StateError::Stream)?;
    }
    Ok(())
}

/// Writes all bytes while rejecting a failed stream or one that makes no progress.
fn transfer_write(stream: &ComRef<'_, IBStream>, mut bytes: &mut [u8]) -> Result<(), StateError> {
    while !bytes.is_empty() {
        let count = i32::try_from(bytes.len()).map_err(|_| StateError::Invalid)?;
        let mut transferred = 0;
        // SAFETY: the bounded readable buffer covers count bytes, and the stream remains alive.
        let result = unsafe { stream.write(bytes.as_mut_ptr().cast(), count, &raw mut transferred) };
        if result != kResultOk || transferred <= 0 || transferred > count {
            return Err(StateError::Stream);
        }
        bytes = bytes.get_mut(transferred as usize..).ok_or(StateError::Stream)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use image_control_dsp::{AnalysisTap, ColourMode, DisplayPreferences, Imager};

    use super::{Parameters, restore_bytes, save_bytes};

    #[test]
    fn version_two_roundtrips_preferences_without_sound_changes() {
        let imager = Imager::new();
        let shared = imager.shared();
        let parameters = Parameters::default();
        let sound = serde_json::to_vec(&parameters.snapshot()).expect("sound snapshot");
        shared.set_analysis_tap(AnalysisTap::Pre);
        let display = DisplayPreferences {
            persistence_seconds: 1.25,
            contrast: 0.2,
            color_mode: ColourMode::Phase,
        };
        assert!(shared.set_display_preferences(display));
        let bytes = save_bytes(&parameters, &shared).expect("save");
        shared.set_analysis_tap(AnalysisTap::Post);
        assert!(shared.set_display_preferences(DisplayPreferences::default()));
        restore_bytes(&bytes, &parameters, &shared).expect("restore v2");
        assert_eq!(shared.analysis_tap(), AnalysisTap::Pre);
        assert_eq!(shared.display_preferences(), display);
        assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), sound);
        assert_eq!(save_bytes(&parameters, &shared).expect("recapture"), bytes);
    }

    #[test]
    fn legacy_state_restores_fixed_display_defaults_not_previous_choices() {
        let imager = Imager::new();
        let shared = imager.shared();
        let parameters = Parameters::default();
        shared.set_analysis_tap(AnalysisTap::Pre);
        assert!(shared.set_display_preferences(DisplayPreferences {
            persistence_seconds: 2.0,
            contrast: 0.1,
            color_mode: ColourMode::Phase
        }));
        restore_bytes(br#"{"version":1,"parameters":{"gain":0.25}}"#, &parameters, &shared).expect("v1");
        assert_eq!(shared.analysis_tap(), AnalysisTap::Post);
        assert_eq!(shared.display_preferences(), DisplayPreferences::default());
        assert_eq!(parameters.get(0), Some(0.25));
    }

    #[test]
    fn malformed_editor_state_never_publishes_valid_sound_or_preferences() {
        let imager = Imager::new();
        let shared = imager.shared();
        let parameters = Parameters::default();
        let before = save_bytes(&parameters, &shared).expect("capture");
        for bytes in [
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"persistence_seconds":0}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"contrast":1.01}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"tap":"middle"}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"color_mode":"random"}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"contrast":null}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"unknown":true}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":2},"editor":{"tap":"pre"}}"#.as_slice(),
        ] {
            assert!(restore_bytes(bytes, &parameters, &shared).is_err());
            assert_eq!(save_bytes(&parameters, &shared).expect("unchanged"), before);
        }
    }

    #[test]
    fn state_restore_completes_without_gui_while_audio_ownership_is_retained() {
        let mut imager = Imager::new();
        let shared = imager.shared();
        let parameters = std::sync::Arc::new(Parameters::default());
        let state_parameters = std::sync::Arc::clone(&parameters);
        let state_thread = std::thread::spawn(move || {
            restore_bytes(
                br#"{"version":2,"parameters":{"gain":0.25},"editor":{"tap":"pre"}}"#,
                &state_parameters,
                &shared,
            )
            .expect("off-thread state");
        });
        imager.begin_block();
        for _ in 0..64 {
            let _ = imager.process_frame([0.0; 2]);
        }
        state_thread.join().expect("bounded state completion without GUI");
        assert_eq!(parameters.get(0), Some(0.25));
    }
}
