//! The `Serato BeatGrid` tag, which holds the beatgrid markers and the BPM of the last one.

use std::fmt;
use std::fmt::Display;

/// Size of the version and marker count header.
const HEADER_BYTES: usize = 6;

/// Size of one marker, a position and a BPM or beat count.
const MARKER_BYTES: usize = 8;

/// A malformed beatgrid header or marker.
#[derive(Debug, Clone, Copy, PartialEq, thiserror::Error)]
pub enum BeatGridError {
    /// The header is incomplete.
    #[error("Data is too short to contain valid beatgrid information")]
    TooShort {
        /// Payload length in bytes.
        length: usize,
    },
    /// The version is not supported.
    #[error("Unsupported beatgrid version")]
    UnsupportedVersion {
        /// Version bytes in the header.
        version: [u8; 2],
    },
    /// The stated marker count cannot fit in memory.
    #[error("Beatgrid marker count overflows")]
    MarkerCountOverflow {
        /// Marker count stated in the header.
        marker_count: u32,
    },
    /// The payload length disagrees with the marker count.
    #[error("Beatgrid size {length} does not match {marker_count} markers")]
    SizeMismatch {
        /// Payload length in bytes.
        length: usize,
        /// Marker count stated in the header.
        marker_count: u32,
    },
    /// The marker range is outside the payload.
    #[error("Beatgrid markers exceed the data")]
    MarkersExceedData,
    /// A marker position is non-finite or decreases.
    #[error("Invalid beatgrid marker position: {position}")]
    InvalidPosition {
        /// Zero-based marker index.
        index: usize,
        /// Rejected position in seconds.
        position: f32,
    },
    /// The terminal BPM is not positive and finite.
    #[error("Invalid beatgrid BPM: {bpm}")]
    InvalidBpm {
        /// Rejected BPM.
        bpm: f32,
    },
    /// An intermediate marker states zero beats.
    #[error("Beatgrid marker has zero beats till the next one")]
    ZeroBeats {
        /// Zero-based marker index.
        index: usize,
    },
}

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
    pub fn parse(data: &[u8]) -> Result<Self, BeatGridError> {
        let count_bytes = read_four_bytes(data, 2).ok_or(BeatGridError::TooShort { length: data.len() })?;
        if !data.starts_with(&[0x01, 0x00]) {
            return Err(BeatGridError::UnsupportedVersion {
                version: data.first_chunk::<2>().copied().unwrap_or_default(),
            });
        }
        let marker_count = u32::from_be_bytes(count_bytes);
        let markers_end = (marker_count as usize)
            .checked_mul(MARKER_BYTES)
            .and_then(|bytes| bytes.checked_add(HEADER_BYTES))
            .ok_or(BeatGridError::MarkerCountOverflow { marker_count })?;
        let empty_without_footer = marker_count == 0 && data.len() == HEADER_BYTES;
        if data.len() != markers_end + 1 && !empty_without_footer {
            return Err(BeatGridError::SizeMismatch {
                length: data.len(),
                marker_count,
            });
        }

        let mut markers = Vec::with_capacity(marker_count as usize);
        let mut previous_position = f32::NEG_INFINITY;
        let (records, _) = data
            .get(HEADER_BYTES..markers_end)
            .ok_or(BeatGridError::MarkersExceedData)?
            .as_chunks::<MARKER_BYTES>();
        for (index, &[p0, p1, p2, p3, v0, v1, v2, v3]) in records.iter().enumerate() {
            let position = f32::from_be_bytes([p0, p1, p2, p3]);
            let value = [v0, v1, v2, v3];
            if !position.is_finite() || position < previous_position {
                return Err(BeatGridError::InvalidPosition { index, position });
            }
            previous_position = position;
            let marker = if index + 1 == marker_count as usize {
                let bpm = f32::from_be_bytes(value);
                if !bpm.is_finite() || bpm <= 0.0 {
                    return Err(BeatGridError::InvalidBpm { bpm });
                }
                BeatGridMarker::Terminal { position, bpm }
            } else {
                let beats_till_next = u32::from_be_bytes(value);
                if beats_till_next == 0 {
                    return Err(BeatGridError::ZeroBeats { index });
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

    /// Returns the payload of a `BeatGrid` tag with the markers, each a position and its four trailing bytes.
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
    fn errors_identify_the_marker_and_keep_display_text() {
        let error = BeatGrid::parse(&beatgrid_bytes(&[
            (0.0, 0_u32.to_be_bytes()),
            (1.0, 120.0_f32.to_be_bytes()),
        ]))
        .expect_err("zero beats");
        assert!(matches!(error, BeatGridError::ZeroBeats { index: 0 }));
        assert_eq!(error.to_string(), "Beatgrid marker has zero beats till the next one");
        let error = BeatGrid::parse(&[HEADER, &2_u32.to_be_bytes()].concat()).expect_err("missing markers");
        assert!(matches!(
            error,
            BeatGridError::SizeMismatch {
                length: 6,
                marker_count: 2
            }
        ));
        assert_eq!(error.to_string(), "Beatgrid size 6 does not match 2 markers");
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
