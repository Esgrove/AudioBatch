//! Colours of the Serato Overview raster, for colouring a waveform drawn from the audio.
//!
//! Ports the colour math of `SeratoWaveformColouring.cpp` from the C++ project.
//! Each cell is an index into a zero-based 6x6x6 RGB cube:
//! red is `index / 36 * 51`, green is `index % 36 / 6 * 51`, and blue is `index % 6 * 51`.
//! Indices 0 and 1 are background, and 216 to 255 are unverified, so both are skipped.
//! The colour of a slice is the mean of its valid cells, whatever their rows.
//! Between slice centres the colour sums and weights are interpolated, not the indices,
//! and a column that spans several slices weights each slice by its overlap.
//! The number of colours is bounded by the visible width, not by the slice count.

use crate::overview::{Overview, Slice};

/// Most colours computed for one viewport, the C++ limit on gradient stops.
pub const MAXIMUM_COLOUR_STOPS: usize = 4096;

/// Number of levels of each component in the colour cube.
const PALETTE_LEVELS: u16 = 6;

/// Number of palette entries in one red plane of the cube.
const PALETTE_PLANE_SIZE: u16 = PALETTE_LEVELS * PALETTE_LEVELS;

/// Number of palette entries in the cube. Higher indices are skipped.
const PALETTE_SIZE: u16 = PALETTE_PLANE_SIZE * PALETTE_LEVELS;

/// Component step between two levels of the cube.
const PALETTE_COMPONENT_STEP: u16 = 51;

/// Highest value of a colour component.
const MAXIMUM_COMPONENT: f64 = 255.0;

/// An opaque colour with components from 0 to 1.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct OverviewColour {
    /// Red component.
    pub red: f32,
    /// Green component.
    pub green: f32,
    /// Blue component.
    pub blue: f32,
}

/// Decoded colour sums and the weight of the valid cells they came from, kept until the final mean.
#[derive(Debug, Clone, Copy, Default)]
struct Contributions {
    components: [f64; 3],
    weight: f64,
}

impl OverviewColour {
    /// Returns the colour as a `0xRRGGBB` value.
    #[must_use]
    pub fn to_rgb_u32(self) -> u32 {
        let component = |value: f32| (value.clamp(0.0, 1.0) * 255.0).round() as u32;
        (component(self.red) << 16) | (component(self.green) << 8) | component(self.blue)
    }
}

impl Contributions {
    /// Returns the contributions of one slice with unit weight per cell.
    fn of_slice(cells: &Slice) -> Self {
        let mut contributions = Self::default();
        contributions.add_slice(cells, 1.0);
        contributions
    }

    /// Adds the valid cells of a slice, each with the given weight.
    fn add_slice(&mut self, cells: &Slice, weight: f64) {
        for &index in cells {
            let index = u16::from(index);
            if index <= 1 || index >= PALETTE_SIZE {
                continue;
            }
            let decoded = [
                index / PALETTE_PLANE_SIZE,
                index % PALETTE_PLANE_SIZE / PALETTE_LEVELS,
                index % PALETTE_LEVELS,
            ];
            for (sum, level) in self.components.iter_mut().zip(decoded) {
                *sum = f64::from(level * PALETTE_COMPONENT_STEP).mul_add(weight, *sum);
            }
            self.weight += weight;
        }
    }

    /// Returns the mean colour, or none when no valid cell contributed.
    fn colour(&self) -> Option<OverviewColour> {
        if self.weight <= 0.0 {
            return None;
        }
        let divisor = self.weight * MAXIMUM_COMPONENT;
        Some(OverviewColour {
            red: (self.components[0] / divisor) as f32,
            green: (self.components[1] / divisor) as f32,
            blue: (self.components[2] / divisor) as f32,
        })
    }
}

/// Returns the mean colour of the valid cells of one slice, or none when every cell is background or reserved.
#[must_use]
pub fn slice_colour(cells: &Slice) -> Option<OverviewColour> {
    Contributions::of_slice(cells).colour()
}

/// Returns the colours across the visible part of the track from `start` to `end`, as fractions of the track.
///
/// There is one colour per column for at most `MAXIMUM_COLOUR_STOPS` columns, evenly spread when there are more.
/// A column without a valid cell has no colour, so the caller draws it in its fallback colour.
/// Returns an empty vector for an overview without slices, a range that is empty or not finite, or no columns.
#[must_use]
pub fn column_colours(overview: &Overview, start: f64, end: f64, columns: usize) -> Vec<Option<OverviewColour>> {
    let slice_count = overview.slices().len();
    if slice_count == 0 || columns == 0 || !start.is_finite() || !end.is_finite() || end <= start {
        return Vec::new();
    }
    let stops = columns.min(MAXIMUM_COLOUR_STOPS);
    let column_length = (end - start) / stops as f64;
    let combine_slices = column_length * slice_count as f64 > 1.0;
    (0..stops)
        .map(|column| {
            let column_start = (column as f64).mul_add(column_length, start);
            let contributions = if combine_slices {
                contributions_over_interval(overview, column_start, column_start + column_length)
            } else {
                contributions_at(overview, 0.5f64.mul_add(column_length, column_start))
            };
            contributions.colour()
        })
        .collect()
}

/// Returns the contributions at a position of the track, interpolated between the centres of adjacent slices.
/// Before the first centre and after the last one the colour of the end slice is used.
fn contributions_at(overview: &Overview, position: f64) -> Contributions {
    let slices = overview.slices();
    let last_index = slices.len() - 1;
    let slice_position = position
        .clamp(0.0, 1.0)
        .mul_add(slices.len() as f64, -0.5)
        .clamp(0.0, last_index as f64);
    let left_index = slice_position as usize;
    let fraction = slice_position - left_index as f64;
    let mut neighbours = slices.iter().skip(left_index).map(Contributions::of_slice);
    let left = neighbours.next().unwrap_or_default();
    let right = neighbours.next().unwrap_or(left);
    let interpolate = |left: f64, right: f64| fraction.mul_add(right - left, left);
    let mut components = left.components;
    for (component, right_component) in components.iter_mut().zip(right.components) {
        *component = interpolate(*component, right_component);
    }
    Contributions {
        components,
        weight: interpolate(left.weight, right.weight),
    }
}

/// Returns the contributions of the slices between two positions of the track, each weighted by its overlap.
fn contributions_over_interval(overview: &Overview, start: f64, end: f64) -> Contributions {
    let slices = overview.slices();
    let slice_count = slices.len() as f64;
    let first = start.clamp(0.0, 1.0) * slice_count;
    let last = end.clamp(0.0, 1.0) * slice_count;
    if last <= first {
        return contributions_at(overview, start);
    }
    let mut contributions = Contributions::default();
    let end_index = (last.ceil() as usize).min(slices.len());
    for (index, cells) in slices.iter().enumerate().take(end_index).skip(first as usize) {
        let overlap = last.min((index + 1) as f64) - first.max(index as f64);
        contributions.add_slice(cells, overlap);
    }
    contributions
}

#[cfg(test)]
mod test_slice_colour {
    use super::*;
    use crate::overview::BAND_COUNT;

    /// Palette index of pure red.
    pub(super) const RED_INDEX: u8 = 180;

    /// Palette index of pure green.
    pub(super) const GREEN_INDEX: u8 = 30;

    /// Asserts that a colour has the given 8-bit components, allowing one step of rounding.
    pub(super) fn assert_colour(colour: Option<OverviewColour>, red: u32, green: u32, blue: u32) {
        let rgb = colour.expect("has a colour").to_rgb_u32();
        let components = [rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF];
        for (actual, expected) in components.into_iter().zip([red, green, blue]) {
            assert!(actual.abs_diff(expected) <= 1, "{rgb:06X} is not {red} {green} {blue}");
        }
    }

    #[test]
    fn indices_decode_through_the_colour_cube() {
        assert_colour(slice_colour(&[2; BAND_COUNT]), 0, 0, 102);
        assert_colour(slice_colour(&[43; BAND_COUNT]), 51, 51, 51);
        assert_colour(slice_colour(&[100; BAND_COUNT]), 102, 204, 204);
        assert_colour(slice_colour(&[RED_INDEX; BAND_COUNT]), 255, 0, 0);
        assert_colour(slice_colour(&[GREEN_INDEX; BAND_COUNT]), 0, 255, 0);
        assert_colour(slice_colour(&[215; BAND_COUNT]), 255, 255, 255);
    }

    #[test]
    fn background_and_reserved_cells_are_skipped() {
        let mut cells = [0; BAND_COUNT];
        cells[1] = 1;
        cells[2] = 216;
        cells[3] = 255;
        cells[15] = 215;
        assert_colour(slice_colour(&cells), 255, 255, 255);
        cells[15] = 0;
        assert_eq!(slice_colour(&cells), None);
    }

    #[test]
    fn colour_is_the_mean_of_the_coloured_cells_whatever_their_rows() {
        let mut cells = [RED_INDEX; BAND_COUNT];
        cells[..8].fill(GREEN_INDEX);
        let mixed = slice_colour(&cells);
        assert_colour(mixed, 128, 128, 0);
        cells.reverse();
        assert_eq!(slice_colour(&cells), mixed);
        cells[..8].fill(0);
        assert_colour(slice_colour(&cells), 0, 255, 0);
    }
}

#[cfg(test)]
mod test_column_colours {
    use super::test_slice_colour::{GREEN_INDEX, RED_INDEX, assert_colour};
    use super::*;
    use crate::overview::BAND_COUNT;

    /// Returns an overview with one uniform slice per palette index.
    fn overview_of(indices: &[u8]) -> Overview {
        Overview::from_slices(indices.iter().map(|&index| [index; BAND_COUNT]).collect())
    }

    #[test]
    fn columns_span_the_visible_range() {
        let colours = column_colours(&overview_of(&[RED_INDEX, GREEN_INDEX]), 0.0, 1.0, 4);
        assert_eq!(colours.len(), 4);
        assert_colour(colours[0], 255, 0, 0);
        assert_colour(colours[3], 0, 255, 0);
    }

    #[test]
    fn close_zoom_blends_between_slice_centres() {
        let colours = column_colours(&overview_of(&[RED_INDEX, GREEN_INDEX]), 0.45, 0.55, 1);
        assert_eq!(colours.len(), 1);
        assert_colour(colours[0], 128, 128, 0);
    }

    #[test]
    fn column_spanning_several_slices_weights_them_by_overlap() {
        let overview = overview_of(&[RED_INDEX, GREEN_INDEX, GREEN_INDEX, GREEN_INDEX]);
        let colours = column_colours(&overview, 0.125, 0.625, 1);
        assert_colour(colours[0], 64, 191, 0);
    }

    #[test]
    fn invalid_input_gives_no_colours() {
        let overview = overview_of(&[RED_INDEX]);
        assert_eq!(column_colours(&Overview::default(), 0.0, 1.0, 10), Vec::new());
        assert_eq!(column_colours(&overview, 0.0, 1.0, 0), Vec::new());
        assert_eq!(column_colours(&overview, 0.5, 0.5, 10), Vec::new());
        assert_eq!(column_colours(&overview, 0.0, f64::INFINITY, 10), Vec::new());
        assert_eq!(column_colours(&overview, f64::NAN, 1.0, 10), Vec::new());
    }

    #[test]
    fn colour_count_is_bounded_on_a_very_wide_waveform() {
        let colours = column_colours(&overview_of(&[RED_INDEX, GREEN_INDEX]), 0.0, 1.0, 100_000);
        assert_eq!(colours.len(), MAXIMUM_COLOUR_STOPS);
    }

    #[test]
    fn background_slice_has_no_colour() {
        let colours = column_colours(&overview_of(&[0, 0]), 0.0, 1.0, 2);
        assert_eq!(colours, vec![None, None]);
    }
}
