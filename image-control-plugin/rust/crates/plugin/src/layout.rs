//! Shared editor geometry, preserving the header, analysis, and five-row band-control arrangement.

/// Initial logical size of a newly opened editor.
pub const DEFAULT_SIZE: (f32, f32) = (1000.0, 764.0);
/// Smallest logical size that keeps the analysis and full control rows usable.
pub const MIN_SIZE: (f32, f32) = (760.0, 600.0);
/// Outer inset and section spacing in logical pixels.
pub const GAP: f32 = 8.0;
/// Height of the compact primary header.
pub const HEADER_HEIGHT: f32 = 28.0;
/// Height of the comparison and preset toolbar.
pub const TOOLBAR_HEIGHT: f32 = 32.0;
/// Height of the optional correlation meter row.
pub const CORRELATION_HEIGHT: f32 = 48.0;
/// Default expanded band section height.
pub const BAND_HEIGHT: f32 = 304.0;
/// Minimum expanded band height, including margins and all five labelled slider rows.
pub const MIN_BAND_HEIGHT: f32 = 280.0;
/// Minimum analysis-section height at the smallest editor size.
pub const MIN_PLOT_HEIGHT: f32 = 160.0;
/// Band-strip inset, matching the name, range, and slider alignment.
pub const BAND_INSET: f32 = 10.0;
/// Height of each band-name and frequency-range row.
pub const BAND_HEADER_HEIGHT: f32 = 20.0;
/// Height of a label above a band slider.
pub const SLIDER_LABEL_HEIGHT: f32 = 18.0;
/// Height of a slider and its adjacent value readout.
pub const SLIDER_HEIGHT: f32 = 26.0;
/// Width of the numeric value beside a band slider.
pub const VALUE_WIDTH: f32 = 78.0;
/// Logical plot insets for left-position and bottom-frequency labels.
pub const PLOT_INSETS: [f32; 4] = [13.5, 20.0, 18.0, 28.0];
/// Stable frequency-band names.
pub const BAND_NAMES: [&str; 4] = ["Low", "Low Mid", "High Mid", "High"];
/// Field labels in the fixed top-to-bottom band order.
pub const FIELD_NAMES: [&str; 5] = ["Mid", "Side", "Width", "Recover", "Pan"];

/// Responsive header widths retain every header control at the minimum editor width.
#[derive(Clone, Copy, Debug)]
pub struct Header {
    pub identity: f32,
    pub tap: f32,
    pub toggle: f32,
    pub output_label: f32,
    pub output: f32,
}

/// Resolved section heights are also used by layout tests without creating a native window.
#[derive(Clone, Copy, Debug)]
pub struct Sections {
    pub band: f32,
    pub band_row: f32,
    pub band_header_gap: f32,
}

impl Header {
    /// Compresses only the same header widths that have compact variants in the native editor.
    pub const fn new(width: f32) -> Self {
        if width < 940.0 {
            Self {
                identity: 112.0,
                tap: 34.0,
                toggle: 48.0,
                output_label: 42.0,
                output: 105.0,
            }
        } else {
            Self {
                identity: 140.0,
                tap: 42.0,
                toggle: 60.0,
                output_label: 52.0,
                output: 190.0,
            }
        }
    }
}

impl Sections {
    /// Gives spare height to analysis while keeping all five controls visible at the minimum size.
    pub fn new(height: f32, toolbar: bool, correlation: bool, requested_band_height: f32) -> Self {
        let top = GAP + HEADER_HEIGHT + GAP + if toolbar { TOOLBAR_HEIGHT + GAP } else { 0.0 };
        let correlation = if correlation { CORRELATION_HEIGHT } else { 18.0 };
        let maximum = (height - top - GAP - MIN_PLOT_HEIGHT - GAP - correlation - GAP).max(MIN_BAND_HEIGHT);
        let band = requested_band_height.clamp(MIN_BAND_HEIGHT, maximum);
        let fixed_header = (BAND_INSET + BAND_HEADER_HEIGHT) * 2.0;
        let inner = band - fixed_header;
        let slider_rows_height = 5.0 * (SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT);
        let band_header_gap = (inner - slider_rows_height).clamp(0.0, 16.0);
        Self {
            band,
            band_row: (inner - band_header_gap) / 5.0,
            band_header_gap,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_sections_keep_the_native_geometry() {
        let sections = Sections::new(DEFAULT_SIZE.1, true, true, BAND_HEIGHT);
        assert!((sections.band - 304.0).abs() < f32::EPSILON);
        assert!((sections.band_header_gap - 16.0).abs() < f32::EPSILON);
        assert!((sections.band_row - 45.6).abs() < 0.001);
    }

    #[test]
    fn minimum_size_keeps_five_complete_rows_and_analysis() {
        let sections = Sections::new(MIN_SIZE.1, true, true, BAND_HEIGHT);
        assert!((sections.band - 284.0).abs() < f32::EPSILON);
        assert!(sections.band_row >= SLIDER_LABEL_HEIGHT + SLIDER_HEIGHT);
        let fixed: f32 = [GAP * 6.0, HEADER_HEIGHT, TOOLBAR_HEIGHT, CORRELATION_HEIGHT]
            .into_iter()
            .sum();
        let plot = MIN_SIZE.1 - fixed - sections.band;
        assert!((plot - MIN_PLOT_HEIGHT).abs() < f32::EPSILON);
    }

    #[test]
    fn compact_header_controls_do_not_overlap() {
        for width in [MIN_SIZE.0, 939.0, 940.0, DEFAULT_SIZE.0, 1600.0] {
            let header = Header::new(width);
            let fixed: f32 = [
                header.identity,
                24.0,
                GAP,
                header.tap * 2.0,
                GAP,
                24.0 * 3.0,
                4.0 * 2.0,
                header.toggle * 3.0,
                GAP * 3.0,
                header.output_label,
                header.output,
            ]
            .into_iter()
            .sum();
            let content_width = width - 16.0;
            assert!(fixed <= content_width, "all header controls fit at {width}");
        }
    }

    #[test]
    fn band_order_and_rows_match_the_five_control_contract() {
        assert_eq!(BAND_NAMES, ["Low", "Low Mid", "High Mid", "High"]);
        assert_eq!(FIELD_NAMES, ["Mid", "Side", "Width", "Recover", "Pan"]);
        for height in [MIN_BAND_HEIGHT, BAND_HEIGHT, 520.0] {
            let sections = Sections::new(1200.0, true, true, height);
            let occupied: f32 = [
                (BAND_INSET + BAND_HEADER_HEIGHT) * 2.0,
                sections.band_header_gap,
                sections.band_row * 5.0,
            ]
            .into_iter()
            .sum();
            assert!((occupied - height).abs() < 0.001);
        }
    }
}
