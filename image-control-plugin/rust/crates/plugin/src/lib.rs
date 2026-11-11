//! Narrow VST3 and CLAP `ImageControl` adapters with shared atomic parameters and exclusive prepared DSP.
//!
//! The class identity and versioned state are independent of the C++ and experimental adapters.
//! Audio processing never enters the editor, calls host UI handlers, or initializes thread-local ownership.

// wgpu's retained Iced renderer exceeds nightly Rust's default auto-trait recursion depth.
// Raising the depth retains the full Send/lifetime analysis instead of disabling its diagnostic.
#![recursion_limit = "256"]

pub mod benchmark_build;
#[cfg(feature = "clap")]
mod clap;
#[cfg(feature = "editor")]
mod correlation_display;
#[cfg(feature = "editor")]
mod deferred_ui;
mod editing;
#[cfg(feature = "editor")]
mod editor;
mod editor_geometry;
mod editor_preferences;
mod host;
#[cfg(feature = "editor")]
mod iced_bridge;
#[cfg(feature = "editor")]
mod iced_executor;
pub mod params;
#[cfg(feature = "render-benchmark")]
pub mod render_benchmark;
mod runtime;
#[cfg(feature = "editor")]
mod spectrogram;
mod state;
#[cfg(feature = "editor")]
mod user_presets;

use vst3::Steinberg::{IPluginFactory, TUID};
use vst3::{ComPtr, ComWrapper, uid};

pub use params::{
    BAND_CONTROL_COUNT, BAND_PARAMETER_START, BYPASS_PARAMETER, BandSnapshot, CROSSOVER_PARAMETER_START,
    DELTA_PARAMETER, GAIN_PARAMETER, MONO_PARAMETER, NEUTRAL_BAND_GAIN, PARAMETER_COUNT, ParameterKind, ParameterSpec,
    Parameters, Snapshot, band_parameter_id, parameter_spec,
};

/// Independent component identifier, unrelated to either experimental adapter's persisted state.
pub const COMPONENT_ID: TUID = uid(0xA76D_95C1, 0x49E2_4D8A, 0xB3F1_617C, 0x8A2E_3904);
/// Product name shared by the factory and bundle.
pub const PLUGIN_NAME: &str = "ImageControl RS";
/// Bundle identity independent of every other `ImageControl` implementation.
pub const BUNDLE_ID: &str = "com.esgrove.imagecontrol-rs-own";
/// Manufacturer reported by the VST3 factory.
pub const PLUGIN_VENDOR: &str = "Esgrove";
/// Plugin version from the authoritative package metadata.
pub const PLUGIN_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Creates an owned in-process factory without loading a native module.
///
/// # Panics
///
/// The statically declared factory interface always exists.
#[must_use]
pub fn factory() -> ComPtr<IPluginFactory> {
    ComWrapper::new(host::factory::Factory)
        .to_com_ptr::<IPluginFactory>()
        .expect("Factory declares IPluginFactory")
}

/// Writes a terminated UTF-16 SDK field and clears unused entries.
fn strings_utf16(text: &str, output: &mut [u16]) {
    output.fill(0);
    let capacity = output.len().saturating_sub(1);
    for (target, unit) in output.iter_mut().take(capacity).zip(text.encode_utf16()) {
        *target = unit;
    }
}

/// Writes a terminated ASCII SDK field and clears unused entries.
fn strings_char8(text: &str, output: &mut [std::ffi::c_char]) {
    output.fill(0);
    let capacity = output.len().saturating_sub(1);
    for (target, byte) in output.iter_mut().take(capacity).zip(text.bytes()) {
        *target = std::ffi::c_char::from_ne_bytes([byte]);
    }
}
