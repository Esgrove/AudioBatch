//! Format-independent bounded JSON sound, display, and logical editor-geometry persistence.

use image_control_dsp::{AnalysisTap, ColourMode, DisplayPreferences, SharedState};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::editor_geometry::{EditorGeometry, LogicalSize};
use crate::params::{CROSSOVER_PARAMETER_START, Parameters, Snapshot, parameter_spec};

/// Largest accepted state payload, independently of its host transport.
pub const MAXIMUM_STATE_BYTES: usize = 16_384;
/// Comparison sounds, provenance, and complete editor preferences extend the independent project schema.
const STATE_VERSION: u32 = 5;
/// Native power-skew normalization was introduced independently of comparisons.
const CROSSOVER_MAPPING_VERSION: u32 = 4;
/// First generation containing host-accepted logical geometry.
const GEOMETRY_VERSION: u32 = 3;

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
    parameters: StoredSnapshot,
    /// Legacy states supply fixed preferences rather than retaining live choices.
    #[serde(default)]
    editor: EditorPreferences,
    /// Logical units do not depend on the state thread's DPI or an attached editor.
    #[serde(default)]
    geometry: LogicalSize,
    /// Comparison metadata is independent from sound and never requires library reads during restore.
    #[serde(default)]
    comparison: Option<crate::editing::ComparisonState>,
    /// Complete non-sound editor layout, with fixed defaults for older generations.
    #[serde(default)]
    layout: crate::editor_preferences::LayoutPreferences,
}

/// Stored crossovers distinguish an absent fixed default from values requiring versioned migration.
#[derive(Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
struct StoredSnapshot {
    /// Continuous normalized output trim retained exactly across recalls.
    gain: f64,
    /// Present arrays are validated before migration. Explicit null is not an absent field.
    #[serde(default, deserialize_with = "deserialize_crossovers")]
    crossovers: Option<[f64; 3]>,
    /// Continuous normalized band values retain legacy off-grid sound until explicitly edited.
    bands: [crate::params::BandSnapshot; 4],
    /// Latency-preserving bypass.
    bypass: f64,
    /// Processed-minus-dry audition.
    delta: f64,
    /// Final mono fold.
    mono: f64,
}

/// Rejects explicit null without losing the distinction between missing and present crossover arrays.
fn deserialize_crossovers<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<Option<[f64; 3]>, D::Error> {
    <[f64; 3]>::deserialize(deserializer).map(Some)
}

impl StoredSnapshot {
    /// Captures the exact live representation without snapping legacy off-grid sound settings.
    const fn capture(snapshot: Snapshot) -> Self {
        Self {
            gain: snapshot.gain,
            crossovers: Some(snapshot.crossovers),
            bands: snapshot.bands,
            bypass: snapshot.bypass,
            delta: snapshot.delta,
            mono: snapshot.mono,
        }
    }

    /// Validates before converting v1-v3 exponential normalized crossovers through their original plain Hz.
    /// The float-skew inverse is rounded to a float host value,
    /// bounding migration error to the native conversion's float precision.
    /// Non-crossover bits are retained, never snapped as a side effect of recall.
    fn migrate(self, version: u32) -> Option<Snapshot> {
        let mut snapshot = Snapshot {
            gain: self.gain,
            crossovers: self.crossovers.unwrap_or_else(|| Snapshot::default().crossovers),
            bands: self.bands,
            bypass: self.bypass,
            delta: self.delta,
            mono: self.mono,
        };
        if !snapshot.is_valid() {
            return None;
        }
        if version < CROSSOVER_MAPPING_VERSION && self.crossovers.is_some() {
            for (index, normalized) in snapshot.crossovers.iter_mut().enumerate() {
                let plain = (20.0 * 1000.0_f64.powf(*normalized)) as f32;
                let spec = parameter_spec(CROSSOVER_PARAMETER_START + index as u32)?;
                *normalized = spec.plain_to_normalized(f64::from(plain));
                // Native float transcendental round trips are limited to two ppm or four millihertz.
                // Reject an unrepresentable migration before publishing any part of the detached document.
                if (spec.normalized_to_plain(*normalized) - f64::from(plain)).abs()
                    > f64::from((plain * 0.000_002).max(0.004))
                {
                    return None;
                }
            }
        }
        snapshot.is_valid().then_some(snapshot)
    }
}

#[expect(
    clippy::derivable_impls,
    reason = "capture supplies nonzero neutral sound defaults, not field defaults"
)]
impl Default for StoredSnapshot {
    fn default() -> Self {
        Self {
            crossovers: None,
            ..Self::capture(Snapshot::default())
        }
    }
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
    /// Horizontal projection is independent of host sound automation.
    mel_scale: bool,
    /// GPU density contours retain the same analysis energy.
    solid_rendering: bool,
    /// Validated channel level difference at the position midpoint.
    pan_midpoint_db: f32,
    /// Validated channel level difference near the position edge.
    pan_edge_db: f32,
    /// Numeric meter mode preserves its transient history.
    smooth_correlation: bool,
    /// Display refresh diagnostic visibility.
    show_fps: bool,
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
            mel_scale: display.mel_scale,
            solid_rendering: display.solid_rendering,
            pan_midpoint_db: display.pan_midpoint_db,
            pan_edge_db: display.pan_edge_db,
            smooth_correlation: display.smooth_correlation,
            show_fps: display.show_fps,
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
            mel_scale: self.mel_scale,
            solid_rendering: self.solid_rendering,
            pan_midpoint_db: self.pan_midpoint_db,
            pan_edge_db: self.pan_edge_db,
            smooth_correlation: self.smooth_correlation,
            show_fps: self.show_fps,
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
            mel_scale: display.mel_scale,
            solid_rendering: display.solid_rendering,
            pan_midpoint_db: display.pan_midpoint_db,
            pan_edge_db: display.pan_edge_db,
            smooth_correlation: display.smooth_correlation,
            show_fps: display.show_fps,
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
    if !(1..=STATE_VERSION).contains(&state.version) || !state.geometry.is_valid() || !state.layout.is_valid() {
        return Err(StateError::Invalid);
    }
    let sound = state.parameters.migrate(state.version).ok_or(StateError::Invalid)?;
    let comparison = state
        .comparison
        .unwrap_or_else(|| crate::editing::ComparisonState::from_sound(crate::editing::SoundSnapshot::capture(sound)));
    if !comparison.is_valid() {
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
    parameters.layout.publish_preferences(state.layout);
    let mut editing = parameters
        .editing
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    parameters.restore_values(sound);
    editing.synchronize(parameters.restore_generation(), sound);
    editing.restore_comparison(comparison);
    drop(editing);
    geometry.publish(if state.version < GEOMETRY_VERSION {
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
    let (snapshot, comparison) = parameters.with_editing(|editing, snapshot| (snapshot, editing.comparison(snapshot)));
    let bytes = serde_json::to_vec(&State {
        version: STATE_VERSION,
        parameters: StoredSnapshot::capture(snapshot),
        editor: EditorPreferences::capture(shared),
        geometry: geometry.snapshot(),
        comparison: Some(comparison),
        layout: parameters.layout.preferences(),
    })?;
    if bytes.len() > MAXIMUM_STATE_BYTES {
        return Err(StateError::Invalid);
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::{MAXIMUM_STATE_BYTES, STATE_VERSION, restore_bytes, save_bytes};
    use crate::runtime::Instance;

    /// Legacy off-grid sound retains exact non-crossover bits. Crossover error is bounded in the native Hz domain.
    #[test]
    fn legacy_sound_migrates_crossovers_and_retains_off_grid_controls_across_v4_saves() {
        let instance = Instance::new();
        for version in [1, 2, 3] {
            for normalized in [[0.0, 0.5, 1.0], [0.1, 0.234_567_891, 0.9], [0.000_001, 0.999_999, 0.75]] {
                let document = serde_json::json!({
                    "version": version,
                    "parameters": {
                        "gain": 0.123_456_789,
                        "crossovers": normalized,
                        "bands": [{"mid":0.123_456_789,"side":0.765_432_1,"width":0.432_123,
                                   "recover":0.123_123,"pan":0.789_123,"solo":0.123}, {}, {}, {}]
                    },
                    "geometry": {"width": 1200.5, "height": 900.25}
                });
                let bytes = serde_json::to_vec(&document).expect("legacy document");
                restore_bytes(&bytes, &instance.parameters, &instance.shared, &instance.geometry).expect("migration");
                instance.parameters.publish(&instance.shared);
                for (actual, legacy) in instance.shared.crossovers().into_iter().zip(normalized) {
                    let expected = (20.0 * 1000.0_f64.powf(legacy)) as f32;
                    // A native exp/log/pow float round trip, at most 0.0002% of the represented Hz or 4 mHz.
                    assert!(
                        (actual - expected).abs() <= (expected * 0.000_002).max(0.004),
                        "v{version}: {legacy}: {actual} != {expected}"
                    );
                }
                let saved = save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("save v4");
                let value: serde_json::Value = serde_json::from_slice(&saved).expect("v4");
                assert_eq!(value["version"], STATE_VERSION);
                assert_eq!(value["parameters"]["gain"], document["parameters"]["gain"]);
                assert_eq!(value["parameters"]["bands"][0], document["parameters"]["bands"][0]);
                let expected_geometry = if version == 3 { 1200.5 } else { 1000.0 };
                assert_eq!(instance.geometry.snapshot().width, expected_geometry);
                restore_bytes(&saved, &instance.parameters, &instance.shared, &instance.geometry).expect("v4 recall");
                assert_eq!(
                    save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("stable"),
                    saved
                );
            }
        }
    }

    /// A dense independent exponential source grid bounds migration over the entire supported Hz domain.
    #[test]
    fn legacy_crossover_migration_is_bounded_across_the_full_normalized_domain() {
        let instance = Instance::new();
        for step in 0..=4096 {
            let normalized = f64::from(step) / 4096.0;
            let bytes =
                format!(r#"{{"version":3,"parameters":{{"crossovers":[{normalized},{normalized},{normalized}]}}}}"#);
            restore_bytes(
                bytes.as_bytes(),
                &instance.parameters,
                &instance.shared,
                &instance.geometry,
            )
            .expect("bounded migration");
            instance.parameters.publish(&instance.shared);
            let expected = (20.0 * 1000.0_f64.powf(normalized)) as f32;
            for actual in instance.shared.crossovers() {
                assert!((actual - expected).abs() <= (expected * 0.000_002).max(0.004));
            }
        }
    }

    /// Missing crossover fields use fixed defaults, while invalid explicit fields reject before any publication.
    #[test]
    fn crossover_migration_validates_all_generations_transactionally() {
        let instance = Instance::new();
        for version in 1..=4 {
            let bytes = format!(r#"{{"version":{version},"parameters":{{}}}}"#);
            restore_bytes(
                bytes.as_bytes(),
                &instance.parameters,
                &instance.shared,
                &instance.geometry,
            )
            .expect("defaults");
            assert_eq!(
                instance.parameters.snapshot().crossovers,
                crate::params::Snapshot::default().crossovers
            );
            let before = save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("before");
            let generation = instance.parameters.restore_generation();
            for invalid in [
                "null",
                "[]",
                "[0,0]",
                "[0,0,0,0]",
                "[-0.1,0.5,1]",
                "[0,0.5,1.1]",
                "[0,1e999,1]",
            ] {
                let bytes = format!(
                    r#"{{"version":{version},"parameters":{{"gain":0.25,"crossovers":{invalid}}},"editor":{{"tap":"pre"}}}}"#
                );
                assert!(
                    restore_bytes(
                        bytes.as_bytes(),
                        &instance.parameters,
                        &instance.shared,
                        &instance.geometry
                    )
                    .is_err()
                );
                assert_eq!(instance.parameters.restore_generation(), generation);
                assert_eq!(
                    save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("unchanged"),
                    before
                );
            }
        }
    }

    /// Project comparisons and layout round trip without filesystem work, and recalls clear transient history.
    #[test]
    fn comparisons_and_layout_roundtrip_with_fixed_legacy_defaults() {
        let source = Instance::new();
        source.parameters.set(0, 0.25);
        source.parameters.with_editing(|editing, snapshot| {
            editing.begin();
            editing.copy(snapshot);
            editing.end(snapshot);
        });
        let preferences = crate::editor_preferences::LayoutPreferences {
            linked: [false, true, false, true],
            toolbar_shown: false,
            correlation_shown: false,
            compact_bands: true,
            band_height: 350.5,
            spectrogram_hidden: true,
            analysis_stopped: true,
        };
        assert!(source.parameters.layout.publish_preferences(preferences));
        let bytes = save_bytes(&source.parameters, &source.shared, &source.geometry).expect("project capture");
        let destination = Instance::new();
        restore_bytes(
            &bytes,
            &destination.parameters,
            &destination.shared,
            &destination.geometry,
        )
        .expect("restore project");
        assert_eq!(destination.parameters.layout.preferences(), preferences);
        let comparison = destination.parameters.with_editing(|editing, snapshot| {
            assert!(!editing.can_undo());
            assert!(!editing.can_redo());
            editing.comparison(snapshot)
        });
        assert_eq!(comparison.slots[0], comparison.slots[1]);
        assert_eq!(
            save_bytes(&destination.parameters, &destination.shared, &destination.geometry).expect("stable"),
            bytes
        );
        for version in 1..=4 {
            restore_bytes(
                format!(r#"{{"version":{version},"parameters":{{}}}}"#).as_bytes(),
                &destination.parameters,
                &destination.shared,
                &destination.geometry,
            )
            .expect("legacy restore");
            assert_eq!(
                destination.parameters.layout.preferences(),
                crate::editor_preferences::LayoutPreferences::default()
            );
        }
    }

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
