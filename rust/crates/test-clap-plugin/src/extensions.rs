//! CLAP parameter, state, channel-configuration, latency, and render extension implementations.

use std::ffi::CStr;
use std::fmt::{self, Write as _};
use std::io::{Read as _, Write as _};
use std::sync::atomic::Ordering;

use clack_extensions::audio_ports::{
    AudioPortFlags, AudioPortInfo, AudioPortInfoWriter, AudioPortType, PluginAudioPortsImpl,
};
use clack_extensions::audio_ports_config::{
    AudioPortConfigWriter, AudioPortsConfiguration, MainPortInfo, PluginAudioPortsConfigImpl,
    PluginAudioPortsConfigInfoImpl,
};
use clack_extensions::latency::PluginLatencyImpl;
use clack_extensions::params::{
    HostParams, ParamDisplayWriter, ParamInfo, ParamInfoFlags, ParamInfoWriter, ParamRescanFlags,
    PluginMainThreadParams,
};
use clack_extensions::render::{PluginRenderImpl, RenderMode};
use clack_extensions::state::PluginStateImpl;
use clack_plugin::prelude::*;
use clack_plugin::stream::{InputStream, OutputStream};
use clack_plugin::utils::Cookie;

use crate::{
    GAIN_PARAMETER, LATENCY_MAXIMUM, LATENCY_PARAMETER, MONO_CONFIG, MainThread, PARAMETER_MAXIMUM,
    PROCESS_FAILURE_PARAMETER, REQUEST_PARAMETER, SLEEP_PARAMETER, STEREO_CONFIG, WIDTH_PARAMETER,
};

/// Versioned fixed-size state magic, followed by three little-endian float32 plain values.
const STATE_MAGIC: [u8; 8] = *b"ABCLAP01";
/// Number of user-facing parameters plus diagnostic controls.
const PARAMETER_COUNT: u32 = 6;

impl PluginMainThreadParams for MainThread<'_> {
    fn count(&self) -> u32 {
        PARAMETER_COUNT
    }

    fn get_info(&self, index: u32, writer: &mut ParamInfoWriter) {
        if let Some(info) = parameter_info(index) {
            writer.set(&info);
        }
    }

    fn get_value(&self, id: ClapId) -> Option<f64> {
        Some(match id {
            GAIN_PARAMETER => f64::from(f32::from_bits(self.state.gain.load(Ordering::Relaxed))),
            WIDTH_PARAMETER => f64::from(f32::from_bits(self.state.width.load(Ordering::Relaxed))),
            LATENCY_PARAMETER => f64::from(self.state.latency.load(Ordering::Relaxed)),
            PROCESS_FAILURE_PARAMETER => f64::from(u8::from(self.state.fail.load(Ordering::Relaxed))),
            SLEEP_PARAMETER => f64::from(u8::from(self.state.sleep.load(Ordering::Relaxed))),
            REQUEST_PARAMETER => 0.0,
            _ => return None,
        })
    }

    fn value_to_text(&self, id: ClapId, value: f64, writer: &mut ParamDisplayWriter) -> fmt::Result {
        if self.get_value(id).is_none() || !value.is_finite() {
            return Err(fmt::Error);
        }
        if id == LATENCY_PARAMETER {
            write!(writer, "{value:.0}")
        } else {
            write!(writer, "{value:.3}")
        }
    }

    fn text_to_value(&self, id: ClapId, text: &CStr) -> Option<f64> {
        self.get_value(id)?;
        let value: f64 = text.to_str().ok()?.trim().parse().ok()?;
        let info = (0..PARAMETER_COUNT)
            .filter_map(parameter_info)
            .find(|info| info.id == id)?;
        (value.is_finite() && (info.min_value..=info.max_value).contains(&value)).then_some(value)
    }

    fn flush(&self, input: &InputEvents, _output: &mut OutputEvents) {
        self.state.flush(input);
    }
}

impl PluginAudioPortsImpl for MainThread<'_> {
    fn count(&self, _is_input: bool) -> u32 {
        1
    }

    fn get(&self, index: u32, is_input: bool, writer: &mut AudioPortInfoWriter) {
        if index == 0 {
            write_port(self.channels.get(), is_input, writer);
        }
    }
}

impl PluginAudioPortsConfigImpl for MainThread<'_> {
    fn count(&self) -> u32 {
        2
    }

    fn get(&self, index: u32, writer: &mut AudioPortConfigWriter) {
        let (id, name, channels) = match index {
            0 => (MONO_CONFIG, b"Mono".as_slice(), 1),
            1 => (STEREO_CONFIG, b"Stereo".as_slice(), 2),
            _ => return,
        };
        let port = Some(MainPortInfo {
            channel_count: channels,
            port_type: AudioPortType::from_channel_count(channels),
        });
        writer.write(&AudioPortsConfiguration {
            id,
            name,
            input_port_count: 1,
            output_port_count: 1,
            main_input: port,
            main_output: port,
        });
    }

    fn select(&self, id: ClapId) -> Result<(), PluginError> {
        let channels = configuration_channels(id).ok_or(PluginError::Message("unknown port configuration"))?;
        self.channels.set(channels);
        Ok(())
    }
}

impl PluginAudioPortsConfigInfoImpl for MainThread<'_> {
    fn current_config(&self) -> Option<ClapId> {
        Some(ClapId::new(self.channels.get()))
    }

    fn get(&self, id: ClapId, index: u32, is_input: bool, writer: &mut AudioPortInfoWriter) {
        if index == 0
            && let Some(channels) = configuration_channels(id)
        {
            write_port(channels, is_input, writer);
        }
    }
}

impl PluginLatencyImpl for MainThread<'_> {
    fn get(&self) -> u32 {
        self.state.active_latency.load(Ordering::Relaxed)
    }
}

impl PluginRenderImpl for MainThread<'_> {
    fn has_hard_realtime_requirement(&self) -> bool {
        false
    }

    fn set(&self, mode: RenderMode) -> Result<(), PluginError> {
        self.render_mode.set(mode);
        Ok(())
    }
}

impl PluginStateImpl for MainThread<'_> {
    fn save(&self, output: &mut OutputStream) -> Result<(), PluginError> {
        output.write_all(&STATE_MAGIC)?;
        for id in [GAIN_PARAMETER, WIDTH_PARAMETER, LATENCY_PARAMETER] {
            let value = self
                .get_value(id)
                .ok_or(PluginError::Message("missing state parameter"))?;
            output.write_all(&(value as f32).to_le_bytes())?;
        }
        Ok(())
    }

    fn load(&self, input: &mut InputStream) -> Result<(), PluginError> {
        let mut magic = [0; STATE_MAGIC.len()];
        input.read_exact(&mut magic)?;
        if magic != STATE_MAGIC {
            return Err(PluginError::Message("invalid fixture state header"));
        }
        let mut values = [0.0_f32; 3];
        for value in &mut values {
            let mut bytes = [0; 4];
            input.read_exact(&mut bytes)?;
            *value = f32::from_le_bytes(bytes);
        }
        for (id, value) in [GAIN_PARAMETER, WIDTH_PARAMETER, LATENCY_PARAMETER]
            .into_iter()
            .zip(values)
        {
            let maximum = if id == LATENCY_PARAMETER {
                f64::from(LATENCY_MAXIMUM)
            } else {
                PARAMETER_MAXIMUM
            };
            if !value.is_finite()
                || !(0.0..=maximum).contains(&f64::from(value))
                || (id == LATENCY_PARAMETER && value.fract().abs() > f32::EPSILON)
            {
                return Err(PluginError::Message("invalid fixture state parameter"));
            }
        }
        let mut trailing = [0];
        if input.read(&mut trailing)? != 0 {
            return Err(PluginError::Message("trailing fixture state data"));
        }
        for (id, value) in [GAIN_PARAMETER, WIDTH_PARAMETER, LATENCY_PARAMETER]
            .into_iter()
            .zip(values)
        {
            self.state.set(id, f64::from(value));
        }
        if let Some(params) = self.host.get_extension::<HostParams>() {
            params.rescan(&self.host, ParamRescanFlags::VALUES);
        }
        Ok(())
    }
}

/// Returns the immutable metadata without conflating plain values with normalized host controls.
fn parameter_info(index: u32) -> Option<ParamInfo<'static>> {
    let (id, name, maximum, default, stepped) = match index {
        0 => (GAIN_PARAMETER, b"Gain".as_slice(), PARAMETER_MAXIMUM, 1.0, false),
        1 => (WIDTH_PARAMETER, b"Width".as_slice(), PARAMETER_MAXIMUM, 1.0, false),
        2 => (
            LATENCY_PARAMETER,
            b"Latency".as_slice(),
            f64::from(LATENCY_MAXIMUM),
            0.0,
            true,
        ),
        3 => (PROCESS_FAILURE_PARAMETER, b"Process failure".as_slice(), 1.0, 0.0, true),
        4 => (REQUEST_PARAMETER, b"Host requests".as_slice(), 7.0, 0.0, true),
        5 => (SLEEP_PARAMETER, b"Sleep status".as_slice(), 1.0, 0.0, true),
        _ => return None,
    };
    let mut flags = ParamInfoFlags::IS_AUTOMATABLE | ParamInfoFlags::REQUIRES_PROCESS;
    if stepped {
        flags |= ParamInfoFlags::IS_STEPPED;
    }
    Some(ParamInfo {
        id,
        flags,
        cookie: Cookie::empty(),
        name,
        module: b"",
        min_value: 0.0,
        max_value: maximum,
        default_value: default,
    })
}

/// Maps stable layout identifiers to their matching input/output channel counts.
fn configuration_channels(id: ClapId) -> Option<u32> {
    match id {
        MONO_CONFIG => Some(1),
        STEREO_CONFIG => Some(2),
        _ => None,
    }
}

/// Describes matching input and output ids so Clack can safely deliver in-place channel pairs.
fn write_port(channels: u32, is_input: bool, writer: &mut AudioPortInfoWriter) {
    writer.set(&AudioPortInfo {
        id: ClapId::new(0),
        name: if is_input { b"Input" } else { b"Output" },
        channel_count: channels,
        flags: AudioPortFlags::IS_MAIN,
        port_type: AudioPortType::from_channel_count(channels),
        in_place_pair: Some(ClapId::new(0)),
    });
}
