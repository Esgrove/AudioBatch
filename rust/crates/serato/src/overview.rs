//! The Serato Overview tag: the small waveform raster Serato shows for a track.
//!
//! Tested payloads hold 240 time columns of 16 vertical cells.
//! Comparisons with Serato screenshots point to indexed palette colors, not frequency band strengths,
//! so `levels` only offers the normalized indices as a heuristic for drawing.

use std::fmt;
use std::fmt::Display;

use anyhow::Result;
use anyhow::anyhow;

/// Number of vertical cells in each overview column.
pub const BAND_COUNT: usize = 16;

/// Raw columns of the embedded Serato Overview raster.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Overview {
    blocks: Vec<[u8; BAND_COUNT]>,
}

impl Overview {
    /// Reads the complete 16-byte columns after the two-byte header.
    ///
    /// The parser is permissive: it does not validate the header,
    /// accepts header-only data, and ignores a partial trailing column.
    ///
    /// | Offset | Length | Raw Value     | Type           | Description
    /// | ------ | ------ | ------------- | -------------- | -----------
    /// |   `00` |   `02` | `01 05`       |                | Observed header
    /// |   `02` |   `10` | Varies        | 16 * `uint8_t` | First vertical palette column
    /// |    ... |    ... | Varies        | 16 * `uint8_t` | Subsequent palette columns
    ///
    /// # Errors
    ///
    /// Returns an error when the data is shorter than the header.
    pub fn parse(data: &[u8]) -> Result<Self> {
        let payload = data
            .get(2..)
            .ok_or_else(|| anyhow!("Data too short to contain initial bytes"))?;
        let blocks = payload.as_chunks::<BAND_COUNT>().0.to_vec();
        Ok(Self { blocks })
    }

    /// Returns the raw columns.
    #[must_use]
    pub fn blocks(&self) -> &[[u8; BAND_COUNT]] {
        &self.blocks
    }

    /// Averages the cell indices of each column into `rows` rows and normalizes them to the range 0 to 1.
    ///
    /// The values are a heuristic for drawing, not frequency strengths or amplitudes.
    /// `rows` must divide 16, otherwise the remaining cells are left out.
    #[must_use]
    pub fn levels(&self, rows: usize) -> Vec<Vec<f32>> {
        let rows = rows.clamp(1, BAND_COUNT);
        let cells_per_row = BAND_COUNT / rows;
        let averaged: Vec<Vec<u16>> = self
            .blocks
            .iter()
            .map(|block| {
                block
                    .chunks_exact(cells_per_row)
                    .take(rows)
                    .map(|cells| cells.iter().map(|&value| u16::from(value)).sum::<u16>() / cells_per_row as u16)
                    .collect()
            })
            .collect();
        let maximum = averaged.iter().flatten().copied().max().unwrap_or(0).max(1);
        averaged
            .iter()
            .map(|column| {
                column
                    .iter()
                    .map(|&value| f32::from(value) / f32::from(maximum))
                    .collect()
            })
            .collect()
    }
}

impl Display for Overview {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(formatter, "{} columns", self.blocks.len())
    }
}

#[cfg(test)]
mod test_overview {
    use super::*;

    #[test]
    fn rejects_data_shorter_than_the_header() {
        assert!(Overview::parse(&[]).is_err());
        assert!(Overview::parse(&[0x01]).is_err());
    }

    #[test]
    fn parses_complete_columns_and_ignores_a_partial_one() {
        let mut data = vec![0x01, 0x05];
        data.extend_from_slice(&[10; 16]);
        data.extend_from_slice(&[20; 16]);
        data.extend_from_slice(&[0xFF; 10]);
        let overview = Overview::parse(&data).expect("parses");
        assert_eq!(overview.blocks(), &[[10; 16], [20; 16]]);
        assert_eq!(overview.to_string(), "2 columns");
    }

    #[test]
    fn header_only_has_no_columns() {
        let overview = Overview::parse(&[0x01, 0x05]).expect("parses");
        assert_eq!(overview.blocks().len(), 0);
    }

    #[test]
    fn levels_average_and_normalize_cells() {
        let mut block = [0u8; 16];
        block[0] = 10;
        block[1] = 30;
        block[14] = 40;
        block[15] = 40;
        let overview = Overview { blocks: vec![block] };
        let levels = overview.levels(8);
        assert_eq!(levels.len(), 1);
        assert!((levels[0][0] - 0.5).abs() < f32::EPSILON);
        assert!((levels[0][7] - 1.0).abs() < f32::EPSILON);
        assert!(levels[0][1..7].iter().all(|&level| level.abs() < f32::EPSILON));
    }

    #[test]
    fn silence_has_zero_levels() {
        let overview = Overview {
            blocks: vec![[0u8; 16]; 240],
        };
        assert!(
            overview
                .levels(8)
                .iter()
                .flatten()
                .all(|&level| level.abs() < f32::EPSILON)
        );
    }
}
