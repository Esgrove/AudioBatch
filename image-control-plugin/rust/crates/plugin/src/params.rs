//! Stable 31-parameter host/UI registry and detached sound snapshots using independent normalized atomics.
//!
//! Only the exclusive audio owner publishes values to DSP. State callbacks never touch mutable DSP or editor state.

use std::sync::atomic::{AtomicU64, Ordering};

use image_control_dsp::{Parameters as DspParameters, SharedState};
use serde::{Deserialize, Serialize};

/// Stable output gain id, expressed in -24 through +12 dB.
pub const GAIN_PARAMETER: u32 = 0;
/// First of three logarithmic crossover ids.
pub const CROSSOVER_PARAMETER_START: u32 = 1;
/// First of four six-control band groups.
pub const BAND_PARAMETER_START: u32 = 4;
/// Latency-preserving bypass id.
pub const BYPASS_PARAMETER: u32 = 28;
/// Processed-minus-dry audition id.
pub const DELTA_PARAMETER: u32 = 29;
/// Final mono fold id.
pub const MONO_PARAMETER: u32 = 30;
/// Number of automatable sound controls. There is no global Width parameter.
pub const PARAMETER_COUNT: usize = 31;
/// Fields per band, ordered Mid, Side, Width, Recover, Pan, Solo.
pub const BAND_CONTROL_COUNT: usize = 6;
/// Number of crossover parameters.
pub const CROSSOVER_COUNT: usize = 3;
/// Individual crossover ids for callers that do not use index arithmetic.
pub const CROSSOVER_PARAMETERS: [u32; CROSSOVER_COUNT] = [1, 2, 3];
/// Normalized unity Width and centered Pan.
pub const NEUTRAL: f64 = 0.5;
/// Normalized zero-dB output gain on its -24..12 dB range.
pub const NEUTRAL_GAIN: f64 = 2.0 / 3.0;
/// Normalized zero-dB source trim on its -60..12 dB range.
pub const NEUTRAL_BAND_GAIN: f64 = 5.0 / 6.0;
/// Lower endpoint of the logarithmic crossover range in Hz.
const CROSSOVER_MINIMUM: f64 = 20.0;
/// Upper endpoint of the logarithmic crossover range in Hz.
const CROSSOVER_MAXIMUM: f64 = 20_000.0;
/// Frequency ratio across the full crossover slider, giving equal travel to equal octave spans.
const CROSSOVER_RATIO: f64 = CROSSOVER_MAXIMUM / CROSSOVER_MINIMUM;

/// Stable titles in band-major, field-minor order.
const BAND_TITLES: [&str; 24] = [
    "Band 1 Mid",
    "Band 1 Side",
    "Band 1 Width",
    "Band 1 Recover",
    "Band 1 Pan",
    "Band 1 Solo",
    "Band 2 Mid",
    "Band 2 Side",
    "Band 2 Width",
    "Band 2 Recover",
    "Band 2 Pan",
    "Band 2 Solo",
    "Band 3 Mid",
    "Band 3 Side",
    "Band 3 Width",
    "Band 3 Recover",
    "Band 3 Pan",
    "Band 3 Solo",
    "Band 4 Mid",
    "Band 4 Side",
    "Band 4 Width",
    "Band 4 Recover",
    "Band 4 Pan",
    "Band 4 Solo",
];

/// Plain-unit mapping shared by host metadata, numeric conversion, and editor controls.
///
/// Gains use 0.01 dB editing steps, Width uses 0.01, and Recover/Pan use whole percentage points.
/// Crossovers are continuous and logarithmic. Midpoint step ties choose the upper plain value.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParameterKind {
    /// Output trim in -24..12 dB.
    OutputGain,
    /// Independent band Side multiplier in 0..2.
    Width,
    /// Logarithmic frequency in 20..20000 Hz.
    Crossover,
    /// Source trim in -60..12 dB, with the lower endpoint muted.
    BandGain,
    /// Finite quadrature recovery in 0..200 percent.
    Recovery,
    /// Signed signal-transfer pan in -100..100 percent.
    Pan,
    /// Off/on switch changing at normalized one half.
    Switch,
}

/// Immutable metadata and conversion for one stable numeric parameter id.
#[derive(Clone, Copy, Debug)]
pub struct ParameterSpec {
    /// Stable automation and persistence identifier.
    pub id: u32,
    /// Host-facing title.
    pub title: &'static str,
    /// Host-facing plain-unit suffix.
    pub units: &'static str,
    /// Mapping used for numeric and textual conversions.
    pub kind: ParameterKind,
    /// Deterministic normalized fallback for missing state properties.
    pub default_normalized: f64,
}

/// Detached normalized controls for one band, independent of every live instance.
#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct BandSnapshot {
    /// Normalized Mid source trim.
    pub mid: f64,
    /// Normalized Side source trim.
    pub side: f64,
    /// Normalized independent Width.
    pub width: f64,
    /// Normalized recovery percentage.
    pub recover: f64,
    /// Normalized pan, centered at one half.
    pub pan: f64,
    /// Normalized Solo switch.
    pub solo: f64,
}

/// Complete detached normalized sound snapshot. Missing fields use fixed defaults, not live values.
#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Snapshot {
    /// Normalized output gain in dB, not a linear multiplier.
    pub gain: f64,
    /// Logarithmically normalized crossover frequencies.
    pub crossovers: [f64; CROSSOVER_COUNT],
    /// Four independent band control groups.
    pub bands: [BandSnapshot; 4],
    /// Normalized latency-preserving bypass switch.
    pub bypass: f64,
    /// Normalized processed-minus-dry audition switch.
    pub delta: f64,
    /// Normalized final mono fold switch.
    pub mono: f64,
}

/// Authoritative per-parameter atomics shared with host interfaces and the editor.
/// No mutable DSP, host callback, GUI reference, or lock is retained here.
pub struct Parameters {
    /// Independent values prevent unrelated UI and automation edits from overwriting each other.
    values: [AtomicU64; PARAMETER_COUNT],
    /// Successful detached recalls invalidate precomputed UI writes without changing ordinary automation semantics.
    restore_generation: AtomicU64,
    /// Non-audio local history and comparison ownership, never read by processing or automation publication.
    pub(crate) editing: std::sync::Mutex<crate::editing::EditingState>,
    /// Remembered non-sound choices shared by editor views and state callbacks, never sampled by audio.
    pub(crate) layout: crate::editor_geometry::EditorGeometry,
}

/// Independent non-audio worker retirement retained by the native editor binding, not the widget cache.
#[cfg(feature = "editor")]
#[derive(Clone, Default)]
pub struct EditorShutdown(std::sync::Arc<EditorWorker>);

/// Admission and join ownership outlive a borrowed Iced program and cannot reopen after native removal.
#[cfg(feature = "editor")]
#[derive(Default)]
struct EditorWorker {
    /// Permanent retirement is independent of the lifetime of any particular worker operation.
    closed: std::sync::atomic::AtomicBool,
    /// Admission and installation share the same lock as retirement, preventing an unowned late spawn.
    worker: std::sync::Mutex<Option<std::thread::JoinHandle<()>>>,
}

#[cfg(feature = "editor")]
impl EditorShutdown {
    /// Rejects work after native removal, including callbacks that still retain the Iced program.
    pub(crate) fn is_closed(&self) -> bool {
        self.0.closed.load(Ordering::Acquire)
    }

    /// Starts and retains a worker atomically with admission, without invoking UI or host callbacks.
    /// Returns false when retirement closed admission or an operation is still retained.
    /// Errors retain no detached worker.
    pub(crate) fn spawn(&self, operation: impl FnOnce() + Send + 'static) -> std::io::Result<bool> {
        let mut worker = self.0.worker.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        if self.is_closed() || worker.is_some() {
            return Ok(false);
        }
        *worker = Some(
            std::thread::Builder::new()
                .name("imagecontrol-presets".into())
                .spawn(operation)?,
        );
        drop(worker);
        Ok(true)
    }

    /// Joins one completed operation without closing admission for the next explicit command.
    pub(crate) fn finish(&self) {
        let worker = self
            .0
            .worker
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take();
        if let Some(worker) = worker {
            let _ = worker.join();
        }
    }

    /// Permanently closes admission before joining outside the mutex. Repeated retirement is harmless.
    pub(crate) fn shutdown(&self) {
        let worker = {
            let mut worker = self.0.worker.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            self.0.closed.store(true, Ordering::Release);
            worker.take()
        };
        if let Some(worker) = worker {
            let _ = worker.join();
        }
    }
}

/// UI-only automation seam. Implementations release any host-handler lock before invoking host code.
pub trait EditorBinding {
    /// Reports whether this UI lifetime still accepts edits after potentially reentrant host callbacks.
    fn is_open(&self) -> bool {
        true
    }
    /// Starts a host gesture for a validated stable id.
    fn begin_edit(&self, id: u32);
    /// Writes one finite normalized value and reports the edit to the host.
    fn set_value(&self, id: u32, normalized: f64);
    /// Admits transport before authoritative publication.
    /// Queue-backed adapters may reject overload without changing sound.
    fn publish_value(&self, parameters: &Parameters, id: u32, normalized: f64) -> bool {
        let Some(accepted) = parameter_spec(id)
            .filter(|_| normalized.is_finite())
            .map(|spec| spec.snap_normalized(normalized))
        else {
            return false;
        };
        if !parameters.set(id, accepted) {
            return false;
        }
        self.set_value(id, accepted);
        true
    }
    /// Ends the matching gesture, including interrupted editor teardown.
    fn end_edit(&self, id: u32);
    /// Marks a changed persisted non-sound preference on the UI thread, without a sound gesture.
    fn mark_dirty(&self) {}
    /// Registers independent non-audio worker ownership for synchronous native removal.
    #[cfg(feature = "editor")]
    fn register_shutdown(&self, _shutdown: EditorShutdown) {}
    /// Balances remaining gestures synchronously when the native editor is removed.
    fn close(&self) {}
    /// Negotiates an editor-originated logical size through the retained host frame.
    fn request_resize(&self, _width: u32, _height: u32) -> bool {
        false
    }
}

impl ParameterSpec {
    /// Returns the plain-unit endpoints and editing step. Crossovers are continuous.
    const fn range(self) -> (f64, f64, f64) {
        match self.kind {
            ParameterKind::OutputGain => (-24.0, 12.0, 0.01),
            ParameterKind::BandGain => (-60.0, 12.0, 0.01),
            ParameterKind::Width => (0.0, 2.0, 0.01),
            ParameterKind::Recovery => (0.0, 200.0, 1.0),
            ParameterKind::Pan => (-100.0, 100.0, 1.0),
            ParameterKind::Switch => (0.0, 1.0, 1.0),
            ParameterKind::Crossover => (CROSSOVER_MINIMUM, CROSSOVER_MAXIMUM, 0.0),
        }
    }

    /// Snaps in double precision from the range minimum. Half-step ties choose the upper step.
    /// Gain's zero-dB step is exactly zero rather than a residual from decimal-step multiplication.
    fn snap_plain(self, plain: f64) -> f64 {
        let (minimum, maximum, interval) = self.range();
        let plain = plain.clamp(minimum, maximum);
        if plain <= minimum || plain >= maximum || interval == 0.0 {
            return plain;
        }
        let step = ((plain - minimum) / interval).round();
        let snapped = step.mul_add(interval, minimum).clamp(minimum, maximum);
        if matches!(self.kind, ParameterKind::OutputGain | ParameterKind::BandGain) && snapped.abs() < interval * 0.5 {
            0.0
        } else {
            snapped
        }
    }

    /// Converts normalized host input to double-precision plain units, including the declared editing steps.
    /// Crossover frequency is `20 * 1000^normalized`, so equal slider travel covers equal octave spans.
    #[must_use]
    pub fn normalized_to_plain(self, normalized: f64) -> f64 {
        let normalized = normalized.clamp(0.0, 1.0);
        let (minimum, maximum, _) = self.range();
        if normalized <= 0.0 {
            return minimum;
        }
        if normalized >= 1.0 {
            return maximum;
        }
        if self.kind == ParameterKind::Crossover {
            CROSSOVER_MINIMUM * CROSSOVER_RATIO.powf(normalized)
        } else {
            self.snap_plain(normalized.mul_add(maximum - minimum, minimum))
        }
    }

    /// Clamps and snaps double-precision plain units into normalized host units with exact range endpoints.
    /// Zero-dB gains use the exact normalized neutral shared by host defaults and DSP publication.
    #[must_use]
    pub fn plain_to_normalized(self, plain: f64) -> f64 {
        let plain = self.snap_plain(plain);
        let (minimum, maximum, _) = self.range();
        if plain <= minimum {
            return 0.0;
        }
        if plain >= maximum {
            return 1.0;
        }
        if self.kind == ParameterKind::Crossover {
            (plain / CROSSOVER_MINIMUM).log(CROSSOVER_RATIO).clamp(0.0, 1.0)
        } else {
            ((plain - minimum) / (maximum - minimum)).clamp(0.0, 1.0)
        }
    }

    /// Returns the authoritative value accepted from a new host or editor edit.
    /// Continuous crossovers retain the normalized input without a redundant logarithmic round trip.
    #[must_use]
    pub fn snap_normalized(self, normalized: f64) -> f64 {
        if self.kind == ParameterKind::Crossover {
            normalized.clamp(0.0, 1.0)
        } else {
            self.plain_to_normalized(self.normalized_to_plain(normalized))
        }
    }

    /// Formats a normalized value outside the audio thread using the same units as the host.
    #[must_use]
    pub fn format(self, normalized: f64) -> String {
        let plain = self.normalized_to_plain(normalized);
        match self.kind {
            ParameterKind::Switch => if plain >= 0.5 { "On" } else { "Off" }.to_owned(),
            ParameterKind::BandGain if plain <= -60.0 => "-inf dB".to_owned(),
            ParameterKind::OutputGain | ParameterKind::BandGain => {
                let shown = (plain * 10.0).round() / 10.0 + 0.0;
                format!("{shown:.1} dB")
            }
            ParameterKind::Crossover if plain.round_ties_even() < 1000.0 => {
                format!("{} Hz", plain.round_ties_even() as u32)
            }
            ParameterKind::Crossover if plain < 10_000.0 => format!("{:.2} kHz", plain / 1000.0),
            ParameterKind::Crossover => format!("{:.1} kHz", plain / 1000.0),
            ParameterKind::Width => format!("{plain:.2}"),
            ParameterKind::Recovery => format!("{plain:.0}%"),
            ParameterKind::Pan if plain.abs() < 0.5 => "C".to_owned(),
            ParameterKind::Pan => format!("{:.0}{}", plain.abs(), if plain < 0.0 { "L" } else { "R" }),
        }
    }

    /// Parses finite double-precision text with optional units, then clamps and snaps like plain-unit conversion.
    /// Pan accepts signed percentages or L/R direction suffixes, including `35% L`. Band gain accepts `Off` as mute.
    /// Malformed, non-finite, overflowing, and nonzero-underflowing numeric input is rejected.
    #[must_use]
    pub fn parse(self, text: &str) -> Option<f64> {
        let text = text.trim().to_ascii_lowercase();
        if (matches!(self.kind, ParameterKind::BandGain | ParameterKind::Recovery) && text == "off")
            || (self.kind == ParameterKind::Switch && matches!(text.as_str(), "off" | "false"))
        {
            return Some(0.0);
        }
        if self.kind == ParameterKind::Switch && matches!(text.as_str(), "on" | "true") {
            return Some(1.0);
        }
        if self.kind == ParameterKind::Pan && text == "c" {
            return Some(0.5);
        }
        let (number, multiplier) = match self.kind {
            ParameterKind::Crossover => text.strip_suffix("khz").map_or_else(
                || (text.strip_suffix("hz").unwrap_or(&text), 1.0),
                |number| (number, 1000.0),
            ),
            ParameterKind::OutputGain | ParameterKind::BandGain => (text.strip_suffix("db").unwrap_or(&text), 1.0),
            ParameterKind::Pan => {
                let (number, direction) = text.strip_suffix('l').map_or_else(
                    || (text.strip_suffix('r').unwrap_or(&text), 1.0),
                    |number| (number, -1.0),
                );
                let number = number.trim();
                (number.strip_suffix('%').unwrap_or(number), direction)
            }
            ParameterKind::Width if text.ends_with('%') => (text.strip_suffix('%')?, 0.01),
            ParameterKind::Recovery => (text.strip_suffix('%').unwrap_or(&text), 1.0),
            _ => (text.as_str(), 1.0),
        };
        let number = number.trim();
        if matches!(self.kind, ParameterKind::OutputGain | ParameterKind::BandGain)
            && matches!(number, "-inf" | "-infinity")
        {
            return Some(0.0);
        }
        let mut plain = number.parse::<f64>().ok()?;
        // Reject nonzero input lost to underflow rather than silently changing its meaning to zero.
        let mantissa = number.split('e').next()?;
        if plain == 0.0 && mantissa.bytes().any(|digit| matches!(digit, b'1'..=b'9')) {
            return None;
        }
        if self.kind == ParameterKind::Pan && (text.ends_with('l') || text.ends_with('r')) {
            plain = plain.abs();
        }
        let scaled = plain * multiplier;
        if scaled == 0.0 && plain != 0.0 {
            return None;
        }
        scaled.is_finite().then(|| self.plain_to_normalized(scaled))
    }
}

impl BandSnapshot {
    /// Returns values in the stable band field order without allocation.
    const fn values(self) -> [f64; BAND_CONTROL_COUNT] {
        [self.mid, self.side, self.width, self.recover, self.pan, self.solo]
    }
}

impl Default for BandSnapshot {
    fn default() -> Self {
        Self {
            mid: NEUTRAL_BAND_GAIN,
            side: NEUTRAL_BAND_GAIN,
            width: NEUTRAL,
            recover: 0.0,
            pan: NEUTRAL,
            solo: 0.0,
        }
    }
}

impl Snapshot {
    /// Returns all normalized values in stable numeric id order.
    fn values(self) -> [f64; PARAMETER_COUNT] {
        let mut values = [0.0; PARAMETER_COUNT];
        for (slot, value) in values.iter_mut().zip(
            std::iter::once(self.gain)
                .chain(self.crossovers)
                .chain(self.bands.into_iter().flat_map(BandSnapshot::values))
                .chain([self.bypass, self.delta, self.mono]),
        ) {
            *slot = value;
        }
        values
    }

    /// Validates the entire state before any authoritative atomic is modified.
    #[must_use]
    pub fn is_valid(self) -> bool {
        self.values()
            .into_iter()
            .all(|value| value.is_finite() && (0.0..=1.0).contains(&value))
    }
}

impl Default for Snapshot {
    fn default() -> Self {
        Self {
            gain: NEUTRAL_GAIN,
            crossovers: default_crossovers(),
            bands: [BandSnapshot::default(); 4],
            bypass: 0.0,
            delta: 0.0,
            mono: 0.0,
        }
    }
}

impl Parameters {
    /// Allocates no DSP or GUI state and initializes every parameter to its fixed default.
    #[must_use]
    pub fn new() -> Self {
        Self {
            values: Snapshot::default()
                .values()
                .map(|value| AtomicU64::new(value.to_bits())),
            restore_generation: AtomicU64::new(0),
            editing: std::sync::Mutex::new(crate::editing::EditingState::new(Snapshot::default())),
            layout: crate::editor_geometry::EditorGeometry::default(),
        }
    }

    /// Reads a known parameter independently, without allocation or locking.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<f64> {
        self.values
            .get(id as usize)
            .map(|slot| f64::from_bits(slot.load(Ordering::Relaxed)))
    }

    /// Rejects unknown ids and non-finite input, then accepts the declared plain-unit editing steps.
    pub fn set(&self, id: u32, value: f64) -> bool {
        let Some(slot) = self.values.get(id as usize).filter(|_| value.is_finite()) else {
            return false;
        };
        let Some(spec) = parameter_spec(id) else {
            return false;
        };
        slot.store(spec.snap_normalized(value).to_bits(), Ordering::Relaxed);
        true
    }

    /// Samples independent atomics into a detached snapshot. Recall is not a sample-atomic transaction.
    #[must_use]
    pub fn snapshot(&self) -> Snapshot {
        let read = |id| self.get(id).unwrap_or_default();
        Snapshot {
            gain: read(GAIN_PARAMETER),
            crossovers: std::array::from_fn(|index| read(CROSSOVER_PARAMETER_START + index as u32)),
            bands: std::array::from_fn(|band| {
                let base = BAND_PARAMETER_START + (band * BAND_CONTROL_COUNT) as u32;
                BandSnapshot {
                    mid: read(base),
                    side: read(base + 1),
                    width: read(base + 2),
                    recover: read(base + 3),
                    pan: read(base + 4),
                    solo: read(base + 5),
                }
            }),
            bypass: read(BYPASS_PARAMETER),
            delta: read(DELTA_PARAMETER),
            mono: read(MONO_PARAMETER),
        }
    }

    /// Applies an entirely validated detached state without entering DSP history or UI objects.
    /// Detached recalls retain their validated normalized values. Only new host/editor edits apply editing steps.
    pub fn restore(&self, snapshot: Snapshot) -> bool {
        if !snapshot.is_valid() {
            return false;
        }
        let mut editing = self.editing.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        self.restore_values(snapshot);
        editing.synchronize(self.restore_generation(), snapshot);
        drop(editing);
        true
    }

    /// Publishes validated restore values while the non-audio editing owner excludes stale local transactions.
    pub(crate) fn restore_values(&self, snapshot: Snapshot) {
        for (slot, value) in self.values.iter().zip(snapshot.values()) {
            slot.store(value.to_bits(), Ordering::Relaxed);
        }
        self.restore_generation.fetch_add(1, Ordering::Release);
    }

    /// Reads successful recall publication, independent of automation and without entering DSP or GUI state.
    #[must_use]
    pub fn restore_generation(&self) -> u64 {
        self.restore_generation.load(Ordering::Acquire)
    }

    /// Samples authoritative atomics into DSP targets. Call only while holding exclusive audio ownership.
    /// This performs no kernel design, allocation, locks, channels, GUI wakes, or reference-count operations.
    pub fn publish(&self, shared: &SharedState) {
        let snapshot = self.snapshot();
        // The normalized zero-dB default publishes exact unity. Other recalled values keep their full precision.
        let gain = if snapshot.gain.to_bits() == NEUTRAL_GAIN.to_bits() {
            1.0
        } else {
            let gain_db = snapshot.gain.mul_add(36.0, -24.0);
            10.0_f64.powf(gain_db / 20.0) as f32
        };
        if let Some(parameters) = DspParameters::new(gain, 1.0) {
            shared.set_parameters(parameters);
        }
        for (band, controls) in snapshot.bands.into_iter().enumerate() {
            for (field, normalized) in controls.values().into_iter().enumerate() {
                shared.set_band_parameter(band, field, normalized as f32);
            }
        }
        for (index, normalized) in snapshot.crossovers.into_iter().enumerate() {
            if let Some(spec) = parameter_spec(CROSSOVER_PARAMETER_START + index as u32) {
                shared.set_crossover(index, spec.normalized_to_plain(normalized) as f32);
            }
        }
        shared.set_bypass(snapshot.bypass >= 0.5);
        shared.set_delta(snapshot.delta >= 0.5);
        shared.set_mono(snapshot.mono >= 0.5);
    }
}

impl Default for Parameters {
    fn default() -> Self {
        Self::new()
    }
}

/// Returns one stable band's parameter id, rejecting unknown bands or fields.
#[must_use]
pub const fn band_parameter_id(band: usize, field: usize) -> Option<u32> {
    if band < 4 && field < BAND_CONTROL_COUNT {
        Some(BAND_PARAMETER_START + (band * BAND_CONTROL_COUNT + field) as u32)
    } else {
        None
    }
}

/// Returns immutable metadata without allocating or depending on GUI types.
#[must_use]
pub fn parameter_spec(id: u32) -> Option<ParameterSpec> {
    let (title, units, kind, default_normalized) = match id {
        GAIN_PARAMETER => ("Output Gain", "dB", ParameterKind::OutputGain, NEUTRAL_GAIN),
        1..=3 => {
            let index = (id - CROSSOVER_PARAMETER_START) as usize;
            (
                *["Crossover 1", "Crossover 2", "Crossover 3"].get(index)?,
                "Hz",
                ParameterKind::Crossover,
                *default_crossovers().get(index)?,
            )
        }
        4..=27 => {
            let index = (id - BAND_PARAMETER_START) as usize;
            let (units, kind, default_normalized) = match index % BAND_CONTROL_COUNT {
                0 | 1 => ("dB", ParameterKind::BandGain, NEUTRAL_BAND_GAIN),
                2 => ("", ParameterKind::Width, NEUTRAL),
                3 => ("%", ParameterKind::Recovery, 0.0),
                4 => ("%", ParameterKind::Pan, NEUTRAL),
                _ => ("", ParameterKind::Switch, 0.0),
            };
            (*BAND_TITLES.get(index)?, units, kind, default_normalized)
        }
        BYPASS_PARAMETER => ("Bypass", "", ParameterKind::Switch, 0.0),
        DELTA_PARAMETER => ("Delta", "", ParameterKind::Switch, 0.0),
        MONO_PARAMETER => ("Mono", "", ParameterKind::Switch, 0.0),
        _ => return None,
    };
    Some(ParameterSpec {
        id,
        title,
        units,
        kind,
        default_normalized,
    })
}

/// Returns fixed nearest-double `log_1000(frequency / 20)` defaults for 100, 500, and 5000 Hz.
/// Constants keep fallback state deterministic across platform libm implementations and Miri.
const fn default_crossovers() -> [f64; CROSSOVER_COUNT] {
    [
        0.232_990_001_445_339_6,
        0.465_980_002_890_679_2,
        0.799_313_336_224_012_5,
    ]
}
