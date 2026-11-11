//! Stable 31-parameter host/UI registry and detached sound snapshots using independent normalized atomics.
//!
//! Only the exclusive audio owner publishes values to DSP. State callbacks never touch mutable DSP or editor state.

use std::sync::atomic::{AtomicU64, Ordering};

use image_control_dsp::{Parameters as DspParameters, SharedState};
use serde::{Deserialize, Serialize};

/// Stable output gain id, expressed in -24 through +12 dB.
pub const GAIN_PARAMETER: u32 = 0;
/// First of three native power-skew crossover ids.
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
/// Crossover range and JUCE float skew, with its geometric centre at sqrt(20 * 20000) Hz.
const CROSSOVER_MINIMUM: f32 = 20.0;
const CROSSOVER_MAXIMUM: f32 = 20_000.0;
// Nearest float to log(0.5f) / log((sqrt(20.f * 20000.f) - 20.f) / (20000.f - 20.f)).
// Keeping the declared float fixes defaults independently of platform libm and Miri evaluation.
const CROSSOVER_SKEW: f32 = f32::from_bits(0x3e4b_aad3);

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
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ParameterKind {
    /// Output trim in -24..12 dB.
    OutputGain,
    /// Independent band Side multiplier in 0..2.
    Width,
    /// Native float power-skew frequency in 20..20000 Hz.
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
    /// Native float power-skew normalized crossover frequencies.
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
    /// Balances remaining gestures synchronously when the native editor is removed.
    fn close(&self) {}
    /// Negotiates an editor-originated logical size through the retained host frame.
    fn request_resize(&self, _width: u32, _height: u32) -> bool {
        false
    }
}

impl ParameterSpec {
    /// Returns the plain range and interval used by JUCE's float `NormalisableRange`.
    const fn range(self) -> (f32, f32, f32) {
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

    /// Applies the native float grid from the range minimum, with midpoint ties toward the upper step.
    #[expect(
        clippy::suboptimal_flops,
        reason = "preserve the native float NormalisableRange grid operation order"
    )]
    fn snap_plain(self, plain: f32) -> f32 {
        let (minimum, maximum, interval) = self.range();
        let plain = plain.clamp(minimum, maximum);
        if interval > 0.0 {
            (minimum + interval * ((plain - minimum) / interval + 0.5).floor()).clamp(minimum, maximum)
        } else {
            plain
        }
    }

    /// Converts normalized host input to snapped plain units with the native float operation order.
    #[must_use]
    #[expect(
        clippy::suboptimal_flops,
        reason = "preserve native float skew conversion without fused rounding"
    )]
    pub fn normalized_to_plain(self, normalized: f64) -> f64 {
        let mut proportion = normalized.clamp(0.0, 1.0) as f32;
        if self.kind == ParameterKind::Crossover && proportion > 0.0 {
            proportion = (proportion.ln() / CROSSOVER_SKEW).exp();
        }
        let (minimum, maximum, _) = self.range();
        f64::from(self.snap_plain(minimum + (maximum - minimum) * proportion))
    }

    /// Converts snapped plain units to host units. Linear controls keep exact double neutral representations.
    #[must_use]
    pub fn plain_to_normalized(self, plain: f64) -> f64 {
        let plain = self.snap_plain(plain as f32);
        let (minimum, maximum, _) = self.range();
        if self.kind == ParameterKind::Crossover {
            f64::from(((plain - minimum) / (maximum - minimum)).powf(CROSSOVER_SKEW))
        } else {
            // The 0 dB grid point must retain the exact neutral used by the linear DSP band protocol.
            let plain =
                if matches!(self.kind, ParameterKind::OutputGain | ParameterKind::BandGain) && plain.abs() < 0.005 {
                    0.0
                } else {
                    f64::from(plain)
                };
            ((plain - f64::from(minimum)) / f64::from(maximum - minimum)).clamp(0.0, 1.0)
        }
    }

    /// Returns the authoritative value accepted from a new host or editor edit.
    /// Crossovers have no grid, so avoid a redundant skew round trip and its accumulated float error.
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
                let shown = f64::from((plain as f32 * 10.0).round() / 10.0) + 0.0;
                format!("{shown:.1} dB")
            }
            ParameterKind::Crossover if plain.round_ties_even() < 1000.0 => {
                format!("{} Hz", plain.round_ties_even() as u32)
            }
            ParameterKind::Crossover if plain < 10_000.0 => format!("{:.2} kHz", f64::from(plain as f32 / 1000.0)),
            ParameterKind::Crossover => format!("{:.1} kHz", f64::from(plain as f32 / 1000.0)),
            ParameterKind::Width => format!("{plain:.2}"),
            ParameterKind::Recovery => format!("{plain:.0}%"),
            ParameterKind::Pan if plain.abs() < 0.5 => "C".to_owned(),
            ParameterKind::Pan => format!("{:.0}{}", plain.abs(), if plain < 0.0 { "L" } else { "R" }),
        }
    }

    /// Parses native finite text grammar, then clamps and snaps just like a host plain-unit conversion.
    #[must_use]
    pub fn parse(self, text: &str) -> Option<f64> {
        let text = text.trim().to_ascii_lowercase();
        if (self.kind == ParameterKind::Recovery && text == "off")
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
            ParameterKind::Pan => text.strip_suffix('l').map_or_else(
                || {
                    (
                        text.strip_suffix('r')
                            .unwrap_or_else(|| text.strip_suffix('%').unwrap_or(&text)),
                        1.0,
                    )
                },
                |number| (number, -1.0),
            ),
            ParameterKind::Width if text.ends_with('%') => (text.strip_suffix('%')?, 0.01),
            ParameterKind::Recovery => (text.strip_suffix('%').unwrap_or(&text), 1.0),
            _ => (text.as_str(), 1.0),
        };
        let number = number.trim();
        if matches!(self.kind, ParameterKind::OutputGain | ParameterKind::BandGain)
            && matches!(number, "-inf" | "-infinity")
        {
            return Some(self.plain_to_normalized(-60.0));
        }
        let mut plain = number.parse::<f32>().ok()?;
        // C++ from_chars rejects underflow instead of accepting a nonzero token as zero.
        let mantissa = number.split('e').next()?;
        if plain == 0.0 && mantissa.bytes().any(|digit| matches!(digit, b'1'..=b'9')) {
            return None;
        }
        if self.kind == ParameterKind::Pan && (text.ends_with('l') || text.ends_with('r')) {
            plain = plain.abs();
        }
        plain *= multiplier;
        plain.is_finite().then(|| self.plain_to_normalized(f64::from(plain)))
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
        }
    }

    /// Reads a known parameter independently, without allocation or locking.
    #[must_use]
    pub fn get(&self, id: u32) -> Option<f64> {
        self.values
            .get(id as usize)
            .map(|slot| f64::from_bits(slot.load(Ordering::Relaxed)))
    }

    /// Rejects unknown ids and non-finite input, then accepts the native plain grid for new edits.
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
    /// Recalls deliberately retain off-grid normalized bits from RS states, including subsequent saves as v4.
    /// Only explicit edits snap to the native grid. Snapping recalls would change v1-v3 sound.
    pub fn restore(&self, snapshot: Snapshot) -> bool {
        if !snapshot.is_valid() {
            return false;
        }
        for (slot, value) in self.values.iter().zip(snapshot.values()) {
            slot.store(value.to_bits(), Ordering::Relaxed);
        }
        self.restore_generation.fetch_add(1, Ordering::Release);
        true
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
        // Float-normalized hosts widen their exact zero-dB default before publication.
        // Recognize only the two neutral representations, retaining every nearby value and the saved normalized bits.
        let gain = if snapshot.gain.to_bits() == NEUTRAL_GAIN.to_bits()
            || snapshot.gain.to_bits() == f64::from(NEUTRAL_GAIN as f32).to_bits()
        {
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

/// Returns native float-skew 100, 500, and 5000 Hz defaults independently of platform libm evaluation.
const fn default_crossovers() -> [f64; CROSSOVER_COUNT] {
    [
        0.333_542_317_152_023_3,
        0.476_343_721_151_351_93,
        0.758_567_452_430_725_1,
    ]
}
