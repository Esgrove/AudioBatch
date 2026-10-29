//! Output device selection and stream configuration.
//!
//! `OutputConfig` is the device, sample rate, and buffer size chosen in the settings,
//! each falling back to what the device prefers when unset.
//! `DeviceCapabilities` lists the common sample rates and buffer sizes a device supports,
//! for the settings window to offer.
//! Windows uses ASIO, and other platforms use their default audio host.

#[cfg(windows)]
use std::cell::OnceCell;

use cpal::traits::{DeviceTrait, HostTrait};
use cpal::{
    BufferSize, Device, SampleFormat, StreamConfig, SupportedBufferSize, SupportedStreamConfig,
    SupportedStreamConfigRange,
};
use thiserror::Error;

/// Sample rates offered when the device supports them.
const COMMON_SAMPLE_RATES: [u32; 6] = [44_100, 48_000, 88_200, 96_000, 176_400, 192_000];

/// Buffer sizes in frames offered when the device supports them.
const COMMON_BUFFER_SIZES: [u32; 7] = [64, 128, 256, 512, 1024, 2048, 4096];

#[cfg(windows)]
thread_local! {
    /// ASIO discovery loads each driver, which fails while a different driver has an open stream.
    /// Keep the discovered device metadata on the player thread, without keeping any driver open.
    static ASIO_OUTPUT_DEVICES: OnceCell<Vec<Device>> = const { OnceCell::new() };
}

/// Failures while selecting an output device or reading its stream configuration.
#[derive(Debug, Error)]
pub enum DeviceError {
    /// The named device is not among the host's outputs.
    #[error("Output device \"{name}\" not found")]
    NotFound {
        /// Requested device name.
        name: String,
    },
    /// The host has no default output.
    #[cfg(windows)]
    #[error("No ASIO audio output device available. Install an ASIO driver for your audio interface.")]
    NoOutput,
    /// The host has no default output.
    #[cfg(not(windows))]
    #[error("No audio output device available")]
    NoOutput,
    /// The ASIO host could not be initialized.
    #[cfg(windows)]
    #[error("Could not initialize the ASIO audio host")]
    InitializeHost(#[source] cpal::Error),
    /// The host could not enumerate outputs.
    #[cfg(windows)]
    #[error("Could not list ASIO output devices")]
    ListOutputs(#[source] cpal::Error),
    /// The host could not enumerate outputs.
    #[cfg(not(windows))]
    #[error("Could not list output devices")]
    ListOutputs(#[source] cpal::Error),
    /// The device did not provide its default stream configuration.
    #[error("Could not read the output device configuration")]
    DefaultConfiguration(#[source] cpal::Error),
    /// The device did not provide its supported configurations.
    #[error("Could not read the supported output configurations")]
    SupportedConfigurations(#[source] cpal::Error),
    /// The requested rate is not supported.
    #[error("The output device does not support {rate} Hz")]
    UnsupportedSampleRate {
        /// Requested sample rate in hertz.
        rate: u32,
    },
    /// No configuration has a usable sample rate.
    #[error("The output device does not report a supported sample rate")]
    NoSupportedSampleRate,
}

/// The output device, sample rate, and buffer size to open, with `None` for what the device prefers.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct OutputConfig {
    /// Device name as reported by the audio host, or its default output device.
    pub device: Option<String>,
    /// Sample rate in hertz, or the device default.
    pub sample_rate: Option<u32>,
    /// Buffer size in frames, or the device default.
    pub buffer_size: Option<u32>,
}

/// Sample rates and buffer sizes an output device supports.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DeviceCapabilities {
    /// Sample rate the device uses by default, in hertz.
    pub default_sample_rate: u32,
    /// Supported common sample rates in ascending order, including the default.
    pub sample_rates: Vec<u32>,
    /// Supported common buffer sizes in frames, empty when the device does not report a range.
    pub buffer_sizes: Vec<u32>,
}

/// Returns the supported sample rates and buffer sizes of the named device, or of the default device.
///
/// # Errors
/// Returns an error when the device cannot be found or does not report its configuration.
pub fn device_capabilities(device_name: Option<&str>) -> Result<DeviceCapabilities, DeviceError> {
    let device = find_device(device_name)?;
    let default = default_output_config(&device)?;
    let ranges: Vec<SupportedStreamConfigRange> = device
        .supported_output_configs()
        .map_err(DeviceError::SupportedConfigurations)?
        .filter(|range| range.channels() == default.channels() && range.sample_format() == default.sample_format())
        .collect();
    let rate_ranges: Vec<(u32, u32)> = ranges
        .iter()
        .map(|range| (range.min_sample_rate(), range.max_sample_rate()))
        .collect();
    let buffer_range = match default.buffer_size() {
        SupportedBufferSize::Range { min, max } => Some((*min, *max)),
        SupportedBufferSize::Unknown => None,
    };
    Ok(DeviceCapabilities {
        default_sample_rate: default.sample_rate(),
        sample_rates: supported_sample_rates(&rate_ranges, default.sample_rate()),
        buffer_sizes: buffer_range.map_or_else(Vec::new, |(min, max)| supported_buffer_sizes(min, max)),
    })
}

/// Finds the named output device, or the host default, which is the first ASIO output on Windows.
pub fn find_device(device_name: Option<&str>) -> Result<Device, DeviceError> {
    if let Some(name) = device_name {
        output_devices()?
            .into_iter()
            .find(|device| device_display_name(device) == name)
            .ok_or_else(|| DeviceError::NotFound { name: name.to_string() })
    } else {
        #[cfg(windows)]
        let device = output_devices()?.into_iter().next();
        #[cfg(not(windows))]
        let device = cpal::default_host().default_output_device();

        device.ok_or(DeviceError::NoOutput)
    }
}

/// Lists outputs from the playback host.
/// ASIO discovery is cached after the first nonempty result, so menus can query it during playback.
pub fn output_devices() -> Result<Vec<Device>, DeviceError> {
    #[cfg(windows)]
    {
        ASIO_OUTPUT_DEVICES.with(|cached| {
            if let Some(devices) = cached.get() {
                return Ok(devices.clone());
            }
            let host = cpal::host_from_id(cpal::HostId::Asio).map_err(DeviceError::InitializeHost)?;
            let devices: Vec<Device> = host.output_devices().map_err(DeviceError::ListOutputs)?.collect();
            if !devices.is_empty() {
                let _ = cached.set(devices.clone());
            }
            Ok(devices)
        })
    }
    #[cfg(not(windows))]
    cpal::default_host()
        .output_devices()
        .map_err(DeviceError::ListOutputs)
        .map(Iterator::collect)
}

/// Returns the name of a device for menus and settings.
pub fn device_display_name(device: &Device) -> String {
    device.description().map_or_else(
        |_| String::from("Unknown device"),
        |description| description.name().to_string(),
    )
}

/// Chooses the stream configuration for an output config.
/// The default configuration of the device is used where the config leaves a value unset,
/// and a requested buffer size outside the range the device reports falls back to the default.
///
/// # Errors
/// Returns an error when the device has no configuration or does not support the requested sample rate.
pub fn stream_config(device: &Device, config: &OutputConfig) -> Result<(StreamConfig, SampleFormat), DeviceError> {
    let default = default_output_config(device)?;
    let supported = match config.sample_rate {
        Some(rate) if rate != default.sample_rate() => device
            .supported_output_configs()
            .map_err(DeviceError::SupportedConfigurations)?
            .filter(|range| range.channels() == default.channels() && range.sample_format() == default.sample_format())
            .find_map(|range| range.try_with_sample_rate(rate))
            .ok_or(DeviceError::UnsupportedSampleRate { rate })?,
        _ => default,
    };
    let mut stream_config = supported.config();
    if let Some(frames) = config.buffer_size
        && let SupportedBufferSize::Range { min, max } = supported.buffer_size()
        && (*min..=*max).contains(&frames)
    {
        stream_config.buffer_size = BufferSize::Fixed(frames);
    }
    Ok((stream_config, supported.sample_format()))
}

/// Reads the preferred output configuration, choosing a supported rate if the driver reports an unknown rate.
fn default_output_config(device: &Device) -> Result<SupportedStreamConfig, DeviceError> {
    let default = device
        .default_output_config()
        .map_err(DeviceError::DefaultConfiguration)?;
    if default.sample_rate() != 0 {
        return Ok(default);
    }
    fallback_output_config(
        device
            .supported_output_configs()
            .map_err(DeviceError::SupportedConfigurations)?
            .filter(|range| range.channels() == default.channels() && range.sample_format() == default.sample_format()),
    )
}

/// Chooses a supported nonzero sample rate, preferring 48 kHz, then 44.1 kHz, then the highest supported rate.
/// Some ASIO drivers cannot report their current rate before a stream starts.
fn fallback_output_config(
    ranges: impl Iterator<Item = SupportedStreamConfigRange>,
) -> Result<SupportedStreamConfig, DeviceError> {
    let range = ranges
        .filter(|range| range.max_sample_rate() > 0)
        .max_by(SupportedStreamConfigRange::cmp_default_heuristics)
        .ok_or(DeviceError::NoSupportedSampleRate)?;
    Ok(range
        .try_with_standard_sample_rate()
        .unwrap_or_else(|| range.with_max_sample_rate()))
}

/// Returns the common sample rates inside any of the supported ranges, with the default rate included.
fn supported_sample_rates(ranges: &[(u32, u32)], default_rate: u32) -> Vec<u32> {
    let mut rates: Vec<u32> = COMMON_SAMPLE_RATES
        .into_iter()
        .filter(|rate| ranges.iter().any(|&(min, max)| (min..=max).contains(rate)))
        .collect();
    if !rates.contains(&default_rate) {
        rates.push(default_rate);
        rates.sort_unstable();
    }
    rates
}

/// Returns the common buffer sizes inside the supported range.
fn supported_buffer_sizes(min: u32, max: u32) -> Vec<u32> {
    COMMON_BUFFER_SIZES
        .into_iter()
        .filter(|frames| (min..=max).contains(frames))
        .collect()
}

#[cfg(test)]
mod test_output_config {
    use super::*;

    /// Builds a stereo float configuration range for a driver whose current rate is unknown.
    fn range(minimum_rate: u32, maximum_rate: u32) -> SupportedStreamConfigRange {
        SupportedStreamConfigRange::new(
            2,
            minimum_rate,
            maximum_rate,
            SupportedBufferSize::Range { min: 64, max: 1024 },
            SampleFormat::F32,
        )
    }

    #[test]
    fn unknown_rate_prefers_standard_rates() {
        let selected = fallback_output_config([range(44_100, 44_100), range(48_000, 48_000)].into_iter())
            .expect("a supported default rate");
        assert_eq!(selected.sample_rate(), 48_000);
        assert_eq!(selected.channels(), 2);
        assert_eq!(selected.sample_format(), SampleFormat::F32);
        assert_eq!(
            *selected.buffer_size(),
            SupportedBufferSize::Range { min: 64, max: 1024 }
        );

        let selected = fallback_output_config([range(44_100, 44_100), range(96_000, 96_000)].into_iter())
            .expect("the CD rate is supported");
        assert_eq!(selected.sample_rate(), 44_100);
    }

    #[test]
    fn unknown_rate_uses_the_highest_rate_without_a_standard_rate() {
        let selected = fallback_output_config([range(0, 0), range(88_200, 96_000)].into_iter())
            .expect("an unusual rate is supported");
        assert_eq!(selected.sample_rate(), 96_000);
    }

    #[test]
    fn unknown_rate_requires_a_nonzero_supported_rate() {
        for error in [
            fallback_output_config(std::iter::empty()),
            fallback_output_config([range(0, 0)].into_iter()),
        ] {
            let error = error.expect_err("a nonzero sample rate is required");
            assert!(matches!(error, DeviceError::NoSupportedSampleRate));
            assert_eq!(
                error.to_string(),
                "The output device does not report a supported sample rate"
            );
        }
    }

    #[test]
    fn device_selection_errors_preserve_the_requested_values() {
        let error = DeviceError::NotFound {
            name: String::from("Studio"),
        };
        assert_eq!(error.to_string(), "Output device \"Studio\" not found");
        let error = DeviceError::UnsupportedSampleRate { rate: 32_000 };
        assert_eq!(error.to_string(), "The output device does not support 32000 Hz");
    }

    #[test]
    fn configuration_errors_preserve_the_native_source() {
        let error = DeviceError::DefaultConfiguration(cpal::Error::new(cpal::ErrorKind::DeviceNotAvailable));
        assert_eq!(error.to_string(), "Could not read the output device configuration");
        let source = std::error::Error::source(&error)
            .and_then(|source| source.downcast_ref::<cpal::Error>())
            .expect("native device source");
        assert_eq!(source.kind(), cpal::ErrorKind::DeviceNotAvailable);
    }
}

#[cfg(test)]
mod test_capabilities {
    use super::*;

    #[test]
    fn sample_rates_follow_the_supported_ranges() {
        assert_eq!(
            supported_sample_rates(&[(44_100, 48_000)], 48_000),
            vec![44_100, 48_000]
        );
        assert_eq!(
            supported_sample_rates(&[(8_000, 192_000)], 48_000),
            vec![44_100, 48_000, 88_200, 96_000, 176_400, 192_000]
        );
    }

    #[test]
    fn an_unusual_default_rate_is_still_offered() {
        assert_eq!(supported_sample_rates(&[(32_000, 32_000)], 32_000), vec![32_000]);
        assert_eq!(
            supported_sample_rates(&[(44_100, 44_100)], 22_050),
            vec![22_050, 44_100]
        );
    }

    #[test]
    fn buffer_sizes_follow_the_supported_range() {
        assert_eq!(supported_buffer_sizes(100, 1000), vec![128, 256, 512]);
        assert_eq!(supported_buffer_sizes(15, 4096), COMMON_BUFFER_SIZES.to_vec());
    }
}
