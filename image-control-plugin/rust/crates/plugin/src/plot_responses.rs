//! Cached analytical crossover responses and transient stereo-position mapping guides, rendered as GPU geometry.

use std::cell::RefCell;
use std::time::{Duration, Instant};

use iced::advanced::{renderer, text};
use iced::widget::canvas::{self, Path, Stroke};
use iced::{Color, Point, Rectangle, Size};
use iced_baseview as iced;
use image_control_dsp::FrequencyScale;

use super::{
    LABEL_GAP, MAX_FREQUENCY, MIN_FREQUENCY, contained_label, frequency_x, label_at, label_size, line, overlaps_labels,
    theme,
};

const RESPONSE_HEIGHT: f32 = 0.8;
const RESPONSE_STEP: f32 = 2.0;
const CURVE_LINE_WIDTH: f32 = 1.5;
const RESPONSE_FILL_ALPHA: f32 = 0.18;
const REGION_TINT_ALPHA: f32 = 0.14;
const RESPONSE_LINE_ALPHA: f32 = 0.85;
const NEIGHBOUR_LINE_ALPHA: f32 = 0.22;
const HOVER_SECONDS: f64 = 0.08;
const HOVER_SNAP: f32 = 0.002;
const MAX_HOVER_STEP: Duration = Duration::from_millis(100);

/// Plot renderers share vector geometry and text capabilities, including the CPU-only semantic test renderer.
pub(super) trait PlotRenderer:
    text::Renderer<Font = iced::Font> + iced_widget::graphics::geometry::Renderer
{
}

impl<Renderer> PlotRenderer for Renderer where
    Renderer: text::Renderer<Font = iced::Font> + iced_widget::graphics::geometry::Renderer
{
}
const CURVE_HOLD: Duration = Duration::from_millis(2500);
const CURVE_FADE_SECONDS: f32 = 0.5;
const GUIDE_DIFFERENCES: [f64; 7] = [3.0, 6.0, 12.0, 18.0, 24.0, 36.0, 48.0];
/// The reference frequency palette is interpolated in linear light, then encoded back to sRGB for the overlay.
const PALETTE: [(f64, [f32; 3]); 11] = [
    (20.0, [1.0, 0.06, 0.04]),
    (50.0, [1.0, 0.1, 0.04]),
    (75.0, [1.0, 0.6, 0.05]),
    (150.0, [1.0, 0.92, 0.1]),
    (400.0, [0.92, 1.0, 0.1]),
    (1000.0, [0.2, 1.0, 0.15]),
    (2500.0, [0.1, 1.0, 0.7]),
    (4000.0, [0.1, 0.9, 1.0]),
    (8000.0, [0.2, 0.45, 1.0]),
    (13_000.0, [0.6, 0.3, 1.0]),
    (20_000.0, [0.95, 0.3, 1.0]),
];

/// One sample holds all four amplitude-complementary target magnitudes at a projected x coordinate.
#[derive(Clone, Copy)]
struct ResponseSample {
    x: f32,
    levels: [f32; 4],
}

/// Response evaluation depends only on the shared plot geometry and ordered target crossovers.
#[derive(Clone, Copy, PartialEq)]
struct ResponseKey {
    bounds: Rectangle,
    crossovers: [f64; 3],
    scale: FrequencyScale,
}

/// One open stroke and one closed under-curve contour share exactly the same response samples.
struct ResponsePaths {
    stroke: Path,
    fill: Path,
}

/// Frame-local hover easing owns no sound state and advances only from explicit redraw timestamps.
#[derive(Default)]
struct HoverAnimation {
    target: Option<usize>,
    highlights: [f32; 4],
    last_frame: Option<Instant>,
}

impl HoverAnimation {
    /// Starts a target transition without changing what is shown or counting time before pointer arrival.
    fn observe(&mut self, hovered: Option<usize>, now: Instant) -> bool {
        let hovered = hovered.filter(|band| *band < self.highlights.len());
        if self.target != hovered {
            self.target = hovered;
            self.last_frame = Some(self.last_frame.map_or(now, |last| last.max(now)));
        }
        self.animating()
    }

    /// Applies the reference exponential approach, capped after refresh gaps and snapped when visually settled.
    fn advance(&mut self, hovered: Option<usize>, now: Instant) -> bool {
        self.observe(hovered, now);
        let previous = self.last_frame.unwrap_or(now);
        self.last_frame = Some(previous.max(now));
        let elapsed = now.saturating_duration_since(previous).min(MAX_HOVER_STEP);
        let share = (1.0 - (-elapsed.as_secs_f64() / HOVER_SECONDS).exp()) as f32;
        for (band, highlight) in self.highlights.iter_mut().enumerate() {
            let target = if self.target == Some(band) { 1.0 } else { 0.0 };
            let next = (target - *highlight).mul_add(share, *highlight);
            *highlight = if (target - next).abs() < HOVER_SNAP {
                target
            } else {
                next
            };
        }
        self.animating()
    }

    /// Exact snapped endpoints stop requesting frames without a permanently running hover animation.
    fn animating(&self) -> bool {
        self.highlights.iter().enumerate().any(|(band, highlight)| {
            highlight.to_bits() != if self.target == Some(band) { 1.0_f32 } else { 0.0_f32 }.to_bits()
        })
    }
}

/// Reuses response evaluation and paths across hover, Width, Solo, and redraw changes.
/// Settled hover choices retain tessellation, while one transient cache recolors the same paths during easing.
pub(super) struct ResponseCache<Renderer: PlotRenderer = iced::Renderer> {
    key: Option<ResponseKey>,
    samples: Vec<ResponseSample>,
    paths: Vec<ResponsePaths>,
    colours: [Color; 4],
    geometry: [canvas::Cache<Renderer>; 4],
    transient_geometry: canvas::Cache<Renderer>,
    rendered_highlights: Option<[f32; 4]>,
    hover: HoverAnimation,
    #[cfg(test)]
    pub(super) builds: usize,
}

impl<Renderer: PlotRenderer> Default for ResponseCache<Renderer> {
    fn default() -> Self {
        Self {
            key: None,
            samples: Vec::new(),
            paths: Vec::new(),
            colours: [Color::TRANSPARENT; 4],
            geometry: std::array::from_fn(|_| canvas::Cache::default()),
            transient_geometry: canvas::Cache::default(),
            rendered_highlights: None,
            hover: HoverAnimation::default(),
            #[cfg(test)]
            builds: 0,
        }
    }
}

/// UI-only age tracking and retained mapping geometry. Initial settings do not show an unsolicited overlay.
pub(super) struct PositionOverlay<Renderer: PlotRenderer = iced::Renderer> {
    settings: Option<(f64, f64)>,
    curve: Option<PositionCurve>,
    changed: Option<Instant>,
    bounds: Option<Rectangle>,
    paths: Vec<Path>,
    geometry: canvas::Cache<Renderer>,
    rendered_opacity: Option<f32>,
    #[cfg(test)]
    builds: usize,
}

impl<Renderer: PlotRenderer> Default for PositionOverlay<Renderer> {
    fn default() -> Self {
        Self {
            settings: None,
            curve: None,
            changed: None,
            bounds: None,
            paths: Vec::new(),
            geometry: canvas::Cache::default(),
            rendered_opacity: None,
            #[cfg(test)]
            builds: 0,
        }
    }
}

/// Immutable rounded mapping coefficients are recomputed only when the curve settings change.
#[derive(Clone, Copy)]
struct PositionCurve {
    midpoint: f64,
    edge: f64,
    exponent: f32,
    scale: f32,
}

impl PositionCurve {
    /// Uses the analyzer's double derivation followed by the same float coefficient rounding.
    fn new(midpoint: f64, edge: f64) -> Self {
        let argument = 0.5_f64.atanh();
        let exponent = (0.99_f64.atanh() / argument).log(edge / midpoint);
        Self {
            midpoint,
            edge,
            exponent: exponent as f32,
            scale: (midpoint / argument.powf(1.0 / exponent)) as f32,
        }
    }

    /// Maps a signed dB difference to signed center-to-edge position without re-deriving coefficients.
    fn position(self, difference: f64) -> f32 {
        (difference.abs() as f32 / self.scale)
            .powf(self.exponent)
            .tanh()
            .copysign(difference as f32)
    }
}

impl<Renderer: PlotRenderer> ResponseCache<Renderer> {
    /// Updates the visual target on input without advancing the frame-local highlight values.
    pub(super) fn observe_hover(&mut self, hovered: Option<usize>, now: Instant) -> bool {
        self.hover.observe(hovered, now)
    }

    /// Advances hover values only on `RedrawRequested`, reporting whether the widget needs another frame.
    pub(super) fn advance_hover(&mut self, hovered: Option<usize>, now: Instant) -> bool {
        self.hover.advance(hovered, now)
    }

    /// Exposes frame-local opacity values for semantic widget tests without renderer pixel readback.
    #[cfg(test)]
    pub(super) const fn highlights(&self) -> [f32; 4] {
        self.hover.highlights
    }

    /// Builds only when the geometry or projection changes, and only while a band highlight remains visible.
    fn prepare(&mut self, bounds: Rectangle, crossovers: [f64; 3], scale: FrequencyScale) {
        let key = ResponseKey {
            bounds,
            crossovers,
            scale,
        };
        if self.key == Some(key) {
            return;
        }
        self.key = Some(key);
        #[cfg(test)]
        {
            self.builds += 1;
        }
        self.samples.clear();
        self.paths.clear();
        for geometry in &self.geometry {
            geometry.clear();
        }
        self.transient_geometry.clear();
        self.rendered_highlights = None;
        self.colours = std::array::from_fn(|band| band_colour(band, crossovers));
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            return;
        }
        let steps = (bounds.width / RESPONSE_STEP).ceil() as usize;
        for step in 0..=steps {
            let x = (step as f32 * RESPONSE_STEP).min(bounds.width);
            let frequency = scale.frequency(f64::from(x / bounds.width));
            self.samples.push(ResponseSample {
                x: bounds.x + x,
                levels: band_magnitudes(frequency, crossovers),
            });
        }
        self.paths = (0..4)
            .map(|band| {
                let contour = |builder: &mut canvas::path::Builder| {
                    for (index, sample) in self.samples.iter().enumerate() {
                        let level = sample.levels.get(band).copied().unwrap_or_default();
                        let point = Point::new(
                            sample.x,
                            bounds.height.mul_add(RESPONSE_HEIGHT.mul_add(-level, 1.0), bounds.y),
                        );
                        if index == 0 {
                            builder.move_to(point);
                        } else {
                            builder.line_to(point);
                        }
                    }
                };
                ResponsePaths {
                    stroke: Path::new(contour),
                    fill: Path::new(|builder| {
                        contour(builder);
                        builder.line_to(Point::new(bounds.x + bounds.width, bounds.y + bounds.height));
                        builder.line_to(Point::new(bounds.x, bounds.y + bounds.height));
                        builder.close();
                    }),
                }
            })
            .collect();
    }

    /// Recolors retained paths during transitions and reuses each settled hover choice's complete vector layer.
    fn draw(&mut self, renderer: &mut Renderer, bounds: Rectangle) {
        let highlights = self.hover.highlights;
        let strongest = highlights.into_iter().fold(0.0, f32::max);
        let geometry = if self.hover.animating() {
            if self.rendered_highlights != Some(highlights) {
                self.transient_geometry.clear();
                self.rendered_highlights = Some(highlights);
            }
            &self.transient_geometry
        } else {
            let Some(geometry) = self.hover.target.and_then(|hovered| self.geometry.get(hovered)) else {
                return;
            };
            geometry
        };
        let geometry = geometry.draw_with_bounds(renderer, bounds, |frame| {
            for (band, paths) in self.paths.iter().enumerate() {
                let colour = self.colours.get(band).copied().unwrap_or(Color::TRANSPARENT);
                let highlight = highlights.get(band).copied().unwrap_or_default();
                if highlight > 0.0 {
                    let gradient = canvas::gradient::Linear::new(
                        Point::new(bounds.x, bounds.height.mul_add(1.0 - RESPONSE_HEIGHT, bounds.y)),
                        Point::new(bounds.x, bounds.y + bounds.height),
                    )
                    .add_stop(0.0, colour.scale_alpha(RESPONSE_FILL_ALPHA * highlight))
                    .add_stop(1.0, colour.scale_alpha(0.0));
                    frame.fill(&paths.fill, canvas::Gradient::Linear(gradient));
                }
                let alpha =
                    (RESPONSE_LINE_ALPHA - NEIGHBOUR_LINE_ALPHA).mul_add(highlight, NEIGHBOUR_LINE_ALPHA * strongest);
                frame.stroke(&paths.stroke, curve_stroke(colour.scale_alpha(alpha)));
            }
        });
        renderer.draw_geometry(geometry);
    }
}

/// Uses true path strokes with rounded joins and consistent logical-unit weight under renderer antialiasing.
fn curve_stroke(colour: Color) -> Stroke<'static> {
    Stroke::default()
        .with_width(CURVE_LINE_WIDTH)
        .with_color(colour)
        .with_line_join(canvas::LineJoin::Round)
}

impl<Renderer: PlotRenderer> PositionOverlay<Renderer> {
    /// Starts the bounded hold only for an actual setting change, and reports whether more frames are needed.
    pub(super) fn observe(&mut self, settings: Option<(f64, f64)>, now: Instant) -> bool {
        if self.settings != settings {
            if self.settings.is_some() && settings.is_some() {
                self.changed = Some(now);
            }
            if settings.is_none() {
                self.changed = None;
            }
            self.settings = settings;
            self.curve = settings.map(|(midpoint, edge)| PositionCurve::new(midpoint, edge));
            self.bounds = None;
            self.paths.clear();
            self.geometry.clear();
            self.rendered_opacity = None;
        }
        self.opacity(now) > 0.0
    }

    /// Reuses the sampled mirrored paths throughout the hold and fade, independent of their opacity.
    fn prepare(&mut self, bounds: Rectangle, curve: PositionCurve) {
        if self.bounds == Some(bounds) {
            return;
        }
        self.bounds = Some(bounds);
        self.geometry.clear();
        #[cfg(test)]
        {
            self.builds += 1;
        }
        self.paths.clear();
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            return;
        }
        let steps = (bounds.width / RESPONSE_STEP).ceil() as usize;
        self.paths = [true, false]
            .into_iter()
            .map(|left| {
                Path::new(|builder| {
                    builder.move_to(curve_point(0.0, left, curve, bounds));
                    for step in 1..=steps {
                        let share = f64::from((step as f32 * RESPONSE_STEP).min(bounds.width) / bounds.width);
                        builder.line_to(curve_point(share * curve.edge * 1.5, left, curve, bounds));
                    }
                })
            })
            .collect();
    }

    /// Caches tessellation throughout the hold. Fade recolors cached paths without re-evaluating the mapping.
    fn draw(&mut self, renderer: &mut Renderer, bounds: Rectangle, opacity: f32) {
        if self.rendered_opacity != Some(opacity) {
            self.geometry.clear();
            self.rendered_opacity = Some(opacity);
        }
        let geometry = self.geometry.draw_with_bounds(renderer, bounds, |frame| {
            for path in &self.paths {
                frame.stroke(path, curve_stroke(theme::YELLOW.scale_alpha(0.9 * opacity)));
            }
        });
        renderer.draw_geometry(geometry);
    }

    /// Uses elapsed wall time rather than frame counts, matching the reference hold and fade duration.
    fn opacity(&self, now: Instant) -> f32 {
        let Some(changed) = self.changed else { return 0.0 };
        let age = now.saturating_duration_since(changed);
        if age <= CURVE_HOLD {
            1.0
        } else {
            (1.0 - age.saturating_sub(CURVE_HOLD).as_secs_f32() / CURVE_FADE_SECONDS).clamp(0.0, 1.0)
        }
    }
}

/// Draws solo veils first, then the hovered region and cached analytical responses without a CPU pixel buffer.
pub(super) fn draw_bands<Renderer: PlotRenderer>(
    renderer: &mut Renderer,
    cache: &RefCell<ResponseCache<Renderer>>,
    bounds: Rectangle,
    crossovers: [f64; 3],
    scale: FrequencyScale,
    soloed: [bool; 4],
) {
    let mut cache = cache.borrow_mut();
    let highlights = cache.hover.highlights;
    let any_solo = soloed.into_iter().any(|solo| solo);
    let mut left = bounds.x;
    for (band, solo) in soloed.into_iter().enumerate() {
        let right = crossovers.get(band).map_or(bounds.x + bounds.width, |frequency| {
            frequency_x(*frequency, bounds, scale)
        });
        let region = Rectangle::new(
            Point::new(left, bounds.y),
            Size::new((right - left).max(0.0), bounds.height),
        );
        if any_solo && !solo {
            line(renderer, region, bounds, theme::GREY_DARK.scale_alpha(0.55));
        }
        let highlight = highlights.get(band).copied().unwrap_or_default();
        if highlight > 0.0 {
            gradient(
                renderer,
                region,
                band_colour(band, crossovers),
                REGION_TINT_ALPHA * highlight,
                0.0,
            );
        }
        left = right;
    }
    if highlights.into_iter().all(|highlight| highlight <= 0.0) {
        return;
    }
    cache.prepare(bounds, crossovers, scale);
    cache.draw(renderer, bounds);
}

/// Emits a vertical alpha gradient over geometric quads, never a generated image.
fn gradient(renderer: &mut impl iced::advanced::Renderer, bounds: Rectangle, colour: Color, top: f32, bottom: f32) {
    if bounds.width <= 0.0 || bounds.height <= 0.0 {
        return;
    }
    renderer.fill_quad(
        renderer::Quad {
            bounds,
            ..renderer::Quad::default()
        },
        iced::Gradient::Linear(
            iced::gradient::Linear::new(std::f32::consts::FRAC_PI_2)
                .add_stop(0.0, colour.scale_alpha(top))
                .add_stop(1.0, colour.scale_alpha(bottom)),
        ),
    );
}

/// Uses the analytical target rather than a finite FIR FFT or gain-dependent response.
fn band_magnitudes(frequency: f64, crossovers: [f64; 3]) -> [f32; 4] {
    let [low, middle, high] = crossovers.map(|crossover| lowpass(frequency, crossover));
    [
        low as f32,
        (middle - low) as f32,
        (high - middle) as f32,
        (1.0 - high) as f32,
    ]
}

/// One-octave raised-cosine crossover magnitude with exact flat passband and stopband endpoints.
fn lowpass(frequency: f64, crossover: f64) -> f64 {
    let position = (frequency / crossover).log2();
    if position <= -0.5 {
        1.0
    } else if position >= 0.5 {
        0.0
    } else {
        (1.0 - (std::f64::consts::PI * position).sin()) * 0.5
    }
}

/// Samples the same bass-red to treble-violet palette at a band's geometric frequency center.
fn band_colour(band: usize, crossovers: [f64; 3]) -> Color {
    let low = band
        .checked_sub(1)
        .and_then(|edge| crossovers.get(edge))
        .copied()
        .unwrap_or(MIN_FREQUENCY);
    let high = crossovers.get(band).copied().unwrap_or(MAX_FREQUENCY);
    let frequency = (low * high).sqrt();
    for pair in PALETTE.windows(2) {
        let [(low_frequency, low), (high_frequency, high)] = pair else {
            continue;
        };
        if frequency <= *high_frequency {
            let share = (frequency / low_frequency).log(high_frequency / low_frequency) as f32;
            let [red, green, blue] = std::array::from_fn(|channel| {
                let lower = decode_srgb(low.get(channel).copied().unwrap_or_default());
                let higher = decode_srgb(high.get(channel).copied().unwrap_or_default());
                encode_srgb((higher - lower).mul_add(share, lower))
            });
            return Color::from_rgb(red, green, blue);
        }
    }
    Color::from_rgb(0.95, 0.3, 1.0)
}

/// Decodes encoded palette channels before linear-light interpolation.
fn decode_srgb(value: f32) -> f32 {
    if value <= 0.040_45 {
        value / 12.92
    } else {
        ((value + 0.055) / 1.055).powf(2.4)
    }
}
/// Encodes linear-light channels for Iced's color contract.
fn encode_srgb(value: f32) -> f32 {
    if value <= 0.003_130_8 {
        value * 12.92
    } else {
        1.055_f32.mul_add(value.powf(1.0 / 2.4), -0.055)
    }
}

/// Uses the same validated setting ranges as the analyzer, without changing any shared processing state.
pub(super) fn valid_position_curve(midpoint: f64, edge: f64) -> bool {
    midpoint.is_finite()
        && edge.is_finite()
        && (3.0..=24.0).contains(&midpoint)
        && (12.0..=60.0).contains(&edge)
        && edge >= midpoint + 3.0
}

/// Zero dB is at the right edge. Increasing differences move left and mirror about the stereo center.
fn curve_point(difference: f64, left: bool, curve: PositionCurve, bounds: Rectangle) -> Point {
    let share = (difference / (curve.edge * 1.5)).clamp(0.0, 1.0) as f32;
    let position = curve.position(difference) * if left { 1.0 } else { -1.0 };
    Point::new(
        bounds.width.mul_add(1.0 - share, bounds.x),
        (bounds.height * (1.0 - position)).mul_add(0.5, bounds.y),
    )
}

/// Paints right-anchored stereo mapping guides, settings-first collision-aware pills, and center-line dB ticks.
pub(super) fn draw_position_curve<Renderer: PlotRenderer>(
    renderer: &mut Renderer,
    overlay: &RefCell<PositionOverlay<Renderer>>,
    settings: Option<(f64, f64)>,
    bounds: Rectangle,
    viewport: &Rectangle,
    now: Instant,
) {
    let mut overlay = overlay.borrow_mut();
    overlay.observe(settings, now);
    let opacity = overlay.opacity(now);
    let Some(curve) = overlay.curve.filter(|_| opacity > 0.0) else {
        return;
    };
    let mut occupied = Vec::new();
    for (difference, setting) in [curve.midpoint, curve.edge]
        .into_iter()
        .map(|value| (value, true))
        .chain(
            GUIDE_DIFFERENCES
                .into_iter()
                .filter(|value| value.to_bits() != curve.midpoint.to_bits() && value.to_bits() != curve.edge.to_bits())
                .map(|value| (value, false)),
        )
    {
        let colour = if setting { theme::YELLOW } else { theme::GREY_LIGHT };
        for left in [true, false] {
            let point = curve_point(difference, left, curve, bounds);
            line(
                renderer,
                Rectangle::new(Point::new(bounds.x, point.y), Size::new(bounds.width, 1.0)),
                bounds,
                colour.scale_alpha(opacity * if setting { 0.8 } else { 0.3 }),
            );
            let label = format!("{difference:.0} dB");
            let size = label_size(&label);
            let area = Rectangle::new(
                Point::new(bounds.x + LABEL_GAP, bounds.y + LABEL_GAP),
                Size::new(
                    LABEL_GAP.mul_add(-2.0, bounds.width).max(0.0),
                    LABEL_GAP.mul_add(-2.0, bounds.height).max(0.0),
                ),
            );
            if let Some(pill) = contained_label(
                Point::new(size.width.mul_add(-0.5, area.x + area.width) - LABEL_GAP, point.y),
                Size::new(LABEL_GAP.mul_add(2.0, size.width), size.height + LABEL_GAP),
                area,
            )
            .filter(|candidate| !overlaps_labels(*candidate, &occupied))
            {
                draw_pill(renderer, label, pill, colour, opacity, viewport);
                occupied.push(pill);
            }
        }
    }
    overlay.prepare(bounds, curve);
    overlay.draw(renderer, bounds, opacity);
    let range = curve.edge * 1.5;
    let tick = 6.0 * (range / 60.0).ceil().max(1.0);
    let ticks = (range / tick).ceil() as usize;
    for index in 1..ticks {
        let difference = index as f64 * tick;
        let point = curve_point(difference, true, curve, bounds);
        let label = format!("{difference:.0}");
        let size = label_size(&label);
        if let Some(pill) = contained_label(
            Point::new(point.x, bounds.center_y()),
            Size::new(LABEL_GAP.mul_add(2.0, size.width), size.height + LABEL_GAP),
            bounds,
        )
        .filter(|candidate| !overlaps_labels(*candidate, &occupied))
        {
            draw_pill(renderer, label, pill, theme::GREY_LIGHT, opacity, viewport);
            occupied.push(pill);
        }
    }
}

/// Uses contained measured text, matching the main plot labels without truncation or an extra image layer.
fn draw_pill(
    renderer: &mut impl text::Renderer<Font = iced::Font>,
    label: String,
    bounds: Rectangle,
    colour: Color,
    opacity: f32,
    viewport: &Rectangle,
) {
    renderer.fill_quad(
        renderer::Quad {
            bounds,
            border: iced::Border {
                radius: theme::CONTROL_RADIUS.into(),
                ..iced::Border::default()
            },
            ..renderer::Quad::default()
        },
        theme::GREY_DARK.scale_alpha(0.7 * opacity),
    );
    label_at(renderer, label, bounds, colour.scale_alpha(opacity), viewport);
}

#[cfg(test)]
mod tests {
    use super::*;
    use canvas::path::lyon_path::Event;

    #[test]
    fn hover_easing_matches_reference_time_constant_and_is_independent_of_frame_count() {
        let now = Instant::now();
        let mut whole = HoverAnimation::default();
        let mut split = HoverAnimation::default();
        assert!(whole.observe(Some(1), now));
        assert_eq!(whole.highlights, [0.0; 4]);
        assert!(whole.advance(Some(1), now + Duration::from_millis(80)));
        split.observe(Some(1), now);
        for milliseconds in [20, 40, 60, 80] {
            assert!(split.advance(Some(1), now + Duration::from_millis(milliseconds)));
        }
        let expected = (1.0 - (-1.0_f64).exp()) as f32;
        assert!((whole.highlights.get(1).copied().expect("band") - expected).abs() < 1.0e-6);
        assert!(
            whole
                .highlights
                .iter()
                .zip(split.highlights)
                .all(|(a, b)| (a - b).abs() < 1.0e-6)
        );
        let before = whole.highlights;
        whole.observe(Some(2), now + Duration::from_millis(80));
        assert_eq!(whole.highlights, before);
        whole.advance(Some(2), now + Duration::from_millis(160));
        let outgoing = expected * (-1.0_f32).exp();
        assert!((whole.highlights.get(1).copied().expect("outgoing band") - outgoing).abs() < 1.0e-6);
        assert!((whole.highlights.get(2).copied().expect("incoming band") - expected).abs() < 1.0e-6);
        for step in 1..=6 {
            whole.advance(None, now + Duration::from_millis(160 + step * 100));
        }
        assert!(!whole.animating());
        assert_eq!(whole.highlights, [0.0; 4]);
    }

    #[test]
    fn hover_easing_caps_refresh_gaps_ignores_backward_time_and_snaps_settled_values() {
        let now = Instant::now();
        let mut animation = HoverAnimation::default();
        animation.observe(Some(0), now);
        animation.advance(Some(0), now + Duration::from_secs(10));
        let expected = (1.0 - (-0.1_f64 / 0.08).exp()) as f32;
        assert!((animation.highlights.first().copied().expect("band") - expected).abs() < 1.0e-6);
        let before = animation.highlights;
        animation.advance(Some(0), now + Duration::from_secs(5));
        assert_eq!(animation.highlights, before);
        for frame in 1..=5 {
            animation.advance(Some(0), now + Duration::from_millis(10_000 + frame * 100));
        }
        assert!(!animation.animating());
        assert_eq!(animation.highlights, [1.0, 0.0, 0.0, 0.0]);
        assert!(!animation.observe(Some(0), now + Duration::from_secs(11)));
    }

    #[test]
    fn analytical_response_is_complementary_and_half_at_each_separated_crossover() {
        let crossovers = [100.0, 1000.0, 10_000.0];
        for frequency in [20.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10_000.0, 20_000.0] {
            let levels = band_magnitudes(frequency, crossovers);
            assert!((levels.into_iter().sum::<f32>() - 1.0).abs() < 1.0e-6);
            assert!(levels.into_iter().all(|level| (0.0..=1.0).contains(&level)));
        }
        assert_eq!(band_magnitudes(100.0, crossovers), [0.5, 0.5, 0.0, 0.0]);
        assert_eq!(band_magnitudes(1000.0, crossovers), [0.0, 0.5, 0.5, 0.0]);
        assert_eq!(band_magnitudes(10_000.0, crossovers), [0.0, 0.0, 0.5, 0.5]);
    }

    #[test]
    fn response_cache_reuses_hover_redraws_and_includes_exact_right_endpoint() {
        let mut cache = ResponseCache::<iced::Renderer>::default();
        let bounds = Rectangle::with_size(Size::new(701.5, 200.0));
        cache.prepare(bounds, [100.0, 500.0, 5000.0], FrequencyScale::Logarithmic);
        assert_eq!(cache.builds, 1);
        assert_eq!(cache.samples.last().map(|sample| sample.x), Some(701.5));
        cache.prepare(bounds, [100.0, 500.0, 5000.0], FrequencyScale::Logarithmic);
        assert_eq!(cache.builds, 1);
        cache.prepare(bounds, [100.0, 500.0, 5000.0], FrequencyScale::Mel);
        assert_eq!(cache.builds, 2);
    }

    #[test]
    fn response_paths_share_one_continuous_contour_and_close_only_along_the_baseline() {
        let mut cache = ResponseCache::<iced::Renderer>::default();
        for width in [0.5, 701.5, 984.0] {
            let bounds = Rectangle::new(Point::new(28.0, 40.0), Size::new(width, 200.0));
            for scale in [FrequencyScale::Logarithmic, FrequencyScale::Mel] {
                cache.prepare(bounds, [100.0, 500.0, 5000.0], scale);
                assert_eq!(cache.paths.len(), 4);
                for paths in &cache.paths {
                    let stroke = paths.stroke.raw().iter().collect::<Vec<_>>();
                    let fill = paths.fill.raw().iter().collect::<Vec<_>>();
                    assert_eq!(fill.len(), stroke.len() + 2);
                    assert!(stroke.iter().zip(&fill).take(stroke.len() - 1).all(|(a, b)| a == b));
                    assert!(
                        matches!(stroke.first(), Some(Event::Begin { at }) if at.x.to_bits() == bounds.x.to_bits())
                    );
                    assert!(
                        matches!(stroke.last(), Some(Event::End { last, close: false, .. }) if last.x.to_bits() == (bounds.x + bounds.width).to_bits())
                    );
                    assert!(matches!(fill.last(), Some(Event::End { close: true, .. })));
                    assert_eq!(
                        fill.iter().filter(|event| matches!(event, Event::Begin { .. })).count(),
                        1
                    );
                    for event in paths.stroke.raw() {
                        if let Event::Line { from, to } = event {
                            assert!(from.x <= to.x);
                            assert!(to.x <= bounds.x + bounds.width);
                            assert!((bounds.y..=bounds.y + bounds.height).contains(&to.y));
                        }
                    }
                    let baseline_edges = fill.iter().rev().skip(1).take(2).collect::<Vec<_>>();
                    assert!(
                        baseline_edges.iter().all(|event| {
                            matches!(event, Event::Line { to, .. } if to.y.to_bits() == (bounds.y + bounds.height).to_bits())
                        })
                    );
                }
            }
        }
    }

    #[test]
    fn position_paths_keep_exact_mirrored_endpoints_and_reuse_evaluation_through_the_fade() {
        let mut overlay = PositionOverlay::<iced::Renderer>::default();
        let bounds = Rectangle::new(Point::new(28.0, 40.0), Size::new(701.5, 200.0));
        let now = Instant::now();
        overlay.observe(Some((12.0, 32.0)), now);
        overlay.observe(Some((10.0, 32.0)), now);
        let curve = overlay.curve.expect("validated mapping");
        overlay.prepare(bounds, curve);
        assert_eq!(overlay.builds, 1);
        assert_eq!(overlay.paths.len(), 2);
        let first = overlay.paths.first().expect("left mapping");
        let second = overlay.paths.last().expect("right mapping");
        for (left, right) in first.raw().iter().zip(second.raw().iter()) {
            match (left, right) {
                (Event::Begin { at: left }, Event::Begin { at: right }) => {
                    assert_eq!(left.x, bounds.x + bounds.width);
                    assert_eq!(left.y, bounds.center_y());
                    assert_eq!(left, right);
                }
                (Event::Line { to: left, .. }, Event::Line { to: right, .. }) => {
                    assert_eq!(left.x, right.x);
                    assert!(bounds.center_y().mul_add(-2.0, left.y + right.y).abs() < 0.0001);
                }
                (Event::End { last: left, .. }, Event::End { last: right, .. }) => {
                    assert_eq!(left.x, bounds.x);
                    assert_eq!(right.x, bounds.x);
                }
                _ => panic!("mirrored paths have matching segment types"),
            }
        }
        assert!(overlay.observe(Some((10.0, 32.0)), now + Duration::from_millis(2750)));
        overlay.prepare(bounds, curve);
        assert_eq!(overlay.builds, 1);
        let resized = Rectangle::new(bounds.position(), Size::new(700.0, 200.0));
        overlay.prepare(resized, curve);
        assert_eq!(overlay.builds, 2);
        overlay.observe(Some((12.0, 32.0)), now);
        overlay.prepare(resized, overlay.curve.expect("changed mapping"));
        assert_eq!(overlay.builds, 3);
        assert_eq!(curve_stroke(theme::YELLOW).width, 1.5);
    }

    #[test]
    fn mapping_uses_right_origin_mirrored_positions_and_bounded_hold_fade() {
        let bounds = Rectangle::new(Point::new(20.0, 40.0), Size::new(700.0, 200.0));
        let settings = (12.0, 32.0);
        let curve = PositionCurve::new(settings.0, settings.1);
        assert_eq!(curve_point(0.0, true, curve, bounds), Point::new(720.0, 140.0));
        let left = curve_point(12.0, true, curve, bounds);
        let right = curve_point(12.0, false, curve, bounds);
        assert!((left.y - 90.0).abs() < 0.001);
        assert!(bounds.center_y().mul_add(-2.0, left.y + right.y).abs() < 0.001);
        assert!(left.x < bounds.x + bounds.width);
        assert!((curve.position(32.0) - 0.99).abs() < 0.0001);
        let mut overlay = PositionOverlay::<iced::Renderer>::default();
        let now = Instant::now();
        assert!(!overlay.observe(Some(settings), now));
        assert!(overlay.observe(Some((10.0, 32.0)), now));
        assert_eq!(overlay.opacity(now + Duration::from_millis(2500)), 1.0);
        assert!((overlay.opacity(now + Duration::from_millis(2750)) - 0.5).abs() < 0.0001);
        assert_eq!(overlay.opacity(now + Duration::from_millis(3000)), 0.0);
    }
}
