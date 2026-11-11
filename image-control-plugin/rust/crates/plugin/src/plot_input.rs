//! Retained plot gestures and pure crossover targets, independent of renderer and host parameter ownership.

use iced::keyboard::{self, Key, Modifiers, key::Named};
use iced::{Event, Point, Rectangle, mouse};
use iced_baseview as iced;
use image_control_dsp::FrequencyScale;

use super::{MAX_FREQUENCY, MIN_FREQUENCY, PlotEvent, frequency_x, hit_test, x_frequency};

/// Nearest float to the reference one-third-octave minimum crossover separation.
const MIN_CROSSOVER_RATIO: f64 = 1.259_921_1_f32 as f64;
const GAIN_DB_PER_PIXEL: f64 = 0.12;
const FINE_DRAG_SHARE: f64 = 0.2;
const WHEEL_DB_PER_STEP: f64 = 0.1;
const FINE_WHEEL_SHARE: f64 = 0.1;
/// The reference macOS adapter scales precise scrolling pixels by 0.5/256 before accumulating at 0.05 per step.
/// Iced exposes pixels directly, so 25.6 logical pixels are one step. Line deltas already count native notches.
const WHEEL_PIXELS_PER_STEP: f64 = 25.6;

/// Captures the operation at pointer-down, independently of later modifier or Link changes.
#[derive(Clone, Copy, Debug)]
enum DragKind {
    Crossover(usize),
    Band(usize),
    Gain(usize, bool),
}

/// Original values make repeated pointer events idempotent and preserve proportional band width.
#[derive(Clone, Copy, Debug)]
struct Drag {
    kind: DragKind,
    origin: Point,
    crossovers: [f64; 3],
    scale: FrequencyScale,
}

/// UI-owned retained state. Epoch changes invalidate origins even if recalled values are unchanged.
#[derive(Default)]
pub(super) struct Interaction {
    drag: Option<Drag>,
    modifiers: Modifiers,
    hovered_band: Option<usize>,
    last_click: Option<iced::advanced::mouse::Click>,
    wheel_remainder: f64,
    epoch: u64,
}

/// Crossover marker or whole band owning hover, including a retained drag beyond the plot.
#[derive(Clone, Copy, Debug, Default)]
pub(super) struct HoverTarget {
    pub(super) marker: Option<usize>,
    pub(super) band: Option<usize>,
}

/// Separates hover notifications from capture, so passive motion does not block global shortcuts.
#[derive(Default)]
pub(super) struct Outcome {
    pub(super) events: Vec<PlotEvent>,
    pub(super) capture: bool,
    pub(super) redraw: bool,
}

impl Interaction {
    /// Routes production mouse and keyboard events, retaining only local gesture origins.
    #[expect(
        clippy::too_many_arguments,
        reason = "Keeps pointer gesture inputs explicit at the event boundary"
    )]
    pub(super) fn update(
        &mut self,
        event: &Event,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        crossovers: [f64; 3],
        scale: FrequencyScale,
        epoch: u64,
        enabled: bool,
    ) -> Outcome {
        let mut result = Outcome::default();
        if self.epoch != epoch || !enabled || self.drag.is_some_and(|drag| drag.scale != scale) {
            self.epoch = epoch;
            self.last_click = None;
            self.wheel_remainder = 0.0;
            let cancelled = self.drag.take().is_some();
            if cancelled {
                result.events.push(PlotEvent::End);
                result.redraw = true;
            }
            self.publish_hover(None, &mut result);
            if cancelled || !enabled {
                return result;
            }
        }
        match event {
            Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers)) => {
                self.modifiers = *modifiers;
                result.redraw = cursor.is_over(bounds);
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) => {
                self.pointer_down(cursor, bounds, crossovers, scale, &mut result);
            }
            Event::Mouse(mouse::Event::CursorMoved { position }) => {
                self.pointer_move(*position, bounds, crossovers, scale, &mut result);
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) => self.end_drag(&mut result),
            Event::Mouse(mouse::Event::CursorLeft) => {
                self.publish_hover(self.drag.and_then(drag_band), &mut result);
                result.redraw = true;
                return result;
            }
            Event::Window(iced::window::Event::Unfocused) => {
                self.end_drag(&mut result);
                self.modifiers = Modifiers::default();
                self.last_click = None;
                self.wheel_remainder = 0.0;
                self.publish_hover(None, &mut result);
                result.redraw = true;
                return result;
            }
            Event::Mouse(mouse::Event::WheelScrolled { delta }) => {
                if let Some(band) = cursor
                    .position()
                    .and_then(|point| band_at(point, bounds, crossovers, scale))
                {
                    self.wheel(*delta, band, &mut result);
                }
            }
            Event::Keyboard(keyboard::Event::KeyPressed { key, modifiers, .. }) => {
                self.modifiers = *modifiers;
                self.key_press(key, cursor, bounds, crossovers, scale, &mut result);
            }
            _ => {}
        }
        let hover_cursor = match event {
            Event::Mouse(mouse::Event::CursorMoved { position }) => mouse::Cursor::Available(*position),
            _ => cursor,
        };
        let hovered = self.hover(hover_cursor, bounds, crossovers, scale, epoch);
        self.publish_hover(hovered.band, &mut result);
        result
    }

    /// Captures one pointer operation, giving marker reset and marker movement precedence over band gains.
    fn pointer_down(
        &mut self,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        crossovers: [f64; 3],
        scale: FrequencyScale,
        result: &mut Outcome,
    ) {
        self.end_drag(result);
        let Some(position) = cursor
            .position()
            .filter(|point| usable(bounds) && bounds.contains(*point))
        else {
            return;
        };
        let click = iced::advanced::mouse::Click::new(position, mouse::Button::Left, self.last_click);
        self.last_click = Some(click);
        let kind = if let Some(index) = hit_test(cursor, bounds, crossovers, scale) {
            if click.kind() == iced::advanced::mouse::click::Kind::Double {
                result.events.push(PlotEvent::Reset(index));
                result.capture = true;
                result.redraw = true;
                self.publish_hover(None, result);
                return;
            }
            result.events.push(PlotEvent::Begin(index));
            DragKind::Crossover(index)
        } else {
            let Some(band) = band_at(position, bounds, crossovers, scale) else {
                return;
            };
            if mid_modifier(self.modifiers) || self.modifiers.alt() {
                let mid = mid_modifier(self.modifiers);
                result.events.push(PlotEvent::BeginGain(band, mid));
                DragKind::Gain(band, mid)
            } else {
                result.events.push(PlotEvent::BeginBand(band));
                DragKind::Band(band)
            }
        };
        self.drag = Some(Drag {
            kind,
            origin: position,
            crossovers,
            scale,
        });
        result.capture = true;
        result.redraw = true;
    }

    /// Applies captured origins during motion, accepting positions beyond the plot while rejecting invalid geometry.
    fn pointer_move(
        &self,
        position: Point,
        bounds: Rectangle,
        crossovers: [f64; 3],
        scale: FrequencyScale,
        result: &mut Outcome,
    ) {
        if let Some(drag) = self.drag {
            if !position.x.is_finite() || !position.y.is_finite() || !usable(bounds) {
                return;
            }
            let change = match drag.kind {
                DragKind::Crossover(index) => PlotEvent::Change(
                    index,
                    clamp_crossover(crossovers, index, x_frequency(position.x, bounds, scale)),
                ),
                DragKind::Band(band) => PlotEvent::ChangeBand(band_drag_target(
                    drag.crossovers,
                    band,
                    drag.origin,
                    position,
                    bounds,
                    scale,
                )),
                DragKind::Gain(band, mid) => {
                    let share = if self.modifiers.shift() { FINE_DRAG_SHARE } else { 1.0 };
                    PlotEvent::ChangeGain(
                        band,
                        mid,
                        f64::from(drag.origin.y - position.y) * GAIN_DB_PER_PIXEL * share,
                    )
                }
            };
            result.events.push(change);
            result.capture = true;
        }
        result.redraw = true;
    }

    /// Closes only a captured pointer gesture, never emitting a duplicate release.
    fn end_drag(&mut self, result: &mut Outcome) {
        if self.drag.take().is_some() {
            result.events.push(PlotEvent::End);
            result.capture = true;
            result.redraw = true;
        }
    }

    /// Converts one admitted wheel event into one complete, balanced gain gesture.
    fn wheel(&mut self, delta: mouse::ScrollDelta, band: usize, result: &mut Outcome) {
        result.capture = true;
        if self.drag.is_some() {
            return;
        }
        let steps = self.wheel_steps(delta);
        if steps.abs() >= 1.0 {
            let mid = mid_modifier(self.modifiers);
            let fine = if self.modifiers.shift() { FINE_WHEEL_SHARE } else { 1.0 };
            result.events.extend([
                PlotEvent::BeginGain(band, mid),
                PlotEvent::ChangeGain(band, mid, steps * WHEEL_DB_PER_STEP * fine),
                PlotEvent::End,
            ]);
        }
    }

    /// Handles hover arrows as discrete transactions and consumes them without nesting during another drag.
    fn key_press(
        &self,
        key: &Key,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        crossovers: [f64; 3],
        scale: FrequencyScale,
        result: &mut Outcome,
    ) {
        if mid_modifier(self.modifiers) || self.modifiers.alt() {
            return;
        }
        let direction = match key {
            Key::Named(Named::ArrowLeft) => -1.0,
            Key::Named(Named::ArrowRight) => 1.0,
            _ => return,
        };
        if self.drag.is_some() {
            result.capture = true;
            return;
        }
        if !cursor.is_over(bounds) || !usable(bounds) {
            return;
        }
        if let Some(index) = hit_test(cursor, bounds, crossovers, scale) {
            result.capture = true;
            let before = edge_frequency(crossovers, index);
            let after = clamp_crossover(
                crossovers,
                index,
                before + key_change(before, direction, self.modifiers.shift()),
            );
            if after.to_bits() != before.to_bits() {
                result
                    .events
                    .extend([PlotEvent::Begin(index), PlotEvent::Change(index, after), PlotEvent::End]);
            }
        } else if let Some(band) = cursor
            .position()
            .and_then(|point| band_at(point, bounds, crossovers, scale))
        {
            result.capture = true;
            let reference = band_reference(crossovers, band);
            let after = ratio_target(
                crossovers,
                band,
                1.0 + key_change(reference, direction, self.modifiers.shift()) / reference,
            );
            if after.map(f64::to_bits) != crossovers.map(f64::to_bits) {
                result
                    .events
                    .extend([PlotEvent::BeginBand(band), PlotEvent::ChangeBand(after), PlotEvent::End]);
            }
        }
    }

    /// Retains hovered ownership during a captured drag, including motion beyond all plot edges.
    pub(super) fn hover(
        &self,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        crossovers: [f64; 3],
        scale: FrequencyScale,
        epoch: u64,
    ) -> HoverTarget {
        if self.epoch == epoch
            && let Some(drag) = self.drag
        {
            return match drag.kind {
                DragKind::Crossover(index) => HoverTarget {
                    marker: Some(index),
                    band: None,
                },
                DragKind::Band(band) | DragKind::Gain(band, _) => HoverTarget {
                    marker: None,
                    band: Some(band),
                },
            };
        }
        let crossover = hit_test(cursor, bounds, crossovers, scale);
        let band = if crossover.is_none() {
            cursor
                .position()
                .and_then(|point| band_at(point, bounds, crossovers, scale))
        } else {
            None
        };
        HoverTarget {
            marker: crossover,
            band,
        }
    }

    /// Mirrors reference cursor priority: marker resize, gain resize, then whole-band hand.
    pub(super) fn cursor(
        &self,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        crossovers: [f64; 3],
        scale: FrequencyScale,
        epoch: u64,
    ) -> mouse::Interaction {
        let hovered = self.hover(cursor, bounds, crossovers, scale, epoch);
        if hovered.marker.is_some() {
            mouse::Interaction::ResizingHorizontally
        } else if self.epoch == epoch && self.drag.is_some_and(|drag| matches!(drag.kind, DragKind::Gain(..)))
            || (hovered.band.is_some() && (mid_modifier(self.modifiers) || self.modifiers.alt()))
        {
            mouse::Interaction::ResizingVertically
        } else if hovered.band.is_some() {
            if self.drag.is_some() {
                mouse::Interaction::Grabbing
            } else {
                mouse::Interaction::Grab
            }
        } else {
            mouse::Interaction::None
        }
    }

    /// Highlights both moved boundaries for a whole-band drag, but not for gain drags.
    pub(super) fn band_edge_active(&self, edge: usize, epoch: u64) -> bool {
        self.epoch == epoch
            && self
                .drag
                .is_some_and(|drag| matches!(drag.kind, DragKind::Band(band) if edge == band || band == edge + 1))
    }

    /// Sends only transitions, without capturing passive pointer movement.
    fn publish_hover(&mut self, band: Option<usize>, result: &mut Outcome) {
        if self.hovered_band != band {
            self.hovered_band = band;
            result.events.push(PlotEvent::HoverBand(band));
            result.redraw = true;
        }
    }

    /// Accumulates substeps and honors horizontal scroll's opposite gain direction.
    /// Iced exposes already-routed deltas but no native direction-inverted flag, so reversal cannot be undone here.
    fn wheel_steps(&mut self, delta: mouse::ScrollDelta) -> f64 {
        let (x, y, pixels) = match delta {
            mouse::ScrollDelta::Lines { x, y } => (x, y, false),
            mouse::ScrollDelta::Pixels { x, y } => (x, y, true),
        };
        let delta = f64::from(if x.abs() > y.abs() { -x } else { y });
        if !delta.is_finite() {
            return 0.0;
        }
        if !pixels {
            self.wheel_remainder = 0.0;
            return if delta.abs() > 0.0 {
                delta.abs().round().max(1.0).copysign(delta)
            } else {
                0.0
            };
        }
        self.wheel_remainder += delta / WHEEL_PIXELS_PER_STEP;
        let steps = self.wheel_remainder.trunc();
        self.wheel_remainder -= steps;
        steps
    }
}

/// Recognizes either supported platform's Mid modifier, with precedence over Alt/Option.
fn mid_modifier(modifiers: Modifiers) -> bool {
    modifiers.control() || modifiers.command()
}

/// Avoids edits from empty, collapsed, or invalid plot rectangles.
fn usable(bounds: Rectangle) -> bool {
    bounds.width.is_finite() && bounds.height.is_finite() && bounds.width > 0.0 && bounds.height > 0.0
}

/// Returns the captured band for hover publication after leaving the window.
const fn drag_band(drag: Drag) -> Option<usize> {
    match drag.kind {
        DragKind::Band(band) | DragKind::Gain(band, _) => Some(band),
        DragKind::Crossover(_) => None,
    }
}

/// Ordinary plot space belongs to a band. Only the outer gutters are empty hit targets.
fn band_at(position: Point, bounds: Rectangle, crossovers: [f64; 3], scale: FrequencyScale) -> Option<usize> {
    (usable(bounds) && bounds.contains(position)).then(|| {
        crossovers
            .into_iter()
            .position(|frequency| position.x < frequency_x(frequency, bounds, scale))
            .unwrap_or(3)
    })
}

/// Reads a fixed crossover without introducing panic paths at the editor's event boundary.
fn edge_frequency(crossovers: [f64; 3], index: usize) -> f64 {
    crossovers.get(index).copied().unwrap_or(MIN_FREQUENCY)
}

/// Clamps a single crossover against its unchanged neighbors using the processor's shared spacing policy.
fn clamp_crossover(crossovers: [f64; 3], index: usize, frequency: f64) -> f64 {
    let lower = if index == 0 {
        MIN_FREQUENCY
    } else {
        edge_frequency(crossovers, index - 1) * MIN_CROSSOVER_RATIO
    };
    let upper = if index == 2 {
        MAX_FREQUENCY
    } else {
        edge_frequency(crossovers, index + 1) / MIN_CROSSOVER_RATIO
    };
    frequency.clamp(lower, upper.max(lower))
}

/// Returns the geometric center used by axis-independent band keys and proportional pointer movement.
fn band_reference(crossovers: [f64; 3], band: usize) -> f64 {
    let low = if band == 0 {
        MIN_FREQUENCY
    } else {
        edge_frequency(crossovers, band - 1)
    };
    let high = if band == 3 {
        MAX_FREQUENCY
    } else {
        edge_frequency(crossovers, band)
    };
    (low * high).sqrt()
}

/// Moves only a band's inner boundaries by one common ratio, retaining outer neighbors and internal spacing.
fn ratio_target(before: [f64; 3], band: usize, ratio: f64) -> [f64; 3] {
    let first = band.saturating_sub(1);
    let last = band.min(2);
    let lower = if first == 0 {
        MIN_FREQUENCY
    } else {
        edge_frequency(before, first - 1) * MIN_CROSSOVER_RATIO
    } / edge_frequency(before, first);
    let upper = if last == 2 {
        MAX_FREQUENCY
    } else {
        edge_frequency(before, last + 1) / MIN_CROSSOVER_RATIO
    } / edge_frequency(before, last);
    let ratio = if lower > upper { 1.0 } else { ratio.clamp(lower, upper) };
    let mut after = before;
    for (edge, value) in after.iter_mut().enumerate() {
        if (first..=last).contains(&edge) {
            *value *= ratio;
        }
    }
    after
}

/// Maps pointer displacement through the displayed axis, applying one ratio to both band boundaries.
/// On Mel the geometric center follows Mel motion while the band's frequency ratio remains unchanged.
fn band_drag_target(
    before: [f64; 3],
    band: usize,
    origin: Point,
    pointer: Point,
    bounds: Rectangle,
    scale: FrequencyScale,
) -> [f64; 3] {
    let reference = band_reference(before, band);
    let offset = f64::from((pointer.x - origin.x) / bounds.width);
    if offset.abs() <= f64::EPSILON {
        return before;
    }
    let share = (scale.normalized(reference) + offset).clamp(0.0, 1.0);
    ratio_target(before, band, scale.frequency(share) / reference)
}

/// Plain-Hz ten-percent/one-percent keyboard changes retain the reference 1–1000 Hz bounds.
fn key_change(reference: f64, direction: f64, fine: bool) -> f64 {
    direction * (reference * if fine { 0.01 } else { 0.1 }).clamp(1.0, 1000.0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use iced::Size;

    const CROSSOVERS: [f64; 3] = [100.0, 500.0, 5000.0];

    fn bounds() -> Rectangle {
        Rectangle::with_size(Size::new(900.0, 300.0))
    }
    fn cursor(x: f32) -> mouse::Cursor {
        mouse::Cursor::Available(Point::new(x, 100.0))
    }
    fn send(input: &mut Interaction, event: &Event, cursor: mouse::Cursor) -> Vec<PlotEvent> {
        input
            .update(
                event,
                cursor,
                bounds(),
                CROSSOVERS,
                FrequencyScale::Logarithmic,
                0,
                true,
            )
            .events
            .into_iter()
            .filter(|event| !matches!(event, PlotEvent::HoverBand(_)))
            .collect()
    }
    fn press() -> Event {
        Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
    }
    fn release() -> Event {
        Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
    }
    fn motion(x: f32, y: f32) -> Event {
        Event::Mouse(mouse::Event::CursorMoved {
            position: Point::new(x, y),
        })
    }
    fn arrow(right: bool, modifiers: Modifiers) -> Event {
        let key = Key::Named(if right { Named::ArrowRight } else { Named::ArrowLeft });
        Event::Keyboard(keyboard::Event::KeyPressed {
            key: key.clone(),
            modified_key: key,
            physical_key: keyboard::key::Physical::Code(if right {
                keyboard::key::Code::ArrowRight
            } else {
                keyboard::key::Code::ArrowLeft
            }),
            location: keyboard::Location::Standard,
            modifiers,
            text: None,
            repeat: false,
        })
    }

    #[test]
    fn pointer_drag_captures_marker_and_releases_outside_with_neighbor_spacing() {
        let mut input = Interaction::default();
        let cursor = cursor(frequency_x(500.0, bounds(), FrequencyScale::Logarithmic));
        assert_eq!(send(&mut input, &press(), cursor), [PlotEvent::Begin(1)]);
        let result = send(&mut input, &motion(2000.0, 900.0), mouse::Cursor::Unavailable);
        assert!(
            matches!(result.as_slice(), [PlotEvent::Change(1, frequency)] if (*frequency - 5000.0 / MIN_CROSSOVER_RATIO).abs() < 1.0e-8)
        );
        assert_eq!(
            send(&mut input, &release(), mouse::Cursor::Unavailable),
            [PlotEvent::End]
        );
        assert_eq!(send(&mut input, &motion(300.0, 100.0), cursor), []);
    }

    #[test]
    fn empty_plot_space_moves_its_band_but_outer_gutters_do_not_open_edits() {
        let mut input = Interaction::default();
        assert_eq!(send(&mut input, &press(), cursor(50.0)), [PlotEvent::BeginBand(0)]);
        assert_eq!(send(&mut input, &release(), cursor(50.0)), [PlotEvent::End]);
        assert_eq!(send(&mut input, &press(), cursor(-1.0)), []);
        assert_eq!(send(&mut input, &press(), mouse::Cursor::Unavailable), []);
        assert!(input.drag.is_none());
    }

    #[test]
    fn whole_band_keeps_ratio_and_stops_at_unchanged_neighbor_on_both_axes() {
        for scale in [FrequencyScale::Logarithmic, FrequencyScale::Mel] {
            let before = [100.0, 500.0, 5000.0];
            let origin = Point::new(300.0, 100.0);
            assert_eq!(band_drag_target(before, 1, origin, origin, bounds(), scale), before);
            for band in 0..4 {
                let target = band_drag_target(before, band, origin, Point::new(3000.0, 100.0), bounds(), scale);
                let first = band.saturating_sub(1);
                let last = band.min(2);
                let ratio = target[first] / before[first];
                assert!((target[last] / before[last] - ratio).abs() < 1.0e-12);
                for edge in 0..3 {
                    if !(first..=last).contains(&edge) {
                        assert_eq!(target[edge], before[edge]);
                    }
                }
                for pair in target.windows(2) {
                    assert!(pair[1] / pair[0] >= MIN_CROSSOVER_RATIO - 1.0e-12);
                }
                assert!(target[0] >= MIN_FREQUENCY && target[2] <= MAX_FREQUENCY);
            }
            let moved = band_drag_target(before, 1, origin, Point::new(345.0, 100.0), bounds(), scale);
            let expected =
                scale.frequency(scale.normalized((100.0_f64 * 500.0).sqrt()) + 0.05) / (100.0_f64 * 500.0).sqrt();
            assert!((moved[0] / 100.0 - expected).abs() < 1.0e-6);
            assert!((moved[1] / moved[0] - 5.0).abs() < 1.0e-12);
        }
    }

    #[test]
    fn keyboard_uses_hover_without_prior_click_and_balances_each_repeat() {
        let mut input = Interaction::default();
        let marker = cursor(frequency_x(500.0, bounds(), FrequencyScale::Logarithmic));
        for (modifiers, expected) in [(Modifiers::default(), 550.0), (Modifiers::SHIFT, 505.0)] {
            assert_eq!(
                send(&mut input, &arrow(true, modifiers), marker),
                [PlotEvent::Begin(1), PlotEvent::Change(1, expected), PlotEvent::End]
            );
        }
        let band = cursor(frequency_x(250.0, bounds(), FrequencyScale::Logarithmic));
        let result = send(&mut input, &arrow(true, Modifiers::default()), band);
        assert!(
            matches!(result.as_slice(), [PlotEvent::BeginBand(1), PlotEvent::ChangeBand(values), PlotEvent::End] if (values[0] - 110.0).abs() < 1.0e-9 && (values[1] - 550.0).abs() < 1.0e-9)
        );
        for modifiers in [Modifiers::CTRL, Modifiers::ALT, Modifiers::COMMAND] {
            assert_eq!(send(&mut input, &arrow(true, modifiers), marker), []);
        }
        assert_eq!(send(&mut input, &press(), marker), [PlotEvent::Begin(1)]);
        let result = input.update(
            &arrow(true, Modifiers::default()),
            marker,
            bounds(),
            CROSSOVERS,
            FrequencyScale::Logarithmic,
            0,
            true,
        );
        assert!(result.capture && result.events.is_empty());
    }

    #[test]
    fn keyboard_limit_noops_create_no_history_or_host_transaction() {
        let mut input = Interaction::default();
        let before = [MIN_FREQUENCY, 500.0, 5000.0];
        let result = input.update(
            &arrow(false, Modifiers::default()),
            cursor(0.0),
            bounds(),
            before,
            FrequencyScale::Logarithmic,
            0,
            true,
        );
        assert!(result.capture && result.events.is_empty());
        assert_eq!(key_change(20.0, -1.0, true), -1.0);
        assert_eq!(key_change(20_000.0, 1.0, false), 1000.0);
    }

    #[test]
    fn gain_drag_is_absolute_from_begin_origin_and_captures_mid_priority() {
        let mut input = Interaction::default();
        let band = cursor(300.0);
        let _ = send(
            &mut input,
            &Event::Keyboard(keyboard::Event::ModifiersChanged(Modifiers::CTRL | Modifiers::ALT)),
            band,
        );
        assert_eq!(send(&mut input, &press(), band), [PlotEvent::BeginGain(1, true)]);
        assert_eq!(
            send(&mut input, &motion(300.0, 90.0), band),
            [PlotEvent::ChangeGain(1, true, 1.2)]
        );
        assert_eq!(
            send(&mut input, &motion(300.0, 90.0), band),
            [PlotEvent::ChangeGain(1, true, 1.2)]
        );
        let _ = send(
            &mut input,
            &Event::Keyboard(keyboard::Event::ModifiersChanged(Modifiers::SHIFT)),
            band,
        );
        assert!(matches!(send(&mut input, &motion(300.0, 50.0), band).as_slice(),
            [PlotEvent::ChangeGain(1, true, delta)] if (*delta - 1.2).abs() < 1.0e-12));
        assert_eq!(send(&mut input, &release(), band), [PlotEvent::End]);
        let _ = send(
            &mut input,
            &Event::Keyboard(keyboard::Event::ModifiersChanged(Modifiers::ALT)),
            band,
        );
        input.last_click = None;
        assert_eq!(send(&mut input, &press(), band), [PlotEvent::BeginGain(1, false)]);
    }

    #[test]
    fn wheel_side_mid_fine_and_substeps_are_discrete_balanced_gestures() {
        let mut input = Interaction::default();
        let band = cursor(300.0);
        let wheel = |delta| Event::Mouse(mouse::Event::WheelScrolled { delta });
        assert_eq!(
            send(&mut input, &wheel(mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 }), band),
            [
                PlotEvent::BeginGain(1, false),
                PlotEvent::ChangeGain(1, false, 0.1),
                PlotEvent::End
            ]
        );
        let _ = send(
            &mut input,
            &Event::Keyboard(keyboard::Event::ModifiersChanged(Modifiers::COMMAND | Modifiers::SHIFT)),
            band,
        );
        let result = send(&mut input, &wheel(mouse::ScrollDelta::Lines { x: 2.0, y: 1.0 }), band);
        assert!(
            matches!(result.as_slice(), [PlotEvent::BeginGain(1, true), PlotEvent::ChangeGain(1, true, delta), PlotEvent::End] if (*delta + 0.02).abs() < 1.0e-12)
        );
        for _ in 0..4 {
            assert_eq!(
                send(&mut input, &wheel(mouse::ScrollDelta::Pixels { x: 0.0, y: 6.0 }), band),
                []
            );
        }
        assert_eq!(
            send(&mut input, &wheel(mouse::ScrollDelta::Pixels { x: 0.0, y: 6.0 }), band).len(),
            3
        );
        let _ = send(&mut input, &press(), band);
        assert_eq!(
            send(&mut input, &wheel(mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 }), band),
            []
        );
    }

    #[test]
    fn double_click_resets_only_a_marker_and_closes_its_previous_drag() {
        let mut input = Interaction::default();
        let marker = cursor(frequency_x(500.0, bounds(), FrequencyScale::Logarithmic));
        assert_eq!(send(&mut input, &press(), marker), [PlotEvent::Begin(1)]);
        assert_eq!(
            send(&mut input, &press(), marker),
            [PlotEvent::End, PlotEvent::Reset(1)]
        );
        assert_eq!(send(&mut input, &release(), marker), []);
    }

    #[test]
    fn recall_epoch_and_focus_loss_cancel_without_stale_writes_or_duplicate_end() {
        for gain in [false, true] {
            let mut input = Interaction::default();
            let cursor = cursor(300.0);
            if gain {
                input.modifiers = Modifiers::CTRL;
            }
            let _ = send(&mut input, &press(), cursor);
            let result = input.update(
                &motion(600.0, 30.0),
                cursor,
                bounds(),
                CROSSOVERS,
                FrequencyScale::Logarithmic,
                1,
                true,
            );
            assert_eq!(
                result
                    .events
                    .iter()
                    .filter(|event| !matches!(event, PlotEvent::HoverBand(_)))
                    .copied()
                    .collect::<Vec<_>>(),
                [PlotEvent::End]
            );
            assert!(input.drag.is_none());
            let result = input.update(
                &release(),
                cursor,
                bounds(),
                CROSSOVERS,
                FrequencyScale::Logarithmic,
                1,
                true,
            );
            assert!(!result.events.contains(&PlotEvent::End));
        }
        let mut input = Interaction::default();
        let _ = send(&mut input, &press(), cursor(300.0));
        assert_eq!(
            send(
                &mut input,
                &Event::Window(iced::window::Event::Unfocused),
                mouse::Cursor::Unavailable
            ),
            [PlotEvent::End]
        );
        assert!(input.drag.is_none());
    }

    #[test]
    fn hover_notifications_track_band_and_markers_without_capturing_motion() {
        let mut input = Interaction::default();
        let result = input.update(
            &motion(300.0, 100.0),
            cursor(300.0),
            bounds(),
            CROSSOVERS,
            FrequencyScale::Logarithmic,
            0,
            true,
        );
        assert_eq!(result.events, [PlotEvent::HoverBand(Some(1))]);
        assert!(!result.capture);
        let marker = frequency_x(500.0, bounds(), FrequencyScale::Logarithmic);
        let result = input.update(
            &motion(marker, 100.0),
            cursor(marker),
            bounds(),
            CROSSOVERS,
            FrequencyScale::Logarithmic,
            0,
            true,
        );
        assert_eq!(result.events, [PlotEvent::HoverBand(None)]);
    }
}
