//! Waveform overview of the selected file, with cue arrows, the playhead, and the optional loudness graph.
//!
//! Follows `ThumbnailComponent.cpp` of the C++ application:
//! channel envelopes stacked on a rounded panel, Serato cue points as inward arrows at the top and bottom,
//! a thin white playhead, and a short-term loudness curve with its axis when the graph is enabled.
//! The envelopes are blue, or coloured column by column from the Serato Overview tag when the file has one,
//! like `SeratoWaveformColouring.cpp` colours them, while their shape still comes from the decoded audio.
//! The view zooms around the playhead and follows it while playing,
//! and it scales the envelope by the input gain.
//! A click or drag seeks, the mouse wheel zooms, and Command or Control with the wheel changes the gain.
//! While audio plays the view requests a new frame on every render, so the playhead moves smoothly.

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Instant;

use audiobatch_core::record::NEGATIVE_INFINITY_LOUDNESS;
use audiobatch_core::serato_tags::Overview;
use audiobatch_core::serato_tags::colouring::{OverviewColour, column_colours};
use audiobatch_core::{ShortTermLoudnessHistory, WaveformOverview};
use audiobatch_playback::PlayheadHandle;
use gpui_kit::{
    App, Bounds, ContentMask, Context, EventEmitter, Hsla, InteractiveElement as _, IntoElement, MouseButton,
    MouseDownEvent, MouseMoveEvent, ParentElement as _, PathBuilder, Pixels, Point, Render, ScrollEvent, SharedString,
    Styled as _, TextAlign, TextRun, Window, canvas, div, fill, font, point, px, size,
};

use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::settings::{DEFAULT_LOUDNESS_MAXIMUM, DEFAULT_LOUDNESS_MINIMUM};
use crate::theme::{TEXT_FONT, black, color, palette, white};

/// Lowest zoom level, the whole file.
pub const MINIMUM_ZOOM: f64 = 1.0;

/// Highest zoom level, a twentieth of the file.
pub const MAXIMUM_ZOOM: f64 = 20.0;

/// Placeholder shown when no file is selected.
pub const NO_FILE_TEXT: &str = "No audio file selected";

/// Inset of the plot inside the panel.
const PLOT_INSET: f32 = 2.0;

/// Width of the loudness axis labels.
const LOUDNESS_AXIS_WIDTH: f32 = 16.0;

/// Gap between the loudness axis labels and the plot.
const LOUDNESS_AXIS_GAP: f32 = 6.0;

/// Target vertical distance between two loudness ticks.
const LOUDNESS_TICK_SPACING: f32 = 32.0;

/// Font size of the loudness axis labels.
const LOUDNESS_FONT_SIZE: f32 = 10.0;

/// Width of the loudness curve.
const LOUDNESS_LINE_WIDTH: f32 = 1.5;

/// Extra width of the dark outline around the loudness curve.
const LOUDNESS_OUTLINE_WIDTH: f32 = 1.4;

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

/// Wheel distance in pixels for one zoom step or one decibel of gain.
const WHEEL_STEP_PIXELS: f32 = 20.0;

/// Zoom factor of one wheel step.
const WHEEL_ZOOM_FACTOR: f64 = 1.2;

/// Events the waveform sends to the app view.
#[derive(Debug, Clone)]
pub enum WaveformEvent {
    /// The user clicked or dragged to a time in seconds.
    SeekRequested(f64),
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
}

/// Paints the overview of one file.
pub struct WaveformView {
    overview: Option<Arc<WaveformOverview>>,
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
    show_loudness: bool,
    loudness_range: LoudnessRange,
    /// Plot bounds from the last paint, used to turn a mouse position into a time.
    plot_bounds: Rc<Cell<Bounds<Pixels>>>,
    /// Counts redraws when `AUDIOBATCH_GUI_DEBUG` is set, for checking the playhead frame rate.
    frame_counter: Option<FrameCounter>,
}

/// Prints the number of redraws per second to stderr.
struct FrameCounter {
    window_start: Instant,
    frames: u32,
}

/// The visible part of the file and its mapping to the plot.
#[derive(Debug, Clone, Copy)]
struct Viewport {
    start: f64,
    length: f64,
    plot: Bounds<Pixels>,
}

/// The loudness at the bottom and the top of the loudness graph axis, in whole LUFS.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct LoudnessRange {
    minimum: i32,
    maximum: i32,
}

/// The loudness readings that fall into one pixel column, as index and value pairs.
#[derive(Debug, Clone, Copy)]
struct ColumnBucket {
    column: i64,
    first: (usize, f32),
    lowest: (usize, f32),
    highest: (usize, f32),
    last: (usize, f32),
}

/// Everything the paint closure needs, copied out of the view for one frame.
struct PaintData {
    overview: Arc<WaveformOverview>,
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

impl FrameCounter {
    /// Records one redraw and prints the rate once per second.
    fn tick(&mut self) {
        self.frames += 1;
        let elapsed = self.window_start.elapsed();
        if elapsed.as_secs_f64() >= 1.0 {
            eprintln!(
                "waveform: {:.1} frames per second",
                f64::from(self.frames) / elapsed.as_secs_f64()
            );
            self.frames = 0;
            self.window_start = Instant::now();
        }
    }
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

impl Viewport {
    /// Returns the horizontal position of a time.
    fn time_to_x(&self, seconds: f64) -> f32 {
        let fraction = if self.length > 0.0 {
            ((seconds - self.start) / self.length) as f32
        } else {
            0.0
        };
        fraction.mul_add(f32::from(self.plot.size.width), f32::from(self.plot.origin.x))
    }

    /// Returns the time at a horizontal position.
    fn x_to_time(&self, x: Pixels) -> f64 {
        let width = f32::from(self.plot.size.width);
        if width <= 0.0 {
            return self.start;
        }
        let fraction = f64::from((f32::from(x - self.plot.origin.x) / width).clamp(0.0, 1.0));
        fraction.mul_add(self.length, self.start)
    }

    /// Returns true when a time is inside the visible range.
    fn contains(&self, seconds: f64) -> bool {
        seconds >= self.start && seconds <= self.start + self.length
    }
}

impl LoudnessRange {
    /// Returns the vertical position of a loudness value, without clamping values outside the axis range.
    fn to_y(self, value: f64, plot: Bounds<Pixels>) -> f32 {
        let minimum = f64::from(self.minimum);
        let fraction = ((value - minimum) / (f64::from(self.maximum) - minimum)) as f32;
        fraction.mul_add(
            -f32::from(plot.size.height),
            f32::from(plot.origin.y + plot.size.height),
        )
    }
}

impl WaveformView {
    /// Creates an empty view.
    #[must_use]
    pub fn new() -> Self {
        Self {
            overview: None,
            serato_overview: None,
            cues: Vec::new(),
            loudness: None,
            placeholder: SharedString::from(NO_FILE_TEXT),
            playhead: None,
            zoom: MINIMUM_ZOOM,
            visible_start: 0.0,
            display_gain: 1.0,
            show_loudness: false,
            loudness_range: LoudnessRange {
                minimum: DEFAULT_LOUDNESS_MINIMUM,
                maximum: DEFAULT_LOUDNESS_MAXIMUM,
            },
            plot_bounds: Rc::new(Cell::new(Bounds::default())),
            frame_counter: crate::debug_enabled().then(|| FrameCounter {
                window_start: Instant::now(),
                frames: 0,
            }),
        }
    }

    /// Shows the overview of an analyzed file.
    /// A different file starts from its beginning, while the same file keeps the visible range.
    pub fn show(
        &mut self,
        overview: Option<Arc<WaveformOverview>>,
        loudness: Option<Arc<ShortTermLoudnessHistory>>,
        cx: &mut Context<Self>,
    ) {
        let same_file = match (&self.overview, &overview) {
            (Some(current), Some(next)) => Arc::ptr_eq(current, next),
            _ => false,
        };
        if !same_file {
            self.visible_start = 0.0;
        }
        self.placeholder = if overview.is_some() {
            SharedString::default()
        } else {
            SharedString::from("No waveform for this file")
        };
        self.overview = overview;
        self.loudness = loudness;
        self.clamp_visible_start();
        cx.notify();
    }

    /// Shows a text instead of an overview, for example while the file is analyzed.
    /// The cue points stay, so they reappear once the overview of the same file is shown.
    pub fn show_placeholder(&mut self, text: impl Into<SharedString>, cx: &mut Context<Self>) {
        self.placeholder = text.into();
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
                .unwrap_or(self.visible_start + old_length / 2.0);
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

    /// Returns the duration of the shown file in seconds.
    fn duration_seconds(&self) -> f64 {
        self.overview
            .as_ref()
            .map_or(0.0, |overview| overview.duration_seconds())
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

    /// Returns the visible range and the plot bounds from the last paint.
    fn viewport(&self) -> Viewport {
        Viewport {
            start: self.visible_start,
            length: self.visible_length(),
            plot: self.plot_bounds.get(),
        }
    }

    /// Keeps the playhead in the middle of the view while playing zoomed in.
    fn follow_playhead(&mut self, position: f64) {
        if self.zoom > MINIMUM_ZOOM {
            self.visible_start = position - self.visible_length() / 2.0;
            self.clamp_visible_start();
        }
    }

    /// Seeks to the clicked time.
    fn on_mouse_down(&mut self, event: &MouseDownEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if self.duration_seconds() > 0.0 {
            cx.emit(WaveformEvent::SeekRequested(
                self.viewport().x_to_time(event.position.x),
            ));
        }
    }

    /// Seeks while the mouse is dragged with the left button held.
    fn on_mouse_move(&mut self, event: &MouseMoveEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if event.pressed_button == Some(MouseButton::Left) && self.duration_seconds() > 0.0 {
            cx.emit(WaveformEvent::SeekRequested(
                self.viewport().x_to_time(event.position.x),
            ));
        }
    }

    /// Zooms with the vertical wheel, changes the gain with Command or Control held,
    /// and scrolls the visible range with the horizontal wheel.
    fn on_scroll(&mut self, event: &ScrollEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if self.duration_seconds() <= 0.0 {
            return;
        }
        let delta = event.delta.pixel_delta(px(WHEEL_STEP_PIXELS));
        let vertical = f32::from(delta.y) / WHEEL_STEP_PIXELS;
        let horizontal = f32::from(delta.x);
        if vertical.abs() > f32::EPSILON && event.modifiers.secondary(cx) {
            cx.emit(WaveformEvent::GainBy(vertical));
        } else if vertical.abs() > horizontal.abs() / WHEEL_STEP_PIXELS {
            cx.emit(WaveformEvent::ZoomBy(WHEEL_ZOOM_FACTOR.powf(f64::from(vertical))));
        } else if horizontal.abs() > f32::EPSILON {
            let width = f32::from(self.plot_bounds.get().size.width).max(1.0);
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

impl Render for WaveformView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if let Some(counter) = &mut self.frame_counter {
            counter.tick();
        }
        let playing = self.playhead.as_ref().is_some_and(PlayheadSource::is_playing);
        let position = self.playhead.as_ref().map(PlayheadSource::position_seconds);
        if playing && self.overview.is_some() {
            window.request_animation_frame();
            if let Some(position) = position {
                self.follow_playhead(position);
            }
        }

        let body = self.overview.clone().map_or_else(
            || {
                div()
                    .size_full()
                    .flex()
                    .items_center()
                    .justify_center()
                    .text_size(px(14.0))
                    .text_color(color(palette::BLUE))
                    .child(self.placeholder.clone())
                    .into_any_element()
            },
            |overview| {
                let playhead = position.and_then(|position| {
                    if playing {
                        Some((position, PLAYING_PLAYHEAD_ALPHA))
                    } else if position > 0.0 {
                        Some((position, PAUSED_PLAYHEAD_ALPHA))
                    } else {
                        None
                    }
                });
                let data = PaintData {
                    overview,
                    serato_overview: self.serato_overview.clone(),
                    loudness: self.loudness.clone(),
                    show_loudness: self.show_loudness,
                    loudness_range: self.loudness_range,
                    cues: self.cues.clone(),
                    visible_start: self.visible_start,
                    visible_length: self.visible_length(),
                    display_gain: self.display_gain,
                    playhead,
                };
                let show_loudness = self.show_loudness;
                let plot_cell = Rc::clone(&self.plot_bounds);
                canvas(
                    move |bounds, _, _| {
                        plot_cell.set(plot_bounds(bounds, show_loudness));
                    },
                    move |bounds, (), window, cx| paint_waveform(&data, bounds, window, cx),
                )
                .size_full()
                .into_any_element()
            },
        );

        div()
            .id("waveform")
            .size_full()
            .p(px(PLOT_INSET))
            .rounded(px(CORNER_RADIUS))
            .bg(color(palette::GREY_MEDIUM_DARK))
            .border_1()
            .border_color(white(OUTLINE_ALPHA))
            .overflow_hidden()
            .child(body)
            .on_mouse_down(MouseButton::Left, cx.listener(Self::on_mouse_down))
            .on_mouse_move(cx.listener(Self::on_mouse_move))
            .on_scroll(cx.listener(Self::on_scroll))
    }
}

/// Returns the plot area inside the canvas, leaving room for the loudness axis labels when the graph is shown.
fn plot_bounds(bounds: Bounds<Pixels>, show_loudness: bool) -> Bounds<Pixels> {
    if !show_loudness {
        return bounds;
    }
    let gutter = (LOUDNESS_AXIS_WIDTH + LOUDNESS_AXIS_GAP).min((f32::from(bounds.size.width) - 1.0).max(0.0));
    Bounds {
        origin: point(bounds.origin.x + px(gutter), bounds.origin.y),
        size: size(bounds.size.width - px(gutter), bounds.size.height),
    }
}

/// Paints the channel envelopes, the loudness graph, the cue arrows, and the playhead.
fn paint_waveform(data: &PaintData, bounds: Bounds<Pixels>, window: &mut Window, cx: &mut App) {
    let plot = plot_bounds(bounds, data.show_loudness);
    if f32::from(plot.size.width) < 2.0 || f32::from(plot.size.height) < 2.0 {
        return;
    }
    let viewport = Viewport {
        start: data.visible_start,
        length: data.visible_length,
        plot,
    };
    paint_channels(data, &viewport, window);
    if data.show_loudness {
        paint_loudness_axis(bounds, plot, data.loudness_range, window, cx);
        if let Some(history) = data.loudness.as_deref() {
            window.with_content_mask(Some(ContentMask { bounds: plot }), |window| {
                paint_loudness_curve(history, &viewport, data.loudness_range, window);
            });
        }
    }
    window.with_content_mask(Some(ContentMask { bounds: plot }), |window| {
        paint_cues(&data.cues, data.overview.duration_seconds(), &viewport, window);
    });
    if let Some((position, alpha)) = data.playhead
        && viewport.contains(position)
    {
        let x = viewport.time_to_x(position);
        window.paint_quad(fill(
            Bounds {
                origin: point(px(x - PLAYHEAD_WIDTH / 2.0), bounds.origin.y),
                size: size(px(PLAYHEAD_WIDTH), bounds.size.height),
            },
            white(alpha),
        ));
    }
}

/// Paints one envelope per channel, stacked, scaled by the display gain.
/// Every column keeps at least one pixel of height, so silence shows as a line.
/// With Serato colours each column is painted in its own colour, otherwise each envelope is one blue shape.
fn paint_channels(data: &PaintData, viewport: &Viewport, window: &mut Window) {
    let plot = viewport.plot;
    let columns = f32::from(plot.size.width) as usize;
    let channels = data.overview.channels().clamp(1, 2) as usize;
    let lane_height = f32::from(plot.size.height) / channels as f32;
    let sample_rate = f64::from(data.overview.sample_rate());
    let start_frame = viewport.start * sample_rate;
    let end_frame = (viewport.start + viewport.length) * sample_rate;
    let colours = serato_column_colours(data, viewport, columns);

    for channel in 0..channels {
        let lane_top = lane_height.mul_add(channel as f32, f32::from(plot.origin.y));
        let center = lane_top + lane_height / 2.0;
        let half = (lane_height / 2.0 - 1.0).max(0.5);
        let scale = |value: f32| (value * data.display_gain).clamp(-1.0, 1.0);
        let extents: Vec<(f32, f32)> = data
            .overview
            .column_peaks(channel, start_frame, end_frame, columns)
            .into_iter()
            .map(|(minimum, maximum)| {
                (
                    scale(maximum).mul_add(-half, center).min(center - 0.5),
                    scale(minimum).mul_add(-half, center).max(center + 0.5),
                )
            })
            .collect();
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
    let duration = data.overview.duration_seconds();
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
        let fill_color = colour.map_or_else(|| color(palette::BLUE), |colour| color(colour.to_rgb_u32()));
        window.paint_quad(fill(
            Bounds {
                origin: point(px(left + column as f32), px(top)),
                size: size(px(1.0), px(bottom - top)),
            },
            fill_color,
        ));
    }
}

/// Paints the loudness axis: evenly spaced labels left of the plot and faint grid lines across it.
fn paint_loudness_axis(
    bounds: Bounds<Pixels>,
    plot: Bounds<Pixels>,
    range: LoudnessRange,
    window: &mut Window,
    cx: &mut App,
) {
    let height = f32::from(plot.size.height);
    let interval = loudness_tick_interval(range.minimum, range.maximum, height);
    let label_right = f32::from(plot.origin.x) - LOUDNESS_AXIS_GAP;
    let label_font = font(TEXT_FONT);
    let first_tick = range.minimum.div_euclid(interval) * interval;
    let first_tick = if first_tick < range.minimum {
        first_tick + interval
    } else {
        first_tick
    };
    for value in (first_tick..=range.maximum).step_by(interval.unsigned_abs() as usize) {
        let y = range.to_y(f64::from(value), plot);
        window.paint_quad(fill(
            Bounds {
                origin: point(plot.origin.x, px(y.round())),
                size: size(plot.size.width, px(1.0)),
            },
            white(0.12),
        ));
        let text = SharedString::from(format!("{value}"));
        let run = TextRun {
            len: text.len(),
            font: label_font.clone(),
            color: white(0.8),
            background_color: None,
            underline: None,
            strikethrough: None,
        };
        let line = window
            .text_system()
            .shape_line(text, px(LOUDNESS_FONT_SIZE), &[run], None);
        let line_height = LOUDNESS_FONT_SIZE + 2.0;
        let top = (y - line_height / 2.0).clamp(
            f32::from(bounds.origin.y),
            (f32::from(bounds.origin.y + bounds.size.height) - line_height).max(f32::from(bounds.origin.y)),
        );
        let origin = point(px(label_right) - line.width(), px(top));
        if let Err(error) = line.paint(origin, px(line_height), TextAlign::Left, None, window, cx) {
            eprintln!("Failed to paint a loudness label: {error:#}");
        }
    }
}

/// Paints the short-term loudness history as an orange-red line with a dark outline.
/// Readings that fall into the same pixel column keep their first, lowest, highest, and last values,
/// and silent readings break the line.
fn paint_loudness_curve(
    history: &ShortTermLoudnessHistory,
    viewport: &Viewport,
    range: LoudnessRange,
    window: &mut Window,
) {
    if history.values.is_empty() || history.sample_rate <= 0.0 || history.samples_per_point == 0 {
        return;
    }
    let interval = f64::from(history.samples_per_point) / history.sample_rate;
    let last_index = history.values.len() - 1;
    let first = ((viewport.start / interval).floor() - 1.0).clamp(0.0, last_index as f64) as usize;
    let last = ((viewport.start + viewport.length) / interval)
        .ceil()
        .clamp(0.0, last_index as f64) as usize;
    let plot_left = f32::from(viewport.plot.origin.x);

    let mut segments: Vec<Vec<Point<Pixels>>> = Vec::new();
    let mut current: Vec<Point<Pixels>> = Vec::new();
    let mut bucket: Option<ColumnBucket> = None;
    let flush = |bucket: &mut Option<ColumnBucket>, current: &mut Vec<Point<Pixels>>| {
        if let Some(column) = bucket.take() {
            let mut readings = [column.first, column.lowest, column.highest, column.last];
            readings.sort_unstable_by_key(|&(index, _)| index);
            let mut previous = None;
            for (index, value) in readings {
                if previous == Some(index) {
                    continue;
                }
                previous = Some(index);
                let x = viewport.time_to_x(history.time_at(index));
                let y = range.to_y(f64::from(value), viewport.plot);
                current.push(point(px(x), px(y)));
            }
        }
    };
    for (index, &value) in history.values.iter().enumerate().take(last + 1).skip(first) {
        if !value.is_finite() || f64::from(value) <= NEGATIVE_INFINITY_LOUDNESS {
            flush(&mut bucket, &mut current);
            if !current.is_empty() {
                segments.push(std::mem::take(&mut current));
            }
            continue;
        }
        let column = (viewport.time_to_x(history.time_at(index)) - plot_left).floor() as i64;
        if bucket.as_ref().is_some_and(|bucket| bucket.column != column) {
            flush(&mut bucket, &mut current);
        }
        match &mut bucket {
            Some(bucket) => {
                if value < bucket.lowest.1 {
                    bucket.lowest = (index, value);
                }
                if value > bucket.highest.1 {
                    bucket.highest = (index, value);
                }
                bucket.last = (index, value);
            }
            None => {
                bucket = Some(ColumnBucket {
                    column,
                    first: (index, value),
                    lowest: (index, value),
                    highest: (index, value),
                    last: (index, value),
                });
            }
        }
    }
    flush(&mut bucket, &mut current);
    if !current.is_empty() {
        segments.push(current);
    }

    let stroke = |width: f32, color: Hsla, window: &mut Window| {
        for segment in &segments {
            let Some((&start, rest)) = segment.split_first() else {
                continue;
            };
            if rest.is_empty() {
                continue;
            }
            let mut builder = PathBuilder::stroke(px(width));
            builder.move_to(start);
            for &point in rest {
                builder.line_to(point);
            }
            if let Ok(path) = builder.build() {
                window.paint_path(path, color);
            }
        }
    };
    stroke(LOUDNESS_LINE_WIDTH + LOUDNESS_OUTLINE_WIDTH, black(0.65), window);
    stroke(LOUDNESS_LINE_WIDTH, color(palette::ORANGE_RED), window);
}

/// Paints each visible cue point as a pair of inward arrows at the top and bottom edges, in the cue color.
fn paint_cues(cues: &[CueMarker], duration: f64, viewport: &Viewport, window: &mut Window) {
    let plot = viewport.plot;
    let arrow_height = CUE_ARROW_HEIGHT.min(f32::from(plot.size.height) / 4.0);
    let half_width = CUE_ARROW_WIDTH.min(arrow_height * CUE_ARROW_WIDTH / CUE_ARROW_HEIGHT) / 2.0;
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
            let mut outline = PathBuilder::stroke(px(CUE_ARROW_OUTLINE_WIDTH));
            outline.add_polygon(triangle, true);
            if let Ok(path) = outline.build() {
                window.paint_path(path, black(0.8));
            }
            let mut body = PathBuilder::fill();
            body.add_polygon(triangle, true);
            if let Ok(path) = body.build() {
                window.paint_path(path, color(cue.color));
            }
        }
    }
}

/// Chooses a whole-LU tick interval that fits the plot height, like the C++ `loudnessTickInterval`.
fn loudness_tick_interval(minimum: i32, maximum: i32, height: f32) -> i32 {
    const INTERVALS: [i32; 9] = [1, 2, 4, 6, 8, 10, 12, 16, 20];
    let target_count = ((height / LOUDNESS_TICK_SPACING) as i32).max(2);
    INTERVALS
        .into_iter()
        .find(|interval| interval * target_count >= maximum - minimum)
        .unwrap_or(20)
}

#[cfg(test)]
mod test_loudness_axis {
    use super::*;

    #[test]
    fn tick_interval_grows_as_the_plot_shrinks() {
        assert_eq!(loudness_tick_interval(-24, -4, 400.0), 2);
        assert_eq!(loudness_tick_interval(-24, -4, 200.0), 4);
        assert_eq!(loudness_tick_interval(-24, -4, 40.0), 10);
    }
}
