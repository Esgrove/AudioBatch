//! Dev-only CLAP effect with independently specified float32 gain, mid/side width, and delay.
//!
//! Parameter events and state contain **plain** values, not normalized knob positions.
//! Gain and width span 0 through 2 with a default of 1, latency spans 0 through 1024 samples.
//! Latency is latched at activation, changes while active request a restart.
//! The fixed delay has no additional effect tail.
//! Mono and stereo configurations have one matching main input and output, stereo is the default.
//! Processing and parameter flushes allocate nothing, acquire no locks, and never wait.
//! Diagnostic failure, request, and sleep controls are transient and are not saved in the effect state.

mod bundle;
mod extensions;
mod processor;

use std::cell::Cell;
use std::ffi::CStr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use clack_extensions::audio_ports::PluginAudioPorts;
use clack_extensions::audio_ports_config::{PluginAudioPortsConfig, PluginAudioPortsConfigInfo};
use clack_extensions::latency::PluginLatency;
use clack_extensions::params::PluginParams;
use clack_extensions::render::{PluginRender, RenderMode};
use clack_extensions::state::PluginState;
use clack_extensions::tail::PluginTail;
use clack_host::entry::{PluginEntry, PluginEntryError};
use clack_plugin::events::event_types::ParamValueEvent;
use clack_plugin::events::io::InputEvents;
use clack_plugin::prelude::*;
use clack_plugin::utils::ClapId;

pub use bundle::{create_bundle, module_library_path};
pub use processor::AudioProcessor;

/// Stable identifier of the sole effect exposed by this entry.
pub const PLUGIN_ID: &CStr = c"com.audiobatch.tests.clap.mid-side";
/// Gain parameter, a plain linear multiplier from 0 through 2, default 1.
pub const GAIN_PARAMETER: ClapId = ClapId::new(0);
/// Stereo side multiplier from 0 through 2, default 1, ignored for mono.
pub const WIDTH_PARAMETER: ClapId = ClapId::new(1);
/// Integer delay in samples, from 0 through 1024, default 0.
pub const LATENCY_PARAMETER: ClapId = ClapId::new(2);
/// Nonzero makes process return an error, without affecting parameter flushes.
pub const PROCESS_FAILURE_PARAMETER: ClapId = ClapId::new(9001);
/// One-shot host requests, bits 0, 1, and 2 request restart, process, and main-thread callback.
/// The value immediately returns to zero after delivery.
pub const REQUEST_PARAMETER: ClapId = ClapId::new(9002);
/// Nonzero returns the CLAP sleep status after producing the current block, without blocking.
pub const SLEEP_PARAMETER: ClapId = ClapId::new(9003);
/// Configuration id for one mono input and output.
pub const MONO_CONFIG: ClapId = ClapId::new(1);
/// Configuration id for one stereo input and output, selected by default.
pub const STEREO_CONFIG: ClapId = ClapId::new(2);
/// Maximum gain and width in plain units.
pub const PARAMETER_MAXIMUM: f64 = 2.0;
/// Maximum delay in samples.
pub const LATENCY_MAXIMUM: u32 = 1024;
/// Entry type accepted directly by `PluginEntry::load_from_clack`.
pub type TestEntry = SinglePluginEntry<MidSidePlugin>;

/// The deterministic effect's type-level implementation and extension registry.
pub struct MidSidePlugin;

/// Independently owned parameter state shared by the plugin's lifecycle components.
pub struct Shared<'a> {
    pub(crate) state: Arc<ParameterState<'a>>,
}

/// Lock-free parameter values and a host handle, without references to Clack's owning wrapper.
pub(crate) struct ParameterState<'a> {
    pub(crate) host: HostSharedHandle<'a>,
    pub(crate) gain: AtomicU32,
    pub(crate) width: AtomicU32,
    pub(crate) latency: AtomicU32,
    pub(crate) active_latency: AtomicU32,
    pub(crate) active: AtomicBool,
    pub(crate) fail: AtomicBool,
    pub(crate) sleep: AtomicBool,
}

/// Main-thread-only configuration and state access, never used by the audio callback.
pub struct MainThread<'a> {
    pub(crate) state: Arc<ParameterState<'a>>,
    pub(crate) host: HostMainThreadHandle<'a>,
    pub(crate) channels: Cell<u32>,
    pub(crate) render_mode: Cell<RenderMode>,
}

impl Plugin for MidSidePlugin {
    type AudioProcessor<'a> = AudioProcessor<'a>;
    type Shared<'a> = Shared<'a>;
    type MainThread<'a> = MainThread<'a>;

    fn declare_extensions(builder: &mut PluginExtensions<Self>, _shared: Option<&Shared<'_>>) {
        builder
            .register::<PluginAudioPorts>()
            .register::<PluginAudioPortsConfig>()
            .register::<PluginAudioPortsConfigInfo>()
            .register::<PluginParams>()
            .register::<PluginState>()
            .register::<PluginLatency>()
            .register::<PluginTail>()
            .register::<PluginRender>();
    }
}

impl DefaultPluginFactory for MidSidePlugin {
    fn get_descriptor() -> PluginDescriptor {
        PluginDescriptor::new(&PLUGIN_ID.to_string_lossy(), "AudioBatch Test MS CLAP")
            .with_vendor("AudioBatch Tests")
            .with_features([c"audio-effect", c"stereo", c"mono"])
    }

    fn new_shared(host: HostSharedHandle<'_>) -> Result<Shared<'_>, PluginError> {
        Ok(Shared {
            state: Arc::new(ParameterState {
                host,
                gain: AtomicU32::new(1.0_f32.to_bits()),
                width: AtomicU32::new(1.0_f32.to_bits()),
                latency: AtomicU32::new(0),
                active_latency: AtomicU32::new(0),
                active: AtomicBool::new(false),
                fail: AtomicBool::new(false),
                sleep: AtomicBool::new(false),
            }),
        })
    }

    fn new_main_thread<'a>(
        host: HostMainThreadHandle<'a>,
        shared: &'a Shared<'a>,
    ) -> Result<MainThread<'a>, PluginError> {
        Ok(MainThread {
            state: Arc::clone(&shared.state),
            host,
            channels: Cell::new(2),
            render_mode: Cell::new(RenderMode::Realtime),
        })
    }
}

impl ParameterState<'_> {
    /// Applies finite plain parameter values, requesting reactivation for a changed active latency.
    pub(crate) fn set(&self, id: ClapId, value: f64) {
        if !value.is_finite() {
            return;
        }
        match id {
            GAIN_PARAMETER => self.gain.store(
                (value.clamp(0.0, PARAMETER_MAXIMUM) as f32).to_bits(),
                Ordering::Relaxed,
            ),
            WIDTH_PARAMETER => self.width.store(
                (value.clamp(0.0, PARAMETER_MAXIMUM) as f32).to_bits(),
                Ordering::Relaxed,
            ),
            LATENCY_PARAMETER => {
                let latency = value.clamp(0.0, f64::from(LATENCY_MAXIMUM)).round() as u32;
                let old = self.latency.swap(latency, Ordering::Relaxed);
                if old != latency && self.active.load(Ordering::Relaxed) {
                    self.host.request_restart();
                }
            }
            PROCESS_FAILURE_PARAMETER => self.fail.store(value >= 0.5, Ordering::Relaxed),
            SLEEP_PARAMETER => self.sleep.store(value >= 0.5, Ordering::Relaxed),
            REQUEST_PARAMETER => {
                let requests = value.clamp(0.0, 7.0) as u32;
                if requests & 1 != 0 {
                    self.host.request_restart();
                }
                if requests & 2 != 0 {
                    self.host.request_process();
                }
                if requests & 4 != 0 {
                    self.host.request_callback();
                }
            }
            _ => {}
        }
    }

    /// Delivers one global parameter event, ignoring note-targeted modulation and unknown events.
    pub(crate) fn apply(&self, event: &UnknownEvent) {
        if let Some(event) = event.as_event::<ParamValueEvent>()
            && event.pckn() == Pckn::match_all()
            && let Some(id) = event.param_id()
        {
            self.set(id, event.value());
        }
    }

    /// Delivers all pending changes without allocating an intermediate event collection.
    pub(crate) fn flush(&self, input: &InputEvents) {
        for event in input {
            self.apply(event);
        }
    }
}

impl<'a> PluginShared<'a> for Shared<'a> {}
impl<'a> PluginMainThread<'a, Shared<'a>> for MainThread<'a> {}

/// Loads the fixture in process using only Clack's safe host and plugin interfaces.
/// No plugin installation, file access, or audio device is needed.
///
/// # Errors
/// Returns Clack's entry initialization error if entry creation fails.
pub fn entry() -> Result<PluginEntry, PluginEntryError> {
    PluginEntry::load_from_clack::<TestEntry>(c"audiobatch-test-clap-plugin.clap")
}

/// Converts a normalized gain or width knob position to a plain CLAP value.
#[must_use]
pub fn gain_width_plain(normalized: f64) -> f64 {
    normalized.clamp(0.0, 1.0) * PARAMETER_MAXIMUM
}

/// Converts a normalized latency knob position to plain integer samples, rounding to the nearest sample.
#[must_use]
pub fn latency_plain(normalized: f64) -> f64 {
    (normalized.clamp(0.0, 1.0) * f64::from(LATENCY_MAXIMUM)).round()
}
