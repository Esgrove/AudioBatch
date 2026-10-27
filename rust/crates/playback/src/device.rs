//! Output device selection and stream configuration.
//!
//! `OutputConfig` is the device, sample rate, and buffer size chosen in the settings,
//! each falling back to what the device prefers when unset.
//! `DeviceCapabilities` lists the common sample rates and buffer sizes a device supports,
//! for the settings window to offer.

use anyhow::{Context, Result, anyhow};
use cpal::traits::{DeviceTrait, HostTrait};
use cpal::{BufferSize, Device, SampleFormat, StreamConfig, SupportedBufferSize, SupportedStreamConfigRange};

/// Sample rates offered when the device supports them.
const COMMON_SAMPLE_RATES: [u32; 6] = [44_100, 48_000, 88_200, 96_000, 176_400, 192_000];

/// Buffer sizes in frames offered when the device supports them.
const COMMON_BUFFER_SIZES: [u32; 7] = [64, 128, 256, 512, 1024, 2048, 4096];

/// The output device, sample rate, and buffer size to open, with `None` for what the device prefers.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct OutputConfig {
    /// Device name as reported by the audio host, or the system default device.
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
pub fn device_capabilities(device_name: Option<&str>) -> Result<DeviceCapabilities> {
    let device = find_device(device_name)?;
    let default = device
        .default_output_config()
        .context("Could not read the output device configuration")?;
    let ranges: Vec<SupportedStreamConfigRange> = device
        .supported_output_configs()
        .context("Could not read the supported output configurations")?
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

/// Finds the named output device, or the default one.
pub fn find_device(device_name: Option<&str>) -> Result<Device> {
    let host = cpal::default_host();
    match device_name {
        Some(name) => host
            .output_devices()?
            .find(|device| device_display_name(device) == name)
            .ok_or_else(|| anyhow!("Output device \"{name}\" not found")),
        None => host
            .default_output_device()
            .ok_or_else(|| anyhow!("No audio output device available")),
    }
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
pub fn stream_config(device: &Device, config: &OutputConfig) -> Result<(StreamConfig, SampleFormat)> {
    let default = device
        .default_output_config()
        .context("Could not read the output device configuration")?;
    let supported = match config.sample_rate {
        Some(rate) if rate != default.sample_rate() => device
            .supported_output_configs()
            .context("Could not read the supported output configurations")?
            .filter(|range| range.channels() == default.channels() && range.sample_format() == default.sample_format())
            .find_map(|range| range.try_with_sample_rate(rate))
            .ok_or_else(|| anyhow!("The output device does not support {rate} Hz"))?,
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
