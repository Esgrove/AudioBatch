//! Plain-unit CLAP metadata, bounded block-sampled automation, and the shared detached state codec.

use std::ffi::CStr;
use std::fmt::Write as _;
use std::io::{Read, Write};

use clack_extensions::params::{
    HostParams, ParamDisplayWriter, ParamInfo, ParamInfoFlags, ParamInfoWriter, ParamRescanFlags,
    PluginAudioProcessorParams, PluginMainThreadParams,
};
use clack_extensions::state::PluginStateImpl;
use clack_plugin::events::event_types::ParamValueEvent;
use clack_plugin::prelude::*;
use clack_plugin::stream::{InputStream, OutputStream};
use clack_plugin::utils::Cookie;

use super::{AudioProcessor, MainThread};
use crate::params::{BYPASS_PARAMETER, PARAMETER_COUNT, ParameterKind, ParameterSpec, Parameters, parameter_spec};

/// Maximum inspected events in one process or parameter-flush callback.
/// This policy bounds work even for pathological hosts independently of their block size.
pub(super) const MAXIMUM_INPUT_EVENTS: usize = 4096;

/// Converts only finite plain-unit host values, never permitting NaN to enter the registry conversion.
pub(super) fn apply_plain(parameters: &Parameters, id: u32, value: f64) -> bool {
    let Some(spec) = parameter_spec(id).filter(|_| value.is_finite()) else {
        return false;
    };
    parameters.set(id, spec.plain_to_normalized(value))
}

/// Visits a bounded event prefix and keeps each parameter's last valid global value at block rate.
/// Per-note events and modulation are unsupported and never advertised by metadata.
pub(super) fn apply_events(parameters: &Parameters, events: &InputEvents, frames: Option<u32>) {
    let mut final_values = [None; PARAMETER_COUNT];
    for index in 0..events.len().min(MAXIMUM_INPUT_EVENTS as u32) {
        let Some(event) = events.get(index) else {
            continue;
        };
        let Some(value) = event.as_event::<ParamValueEvent>() else {
            continue;
        };
        if !value.pckn().matches_all() || !value.value().is_finite() {
            continue;
        }
        if frames.is_some_and(|frames| {
            if frames == 0 {
                value.header().time() != 0
            } else {
                value.header().time() >= frames
            }
        }) {
            continue;
        }
        if let Some(slot) = value.param_id().and_then(|id| final_values.get_mut(id.get() as usize)) {
            *slot = Some(value.value());
        }
    }
    for (id, value) in final_values.into_iter().enumerate() {
        if let Some(value) = value {
            apply_plain(parameters, id as u32, value);
        }
    }
}

/// Constructs metadata from the authoritative parameter registry without duplicating any parameter ranges.
pub(super) fn info(spec: ParameterSpec) -> ParamInfo<'static> {
    let mut flags = ParamInfoFlags::IS_AUTOMATABLE | ParamInfoFlags::REQUIRES_PROCESS;
    if spec.kind == ParameterKind::Switch {
        flags |= ParamInfoFlags::IS_STEPPED;
    }
    if spec.id == BYPASS_PARAMETER {
        flags |= ParamInfoFlags::IS_BYPASS;
    }
    ParamInfo {
        id: ClapId::new(spec.id),
        flags,
        cookie: Cookie::empty(),
        name: spec.title.as_bytes(),
        module: b"",
        min_value: spec.normalized_to_plain(0.0),
        max_value: spec.normalized_to_plain(1.0),
        default_value: spec.normalized_to_plain(spec.default_normalized),
    }
}

impl PluginMainThreadParams for MainThread<'_> {
    /// Reports the fixed append-only registry size.
    fn count(&self) -> u32 {
        PARAMETER_COUNT as u32
    }

    /// Writes stable numeric ids and plain-unit ranges, rather than VST3 normalized host values.
    fn get_info(&self, index: u32, writer: &mut ParamInfoWriter) {
        if let Some(spec) = parameter_spec(index) {
            writer.set(&info(spec));
        }
    }

    /// Reads independent parameter atomics without borrowing DSP or GUI state.
    fn get_value(&self, id: ClapId) -> Option<f64> {
        let spec = parameter_spec(id.get())?;
        Some(spec.normalized_to_plain(self.shared.instance.parameters.get(id.get())?))
    }

    /// Uses the same units, muted-gain labels, and signed pan labels as the existing editor.
    fn value_to_text(&self, id: ClapId, value: f64, writer: &mut ParamDisplayWriter) -> std::fmt::Result {
        let spec = parameter_spec(id.get())
            .filter(|_| value.is_finite())
            .ok_or(std::fmt::Error)?;
        writer.write_str(&spec.format(spec.plain_to_normalized(value)))
    }

    /// Parses finite range-checked text using the shared registry and returns CLAP plain units.
    fn text_to_value(&self, id: ClapId, text: &CStr) -> Option<f64> {
        let spec = parameter_spec(id.get())?;
        Some(spec.normalized_to_plain(spec.parse(text.to_str().ok()?)?))
    }

    /// Applies stopped automation without touching unprepared DSP history.
    fn flush(&self, input: &InputEvents, output: &mut OutputEvents) {
        apply_events(&self.shared.instance.parameters, input, None);
        #[cfg(feature = "editor")]
        self.shared.edits.flush(output, &self.shared.instance.parameters);
        #[cfg(not(feature = "editor"))]
        let _ = output;
    }
}

impl PluginAudioProcessorParams for AudioProcessor<'_> {
    /// Applies active flushes through atomics only, with the next audio block sampling the new targets.
    fn flush(&mut self, input: &InputEvents, output: &mut OutputEvents) {
        apply_events(&self.shared.instance.parameters, input, None);
        #[cfg(feature = "editor")]
        self.shared.edits.flush(output, &self.shared.instance.parameters);
        #[cfg(not(feature = "editor"))]
        let _ = output;
    }
}

/// Writes bounded progress explicitly because CLAP hosts may transfer partial stream chunks.
fn write_state(output: &mut impl Write, bytes: &[u8]) -> Result<(), PluginError> {
    let mut remaining = bytes;
    while !remaining.is_empty() {
        let written = output.write(remaining)?;
        if written == 0 || written > remaining.len() {
            return Err(PluginError::Message("CLAP state stream made invalid write progress"));
        }
        remaining = remaining
            .get(written..)
            .ok_or(PluginError::Message("Invalid CLAP state write length"))?;
    }
    Ok(())
}

/// Reads at most one bounded payload plus one overflow sentinel before admitting detached state.
fn read_state(input: &mut impl Read) -> Result<Vec<u8>, PluginError> {
    let mut bytes = vec![0; crate::state::MAXIMUM_STATE_BYTES + 1];
    let mut length = 0;
    while length < bytes.len() {
        let remaining = bytes
            .get_mut(length..)
            .ok_or(PluginError::Message("Invalid CLAP state read length"))?;
        let count = input.read(remaining)?;
        if count > bytes.len() - length {
            return Err(PluginError::Message("CLAP state stream made invalid read progress"));
        }
        if count == 0 {
            break;
        }
        length += count;
    }
    if length == 0 || length > crate::state::MAXIMUM_STATE_BYTES {
        return Err(PluginError::Message("CLAP state exceeds its bounded payload"));
    }
    bytes.truncate(length);
    Ok(bytes)
}

impl PluginStateImpl for MainThread<'_> {
    /// Serializes the common versioned JSON document without a format-specific envelope.
    fn save(&self, output: &mut OutputStream) -> Result<(), PluginError> {
        let instance = &self.shared.instance;
        let bytes = crate::state::save_bytes(&instance.parameters, &instance.shared, &instance.geometry)?;
        write_state(output, &bytes)
    }

    /// Validates the entire bounded payload before atomically publishing independent sound/display fields.
    fn load(&self, input: &mut InputStream) -> Result<(), PluginError> {
        let bytes = read_state(input)?;
        let instance = &self.shared.instance;
        crate::state::restore_bytes(&bytes, &instance.parameters, &instance.shared, &instance.geometry)?;
        if let Some(params) = self.host.get_extension::<HostParams>() {
            params.rescan(&self.host, ParamRescanFlags::VALUES);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every id's plain metadata and default come from the one stable registry.
    #[test]
    fn metadata_is_stable_plain_units_and_switches_are_stepped() {
        for id in 0..31 {
            let spec = parameter_spec(id).expect("stable id");
            let metadata = info(spec);
            assert_eq!(metadata.id.get(), id);
            assert_eq!(
                metadata.flags.contains(ParamInfoFlags::IS_STEPPED),
                spec.kind == ParameterKind::Switch
            );
            assert_eq!(
                metadata.flags.contains(ParamInfoFlags::IS_BYPASS),
                id == BYPASS_PARAMETER
            );
            assert_eq!(
                metadata.default_value.to_bits(),
                spec.normalized_to_plain(spec.default_normalized).to_bits()
            );
            assert!(metadata.min_value <= metadata.default_value && metadata.default_value <= metadata.max_value);
        }
        assert_eq!(
            (
                info(parameter_spec(0).expect("valid contract input")).min_value,
                info(parameter_spec(0).expect("valid contract input")).max_value
            ),
            (-24.0, 12.0)
        );
        assert_eq!(
            info(parameter_spec(1).expect("valid contract input")).max_value,
            20_000.0
        );
        assert_eq!(info(parameter_spec(7).expect("valid contract input")).max_value, 200.0);
    }

    /// The final valid global point wins, with unsupported targets and non-finite values ignored.
    #[test]
    fn automation_is_finite_block_sampled_and_bounded() {
        let parameters = Parameters::new();
        let event = |time, id, value| ParamValueEvent::new(time, ClapId::new(id), Pckn::match_all(), value);
        let events = [
            event(0, 0, -12.0),
            event(3, 0, 6.0),
            event(4, 0, f64::NAN),
            event(5, 0, 12.0),
        ];
        apply_events(&parameters, &InputEvents::from_buffer(&events), Some(5));
        assert_eq!(
            parameters.get(0),
            Some(
                parameter_spec(0)
                    .expect("valid contract input")
                    .plain_to_normalized(6.0)
            )
        );
        let before = parameters.get(0);
        assert!(!apply_plain(&parameters, 0, f64::INFINITY));
        assert!(!apply_plain(&parameters, 31, 0.0));
        assert_eq!(parameters.get(0), before);
        let events = vec![event(0, 0, -24.0); MAXIMUM_INPUT_EVENTS + 1];
        let mut events = events;
        events[MAXIMUM_INPUT_EVENTS] = event(0, 0, 12.0);
        apply_events(&parameters, &InputEvents::from_buffer(&events), Some(0));
        assert_eq!(parameters.get(0), Some(0.0));
        let targeted = [ParamValueEvent::new(
            0,
            ClapId::new(0),
            Pckn::new(0u16, 0u16, 60u16, 1u32),
            12.0,
        )];
        apply_events(&parameters, &InputEvents::from_buffer(&targeted), None);
        assert_eq!(parameters.get(0), Some(0.0));
    }

    /// Partial progress is supported, while zero writes, empty reads, and oversized payloads fail.
    #[test]
    fn state_streams_are_bounded_and_support_partial_progress() {
        struct Partial(Vec<u8>);
        impl Write for Partial {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                self.0.push(bytes[0]);
                Ok(1)
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let mut output = Partial(Vec::new());
        write_state(&mut output, b"state").expect("valid contract input");
        assert_eq!(output.0, b"state");
        assert_eq!(
            read_state(&mut output.0.as_slice()).expect("valid contract input"),
            b"state"
        );
        assert!(write_state(&mut [].as_mut_slice(), b"state").is_err());
        assert!(read_state(&mut std::io::empty()).is_err());
        assert!(read_state(&mut vec![0; crate::state::MAXIMUM_STATE_BYTES + 1].as_slice()).is_err());
    }
}
