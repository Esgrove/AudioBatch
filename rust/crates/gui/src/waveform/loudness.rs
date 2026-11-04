//! Source loudness axis and bounded pixel-column curve reduction, independent of waveform or spectral hosting.

use audiobatch_core::ShortTermLoudnessHistory;
use audiobatch_core::record::NEGATIVE_INFINITY_LOUDNESS;
use gpui_kit::{App, Bounds, Hsla, PathBuilder, Pixels, Point, Window, fill, point, px, size};

use super::axes::paint_label;
use super::geometry::{Viewport, relative};
use crate::theme::{black, color, palette, white};

/// Target vertical separation between adjacent LUFS captions at the reference rem.
const TICK_SPACING: f32 = 32.0;
/// Width of the source loudness curve at the reference rem.
const LINE_WIDTH: f32 = 1.5;
/// Additional contrast outline around the loudness curve.
const OUTLINE_WIDTH: f32 = 1.4;

/// Validated lower and upper whole-LUFS limits supplied by the settings owner.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct LoudnessRange {
    pub(super) minimum: i32,
    pub(super) maximum: i32,
}

/// Chronological extrema retained when many source readings share one display column.
#[derive(Debug, Clone, Copy)]
struct ColumnBucket {
    column: i64,
    first: (usize, f32),
    lowest: (usize, f32),
    highest: (usize, f32),
    last: (usize, f32),
}

impl LoudnessRange {
    /// Projects an unclamped value so pane clipping preserves curve continuity at the axis limits.
    fn to_y(self, value: f64, plot: Bounds<Pixels>) -> f32 {
        let minimum = f64::from(self.minimum);
        let fraction = ((value - minimum) / (f64::from(self.maximum) - minimum)) as f32;
        fraction.mul_add(-f32::from(plot.size.height), f32::from(plot.bottom()))
    }
}

/// Paints source LUFS captions in their separate left lane and subtle grid lines across only the host plot.
pub(super) fn paint_axis(
    gutter: Bounds<Pixels>,
    plot: Bounds<Pixels>,
    range: LoudnessRange,
    window: &mut Window,
    cx: &mut App,
) {
    let height = f32::from(plot.size.height);
    let interval = tick_interval(range.minimum, range.maximum, height / relative(1.0, window.rem_size()));
    let label_height = relative(12.0, window.rem_size());
    let first = range.minimum.div_euclid(interval) * interval;
    let first = if first < range.minimum { first + interval } else { first };
    for value in (first..=range.maximum).step_by(interval.unsigned_abs() as usize) {
        let y = range.to_y(f64::from(value), plot);
        window.paint_quad(fill(
            Bounds::new(point(plot.origin.x, px(y.round())), size(plot.size.width, px(1.0))),
            white(0.12),
        ));
        let top = label_height.mul_add(-0.5, y).clamp(
            f32::from(gutter.origin.y),
            (f32::from(gutter.bottom()) - label_height).max(f32::from(gutter.origin.y)),
        );
        let bounds = Bounds::new(
            point(gutter.origin.x, px(top)),
            size(gutter.size.width, px(label_height)),
        );
        paint_label(format!("{value}").into(), bounds, false, white(0.8), window, cx);
    }
}

/// Paints chronological first/minimum/maximum/last readings per display column, breaking the line at silence.
pub(super) fn paint_curve(
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
                current.push(point(
                    px(viewport.time_to_x(history.time_at(index))),
                    px(range.to_y(f64::from(value), viewport.plot)),
                ));
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
    let width = relative(LINE_WIDTH, window.rem_size());
    let outline = relative(OUTLINE_WIDTH, window.rem_size());
    stroke(width + outline, black(0.65), window);
    stroke(width, color(palette::ORANGE_RED), window);
}

/// Chooses a whole-LU interval that remains readable as the host pane shrinks.
fn tick_interval(minimum: i32, maximum: i32, height: f32) -> i32 {
    const INTERVALS: [i32; 9] = [1, 2, 4, 6, 8, 10, 12, 16, 20];
    let target_count = ((height / TICK_SPACING) as i32).max(2);
    INTERVALS
        .into_iter()
        .find(|interval| interval * target_count >= maximum - minimum)
        .unwrap_or(20)
}

#[cfg(test)]
mod test_axis {
    use super::*;

    #[test]
    fn tick_interval_grows_as_the_plot_shrinks() {
        assert_eq!(tick_interval(-24, -4, 400.0), 2);
        assert_eq!(tick_interval(-24, -4, 200.0), 4);
        assert_eq!(tick_interval(-24, -4, 40.0), 10);
    }
}
