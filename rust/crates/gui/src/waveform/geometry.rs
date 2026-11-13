//! Pane-local geometry and source-time projection shared by preview painting and pointer gestures.
//! Every visible pane reserves identical relative gutters, keeping paired plots synchronized and equally sized.

use std::sync::OnceLock;

use gpui_kit::{Bounds, Pixels, Point, point, px, size};

use crate::settings::PreviewViewMode;

/// Design reference font size for relative canvas chrome.
const REFERENCE_REM: f32 = 15.0;
/// Shared axis lane, including the gap between labels and source pixels.
const AXIS_GUTTER: f32 = 22.0;
/// Clearance between an axis label and the plot.
const AXIS_GAP: f32 = 2.0;
/// Separation between equally important source representations.
const PANE_GAP: f32 = 2.0;
/// Height of the separate spectral-only source warning strip.
const WARNING_HEIGHT: f32 = 4.0;
/// Magnitude references, with the near-full-scale warning prioritized in compact lanes.
const DECIBEL_LEVELS: [(f32, &str); 2] = [(-1.0, "-1"), (-6.0, "-6")];
/// Immutable display magnitude references, separate from pane-local label occupancy.
static MAGNITUDES: OnceLock<[f32; 2]> = OnceLock::new();

/// The visible part of the file mapped into one explicitly chosen plot.
#[derive(Debug, Clone, Copy, Default)]
pub(super) struct Viewport {
    pub(super) start: f64,
    pub(super) length: f64,
    pub(super) plot: Bounds<Pixels>,
}

/// Resolved plots and independent axis lanes for one frame.
#[derive(Debug, Clone, Copy, Default)]
pub(super) struct PreviewGeometry {
    pub(super) waveform: Bounds<Pixels>,
    pub(super) spectrogram: Bounds<Pixels>,
    pub(super) amplitude_axis: Bounds<Pixels>,
    pub(super) frequency_axis: Bounds<Pixels>,
    pub(super) loudness_axis: Bounds<Pixels>,
    pub(super) loudness: Bounds<Pixels>,
    pub(super) warning: Bounds<Pixels>,
}

/// A collision-filtered magnitude caption at a signed linear amplitude.
#[derive(Debug, Clone, Copy)]
pub(super) struct AmplitudeLabel {
    pub(super) bounds: Bounds<Pixels>,
    pub(super) y: f32,
    #[cfg_attr(not(test), expect(dead_code, reason = "tests assert exact decibel priority"))]
    pub(super) decibels: f32,
    pub(super) text: &'static str,
}

/// Reserved left and optional right axis gutters around one source plot.
struct AxisGutters {
    left: Bounds<Pixels>,
    right: Bounds<Pixels>,
}

impl Viewport {
    /// Projects a source time without clamping, so clipping preserves off-screen history geometry.
    pub(super) fn time_to_x(self, seconds: f64) -> f32 {
        let fraction = if self.length > 0.0 {
            ((seconds - self.start) / self.length) as f32
        } else {
            0.0
        };
        fraction.mul_add(f32::from(self.plot.size.width), f32::from(self.plot.origin.x))
    }

    /// Clamps a crossing drag to the originally pressed pane's visible endpoints.
    pub(super) fn x_to_time(self, x: Pixels) -> f64 {
        let width = f32::from(self.plot.size.width);
        if width <= 0.0 {
            return self.start;
        }
        let fraction = f64::from((f32::from(x - self.plot.origin.x) / width).clamp(0.0, 1.0));
        fraction.mul_add(self.length, self.start)
    }

    /// Reports whether a time lies within the shared visible interval.
    pub(super) fn contains(self, seconds: f64) -> bool {
        seconds >= self.start && seconds <= self.start + self.length
    }
}

impl PreviewGeometry {
    /// Divides the supplied content region into equal panes before reserving common axis lanes.
    pub(super) fn new(bounds: Bounds<Pixels>, mode: PreviewViewMode, loudness: bool, rem: Pixels) -> Self {
        let mut geometry = Self::default();
        if !has_area(bounds) {
            return geometry;
        }
        let scale = f32::from(rem) / REFERENCE_REM;
        let gap = PANE_GAP * scale;
        match mode {
            PreviewViewMode::Waveform => geometry.waveform = bounds,
            PreviewViewMode::Spectrogram => {
                let height = (WARNING_HEIGHT * scale).min(f32::from(bounds.size.height));
                geometry.warning = Bounds::new(bounds.origin, size(bounds.size.width, px(height)));
                geometry.spectrogram = Bounds::new(
                    point(bounds.origin.x, bounds.origin.y + px(height)),
                    size(bounds.size.width, bounds.size.height - px(height)),
                );
            }
            PreviewViewMode::SideBySide => {
                let width = ((f32::from(bounds.size.width) - gap).max(0.0)) * 0.5;
                geometry.waveform = Bounds::new(bounds.origin, size(px(width), bounds.size.height));
                geometry.spectrogram = Bounds::new(
                    point(bounds.origin.x + px(width + gap), bounds.origin.y),
                    size(px(width), bounds.size.height),
                );
            }
            PreviewViewMode::Stacked => {
                let height = ((f32::from(bounds.size.height) - gap).max(0.0)) * 0.5;
                geometry.waveform = Bounds::new(bounds.origin, size(bounds.size.width, px(height)));
                geometry.spectrogram = Bounds::new(
                    point(bounds.origin.x, bounds.origin.y + px(height + gap)),
                    size(bounds.size.width, px(height)),
                );
            }
        }
        let AxisGutters {
            left: waveform_left,
            right: waveform_right,
        } = reserve_axes(&mut geometry.waveform, loudness, scale);
        let AxisGutters {
            left: spectral_left,
            right: spectral_right,
        } = reserve_axes(&mut geometry.spectrogram, loudness, scale);
        geometry.amplitude_axis = waveform_left;
        geometry.frequency_axis = spectral_left;
        if loudness {
            if mode == PreviewViewMode::Spectrogram {
                geometry.loudness = geometry.spectrogram;
                geometry.loudness_axis = spectral_right;
            } else {
                geometry.loudness = geometry.waveform;
                geometry.loudness_axis = waveform_right;
            }
        }
        geometry.warning.origin.x = geometry.spectrogram.origin.x;
        geometry.warning.size.width = geometry.spectrogram.size.width;
        geometry
    }

    /// Returns only a source plot, never an axis, gap, or warning strip, under the initial press.
    pub(super) fn plot_at(self, position: Point<Pixels>) -> Option<Bounds<Pixels>> {
        [self.waveform, self.spectrogram]
            .into_iter()
            .find(|plot| has_area(*plot) && plot.contains(&position))
    }
}

/// Converts a design-relative canvas dimension using the current window, not cached global typography.
pub(super) fn relative(value: f32, rem: Pixels) -> f32 {
    value * f32::from(rem) / REFERENCE_REM
}

/// Returns whether a pane has source pixels to paint or hit test.
pub(super) fn has_area(bounds: Bounds<Pixels>) -> bool {
    bounds.size.width > Pixels::ZERO && bounds.size.height > Pixels::ZERO
}

/// Fits only -1 and -6 dBFS on both polarities, prioritizing -1 dBFS when captions would collide.
pub(super) fn amplitude_labels(geometry: PreviewGeometry, channels: u32, rem: Pixels) -> Vec<AmplitudeLabel> {
    if !has_area(geometry.waveform) || !has_area(geometry.amplitude_axis) || channels == 0 {
        return Vec::new();
    }
    let height = relative(10.0, rem);
    let spacing = relative(2.0, rem);
    let lane_height = f32::from(geometry.waveform.size.height) / channels as f32;
    let magnitudes = MAGNITUDES.get_or_init(|| DECIBEL_LEVELS.map(|(decibels, _)| 10.0_f32.powf(decibels / 20.0)));
    let mut labels = Vec::new();
    for channel in 0..channels {
        if lane_height < height {
            continue;
        }
        let top = (channel as f32).mul_add(lane_height, f32::from(geometry.waveform.origin.y));
        let mut occupied: Vec<(f32, f32)> = Vec::new();
        let mut add = |amplitude: f32, decibels: f32, text: &'static str| {
            let y = (1.0 - amplitude).mul_add(lane_height * 0.5, top);
            let label_top = height.mul_add(-0.5, y).clamp(top, top + lane_height - height);
            if occupied
                .iter()
                .any(|&(start, end)| label_top < end + spacing && label_top + height > start - spacing)
            {
                return;
            }
            occupied.push((label_top, label_top + height));
            labels.push(AmplitudeLabel {
                bounds: Bounds::new(
                    point(geometry.amplitude_axis.origin.x, px(label_top)),
                    size(geometry.amplitude_axis.size.width, px(height)),
                ),
                y,
                decibels,
                text,
            });
        };
        for ((decibels, text), &magnitude) in DECIBEL_LEVELS.into_iter().zip(magnitudes) {
            for polarity in [1.0, -1.0] {
                add(polarity * magnitude, decibels, text);
            }
        }
    }
    labels
}

/// Removes common left and optional right gutters, even when a paired pane does not use its LUFS lane.
fn reserve_axes(plot: &mut Bounds<Pixels>, loudness: bool, scale: f32) -> AxisGutters {
    if !has_area(*plot) {
        return AxisGutters {
            left: Bounds::default(),
            right: Bounds::default(),
        };
    }
    let left_width = (AXIS_GUTTER * scale).min(f32::from(plot.size.width));
    let left = Bounds::new(
        plot.origin,
        size(px(AXIS_GAP.mul_add(-scale, left_width).max(0.0)), plot.size.height),
    );
    plot.origin.x += px(left_width);
    plot.size.width -= px(left_width);
    let right = if loudness {
        let width = (AXIS_GUTTER * scale).min(f32::from(plot.size.width));
        plot.size.width -= px(width);
        Bounds::new(
            point(
                plot.origin.x + plot.size.width + px((AXIS_GAP * scale).min(width)),
                plot.origin.y,
            ),
            size(px(AXIS_GAP.mul_add(-scale, width).max(0.0)), plot.size.height),
        )
    } else {
        Bounds::default()
    };
    AxisGutters { left, right }
}

#[cfg(test)]
mod test_geometry {
    use super::*;

    /// Supplies a non-origin host to expose accidental global-coordinate assumptions.
    fn bounds() -> Bounds<Pixels> {
        Bounds::new(point(px(10.0), px(20.0)), size(px(602.0), px(302.0)))
    }

    #[test]
    fn paired_plots_share_width_and_stacked_edges_at_each_rem() {
        for rem in [12.0, 15.0, 20.0] {
            for mode in [PreviewViewMode::SideBySide, PreviewViewMode::Stacked] {
                for loudness in [false, true] {
                    let geometry = PreviewGeometry::new(bounds(), mode, loudness, px(rem));
                    assert!((f32::from(geometry.waveform.size.width - geometry.spectrogram.size.width)).abs() < 0.001);
                    if mode == PreviewViewMode::Stacked {
                        assert!((f32::from(geometry.waveform.origin.x - geometry.spectrogram.origin.x)).abs() < 0.001);
                    }
                    assert_eq!(has_area(geometry.loudness), loudness);
                }
            }
        }
    }

    #[test]
    fn projection_and_crossing_drag_use_latched_pane_not_other_pane() {
        let geometry = PreviewGeometry::new(bounds(), PreviewViewMode::SideBySide, false, px(15.0));
        let viewport = Viewport {
            start: 4.0,
            length: 8.0,
            plot: geometry.waveform,
        };
        assert!((viewport.x_to_time(px(viewport.time_to_x(7.0))) - 7.0).abs() < 0.00001);
        assert!((viewport.x_to_time(geometry.spectrogram.origin.x) - 12.0).abs() < f64::EPSILON);
        assert!(geometry.plot_at(geometry.frequency_axis.origin).is_none());
    }

    #[test]
    fn loudness_stays_right_of_its_host_with_separate_source_gutters() {
        for rem in [12.0, 15.0, 20.0] {
            for mode in [
                PreviewViewMode::Waveform,
                PreviewViewMode::Spectrogram,
                PreviewViewMode::SideBySide,
                PreviewViewMode::Stacked,
            ] {
                let geometry = PreviewGeometry::new(bounds(), mode, true, px(rem));
                assert!(geometry.loudness_axis.origin.x > geometry.loudness.right());
                assert!(geometry.plot_at(geometry.loudness_axis.origin).is_none());
                if has_area(geometry.waveform) {
                    assert!(geometry.amplitude_axis.right() < geometry.waveform.origin.x);
                    assert_eq!(geometry.loudness.origin, geometry.waveform.origin);
                }
                if has_area(geometry.spectrogram) {
                    assert!(geometry.frequency_axis.right() < geometry.spectrogram.origin.x);
                    assert!(geometry.plot_at(geometry.frequency_axis.origin).is_none());
                }
                if mode == PreviewViewMode::Spectrogram {
                    assert_eq!(geometry.loudness.origin, geometry.spectrogram.origin);
                    assert!(geometry.warning.bottom() <= geometry.spectrogram.origin.y);
                    assert!(!geometry.frequency_axis.intersects(&geometry.loudness_axis));
                }
            }
        }
    }

    #[test]
    fn compact_stereo_lanes_prioritize_minus_one_and_keep_labels_inside_each_lane() {
        let host = Bounds::new(bounds().origin, size(bounds().size.width, px(60.0)));
        let geometry = PreviewGeometry::new(host, PreviewViewMode::Waveform, true, px(15.0));
        let labels = amplitude_labels(geometry, 2, px(15.0));
        assert_eq!(labels.len(), 4);
        assert!(labels.iter().all(|label| (label.decibels + 1.0).abs() < f32::EPSILON));
        for (index, label) in labels.iter().enumerate() {
            assert!(label.bounds.origin.y >= host.origin.y);
            assert!(label.bounds.bottom() <= host.bottom());
            for other in labels.iter().skip(index + 1) {
                assert!(!label.bounds.intersects(&other.bounds));
            }
        }
    }

    #[test]
    fn amplitude_labels_show_only_minus_one_and_minus_six_on_both_polarities() {
        let host = Bounds::new(bounds().origin, size(bounds().size.width, px(1000.0)));
        let geometry = PreviewGeometry::new(host, PreviewViewMode::Waveform, false, px(15.0));
        let labels = amplitude_labels(geometry, 1, px(15.0));
        assert_eq!(labels.len(), 4);
        for decibels in [-1.0, -6.0] {
            assert_eq!(
                labels
                    .iter()
                    .filter(|label| (label.decibels - decibels).abs() < f32::EPSILON)
                    .count(),
                2
            );
        }
        for (index, label) in labels.iter().enumerate() {
            for other in labels.iter().skip(index + 1) {
                assert!(!label.bounds.intersects(&other.bounds));
            }
        }
    }
}
