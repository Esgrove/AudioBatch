//! Logical-pixel geometry transcribed from the current C++ editor, without renderer-dependent measurements.

use iced_baseview::{Point, Rectangle, Size};

pub const DEFAULT_SIZE: (f32, f32) = (
    crate::editor_geometry::DEFAULT_EDITOR_SIZE.0 as f32,
    crate::editor_geometry::DEFAULT_EDITOR_SIZE.1 as f32,
);
pub const MIN_SIZE: (f32, f32) = (
    crate::editor_geometry::MIN_EDITOR_SIZE.0 as f32,
    crate::editor_geometry::MIN_EDITOR_SIZE.1 as f32,
);
pub const MAX_SIZE: (f32, f32) = (
    crate::editor_geometry::MAX_EDITOR_SIZE.0 as f32,
    crate::editor_geometry::MAX_EDITOR_SIZE.1 as f32,
);
pub const GAP: f32 = 8.0;
pub const HEADER_HEIGHT: f32 = 28.0;
pub const TOOLBAR_HEIGHT: f32 = 32.0;
pub const CORRELATION_HEIGHT: f32 = 48.0;
pub const CONTROLS_ACTION_HEIGHT: f32 = 12.0;
pub const RESTORE_HEIGHT: f32 = 18.0;
pub const BAND_HEIGHT: f32 = 304.0;
pub const COMPACT_BAND_HEIGHT: f32 = 28.0;
pub const MIN_BAND_HEIGHT: f32 = 280.0;
pub const MIN_PLOT_HEIGHT: f32 = 160.0;
pub const BAND_INSET: f32 = 10.0;
pub const BAND_HEADER_HEIGHT: f32 = 20.0;
pub const SLIDER_LABEL_HEIGHT: f32 = 18.0;
pub const SLIDER_HEIGHT: f32 = 26.0;
pub const VALUE_WIDTH: f32 = 78.0;
pub const PLOT_INSETS: [f32; 4] = [11.5, 16.0, 18.0, 16.0];
pub const BAND_NAMES: [&str; 4] = ["Low", "Low Mid", "High Mid", "High"];
pub const FIELD_NAMES: [&str; 5] = ["Mid", "Side", "Width", "Recover", "Pan"];

/// Absolute header targets keep Output immediately before the final toolbar chevron.
#[derive(Clone, Copy, Debug)]
pub struct Header {
    pub title: Rectangle,
    pub settings: Rectangle,
    pub pre: Rectangle,
    pub post: Rectangle,
    pub undo: Rectangle,
    pub redo: Rectangle,
    pub mono: Rectangle,
    pub delta: Rectangle,
    pub bypass: Rectangle,
    pub output_label: Rectangle,
    pub output: Rectangle,
    pub toolbar: Rectangle,
}

/// Section bounds include the separate twelve-pixel controls-disclosure margin.
#[derive(Clone, Copy, Debug)]
pub struct Sections {
    pub plot: Rectangle,
    pub divider: Rectangle,
    pub correlation: Rectangle,
    pub bands: Rectangle,
    pub band: f32,
    pub band_row: f32,
    pub band_header_gap: f32,
    pub strip_width: f32,
}

/// Constructs a rectangle without positional tuples at layout call sites.
pub const fn rect(x: f32, y: f32, width: f32, height: f32) -> Rectangle {
    Rectangle::new(Point::new(x, y), Size::new(width, height))
}

impl Header {
    /// Applies the C++ whole-pixel compression order, rather than a width-threshold redesign.
    pub fn new(width: f32) -> Self {
        let width = GAP.mul_add(-2.0, width).floor() as i32;
        let mut pressure = (798 - width).max(0);
        let mut group_gaps = [8; 6];
        let mut icon_gaps = [4; 2];
        let mut identity = [140];
        let mut buttons = [60; 3];
        let mut taps = [42; 2];
        let mut label = [52];
        for (slots, minimum) in [
            (group_gaps.as_mut_slice(), 2),
            (icon_gaps.as_mut_slice(), 2),
            (identity.as_mut_slice(), 112),
            (buttons.as_mut_slice(), 48),
            (taps.as_mut_slice(), 34),
            (label.as_mut_slice(), 42),
        ] {
            while pressure > 0 && slots.iter().any(|slot| *slot > minimum) {
                for slot in &mut *slots {
                    if pressure > 0 && *slot > minimum {
                        *slot -= 1;
                        pressure -= 1;
                    }
                }
            }
        }
        let output = width
            - identity[0]
            - 96
            - taps.iter().sum::<i32>()
            - buttons.iter().sum::<i32>()
            - label[0]
            - group_gaps.iter().sum::<i32>()
            - icon_gaps.iter().sum::<i32>();
        let mut x = GAP;
        let mut take = |width: i32, gap: i32| {
            x += gap as f32;
            let bounds = rect(x, GAP, width as f32, HEADER_HEIGHT);
            x += width as f32;
            bounds
        };
        Self {
            title: take(identity[0], 0),
            settings: take(24, 0),
            pre: take(taps[0], group_gaps[0]),
            post: take(taps[1], 0),
            undo: take(24, group_gaps[1]),
            redo: take(24, icon_gaps[0]),
            mono: take(buttons[0], group_gaps[2] + (output - 240).max(0)),
            delta: take(buttons[1], group_gaps[3]),
            bypass: take(buttons[2], group_gaps[4]),
            output_label: take(label[0], group_gaps[5]),
            output: take(output.min(240), 0),
            toolbar: take(24, icon_gaps[1]),
        }
    }
}

impl Sections {
    /// Resolves current C++ section allocation, retaining full-strip height while compact values release plot space.
    pub fn new(size: Size, toolbar: bool, correlation: bool, requested_band_height: f32, compact: bool) -> Self {
        let top = GAP + HEADER_HEIGHT + GAP + if toolbar { TOOLBAR_HEIGHT + GAP } else { 0.0 };
        let correlation_height = if correlation {
            CORRELATION_HEIGHT
        } else {
            RESTORE_HEIGHT
        };
        let maximum = (size.height - GAP - top - GAP - MIN_PLOT_HEIGHT - correlation_height - CONTROLS_ACTION_HEIGHT)
            .max(MIN_BAND_HEIGHT);
        let band = if compact {
            COMPACT_BAND_HEIGHT
        } else {
            requested_band_height.clamp(MIN_BAND_HEIGHT, maximum)
        };
        let bands_y = size.height - GAP - band;
        let correlation_y = bands_y - CONTROLS_ACTION_HEIGHT - correlation_height;
        let width = GAP.mul_add(-2.0, size.width);
        let inner = BAND_HEADER_HEIGHT.mul_add(-2.0, BAND_INSET.mul_add(-2.0, band));
        let band_header_gap = 5.0_f32
            .mul_add(-(SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT), inner)
            .clamp(0.0, 8.0);
        Self {
            plot: rect(GAP, top, width, (correlation_y - GAP - top).max(0.0)),
            divider: rect(GAP, correlation_y - GAP - 4.0, width, GAP + 8.0),
            correlation: rect(GAP, correlation_y, width, correlation_height),
            bands: rect(GAP, bands_y, width, band),
            band,
            band_row: ((inner - band_header_gap) / 5.0).floor(),
            band_header_gap,
            strip_width: (GAP.mul_add(-3.0, width) * 0.25).floor(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_bounds_match_independently_calculated_cpp_coordinates() {
        let sections = Sections::new(Size::new(1000.0, 764.0), true, true, 304.0, false);
        assert_eq!(sections.plot, rect(8.0, 84.0, 984.0, 300.0));
        assert_eq!(sections.correlation, rect(8.0, 392.0, 984.0, 48.0));
        assert_eq!(sections.bands, rect(8.0, 452.0, 984.0, 304.0));
        assert_eq!(sections.strip_width, 240.0);
        assert_eq!(sections.band_row, 47.0);
        assert_eq!(sections.band_header_gap, 8.0);
        let header = Header::new(1000.0);
        assert_eq!(header.settings, rect(148.0, 8.0, 24.0, 28.0));
        assert_eq!(header.mono, rect(468.0, 8.0, 60.0, 28.0));
        assert_eq!(header.output, rect(724.0, 8.0, 240.0, 28.0));
        assert_eq!(header.toolbar, rect(968.0, 8.0, 24.0, 28.0));
    }

    #[test]
    fn minimum_editor_uses_cpp_compression_and_complete_slider_rows() {
        let sections = Sections::new(Size::new(760.0, 600.0), true, true, 304.0, false);
        assert_eq!(sections.plot, rect(8.0, 84.0, 744.0, 160.0));
        assert_eq!(sections.bands, rect(8.0, 312.0, 744.0, 280.0));
        assert_eq!(sections.band_row, 44.0);
        let header = Header::new(760.0);
        assert_eq!(header.output, rect(536.0, 8.0, 190.0, 28.0));
        assert_eq!(header.toolbar.x + header.toolbar.width, 752.0);
    }

    #[test]
    fn compact_values_keep_window_dimensions_and_release_plot_space() {
        let compact = Sections::new(Size::new(1000.0, 764.0), true, true, 304.0, true);
        assert_eq!(compact.bands, rect(8.0, 728.0, 984.0, 28.0));
        assert_eq!(compact.plot, rect(8.0, 84.0, 984.0, 576.0));
        assert_eq!(FIELD_NAMES, ["Mid", "Side", "Width", "Recover", "Pan"]);
    }

    #[test]
    fn output_stays_right_anchored_at_every_supported_width() {
        for width in 760..=4096 {
            let header = Header::new(width as f32);
            assert_eq!(header.toolbar.x + 24.0, width as f32 - 8.0);
            assert!((190.0..=240.0).contains(&header.output.width));
            assert!(header.output_label.x >= header.bypass.x + header.bypass.width);
        }
    }
}
