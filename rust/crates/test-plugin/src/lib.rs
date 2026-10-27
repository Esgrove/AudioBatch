//! Fake VST3 plugins for testing the host without real plugins.
//!
//! Every kind of plugin has a gain parameter and a separate controller connected through connection points.
//! The kinds differ in what they do to the audio and which bus arrangements they accept:
//! a plain gain, a delay that reports its latency, an echo with a tail and a sidechain input,
//! a plugin that outputs NaN, mono-only and stereo-only plugins, a recorder that logs every call it gets,
//! and a plugin whose processing and component state calls fail.
//!
//! `factory()` returns the plugin factory for use inside the test process.
//! The `module` example builds the same factory into a library with the VST3 entry points,
//! which tests wrap into a `.vst3` bundle with `create_bundle`.

#![expect(unsafe_code, reason = "implements the VST3 COM interfaces of the fake plugins")]

mod bundle;
mod controller;
mod factory;
mod processor;
mod text;

use std::sync::{Mutex, PoisonError};
use std::thread::ThreadId;

use vst3::Steinberg::{IPluginFactory, TUID};
use vst3::{ComPtr, ComWrapper, uid};

pub use bundle::{create_bundle, module_library_path};

/// Parameter id of the gain, a normalized value used directly as the linear gain.
pub const GAIN_PARAMETER: u32 = 0;

/// Parameter id that makes the controller report an edit of the gain, as if its editor changed it.
/// Setting it with `setParamNormalized` calls `performEdit` for the gain with the same value.
pub const EDIT_TRIGGER_PARAMETER: u32 = 1;

/// Latency of the delay plugin, in samples.
pub const DELAY_LATENCY: u32 = 64;

/// Distance between the input and its single echo in the echo plugin, in samples, which is also its tail.
pub const ECHO_DELAY: u32 = 100;

/// Level of the echo relative to the input.
pub const ECHO_LEVEL: f32 = 0.5;

/// The controller state every controller starts with.
pub const INITIAL_CONTROLLER_STATE: [u8; 4] = *b"ctrl";

/// Bytes of its gain state that the failing plugin writes before its `getState` reports a failure.
pub const FAILING_STATE_BYTES: usize = 4;

/// Vendor reported by the factory.
pub const VENDOR: &str = "AudioBatch Tests";

/// Class id of the controller shared by all kinds except the recorder.
const CONTROLLER_ID: TUID = uid(0x5A1C_0001, 0x0000_4000, 0x8000_0000, 0x0000_0001);

/// Class id of the recorder's controller, which logs its calls too.
const RECORDER_CONTROLLER_ID: TUID = uid(0x5A1C_0001, 0x0000_4000, 0x8000_0000, 0x0000_0002);

/// Calls logged by recorder plugins.
static RECORDED_CALLS: Mutex<Vec<RecordedCall>> = Mutex::new(Vec::new());

/// What a fake plugin does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// Multiplies the input by the gain parameter, in mono or stereo.
    Gain,
    /// Delays the input by `DELAY_LATENCY` samples and reports that latency.
    Delay,
    /// Adds one echo `ECHO_DELAY` samples later and reports that as its tail.
    /// It also has a mono sidechain input bus.
    Echo,
    /// Outputs NaN.
    NotFinite,
    /// Accepts only a mono arrangement.
    MonoOnly,
    /// Accepts only a stereo arrangement.
    StereoOnly,
    /// A gain that logs every call to its component, processor and controller.
    Recorder,
    /// A gain whose `process` fails for every block with samples,
    /// whose component `setState` refuses every state without reading it,
    /// and whose component `getState` fails after writing the first `FAILING_STATE_BYTES` bytes of the gain.
    Failing,
}

/// One call logged by a recorder plugin.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RecordedCall {
    /// The interface method, such as "component.initialize".
    pub name: &'static str,
    /// The thread the call ran on.
    pub thread: ThreadId,
}

impl Kind {
    /// Every kind, in the order the factory lists them.
    pub const ALL: [Self; 8] = [
        Self::Gain,
        Self::Delay,
        Self::Echo,
        Self::NotFinite,
        Self::MonoOnly,
        Self::StereoOnly,
        Self::Recorder,
        Self::Failing,
    ];

    /// Returns the class id of the plugin's component.
    #[must_use]
    pub const fn class_id(self) -> TUID {
        let index = match self {
            Self::Gain => 1,
            Self::Delay => 2,
            Self::Echo => 3,
            Self::NotFinite => 4,
            Self::MonoOnly => 5,
            Self::StereoOnly => 6,
            Self::Recorder => 7,
            Self::Failing => 8,
        };
        uid(0x5A1C_0000, 0x0000_4000, 0x8000_0000, index)
    }

    /// Returns the plugin name.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Gain => "Test Gain",
            Self::Delay => "Test Delay",
            Self::Echo => "Test Echo",
            Self::NotFinite => "Test Not Finite",
            Self::MonoOnly => "Test Mono Only",
            Self::StereoOnly => "Test Stereo Only",
            Self::Recorder => "Test Recorder",
            Self::Failing => "Test Failing",
        }
    }

    /// Returns the subcategories the factory reports.
    #[must_use]
    pub const fn subcategories(self) -> &'static str {
        match self {
            Self::Delay | Self::Echo => "Fx|Delay",
            Self::Gain | Self::NotFinite | Self::MonoOnly | Self::StereoOnly | Self::Recorder | Self::Failing => {
                "Fx|Dynamics"
            }
        }
    }

    /// Returns the kind whose component has a class id.
    fn from_class_id(id: &TUID) -> Option<Self> {
        Self::ALL.into_iter().find(|kind| kind.class_id() == *id)
    }

    /// Returns the class id of the plugin's controller.
    const fn controller_id(self) -> TUID {
        match self {
            Self::Recorder => RECORDER_CONTROLLER_ID,
            _ => CONTROLLER_ID,
        }
    }
}

/// Returns the factory of the fake plugins, as a module's `GetPluginFactory` would.
///
/// # Panics
///
/// Never panics, because the factory class always implements `IPluginFactory`.
#[must_use]
pub fn factory() -> ComPtr<IPluginFactory> {
    ComWrapper::new(factory::Factory)
        .to_com_ptr::<IPluginFactory>()
        .expect("the factory implements IPluginFactory")
}

/// Returns the calls logged by recorder plugins so far, and clears the log.
#[must_use]
pub fn take_recorded_calls() -> Vec<RecordedCall> {
    std::mem::take(&mut *RECORDED_CALLS.lock().unwrap_or_else(PoisonError::into_inner))
}

/// Logs a call when `enabled` is true.
fn record(enabled: bool, name: &'static str) {
    if enabled {
        RECORDED_CALLS
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .push(RecordedCall {
                name,
                thread: std::thread::current().id(),
            });
    }
}
