//! Waveform overview of the selected file, with cue arrows, the playhead, and the optional loudness graph.
//!
//! It draws channel envelopes stacked on a rounded panel, Serato cue points as inward arrows at the top and bottom,
//! a thin white playhead, and a short-term loudness curve with its axis when the graph is enabled.
//! The envelopes are blue, or coloured column by column from the Serato Overview tag when the file has one,
//! while their shape still comes from the decoded audio.
//! Identity-matched source overloads add equally sized solid yellow sample caps and orange true peak caps,
//! independent of input gain.
//! The view zooms around the playhead and follows it while playing,
//! and it scales the envelope by the input gain.
//! Pressing and dragging show a pending target without changing playback, releasing seeks and plays once,
//! and a double-click plays from the start.
//! The mouse wheel zooms, Command or Control with the wheel changes the gain,
//! and adding Shift changes it in finer steps.
//! While audio plays the view requests a new frame on every render, so the playhead moves smoothly.

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Instant;

use audiobatch_core::overload::OverloadHistory;
use audiobatch_core::serato_tags::Overview;
use audiobatch_core::serato_tags::colouring::{OverviewColour, column_colours};
use audiobatch_core::spectrogram::SpectrogramData;
use audiobatch_core::{AnalysisRecord, ShortTermLoudnessHistory, WaveformOverview};
use audiobatch_playback::PlayheadHandle;
use gpui_kit::{
    App, Bounds, Context, EventEmitter, FocusHandle, Hsla, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent,
    PathBuilder, Pixels, Point, ScrollEvent, SharedString, TextAlign, TextRun, Window, fill, font, point, px, rgb,
    size,
};

use crate::settings::{DEFAULT_LOUDNESS_MAXIMUM, DEFAULT_LOUDNESS_MINIMUM, PreviewViewMode};
use crate::theme::{TEXT_FONT, black, color, palette, white};

use self::geometry::{PreviewGeometry, Viewport, has_area, relative};
use self::loudness::LoudnessRange;
use self::overloads::{OverloadSettings, SourceOverloads};
use self::spectrogram::SpectrogramRaster;

mod axes;
mod geometry;
mod loudness;
mod overloads;
mod render;
mod spectrogram;

/// Lowest zoom level, the whole file.
pub const MINIMUM_ZOOM: f64 = 1.0;

/// Highest zoom level, a twentieth of the file.
pub const MAXIMUM_ZOOM: f64 = 20.0;

/// Placeholder shown when no file is selected.
pub const NO_FILE_TEXT: &str = "No audio file selected";

/// Inset of the plot inside the panel.
const PLOT_INSET: f32 = 2.0;

/// Height of a cue arrow.
const CUE_ARROW_HEIGHT: f32 = 8.0;

/// Width of a cue arrow.
const CUE_ARROW_WIDTH: f32 = 10.0;

/// Width of the dark outline around a cue arrow.
const CUE_ARROW_OUTLINE_WIDTH: f32 = 1.5;

/// Width of the playhead line.
const PLAYHEAD_WIDTH: f32 = 1.5;

/// Opacity of the playhead while playing.
const PLAYING_PLAYHEAD_ALPHA: f32 = 0.85;

/// Opacity of the playhead while paused away from the start.
const PAUSED_PLAYHEAD_ALPHA: f32 = 0.4;

/// Opacity of the pending release target, quieter than the actual playback cursor.
const PENDING_PLAYHEAD_ALPHA: f32 = 0.45;

/// Explanation is attached only to the waveform pane, never the spectral plot or frequency axis.
const SOURCE_OVERS_TOOLTIP: &str =
    "Source overs: yellow = sample > 0 dBFS, orange = true peak > 0 dBTP. Orange takes precedence.";

/// Wheel distance in pixels for one zoom step.
const WHEEL_STEP_PIXELS: f32 = 20.0;

/// Gain change of one Command or Control wheel event over the waveform in decibels, the step of the gain slider.
const GAIN_WHEEL_STEP_DB: f32 = 0.1;

/// Command or Control and Shift wheel events per gain step, for fine adjustment of 0.01 dB per event.
/// The gain keeps the resolution of the slider, so the events add up before the gain changes.
const FINE_GAIN_WHEEL_EVENTS_PER_STEP: i32 = 10;

/// Zoom factor of one wheel step.
const WHEEL_ZOOM_FACTOR: f64 = 1.2;

/// Events the waveform sends to the app view.
#[derive(Debug, Clone)]
pub enum WaveformEvent {
    /// The user committed a pending target on release, requesting one seek followed by playback.
    SeekAndPlayRequested(f64),
    /// The user double-clicked, which plays from the start.
    RestartRequested,
    /// The user zoomed with the wheel by a factor.
    ZoomBy(f64),
    /// The user changed the gain with the wheel by an amount in decibels.
    GainBy(f32),
}

/// Where the playhead position comes from.
#[derive(Debug, Clone)]
pub enum PlayheadSource {
    /// The preview player.
    Player(PlayheadHandle),
    /// A simulated clock looping over the duration, for the synthetic stress test.
    Simulated {
        /// When the simulated playback started.
        started: Instant,
        /// Length of the loop in seconds.
        duration: f64,
    },
}

/// A cue point drawn on the waveform, for example from the Serato markers.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CueMarker {
    /// Position in seconds.
    pub seconds: f64,
    /// Color as a `0xRRGGBB` value.
    pub color: u32,
    /// Zero-based hot cue slot, which the keys 1 to 8 jump to.
    pub slot: u8,
}

/// Paints the overview of one file.
pub struct WaveformView {
    overview: Option<Arc<WaveformOverview>>,
    spectrogram: Option<SpectrogramRaster>,
    spectrogram_completed: bool,
    view_mode: PreviewViewMode,
    source_rate: u32,
    source_channels: u32,
    source_frames: u64,
    source_duration: f64,
    overload_settings: OverloadSettings,
    /// Captured analysis source, independent of waveform allocation and selected-preview generation.
    source: Option<(std::path::PathBuf, u64, i64)>,
    /// Source-only warnings validated against the record, waveform, and on-disk file identity.
    overloads: SourceOverloads,
    /// Serato Overview of the file, which colours the envelope when present.
    serato_overview: Option<Arc<Overview>>,
    cues: Vec<CueMarker>,
    loudness: Option<Arc<ShortTermLoudnessHistory>>,
    placeholder: SharedString,
    playhead: Option<PlayheadSource>,
    zoom: f64,
    /// Start of the visible range in seconds.
    visible_start: f64,
    /// Linear gain applied to the drawn envelope.
    display_gain: f32,
    /// Signed count of Command or Control and Shift wheel events toward the next fine gain step.
    fine_gain_wheel_events: i32,
    /// Pending display-only target and the pressed pane's latched source-time mapping.
    pending_seek: Option<PendingSeek>,
    /// Created once for this retained view, preserving root action bubbling and hot-cue shortcuts.
    focus_handle: Option<FocusHandle>,
    /// Whether the Serato Overview colours the envelope.
    serato_colours: bool,
    show_loudness: bool,
    loudness_range: LoudnessRange,
    /// Current frame's shared pane geometry, resolved from the window's rem size.
    geometry: Rc<Cell<PreviewGeometry>>,
    /// Counts redraws when `AUDIOBATCH_GUI_DEBUG` is set, for checking the playhead frame rate.
    frame_counter: Option<FrameCounter>,
}

/// Prints the number of redraws per second to stderr.
struct FrameCounter {
    window_start: Instant,
    frames: u32,
}

/// A display-only seek gesture, latched to its initial pane and viewport until committed or cleared.
#[derive(Debug, Clone, Copy)]
struct PendingSeek {
    viewport: Viewport,
    seconds: f64,
    restart: bool,
}

/// Everything the paint closure needs, copied out of the view for one frame.
struct PaintData {
    overview: Option<Arc<WaveformOverview>>,
    spectrogram: Option<SpectrogramRaster>,
    spectrogram_completed: bool,
    view_mode: PreviewViewMode,
    source_rate: u32,
    source_channels: u32,
    source_duration: f64,
    placeholder: SharedString,
    overload_settings: OverloadSettings,
    pending_seek: Option<f64>,
    overloads: Option<Arc<OverloadHistory>>,
    serato_overview: Option<Arc<Overview>>,
    loudness: Option<Arc<ShortTermLoudnessHistory>>,
    show_loudness: bool,
    loudness_range: LoudnessRange,
    cues: Vec<CueMarker>,
    visible_start: f64,
    visible_length: f64,
    display_gain: f32,
    playhead: Option<(f64, f32)>,
}

impl PlayheadSource {
    /// Returns the playhead time in seconds.
    #[must_use]
    pub fn position_seconds(&self) -> f64 {
        match self {
            Self::Player(handle) => handle.position_seconds(),
            Self::Simulated { started, duration } => {
                if *duration > 0.0 {
                    started.elapsed().as_secs_f64() % duration
                } else {
                    0.0
                }
            }
        }
    }

    /// Returns true while the playhead moves.
    #[must_use]
    pub fn is_playing(&self) -> bool {
        match self {
            Self::Player(handle) => handle.is_playing(),
            Self::Simulated { .. } => true,
        }
    }
}

impl WaveformView {
    /// Creates an empty view.
    #[must_use]
    pub fn new() -> Self {
        Self {
            overview: None,
            spectrogram: None,
            spectrogram_completed: false,
            view_mode: PreviewViewMode::Waveform,
            source_rate: 0,
            source_channels: 0,
            source_frames: 0,
            source_duration: 0.0,
            overload_settings: OverloadSettings::default(),
            source: None,
            overloads: SourceOverloads::default(),
            serato_overview: None,
            cues: Vec::new(),
            loudness: None,
            placeholder: SharedString::from(NO_FILE_TEXT),
            playhead: None,
            zoom: MINIMUM_ZOOM,
            visible_start: 0.0,
            display_gain: 1.0,
            fine_gain_wheel_events: 0,
            pending_seek: None,
            focus_handle: None,
            serato_colours: true,
            show_loudness: false,
            loudness_range: LoudnessRange {
                minimum: DEFAULT_LOUDNESS_MINIMUM,
                maximum: DEFAULT_LOUDNESS_MAXIMUM,
            },
            geometry: Rc::new(Cell::new(PreviewGeometry::default())),
            frame_counter: crate::debug_enabled().then(|| FrameCounter {
                window_start: Instant::now(),
                frames: 0,
            }),
        }
    }

    /// Shows an analyzed record and attaches source warnings only while its file identity and waveform match.
    /// Warning attachment does not change input gain, cues, playback, or the visible range of the same overview.
    pub fn set_record(&mut self, record: &AnalysisRecord, cx: &mut Context<Self>) {
        let source = (record.path.clone(), record.file_size, record.modified_time_ms);
        if self.source.as_ref() != Some(&source) {
            self.clear_spectrogram(cx);
        }
        self.update_record(record);
        if self.spectrogram.as_ref().is_some_and(|raster| {
            !raster
                .source()
                .matches_stream(self.source_rate, self.source_frames, self.source_channels)
        }) {
            self.clear_spectrogram(cx);
        }
        cx.notify();
    }

    /// Changes the presentation only, retaining artifacts, viewport, gain, and transport state.
    pub fn set_view_mode(&mut self, mode: PreviewViewMode, cx: &mut Context<Self>) {
        self.view_mode = mode;
        cx.notify();
    }

    /// Sets independent warning filters before precedence and the shared lane-tint preference.
    pub fn set_overload_settings(&mut self, sample: bool, true_peak: bool, lines: bool, cx: &mut Context<Self>) {
        self.overload_settings = OverloadSettings::new(sample, true_peak, lines);
        cx.notify();
    }

    /// Adopts only source-matched bounded data, building one raster when the immutable artifact changes.
    /// The caller rejects stale asynchronous selection generations before calling this method.
    pub fn update_spectrogram(&mut self, source: Option<Arc<SpectrogramData>>, cx: &mut Context<Self>) {
        let source =
            source.filter(|data| data.matches_stream(self.source_rate, self.source_frames, self.source_channels));
        let same = match (&self.spectrogram, &source) {
            (Some(current), Some(next)) => Arc::ptr_eq(current.source(), next),
            (None, None) => true,
            _ => false,
        };
        if self.spectrogram_completed && same {
            return;
        }
        self.clear_spectrogram(cx);
        self.spectrogram_completed = true;
        self.spectrogram = source.map(SpectrogramRaster::new);
        cx.notify();
    }

    /// Reports completed spectral loading, including an unavailable optional artifact.
    pub const fn has_spectrogram_result(&self) -> bool {
        self.spectrogram_completed
    }

    /// Replaces only the decoded waveform prefix, retaining the complete source timeline and viewport.
    /// The owner validates run, source identity, and selection generation before adoption.
    pub fn update_progressive_overview(
        &mut self,
        overview: Arc<WaveformOverview>,
        expected_frames: Option<u64>,
        cx: &mut Context<Self>,
    ) {
        self.source_rate = overview.sample_rate();
        self.source_channels = overview.channels();
        self.source_frames = expected_frames
            .unwrap_or(self.source_frames)
            .max(overview.length_in_frames());
        self.source_duration = self.source_frames as f64 / f64::from(self.source_rate);
        self.overview = Some(overview);
        cx.notify();
    }

    /// Reports the pending display target independently of the actual playback cursor.
    #[cfg(test)]
    pub fn pending_seek_position(&self) -> Option<f64> {
        self.pending_seek.map(|pending| pending.seconds)
    }

    /// Reports retained raster availability without exposing pixels to semantic integration tests.
    #[cfg(test)]
    pub const fn has_spectrogram_raster(&self) -> bool {
        self.spectrogram.is_some()
    }

    /// Returns the two source plots for production-handler integration tests, excluding all axis gutters.
    #[cfg(test)]
    pub fn preview_plots(&self) -> [Bounds<Pixels>; 2] {
        let geometry = self.geometry.get();
        [geometry.waveform, geometry.spectrogram]
    }

    /// Returns the source currently owning a real waveform, for nonvisual workflow assertions.
    #[cfg(test)]
    pub fn waveform_source(&self) -> Option<&std::path::Path> {
        self.overview.as_ref()?;
        self.source.as_ref().map(|(path, _, _)| path.as_path())
    }

    /// Shows an overview without source warnings, for example for synthetic demo data.
    /// A different file starts from its beginning, while the same file keeps the visible range.
    pub fn show(
        &mut self,
        overview: Option<Arc<WaveformOverview>>,
        loudness: Option<Arc<ShortTermLoudnessHistory>>,
        cx: &mut Context<Self>,
    ) {
        self.clear_spectrogram(cx);
        self.update_overview(overview, loudness);
        cx.notify();
    }

    /// Shows a text instead of an overview, for example while the file is analyzed.
    /// The cue points stay, so they reappear once the overview of the same file is shown.
    pub fn show_placeholder(&mut self, text: impl Into<SharedString>, cx: &mut Context<Self>) {
        self.clear_spectrogram(cx);
        self.pending_seek = None;
        self.source_rate = 0;
        self.source_channels = 0;
        self.source_frames = 0;
        self.source_duration = 0.0;
        self.geometry.set(PreviewGeometry::default());
        self.placeholder = text.into();
        self.overloads.clear();
        self.source = None;
        self.overview = None;
        self.loudness = None;
        self.visible_start = 0.0;
        cx.notify();
    }

    /// Sets the cue points drawn on top of the overview.
    pub fn set_cues(&mut self, cues: Vec<CueMarker>, cx: &mut Context<Self>) {
        self.cues = cues;
        cx.notify();
    }

    /// Sets the Serato Overview that colours the envelope, or none for the plain blue envelope.
    /// Like the cue points, it stays while a placeholder is shown.
    pub fn set_serato_overview(&mut self, overview: Option<Arc<Overview>>, cx: &mut Context<Self>) {
        self.serato_overview = overview;
        cx.notify();
    }

    /// Turns colouring the envelope from the Serato Overview on or off, from Options > Serato Waveform Colours.
    pub fn set_serato_colours(&mut self, enabled: bool, cx: &mut Context<Self>) {
        self.serato_colours = enabled;
        cx.notify();
    }

    /// Sets where the playhead position comes from.
    pub fn set_playhead(&mut self, playhead: Option<PlayheadSource>, cx: &mut Context<Self>) {
        self.playhead = playhead;
        cx.notify();
    }

    /// Sets the zoom level, keeping the playhead at the same place on screen when it is visible.
    pub fn set_zoom(&mut self, zoom: f64, cx: &mut Context<Self>) {
        let zoom = zoom.clamp(MINIMUM_ZOOM, MAXIMUM_ZOOM);
        let duration = self.duration_seconds();
        if duration > 0.0 {
            let old_length = self.visible_length();
            let new_length = duration / zoom;
            let anchor = self
                .playhead
                .as_ref()
                .map(PlayheadSource::position_seconds)
                .filter(|&position| position >= self.visible_start && position <= self.visible_start + old_length)
                .unwrap_or_else(|| {
                    let half_length = old_length * 0.5;
                    self.visible_start + half_length
                });
            let anchor_fraction = if old_length > 0.0 {
                (anchor - self.visible_start) / old_length
            } else {
                0.0
            };
            self.zoom = zoom;
            self.visible_start = anchor_fraction.mul_add(-new_length, anchor);
            self.clamp_visible_start();
        } else {
            self.zoom = zoom;
        }
        cx.notify();
    }

    /// Sets the linear gain applied to the drawn envelope.
    pub fn set_display_gain(&mut self, gain: f32, cx: &mut Context<Self>) {
        self.display_gain = gain.clamp(0.0, 100.0);
        cx.notify();
    }

    /// Shows or hides the loudness graph.
    pub fn set_show_loudness(&mut self, show: bool, cx: &mut Context<Self>) {
        self.show_loudness = show;
        cx.notify();
    }

    /// Sets the loudness at the bottom and the top of the loudness graph axis, in LUFS.
    /// The caller validates the range, so the top is always above the bottom.
    pub fn set_loudness_range(&mut self, minimum: i32, maximum: i32, cx: &mut Context<Self>) {
        self.loudness_range = LoudnessRange { minimum, maximum };
        cx.notify();
    }

    /// Drops the old image after GPUI releases any window borrow held by the current update.
    fn clear_spectrogram(&mut self, cx: &mut Context<Self>) {
        if let Some(raster) = self.spectrogram.take() {
            let image = raster.into_image();
            cx.defer(move |cx| cx.drop_image(image, None));
        }
        self.spectrogram_completed = false;
    }

    /// Applies source-matched artifacts without touching transport, cue, gain, zoom or pending same-source gestures.
    fn update_record(&mut self, record: &AnalysisRecord) {
        let source = (record.path.clone(), record.file_size, record.modified_time_ms);
        let same_source = self.source.as_ref() == Some(&source);
        let visible_start = self.visible_start;
        let pending_seek = self.pending_seek;
        let loudness = record
            .short_term_loudness
            .clone()
            .or_else(|| same_source.then(|| self.loudness.clone()).flatten());
        let overview = record
            .waveform
            .clone()
            .or_else(|| same_source.then(|| self.overview.clone()).flatten());
        self.update_overview(overview, loudness);
        self.source = Some(source);
        self.source_rate = record.sample_rate;
        self.source_channels = record.channels;
        self.source_frames = record.length_in_samples;
        self.source_duration = if record.sample_rate > 0 && record.length_in_samples > 0 {
            record.length_in_samples as f64 / f64::from(record.sample_rate)
        } else if record.duration_seconds.is_finite() {
            record.duration_seconds.max(0.0)
        } else {
            0.0
        };
        if self.source_duration <= 0.0 {
            self.source_duration = self
                .overview
                .as_ref()
                .map_or(0.0, |overview| overview.duration_seconds());
        }
        if same_source {
            self.pending_seek = pending_seek;
            self.visible_start = visible_start;
            self.clamp_visible_start();
        }
        if !same_source {
            self.visible_start = 0.0;
            self.pending_seek = None;
            self.geometry.set(PreviewGeometry::default());
        }
        self.overloads.set_record(record);
    }

    /// Updates only overview presentation state so pure tests can prove same-source viewport retention.
    fn update_overview(
        &mut self,
        overview: Option<Arc<WaveformOverview>>,
        loudness: Option<Arc<ShortTermLoudnessHistory>>,
    ) {
        let same_file = match (&self.overview, &overview) {
            (Some(current), Some(next)) => Arc::ptr_eq(current, next),
            _ => false,
        };
        if !same_file {
            self.visible_start = 0.0;
            self.pending_seek = None;
        }
        self.placeholder = if overview.is_some() {
            SharedString::default()
        } else {
            SharedString::from("No waveform for this file")
        };
        self.overloads.clear();
        self.source = None;
        self.source_rate = overview.as_ref().map_or(0, |overview| overview.sample_rate());
        self.source_channels = overview.as_ref().map_or(0, |overview| overview.channels());
        self.source_duration = overview.as_ref().map_or(0.0, |overview| overview.duration_seconds());
        self.source_frames = (self.source_duration * f64::from(self.source_rate)).round() as u64;
        self.overview = overview;
        self.loudness = loudness;
        self.clamp_visible_start();
    }

    /// Returns the duration of the shown file in seconds.
    const fn duration_seconds(&self) -> f64 {
        self.source_duration
    }

    /// Returns the length of the visible range in seconds.
    fn visible_length(&self) -> f64 {
        self.duration_seconds() / self.zoom
    }

    /// Keeps the visible range inside the file.
    fn clamp_visible_start(&mut self) {
        let latest_start = (self.duration_seconds() - self.visible_length()).max(0.0);
        self.visible_start = self.visible_start.clamp(0.0, latest_start);
    }

    /// Keeps the playhead in the middle of the view while playing zoomed in.
    fn follow_playhead(&mut self, position: f64) {
        if self.zoom > MINIMUM_ZOOM {
            let half_length = self.visible_length() * 0.5;
            self.visible_start = position - half_length;
            self.clamp_visible_start();
        }
    }

    /// Latches the pressed source pane and shows a target without pausing or seeking the player.
    fn on_mouse_down(&mut self, event: &MouseDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        if self.duration_seconds() <= 0.0 {
            return;
        }
        if cfg!(target_os = "macos") && event.modifiers.control {
            self.cancel_pending_seek(cx);
            return;
        }
        let Some(plot) = self.geometry.get().plot_at(event.position) else {
            return;
        };
        if let Some(handle) = &self.focus_handle {
            window.focus(handle, cx);
        }
        let viewport = Viewport {
            start: self.visible_start,
            length: self.visible_length(),
            plot,
        };
        self.pending_seek = Some(PendingSeek {
            viewport,
            seconds: if event.click_count == 2 {
                0.0
            } else {
                viewport.x_to_time(event.position.x)
            },
            restart: event.click_count == 2,
        });
        cx.notify();
    }

    /// Updates only the grey target, clamping crossing drags to the initially pressed pane.
    fn on_mouse_move(&mut self, event: &MouseMoveEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if let Some(pending) = &mut self.pending_seek
            && event.pressed_button == Some(MouseButton::Left)
        {
            if !pending.restart {
                pending.seconds = pending.viewport.x_to_time(event.position.x);
            }
            cx.notify();
        }
    }

    /// Clears pointer intent before a popup gesture without changing transport.
    fn cancel_pending_seek(&mut self, cx: &mut Context<Self>) {
        if self.pending_seek.take().is_some() {
            cx.notify();
        }
    }

    /// Commits the release coordinate exactly once, including a release outside the preview.
    fn on_mouse_up(&mut self, event: &MouseUpEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if cfg!(target_os = "macos") && event.modifiers.control {
            self.cancel_pending_seek(cx);
            return;
        }
        if let Some(pending) = self.pending_seek.take() {
            if pending.restart {
                cx.emit(WaveformEvent::RestartRequested);
            } else {
                cx.emit(WaveformEvent::SeekAndPlayRequested(
                    pending.viewport.x_to_time(event.position.x),
                ));
            }
            cx.notify();
        }
    }

    /// Zooms with the vertical wheel, changes the gain by one step per event with Command or Control held,
    /// by one step per ten events with Shift also held, and scrolls the visible range with the horizontal wheel.
    fn on_scroll(&mut self, event: &ScrollEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if self.duration_seconds() <= 0.0 {
            return;
        }
        let delta = event.delta.pixel_delta(px(WHEEL_STEP_PIXELS));
        let vertical = f32::from(delta.y) / WHEEL_STEP_PIXELS;
        let horizontal = f32::from(delta.x);
        let fine = event.modifiers.shift;
        // macOS turns a vertical mouse wheel into a horizontal one while Shift is held,
        // so the fine gesture also reads the horizontal delta.
        let gain_delta = if vertical.abs() > f32::EPSILON {
            vertical
        } else if fine {
            horizontal
        } else {
            0.0
        };
        if gain_delta.abs() > f32::EPSILON && event.modifiers.secondary(cx) {
            let direction: i32 = if gain_delta > 0.0 { 1 } else { -1 };
            if fine {
                // Reversing the wheel starts the count again, so the gain never moves against the wheel.
                if self.fine_gain_wheel_events * direction < 0 {
                    self.fine_gain_wheel_events = 0;
                }
                self.fine_gain_wheel_events += direction;
                if self.fine_gain_wheel_events.abs() >= FINE_GAIN_WHEEL_EVENTS_PER_STEP {
                    self.fine_gain_wheel_events = 0;
                    cx.emit(WaveformEvent::GainBy(GAIN_WHEEL_STEP_DB.copysign(gain_delta)));
                }
            } else {
                self.fine_gain_wheel_events = 0;
                cx.emit(WaveformEvent::GainBy(GAIN_WHEEL_STEP_DB.copysign(gain_delta)));
            }
        } else if vertical.abs() > horizontal.abs() / WHEEL_STEP_PIXELS {
            cx.emit(WaveformEvent::ZoomBy(WHEEL_ZOOM_FACTOR.powf(f64::from(vertical))));
        } else if horizontal.abs() > f32::EPSILON {
            let geometry = self.geometry.get();
            let plot = geometry.plot_at(event.position).unwrap_or_else(|| {
                if has_area(geometry.waveform) {
                    geometry.waveform
                } else {
                    geometry.spectrogram
                }
            });
            let width = f32::from(plot.size.width).max(1.0);
            self.visible_start = f64::from(-horizontal / width).mul_add(self.visible_length(), self.visible_start);
            self.clamp_visible_start();
            cx.notify();
        }
    }
}

impl Default for WaveformView {
    fn default() -> Self {
        Self::new()
    }
}

impl EventEmitter<WaveformEvent> for WaveformView {}

impl FrameCounter {
    /// Records one redraw and prints the rate once per second.
    fn tick(&mut self) {
        self.frames += 1;
        let elapsed = self.window_start.elapsed();
        if elapsed.as_secs_f64() >= 1.0 {
            tracing::info!(
                "waveform: {:.1} frames per second",
                f64::from(self.frames) / elapsed.as_secs_f64()
            );
            self.frames = 0;
            self.window_start = Instant::now();
        }
    }
}

/// Paints a source-time cursor inside one explicit pane, leaving all axis gutters untouched.
fn paint_position(seconds: f64, color: Hsla, viewport: Viewport, window: &mut Window) {
    if viewport.contains(seconds) {
        let width = relative(PLAYHEAD_WIDTH, window.rem_size());
        window.paint_quad(fill(
            Bounds::new(
                point(
                    px(width.mul_add(-0.5, viewport.time_to_x(seconds))),
                    viewport.plot.origin.y,
                ),
                size(px(width), viewport.plot.size.height),
            ),
            color,
        ));
    }
}

/// Centers a pending or unavailable-source message inside its own pane without allocating a retained entity.
fn paint_placeholder(text: SharedString, plot: Bounds<Pixels>, window: &mut Window, cx: &mut App) {
    let run = TextRun {
        len: text.len(),
        font: font(TEXT_FONT),
        color: white(0.7),
        background_color: None,
        underline: None,
        strikethrough: None,
    };
    let height = relative(14.0, window.rem_size());
    let line = window.text_system().shape_line(text, px(height), &[run], None);
    let origin = point(
        plot.origin.x + (plot.size.width - line.width()) * 0.5,
        plot.origin.y + (plot.size.height - px(height)) * 0.5,
    );
    if let Err(error) = line.paint(origin, px(height), TextAlign::Left, None, window, cx) {
        tracing::warn!("Failed to paint preview placeholder: {error:#}");
    }
}

/// Paints one envelope per channel, stacked, scaled by the display gain.
/// Every column keeps at least one pixel of height, so silence shows as a line.
/// With Serato colours each column is painted in its own colour, otherwise each envelope is one blue shape.
fn paint_channels(data: &PaintData, viewport: &Viewport, window: &mut Window) {
    let Some(overview) = data.overview.as_deref() else {
        return;
    };
    let plot = viewport.plot;
    let columns = f32::from(plot.size.width) as usize;
    let channels = overview.channels().clamp(1, 2) as usize;
    let lane_height = f32::from(plot.size.height) / channels as f32;
    let half_height = lane_height * 0.5;
    let half = (half_height - 1.0).max(0.5);
    let sample_rate = f64::from(overview.sample_rate());
    let start_frame = viewport.start * sample_rate;
    let end_frame = (viewport.start + viewport.length) * sample_rate;
    let colours = serato_column_colours(data, viewport, columns);

    for channel in 0..channels {
        let lane_top = lane_height.mul_add(channel as f32, f32::from(plot.origin.y));
        let center = lane_top + half_height;
        let scale = |value: f32| (value * data.display_gain).clamp(-1.0, 1.0);
        let mut extents = overview.column_peaks(channel, start_frame, end_frame, columns);
        for (minimum, maximum) in &mut extents {
            let top = scale(*maximum).mul_add(-half, center).min(center - 0.5);
            let bottom = scale(*minimum).mul_add(-half, center).max(center + 0.5);
            *minimum = top;
            *maximum = bottom;
        }
        if colours.is_empty() {
            paint_envelope(&extents, f32::from(plot.origin.x), window);
        } else {
            paint_coloured_columns(&extents, &colours, f32::from(plot.origin.x), window);
        }
    }
}

/// Returns the Serato colour of each plot column, or nothing when the file has no Serato Overview.
/// When the plot is wider than the colour limit, each colour covers several neighbouring columns.
fn serato_column_colours(data: &PaintData, viewport: &Viewport, columns: usize) -> Vec<Option<OverviewColour>> {
    let duration = data.source_duration;
    let Some(serato_overview) = data.serato_overview.as_deref() else {
        return Vec::new();
    };
    if duration <= 0.0 || columns == 0 {
        return Vec::new();
    }
    let start = viewport.start / duration;
    let end = (viewport.start + viewport.length) / duration;
    let colours = column_colours(serato_overview, start, end, columns);
    if colours.is_empty() || colours.len() == columns {
        return colours;
    }
    (0..columns)
        .map(|column| colours.get(column * colours.len() / columns).copied().flatten())
        .collect()
}

/// Paints one envelope as a single blue shape through the top and bottom of every column.
fn paint_envelope(extents: &[(f32, f32)], left: f32, window: &mut Window) {
    let mut outline: Vec<Point<Pixels>> = Vec::with_capacity(extents.len() * 2);
    outline.extend(
        extents
            .iter()
            .enumerate()
            .map(|(column, &(top, _))| point(px(left + column as f32), px(top))),
    );
    outline.extend(
        extents
            .iter()
            .enumerate()
            .rev()
            .map(|(column, &(_, bottom))| point(px(left + column as f32), px(bottom))),
    );
    if outline.len() < 3 {
        return;
    }
    let mut builder = PathBuilder::fill();
    builder.add_polygon(&outline, true);
    if let Ok(path) = builder.build() {
        window.paint_path(path, color(palette::BLUE));
    }
}

/// Paints one envelope as a one pixel wide bar per column in its Serato colour, blue where a column has none.
fn paint_coloured_columns(extents: &[(f32, f32)], colours: &[Option<OverviewColour>], left: f32, window: &mut Window) {
    for (column, (&(top, bottom), colour)) in extents.iter().zip(colours).enumerate() {
        let fill_color = colour.map_or_else(|| color(palette::BLUE), |colour| color(rgb(colour.to_rgb_u32())));
        window.paint_quad(fill(
            Bounds {
                origin: point(px(left + column as f32), px(top)),
                size: size(px(1.0), px(bottom - top)),
            },
            fill_color,
        ));
    }
}

/// Paints each visible cue point as a pair of inward arrows at the top and bottom edges, in the cue color.
fn paint_cues(cues: &[CueMarker], duration: f64, viewport: &Viewport, window: &mut Window) {
    let plot = viewport.plot;
    let arrow_height = relative(CUE_ARROW_HEIGHT, window.rem_size()).min(f32::from(plot.size.height) / 4.0);
    let half_width =
        relative(CUE_ARROW_WIDTH, window.rem_size()).min(arrow_height * CUE_ARROW_WIDTH / CUE_ARROW_HEIGHT) * 0.5;
    let top = f32::from(plot.origin.y);
    let bottom = top + f32::from(plot.size.height);
    for cue in cues
        .iter()
        .filter(|cue| cue.seconds <= duration && viewport.contains(cue.seconds))
    {
        let x = viewport.time_to_x(cue.seconds);
        let triangles = [
            [
                point(px(x - half_width), px(top)),
                point(px(x + half_width), px(top)),
                point(px(x), px(top + arrow_height)),
            ],
            [
                point(px(x - half_width), px(bottom)),
                point(px(x + half_width), px(bottom)),
                point(px(x), px(bottom - arrow_height)),
            ],
        ];
        for triangle in &triangles {
            let mut outline = PathBuilder::stroke(px(relative(CUE_ARROW_OUTLINE_WIDTH, window.rem_size())));
            outline.add_polygon(triangle, true);
            if let Ok(path) = outline.build() {
                window.paint_path(path, black(0.8));
            }
            let mut body = PathBuilder::fill();
            body.add_polygon(triangle, true);
            if let Ok(path) = body.build() {
                window.paint_path(path, color(rgb(cue.color)));
            }
        }
    }
}

#[cfg(test)]
mod test_artifact_presentation {
    use audiobatch_core::AnalysisStatus;
    use audiobatch_core::waveform::WaveformBuilder;

    use super::*;

    /// Supplies a small overview whose source duration permits a nonzero viewport.
    fn record() -> AnalysisRecord {
        let mut builder = WaveformBuilder::new(1, Some(4_800));
        let samples = vec![0.25; 4_800];
        builder.push(&[&samples]);
        AnalysisRecord {
            path: "preview.wav".into(),
            file_size: 100,
            modified_time_ms: 200,
            status: AnalysisStatus::Cached,
            sample_rate: 48_000,
            channels: 1,
            length_in_samples: 4_800,
            duration_seconds: 0.1,
            waveform: Some(builder.finish(48_000, 4_800)),
            ..AnalysisRecord::default()
        }
    }

    /// Preserves navigation and transport state across a same-source artifact refresh so enrichment cannot disrupt use.
    #[test]
    fn replacing_same_source_artifact_preserves_viewport_and_interaction_state() {
        let mut view = WaveformView::new();
        view.update_record(&record());
        view.zoom = 4.0;
        view.visible_start = 0.04;
        view.display_gain = 0.5;
        view.pending_seek = Some(PendingSeek {
            viewport: Viewport::default(),
            seconds: 0.02,
            restart: false,
        });
        view.cues = vec![CueMarker {
            seconds: 0.02,
            color: 0,
            slot: 0,
        }];
        view.playhead = Some(PlayheadSource::Simulated {
            started: Instant::now(),
            duration: 10.0,
        });
        view.update_record(&record());
        assert!((view.visible_start - 0.04).abs() < f64::EPSILON);
        assert!((view.zoom - 4.0).abs() < f64::EPSILON);
        assert!((view.display_gain - 0.5).abs() < f32::EPSILON);
        assert!(view.pending_seek.is_some());
        assert_eq!(view.cues.len(), 1);
        assert!(view.playhead.is_some());
    }

    /// Retains the selected overview across waveform-free measurement refreshes
    /// so independent cache reads do not blank it.
    #[test]
    fn waveform_free_same_source_measurements_keep_the_selected_artifact() {
        let mut record = record();
        let overview = record.waveform.clone().expect("overview");
        let mut view = WaveformView::new();
        view.update_record(&record);
        view.zoom = 4.0;
        view.visible_start = 0.04;
        record.waveform = None;
        view.update_record(&record);
        assert!(Arc::ptr_eq(
            view.overview.as_ref().expect("retained artifact"),
            &overview
        ));
        assert!((view.visible_start - 0.04).abs() < f64::EPSILON);
    }

    #[test]
    fn waveform_free_sources_retain_independent_timing_for_spectral_interaction() {
        let mut record = record();
        record.waveform = None;
        let mut view = WaveformView::new();
        view.update_record(&record);
        assert!(view.overview.is_none());
        assert!((view.duration_seconds() - 0.1).abs() < f64::EPSILON);
        assert_eq!(view.source_rate, 48_000);
        assert_eq!(view.source_frames, 4_800);
        assert_eq!(view.source_channels, 1);
    }

    /// Zooming or scrolling during a held drag must not move the target, so drags keep the press-time viewport.
    #[test]
    fn pending_drag_targets_keep_the_latched_viewport_after_zoom() {
        let mut view = WaveformView::new();
        view.update_record(&record());
        let plot = Bounds::new(point(px(10.0), px(0.0)), size(px(100.0), px(50.0)));
        let latched = Viewport {
            start: 0.0,
            length: 0.1,
            plot,
        };
        view.pending_seek = Some(PendingSeek {
            viewport: latched,
            seconds: latched.x_to_time(px(60.0)),
            restart: false,
        });
        view.zoom = 4.0;
        view.visible_start = 0.05;
        let pending = view.pending_seek.expect("held gesture");
        assert!((pending.viewport.x_to_time(px(60.0)) - 0.05).abs() < 1.0e-9);
        assert!((pending.seconds - 0.05).abs() < 1.0e-9);
    }

    #[test]
    fn source_change_discards_pending_pointer_intent() {
        let mut record = record();
        let mut view = WaveformView::new();
        view.update_record(&record);
        view.pending_seek = Some(PendingSeek {
            viewport: Viewport::default(),
            seconds: 0.02,
            restart: false,
        });
        record.modified_time_ms += 1;
        view.update_record(&record);
        assert!(view.pending_seek.is_none());
        assert!(view.visible_start.abs() < f64::EPSILON);
    }

    /// Clears an overview after a source identity change so replacement audio cannot display the previous source's peaks.
    #[test]
    fn different_source_does_not_borrow_a_released_waveform() {
        let mut record = record();
        let mut view = WaveformView::new();
        view.update_record(&record);
        record.modified_time_ms += 1;
        record.waveform = None;
        view.update_record(&record);
        assert!(view.overview.is_none());
    }
}
