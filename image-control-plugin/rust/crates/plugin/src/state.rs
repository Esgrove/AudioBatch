//! Format-independent bounded JSON sound, display, and logical editor-geometry persistence.

use image_control_dsp::{AnalysisTap, DisplayPreferences, SharedState};
use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::editing::{ComparisonState, SoundSnapshot};
use crate::editor_geometry::{EditorGeometry, LogicalSize};
use crate::params::{Parameters, Snapshot};

/// Largest accepted state payload, independently of its host transport.
pub const MAXIMUM_STATE_BYTES: usize = 16_384;
/// Only this independent project schema is accepted. Other generations are not reinterpreted.
pub const STATE_VERSION: u32 = 5;

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
    /// Selected observational source, unrelated to sound controls.
    #[serde(default)]
    tap: AnalysisTap,
    /// Detached display preferences share their validation and serde representation with the DSP boundary.
    #[serde(default)]
    display: DisplayPreferences,
    /// Logical units do not depend on the state thread's DPI or an attached editor.
    #[serde(default)]
    geometry: LogicalSize,
    /// Absent comparisons initialize both slots from the restored sound without library reads or provenance.
    #[serde(default)]
    comparison: Option<ComparisonState>,
    /// Non-sound layout fields use fixed defaults, not the current instance's choices.
    #[serde(default)]
    layout: crate::editor_preferences::LayoutPreferences,
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
    if state.version != STATE_VERSION
        || !state.parameters.is_valid()
        || !state.display.is_valid()
        || !state.geometry.is_valid()
        || !state.layout.is_valid()
    {
        return Err(StateError::Invalid);
    }
    let sound = state.parameters;
    let comparison = state
        .comparison
        .unwrap_or_else(|| ComparisonState::from_sound(SoundSnapshot::capture(sound)));
    if !comparison.is_valid() {
        return Err(StateError::Invalid);
    }
    if !shared.set_display_preferences(state.display) {
        return Err(StateError::Invalid);
    }
    shared.set_analysis_tap(state.tap);
    parameters.layout.publish_preferences(state.layout);
    let mut editing = parameters
        .editing
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    parameters.restore_values(sound);
    editing.synchronize(parameters.restore_generation(), sound);
    editing.restore_comparison(comparison);
    drop(editing);
    geometry.publish(state.geometry);
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
        parameters: snapshot,
        tap: shared.analysis_tap(),
        display: shared.display_preferences(),
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

    /// Detached recall preserves every accepted normalized bit without snapping or plain-unit conversion.
    #[test]
    fn current_sound_precision_survives_repeated_saves_and_recalls() {
        let instance = Instance::new();
        let document = serde_json::json!({
            "version": STATE_VERSION,
            "parameters": {
                "gain": 0.123_456_789_012_345_6,
                "crossovers": [0.000_001_234_567_89, 0.234_567_891_234_567_8, 0.999_999_999_999_999_9],
                "bands": [{"mid":0.123_456_789,"side":0.765_432_1,"width":0.432_123,
                           "recover":0.123_123,"pan":0.789_123,"solo":0.123}, {}, {}, {}],
                "bypass": 0.123_456_789,
                "delta": 0.987_654_321,
                "mono": 0.567_890_123
            },
            "geometry": {"width": 1200.5, "height": 900.25}
        });
        let bytes = serde_json::to_vec(&document).expect("current document");
        restore_bytes(&bytes, &instance.parameters, &instance.shared, &instance.geometry).expect("recall");
        let original_sound = serde_json::to_vec(&instance.parameters.snapshot()).expect("sound");
        instance.parameters.publish(&instance.shared);
        let crossovers = instance.shared.crossovers().map(f32::to_bits);
        let saved = save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("save");
        let value: serde_json::Value = serde_json::from_slice(&saved).expect("current JSON");
        assert_eq!(value["version"], STATE_VERSION);
        assert_eq!(value["parameters"]["gain"], document["parameters"]["gain"]);
        assert_eq!(value["parameters"]["bands"][0], document["parameters"]["bands"][0]);
        assert_eq!(value["parameters"]["crossovers"], document["parameters"]["crossovers"]);
        for _ in 0..8 {
            let generation = instance.parameters.restore_generation();
            restore_bytes(&saved, &instance.parameters, &instance.shared, &instance.geometry).expect("recall");
            assert_eq!(instance.parameters.restore_generation(), generation + 1);
            instance.parameters.publish(&instance.shared);
            assert_eq!(instance.shared.crossovers().map(f32::to_bits), crossovers);
            assert_eq!(
                serde_json::to_vec(&instance.parameters.snapshot()).expect("sound"),
                original_sound
            );
            assert_eq!(
                save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("stable"),
                saved
            );
        }
    }

    /// Current crossovers survive a dense normalized grid without a state-specific mapping or quantization.
    #[test]
    fn current_crossovers_preserve_the_full_normalized_domain() {
        let instance = Instance::new();
        for step in 0..=4096 {
            let normalized = f64::from(step) / 4096.0;
            let bytes = format!(
                r#"{{"version":{STATE_VERSION},"parameters":{{"crossovers":[{normalized},{normalized},{normalized}]}}}}"#
            );
            restore_bytes(
                bytes.as_bytes(),
                &instance.parameters,
                &instance.shared,
                &instance.geometry,
            )
            .expect("current crossover recall");
            assert_eq!(
                instance.parameters.snapshot().crossovers.map(f64::to_bits),
                [normalized.to_bits(); 3]
            );
        }
    }

    /// Missing fields use fixed defaults, while invalid explicit arrays reject before any publication.
    #[test]
    fn current_crossovers_validate_transactionally() {
        let instance = Instance::new();
        let defaults = format!(r#"{{"version":{STATE_VERSION},"parameters":{{}}}}"#);
        restore_bytes(
            defaults.as_bytes(),
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
                r#"{{"version":{STATE_VERSION},"parameters":{{"gain":0.25,"crossovers":{invalid}}},"tap":"Pre"}}"#
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

    /// Project comparisons and layout round trip without filesystem work, and recalls clear transient history.
    #[test]
    fn comparisons_and_layout_roundtrip_with_fixed_missing_defaults() {
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
        let sparse = format!(r#"{{"version":{STATE_VERSION},"parameters":{{"gain":0.125}}}}"#);
        restore_bytes(
            sparse.as_bytes(),
            &destination.parameters,
            &destination.shared,
            &destination.geometry,
        )
        .expect("fixed defaults");
        assert_eq!(
            destination.parameters.layout.preferences(),
            crate::editor_preferences::LayoutPreferences::default()
        );
        destination.parameters.with_editing(|editing, snapshot| {
            let expected = crate::editing::SoundSnapshot::capture(snapshot);
            let comparison = editing.comparison(snapshot);
            assert_eq!(comparison.slots, [expected; 2]);
            assert_eq!(comparison.active, 0);
            assert_eq!(comparison.origins, [None, None]);
        });
    }

    /// Rejected metadata and schema generations leave values, preferences, geometry, and recall identity untouched.
    #[test]
    fn unsupported_versions_and_invalid_metadata_are_transactional() {
        let instance = Instance::new();
        let before = save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("before");
        let generation = instance.parameters.restore_generation();
        let base: serde_json::Value = serde_json::from_slice(&before).expect("document");
        let invalid_fields = [
            ("/comparison/active", serde_json::json!(2)),
            ("/comparison/slots/1/values/0", serde_json::json!(-0.1)),
            (
                "/comparison/origins/0",
                serde_json::json!({"id":"", "name":"Invalid", "baseline":base["comparison"]["slots"][0]}),
            ),
            ("/layout/band_height", serde_json::json!(0)),
            ("/display/pan_edge_db", serde_json::json!(12)),
        ];
        for (pointer, value) in invalid_fields {
            let mut invalid = base.clone();
            *invalid.pointer_mut(pointer).expect("field") = value;
            invalid["parameters"]["gain"] = serde_json::json!(0.25);
            assert!(
                restore_bytes(
                    &serde_json::to_vec(&invalid).expect("JSON"),
                    &instance.parameters,
                    &instance.shared,
                    &instance.geometry
                )
                .is_err()
            );
        }
        for version in [0, 1, 2, 3, 4, STATE_VERSION + 1, u32::MAX] {
            let bytes = format!(r#"{{"version":{version},"parameters":{{"gain":0.25}}}}"#);
            assert!(
                restore_bytes(
                    bytes.as_bytes(),
                    &instance.parameters,
                    &instance.shared,
                    &instance.geometry
                )
                .is_err()
            );
        }
        assert_eq!(instance.parameters.restore_generation(), generation);
        assert_eq!(
            save_bytes(&instance.parameters, &instance.shared, &instance.geometry).expect("unchanged"),
            before
        );
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
