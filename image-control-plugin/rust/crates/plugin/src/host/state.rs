//! Bounded length-prefixed JSON persistence with complete detached validation before publication.

#![expect(unsafe_code, reason = "transfers bounded bytes through the host's VST3 stream ABI")]

use image_control_dsp::SharedState;
use vst3::ComRef;
use vst3::Steinberg::{IBStream, IBStreamTrait as _, kResultOk};

use crate::editor_geometry::EditorGeometry;
#[cfg(test)]
use crate::editor_geometry::LogicalSize;
use crate::params::Parameters;
use crate::state::{MAXIMUM_STATE_BYTES, StateError, restore_bytes, save_bytes};

/// Restores parameters without entering audio-owned history or waiting for the GUI.
///
/// # Safety
///
/// A non-null stream must reference a live host `IBStream` throughout this call.
pub(super) unsafe fn restore(
    stream: *mut IBStream,
    parameters: &Parameters,
    shared: &SharedState,
    geometry: &EditorGeometry,
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
    restore_bytes(&bytes, parameters, shared, geometry)
}

/// Serializes an independent snapshot on a non-audio state thread, using only short geometry locking.
///
/// # Safety
///
/// A non-null stream must reference a live writable host `IBStream` throughout this call.
pub(super) unsafe fn save(
    stream: *mut IBStream,
    parameters: &Parameters,
    shared: &SharedState,
    geometry: &EditorGeometry,
) -> Result<(), StateError> {
    // SAFETY: caller guarantees a valid stream or null, rejected here.
    let stream = unsafe { ComRef::from_raw(stream) }.ok_or(StateError::Stream)?;
    let mut bytes = save_bytes(parameters, shared, geometry)?;
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

    use super::{EditorGeometry, LogicalSize, Parameters, restore_bytes, save_bytes};

    #[test]
    fn version_four_roundtrips_preferences_and_fractional_logical_geometry() {
        let imager = Imager::new();
        let shared = imager.shared();
        let parameters = Parameters::default();
        let geometry = EditorGeometry::default();
        let sound = serde_json::to_vec(&parameters.snapshot()).expect("sound snapshot");
        shared.set_analysis_tap(AnalysisTap::Pre);
        let display = DisplayPreferences {
            persistence_seconds: 1.25,
            contrast: 0.2,
            color_mode: ColourMode::Phase,
            ..DisplayPreferences::default()
        };
        assert!(shared.set_display_preferences(display));
        let remembered = LogicalSize::default().acknowledged(1501, 1201, 1.5);
        geometry.publish(remembered);
        let bytes = save_bytes(&parameters, &shared, &geometry).expect("save");
        let json: serde_json::Value = serde_json::from_slice(&bytes).expect("v4 JSON");
        assert_eq!(json.get("version"), Some(&serde_json::Value::from(5)));
        shared.set_analysis_tap(AnalysisTap::Post);
        geometry.publish(LogicalSize::default());
        assert!(shared.set_display_preferences(DisplayPreferences::default()));
        restore_bytes(&bytes, &parameters, &shared, &geometry).expect("restore v4");
        assert_eq!(shared.analysis_tap(), AnalysisTap::Pre);
        assert_eq!(shared.display_preferences(), display);
        assert_eq!(geometry.snapshot(), remembered);
        assert_eq!(serde_json::to_vec(&parameters.snapshot()).expect("sound"), sound);
        assert_eq!(save_bytes(&parameters, &shared, &geometry).expect("recapture"), bytes);
    }

    #[test]
    fn legacy_versions_restore_fixed_geometry_not_previous_instance_choices() {
        let imager = Imager::new();
        let shared = imager.shared();
        let parameters = Parameters::default();
        let geometry = EditorGeometry::default();
        for bytes in [
            br#"{"version":1,"parameters":{"gain":0.25}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25}}"#.as_slice(),
            br#"{"version":3,"parameters":{"gain":0.25}}"#.as_slice(),
        ] {
            geometry.publish(LogicalSize {
                width: 1200.0,
                height: 900.0,
            });
            shared.set_analysis_tap(AnalysisTap::Pre);
            assert!(shared.set_display_preferences(DisplayPreferences {
                persistence_seconds: 2.0,
                contrast: 0.1,
                color_mode: ColourMode::Phase,
                ..DisplayPreferences::default()
            }));
            restore_bytes(bytes, &parameters, &shared, &geometry).expect("fixed missing defaults");
            assert_eq!(shared.analysis_tap(), AnalysisTap::Post);
            assert_eq!(shared.display_preferences(), DisplayPreferences::default());
            assert_eq!(geometry.snapshot(), LogicalSize::default());
            assert_eq!(parameters.get(0), Some(0.25));
        }
        restore_bytes(
            br#"{"version":2,"parameters":{},"editor":{"tap":"pre","contrast":0.25,"color_mode":"phase"}}"#,
            &parameters,
            &shared,
            &geometry,
        )
        .expect("version two preferences");
        assert_eq!(shared.analysis_tap(), AnalysisTap::Pre);
        assert_eq!(shared.display_preferences().color_mode, ColourMode::Phase);
        assert_eq!(geometry.snapshot(), LogicalSize::default());
    }

    #[test]
    fn malformed_editor_state_never_publishes_sound_preferences_or_geometry() {
        let imager = Imager::new();
        let shared = imager.shared();
        let parameters = Parameters::default();
        let geometry = EditorGeometry::default();
        geometry.publish(LogicalSize {
            width: 1280.0,
            height: 960.0,
        });
        let before = save_bytes(&parameters, &shared, &geometry).expect("capture");
        for bytes in [
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"persistence_seconds":0}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"contrast":1.01}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"tap":"middle"}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"color_mode":"random"}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"contrast":null}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":0.25},"editor":{"unknown":true}}"#.as_slice(),
            br#"{"version":2,"parameters":{"gain":2},"editor":{"tap":"pre"}}"#.as_slice(),
            br#"{"version":99,"parameters":{"gain":0.25}}"#.as_slice(),
        ] {
            assert!(restore_bytes(bytes, &parameters, &shared, &geometry).is_err());
            assert_eq!(save_bytes(&parameters, &shared, &geometry).expect("unchanged"), before);
        }
        for version in [1, 2, 3, 4] {
            for invalid in [
                "null",
                "{}",
                r#"{"width":1200}"#,
                r#"{"width":1200,"height":null}"#,
                r#"{"width":0,"height":900}"#,
                r#"{"width":1200,"height":599}"#,
                r#"{"width":4097,"height":900}"#,
                r#"{"width":1200,"height":4097}"#,
                r#"{"width":1e999,"height":900}"#,
                r#"{"width":1200,"height":900,"unknown":0}"#,
            ] {
                let bytes = format!(
                    r#"{{"version":{version},"parameters":{{"gain":0.25}},"editor":{{"tap":"pre"}},"geometry":{invalid}}}"#
                );
                assert!(restore_bytes(bytes.as_bytes(), &parameters, &shared, &geometry).is_err());
                assert_eq!(save_bytes(&parameters, &shared, &geometry).expect("unchanged"), before);
            }
        }
    }

    #[test]
    fn state_restore_completes_without_gui_while_audio_ownership_is_retained() {
        let mut imager = Imager::new();
        let shared = imager.shared();
        let parameters = std::sync::Arc::new(Parameters::default());
        let geometry = std::sync::Arc::new(EditorGeometry::default());
        let state_parameters = std::sync::Arc::clone(&parameters);
        let state_geometry = std::sync::Arc::clone(&geometry);
        let state_thread = std::thread::spawn(move || {
            restore_bytes(
                br#"{"version":3,"parameters":{"gain":0.25},"editor":{"tap":"pre"},"geometry":{"width":1200,"height":900}}"#,
                &state_parameters,
                &shared,
                &state_geometry,
            )
            .expect("off-thread state");
            save_bytes(&state_parameters, &shared, &state_geometry).expect("off-thread capture")
        });
        imager.begin_block();
        for _ in 0..64 {
            let _ = imager.process_frame([0.0; 2]);
        }
        let saved = state_thread.join().expect("bounded state completion without GUI");
        assert_eq!(parameters.get(0), Some(0.25));
        assert_eq!(
            geometry.snapshot(),
            LogicalSize {
                width: 1200.0,
                height: 900.0
            }
        );
        let other_imager = Imager::new();
        let other_parameters = Parameters::default();
        let other_geometry = EditorGeometry::default();
        restore_bytes(&saved, &other_parameters, &other_imager.shared(), &other_geometry).expect("new component");
        assert_eq!(other_geometry.snapshot(), geometry.snapshot());
    }
}
