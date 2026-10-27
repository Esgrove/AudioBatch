//! The `Serato BeatGrid` tag, which holds the beatgrid markers and the BPM of the last one.

use std::fmt;
use std::fmt::Display;

use anyhow::Context;
use anyhow::Result;
use anyhow::anyhow;

/// Size of the version and marker count header.
const HEADER_BYTES: usize = 6;

/// Size of one marker, a position and a BPM or beat count.
const MARKER_BYTES: usize = 8;

/// A single beatgrid marker.
#[derive(Debug, Clone, PartialEq)]
pub enum BeatGridMarker {
    /// Last marker of the beatgrid, with the BPM value.
    Terminal {
        /// Position in seconds.
        position: f32,
        /// Beats per minute from this marker on.
        bpm: f32,
    },
    /// Intermediate marker with the number of beats until the next marker.
    NonTerminal {
        /// Position in seconds.
        position: f32,
        /// Number of beats until the next marker.
        beats_till_next: u32,
    },
}

/// Serato beatgrid tag containing all beatgrid markers.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct BeatGrid {
    /// Number of markers stated in the header.
    pub marker_count: u32,
    /// The markers, the last one terminal.
    pub markers: Vec<BeatGridMarker>,
}

impl Display for BeatGridMarker {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::Terminal { position, bpm } => write!(formatter, "{position:.3}s {bpm:.3} BPM"),
            Self::NonTerminal {
                position,
                beats_till_next,
            } => write!(formatter, "{position:.3}s {beats_till_next} beats"),
        }
    }
}

impl BeatGrid {
    /// Parses the beatgrid tag, a header followed by zero or more markers and a single footer byte.
    ///
    /// The header holds the version bytes `01 00` and the marker count as a big-endian `u32`.
    /// Every marker is a big-endian `f32` position followed by four bytes:
    /// the BPM as an `f32` for the last, terminal marker,
    /// and the number of beats till the next marker as a `u32` for the others.
    /// The checks are strict, so a damaged grid fails instead of yielding wrong beat positions.
    ///
    /// # Errors
    ///
    /// Returns an error when the version is unknown, the size does not match the stated markers,
    /// the positions decrease, a beat count is zero, or the final BPM is not a positive finite number.
    pub fn parse(data: &[u8]) -> Result<Self> {
        let count_bytes = read_four_bytes(data, 2)
            .ok_or_else(|| anyhow!("Data is too short to contain valid beatgrid information"))?;
        if !data.starts_with(&[0x01, 0x00]) {
            return Err(anyhow!("Unsupported beatgrid version"));
        }
        let marker_count = u32::from_be_bytes(count_bytes);
        let markers_end = (marker_count as usize)
            .checked_mul(MARKER_BYTES)
            .and_then(|bytes| bytes.checked_add(HEADER_BYTES))
            .ok_or_else(|| anyhow!("Beatgrid marker count overflows"))?;
        let empty_without_footer = marker_count == 0 && data.len() == HEADER_BYTES;
        if data.len() != markers_end + 1 && !empty_without_footer {
            return Err(anyhow!(
                "Beatgrid size {} does not match {marker_count} markers",
                data.len()
            ));
        }

        let mut markers = Vec::with_capacity(marker_count as usize);
        let mut previous_position = f32::NEG_INFINITY;
        let (records, _) = data
            .get(HEADER_BYTES..markers_end)
            .context("Beatgrid markers exceed the data")?
            .as_chunks::<MARKER_BYTES>();
        for (index, &[p0, p1, p2, p3, v0, v1, v2, v3]) in records.iter().enumerate() {
            let position = f32::from_be_bytes([p0, p1, p2, p3]);
            let value = [v0, v1, v2, v3];
            if !position.is_finite() || position < previous_position {
                return Err(anyhow!("Invalid beatgrid marker position: {position}"));
            }
            previous_position = position;
            let marker = if index + 1 == marker_count as usize {
                let bpm = f32::from_be_bytes(value);
                if !bpm.is_finite() || bpm <= 0.0 {
                    return Err(anyhow!("Invalid beatgrid BPM: {bpm}"));
                }
                BeatGridMarker::Terminal { position, bpm }
            } else {
                let beats_till_next = u32::from_be_bytes(value);
                if beats_till_next == 0 {
                    return Err(anyhow!("Beatgrid marker has zero beats till the next one"));
                }
                BeatGridMarker::NonTerminal {
                    position,
                    beats_till_next,
                }
            };
            markers.push(marker);
        }
        Ok(Self { marker_count, markers })
    }

    /// Returns the BPM of the terminal marker.
    #[must_use]
    pub fn bpm(&self) -> Option<f32> {
        self.markers.iter().rev().find_map(|marker| match marker {
            BeatGridMarker::Terminal { bpm, .. } => Some(*bpm),
            BeatGridMarker::NonTerminal { .. } => None,
        })
    }
}

impl Display for BeatGrid {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        match self.markers.as_slice() {
            [] => write!(formatter, "Empty"),
            [marker] => write!(formatter, "Beatgrid {marker}"),
            markers => {
                write!(formatter, "Beatgrid ({}):", self.marker_count)?;
                for marker in markers {
                    write!(formatter, "\n  {marker}")?;
                }
                Ok(())
            }
        }
    }
}

/// Returns the four bytes starting at `offset`, or `None` when the data ends before them.
fn read_four_bytes(data: &[u8], offset: usize) -> Option<[u8; 4]> {
    data.get(offset..offset.checked_add(4)?)
        .and_then(|bytes| bytes.try_into().ok())
}

#[cfg(test)]
mod test_beatgrid {
    use super::*;

    /// Beatgrid payload version header.
    const HEADER: &[u8] = &[0x01, 0x00];

    /// Single footer byte after the markers.
    const FOOTER: u8 = 0x00;

    fn beatgrid_bytes(markers: &[(f32, [u8; 4])]) -> Vec<u8> {
        let count = u32::try_from(markers.len()).expect("fits");
        let mut data = [HEADER, &count.to_be_bytes()].concat();
        for (position, value) in markers {
            data.extend_from_slice(&position.to_be_bytes());
            data.extend_from_slice(value);
        }
        data.push(FOOTER);
        data
    }

    #[test]
    fn short_data_is_rejected() {
        assert!(BeatGrid::parse(&[0x01, 0x00, 0x00, 0x00, 0x00]).is_err());
        assert!(BeatGrid::parse(&[]).is_err());
        assert!(BeatGrid::parse(&[HEADER, &1_u32.to_be_bytes(), &[0x00, 0x00]].concat()).is_err());
    }

    #[test]
    fn damaged_grids_are_rejected() {
        let mut wrong_version = beatgrid_bytes(&[(0.0, 128.0_f32.to_be_bytes())]);
        wrong_version[1] = 0x01;
        assert!(BeatGrid::parse(&wrong_version).is_err());
        let mut trailing = beatgrid_bytes(&[(0.0, 128.0_f32.to_be_bytes())]);
        trailing.push(0x00);
        assert!(BeatGrid::parse(&trailing).is_err());
        let decreasing = beatgrid_bytes(&[(5.0, 4_u32.to_be_bytes()), (1.0, 120.0_f32.to_be_bytes())]);
        assert!(BeatGrid::parse(&decreasing).is_err());
        let zero_beats = beatgrid_bytes(&[(0.0, 0_u32.to_be_bytes()), (1.0, 120.0_f32.to_be_bytes())]);
        assert!(BeatGrid::parse(&zero_beats).is_err());
        assert!(BeatGrid::parse(&beatgrid_bytes(&[(0.0, 0.0_f32.to_be_bytes())])).is_err());
        assert!(BeatGrid::parse(&beatgrid_bytes(&[(f32::NAN, 120.0_f32.to_be_bytes())])).is_err());
    }

    #[test]
    fn zero_markers_is_empty() {
        let beatgrid = BeatGrid::parse(&[0x01, 0x00, 0x00, 0x00, 0x00, 0x00]).expect("parses");
        assert_eq!(beatgrid, BeatGrid::default());
        assert_eq!(beatgrid.to_string(), "Empty");
    }

    #[test]
    fn single_terminal_marker() {
        let beatgrid = BeatGrid::parse(&beatgrid_bytes(&[(0.0, 128.0_f32.to_be_bytes())])).expect("parses");
        assert_eq!(
            beatgrid.markers,
            vec![BeatGridMarker::Terminal {
                position: 0.0,
                bpm: 128.0
            }]
        );
        assert_eq!(beatgrid.bpm(), Some(128.0));
        assert_eq!(beatgrid.to_string(), "Beatgrid 0.000s 128.000 BPM");
    }

    #[test]
    fn non_terminal_then_terminal() {
        let data = beatgrid_bytes(&[(0.5, 4_u32.to_be_bytes()), (10.0, 120.0_f32.to_be_bytes())]);
        let beatgrid = BeatGrid::parse(&data).expect("parses");
        assert_eq!(
            beatgrid.markers,
            vec![
                BeatGridMarker::NonTerminal {
                    position: 0.5,
                    beats_till_next: 4
                },
                BeatGridMarker::Terminal {
                    position: 10.0,
                    bpm: 120.0
                },
            ]
        );
        assert_eq!(
            beatgrid.to_string(),
            "Beatgrid (2):\n  0.500s 4 beats\n  10.000s 120.000 BPM"
        );
    }
}
