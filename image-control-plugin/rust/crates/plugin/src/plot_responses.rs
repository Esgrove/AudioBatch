//! Cached analytical crossover responses and transient stereo-position mapping guides, rendered as GPU geometry.

use std::cell::RefCell;
use std::time::{Duration, Instant};

use iced::advanced::{renderer, text};
use iced::{Color, Point, Rectangle, Size};
use iced_baseview as iced;
use image_control_dsp::FrequencyScale;

use super::{
    LABEL_GAP, MAX_FREQUENCY, MIN_FREQUENCY, contained_label, frequency_x, label_at, label_size, line, overlaps_labels,
    theme,
};

const RESPONSE_HEIGHT: f32 = 0.8;
const RESPONSE_STEP: f32 = 2.0;
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

/// Reuses response evaluation across hover, Width, Solo, and redraw changes.
#[derive(Default)]
pub(super) struct ResponseCache {
    key: Option<(Rectangle, [f64; 3], FrequencyScale)>,
    samples: Vec<ResponseSample>,
    colours: [Color; 4],
    #[cfg(test)]
    pub(super) builds: usize,
}

/// UI-only age tracking. Initial settings do not produce an unsolicited mapping overlay.
#[derive(Default)]
pub(super) struct PositionOverlay {
    settings: Option<(f64, f64)>,
    curve: Option<PositionCurve>,
    changed: Option<Instant>,
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

impl ResponseCache {
    /// Builds only when the geometry or projection changes, and only while a band is visible as hovered.
    fn prepare(&mut self, bounds: Rectangle, crossovers: [f64; 3], scale: FrequencyScale) {
        let key = (bounds, crossovers, scale);
        if self.key == Some(key) {
            return;
        }
        self.key = Some(key);
        #[cfg(test)]
        {
            self.builds += 1;
        }
        self.samples.clear();
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
    }
}

impl PositionOverlay {
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
        }
        self.opacity(now) > 0.0
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
#[allow(clippy::too_many_arguments)]
pub(super) fn draw_bands(
    renderer: &mut impl iced::advanced::Renderer,
    cache: &RefCell<ResponseCache>,
    bounds: Rectangle,
    crossovers: [f64; 3],
    scale: FrequencyScale,
    soloed: [bool; 4],
    hovered: Option<usize>,
) {
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
        if hovered == Some(band) {
            gradient(renderer, region, band_colour(band, crossovers), 0.14, 0.0);
        }
        left = right;
    }
    let Some(hovered) = hovered else { return };
    let mut cache = cache.borrow_mut();
    cache.prepare(bounds, crossovers, scale);
    for (band, colour) in cache.colours.iter().copied().enumerate() {
        for pair in cache.samples.windows(2) {
            let [before, after] = pair else { continue };
            let before_level = before.levels.get(band).copied().unwrap_or_default();
            let after_level = after.levels.get(band).copied().unwrap_or_default();
            let previous_y = bounds
                .height
                .mul_add(RESPONSE_HEIGHT.mul_add(-before_level, 1.0), bounds.y);
            let next_y = bounds
                .height
                .mul_add(RESPONSE_HEIGHT.mul_add(-after_level, 1.0), bounds.y);
            let segment = Rectangle::new(
                Point::new(before.x, previous_y.min(next_y)),
                Size::new((after.x - before.x).max(0.5), (next_y - previous_y).abs() + 1.5),
            );
            if band == hovered {
                let fill = Rectangle::new(
                    Point::new(before.x, previous_y.midpoint(next_y)),
                    Size::new(
                        (after.x - before.x).max(0.5),
                        (bounds.y + bounds.height - previous_y.midpoint(next_y)).max(0.0),
                    ),
                );
                gradient(renderer, fill, colour, 0.18 * before_level, 0.0);
            }
            line(
                renderer,
                segment,
                bounds,
                colour.scale_alpha(if band == hovered { 0.85 } else { 0.22 }),
            );
        }
    }
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
pub(super) fn draw_position_curve(
    renderer: &mut impl text::Renderer<Font = iced::Font>,
    overlay: &RefCell<PositionOverlay>,
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
    let steps = (bounds.width / RESPONSE_STEP).ceil() as usize;
    let range = curve.edge * 1.5;
    for left in [true, false] {
        let mut before = curve_point(0.0, left, curve, bounds);
        for step in 1..=steps {
            let difference = f64::from((step as f32 * RESPONSE_STEP / bounds.width).min(1.0)) * range;
            let after = curve_point(difference, left, curve, bounds);
            line(
                renderer,
                Rectangle::new(
                    Point::new(after.x, before.y.min(after.y)),
                    Size::new((before.x - after.x).max(0.5), (after.y - before.y).abs() + 2.0),
                ),
                bounds,
                theme::YELLOW.scale_alpha(0.9 * opacity),
            );
            before = after;
        }
    }
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
        let mut cache = ResponseCache::default();
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
        let mut overlay = PositionOverlay::default();
        let now = Instant::now();
        assert!(!overlay.observe(Some(settings), now));
        assert!(overlay.observe(Some((10.0, 32.0)), now));
        assert_eq!(overlay.opacity(now + Duration::from_millis(2500)), 1.0);
        assert!((overlay.opacity(now + Duration::from_millis(2750)) - 0.5).abs() < 0.0001);
        assert_eq!(overlay.opacity(now + Duration::from_millis(3000)), 0.0);
    }
}
