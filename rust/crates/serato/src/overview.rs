//! The Serato Overview tag: the small coloured raster Serato shows for a whole track.
//!
//! A payload holds consecutive time slices of 16 vertical cells, commonly 240 slices whatever the track length.
//! Each cell is an index into Serato's palette, not a frequency band strength,
//! so `colouring` decodes the indices into colours instead of reading them as levels.
//! The cache representation is a `SOVR` header with big-endian fields, then the raw indices.
//! It is the representation the C++ application stores, so both `serato_tag_cache` tables share one schema.

use std::fmt;
use std::fmt::Display;

/// Number of vertical cells in each overview slice.
pub const BAND_COUNT: usize = 16;

/// Highest number of slices accepted when parsing or deserializing.
pub const MAXIMUM_SLICES: usize = 65_536;

/// Version of the overview cache representation, raised when it changes.
pub const OVERVIEW_PARSER_VERSION: u32 = 1;

/// Stable identifier of the Overview tag kind in the cache, part of the shared `serato_tag_cache` schema.
pub const OVERVIEW_TAG_KIND: u32 = 1;

/// The only payload header Serato is known to write.
const PAYLOAD_HEADER: [u8; 2] = [0x01, 0x05];

/// Largest valid payload, including the header.
const MAXIMUM_PAYLOAD_BYTES: usize = PAYLOAD_HEADER.len() + MAXIMUM_SLICES * BAND_COUNT;

/// Magic bytes at the start of a cached overview.
const CACHE_MAGIC: [u8; 4] = *b"SOVR";

/// Size of the cache header: the magic, the kind, the parser version, and the slice count.
const CACHE_HEADER_BYTES: usize = 16;

/// Palette indices of one time slice, top to bottom.
pub type Slice = [u8; BAND_COUNT];

/// A malformed Serato Overview payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum OverviewError {
    /// The payload exceeds the supported slice limit.
    #[error("Overview has more than {MAXIMUM_SLICES} slices")]
    TooManySlices {
        /// Payload length in bytes, including the header.
        length: usize,
    },
    /// The payload does not start with the supported header.
    #[error("Unsupported overview header")]
    UnsupportedHeader,
    /// The header is not followed by any slices.
    #[error("Overview has no slices")]
    NoSlices,
    /// The last slice has fewer than sixteen cells.
    #[error("Overview ends with a partial slice")]
    PartialSlice {
        /// Number of cells in the incomplete slice.
        remaining: usize,
    },
}

/// Palette index slices of an embedded Serato Overview raster.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Overview {
    slices: Vec<Slice>,
}

impl Overview {
    /// Creates an overview from slices, for example decoded elsewhere or built in tests.
    #[must_use]
    pub const fn from_slices(slices: Vec<Slice>) -> Self {
        Self { slices }
    }

    /// Parses the payload of the tag after its GEOB or FLAC wrapper has been removed.
    ///
    /// | Offset | Length | Raw Value     | Type           | Description
    /// | ------ | ------ | ------------- | -------------- | -----------
    /// |   `00` |   `02` | `01 05`       |                | Supported header, meaning unspecified
    /// |   `02` |   `10` | Varies        | 16 * `uint8_t` | Palette indices of time slice 0
    /// |   `12` |   `10` | Varies        | 16 * `uint8_t` | Palette indices of time slice 1
    /// |    ... |    ... | Varies        | 16 * `uint8_t` | Subsequent time slices
    ///
    /// A payload of N slices is 2 + 16 * N bytes, 3842 bytes for the common 240 slices.
    ///
    /// # Errors
    ///
    /// Returns an error for another header, no slices, more than `MAXIMUM_SLICES` slices, or a partial slice.
    pub fn parse(data: &[u8]) -> Result<Self, OverviewError> {
        if data.len() > MAXIMUM_PAYLOAD_BYTES {
            return Err(OverviewError::TooManySlices { length: data.len() });
        }
        let Some(payload) = data.strip_prefix(&PAYLOAD_HEADER) else {
            return Err(OverviewError::UnsupportedHeader);
        };
        if payload.is_empty() {
            return Err(OverviewError::NoSlices);
        }
        let (slices, remainder) = payload.as_chunks::<BAND_COUNT>();
        if !remainder.is_empty() {
            return Err(OverviewError::PartialSlice {
                remaining: remainder.len(),
            });
        }
        Ok(Self {
            slices: slices.to_vec(),
        })
    }

    /// Reads a cached overview, or returns `None` for another magic, kind, version, count, or length.
    #[must_use]
    pub fn deserialize(bytes: &[u8]) -> Option<Self> {
        let (header, cells) = bytes.split_first_chunk::<CACHE_HEADER_BYTES>()?;
        let ([magic, kind, version, count], []) = header.as_chunks::<4>() else {
            return None;
        };
        if *magic != CACHE_MAGIC
            || u32::from_be_bytes(*kind) != OVERVIEW_TAG_KIND
            || u32::from_be_bytes(*version) != OVERVIEW_PARSER_VERSION
        {
            return None;
        }
        let count = usize::try_from(u32::from_be_bytes(*count)).ok()?;
        if count == 0 || count > MAXIMUM_SLICES || cells.len() != count * BAND_COUNT {
            return None;
        }
        Some(Self {
            slices: cells.as_chunks::<BAND_COUNT>().0.to_vec(),
        })
    }

    /// Writes the cache representation, with the raw palette indices after a header.
    /// The header has the magic, then the kind, the parser version, and the slice count as big-endian fields.
    /// Returns an empty vector for an overview without slices or with more than `MAXIMUM_SLICES`.
    #[must_use]
    pub fn serialize(&self) -> Vec<u8> {
        let Ok(count) = u32::try_from(self.slices.len()) else {
            return Vec::new();
        };
        if self.slices.is_empty() || self.slices.len() > MAXIMUM_SLICES {
            return Vec::new();
        }
        let mut bytes = Vec::with_capacity(CACHE_HEADER_BYTES + self.slices.len() * BAND_COUNT);
        bytes.extend_from_slice(&CACHE_MAGIC);
        bytes.extend_from_slice(&OVERVIEW_TAG_KIND.to_be_bytes());
        bytes.extend_from_slice(&OVERVIEW_PARSER_VERSION.to_be_bytes());
        bytes.extend_from_slice(&count.to_be_bytes());
        bytes.extend(self.slices.iter().flatten());
        bytes
    }

    /// Returns the palette index slices in time order.
    #[must_use]
    pub fn slices(&self) -> &[Slice] {
        &self.slices
    }
}

impl Display for Overview {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(formatter, "{} slices", self.slices.len())
    }
}

#[cfg(test)]
mod test_parse {
    use super::*;

    /// Returns a payload of synthetic slices whose cells count up from zero.
    fn payload(count: usize) -> Vec<u8> {
        let mut bytes = PAYLOAD_HEADER.to_vec();
        bytes.extend((0..count * BAND_COUNT).map(|index| (index % 256) as u8));
        bytes
    }

    #[test]
    fn keeps_any_complete_slice_count() {
        for count in [1, 3, 240, 257] {
            let overview = Overview::parse(&payload(count)).expect("parses");
            assert_eq!(overview.slices().len(), count);
            assert_eq!(overview.slices()[0][15], 15);
        }
        assert_eq!(Overview::parse(&payload(2)).expect("parses").to_string(), "2 slices");
    }

    #[test]
    fn rejects_short_payloads_and_partial_slices() {
        for size in [0, 1, 2, 3, 17, 19, 33] {
            let mut bytes = payload(2);
            bytes.truncate(size);
            assert!(Overview::parse(&bytes).is_err(), "size {size}");
        }
    }

    #[test]
    fn rejects_unknown_headers() {
        for position in [0, 1] {
            let mut bytes = payload(1);
            bytes[position] = 0xFF;
            assert!(Overview::parse(&bytes).is_err());
        }
    }

    #[test]
    fn errors_identify_incomplete_slices_and_keep_display_text() {
        let error = Overview::parse(&[1, 5]).expect_err("no slices");
        assert_eq!(error, OverviewError::NoSlices);
        assert_eq!(error.to_string(), "Overview has no slices");
        let error = Overview::parse(&[1, 5, 0]).expect_err("partial slice");
        assert_eq!(error, OverviewError::PartialSlice { remaining: 1 });
        assert_eq!(error.to_string(), "Overview ends with a partial slice");
    }

    #[test]
    fn accepts_silent_slices_and_bounds_the_slice_count() {
        let mut silent = payload(1);
        silent[2..].fill(0);
        assert_eq!(Overview::parse(&silent).expect("parses").slices(), &[[0; BAND_COUNT]]);
        assert_eq!(
            Overview::parse(&payload(MAXIMUM_SLICES))
                .expect("parses")
                .slices()
                .len(),
            MAXIMUM_SLICES
        );
        assert!(Overview::parse(&payload(MAXIMUM_SLICES + 1)).is_err());
    }
}

#[cfg(test)]
mod test_cache_representation {
    use super::*;

    /// Returns an overview of synthetic slices whose cells count up from zero.
    fn overview(count: usize) -> Overview {
        Overview::from_slices(
            (0..count)
                .map(|slice| std::array::from_fn(|cell| ((slice * BAND_COUNT + cell) % 256) as u8))
                .collect(),
        )
    }

    #[test]
    fn header_has_magic_kind_version_and_big_endian_count() {
        let overview = overview(257);
        let bytes = overview.serialize();
        assert_eq!(bytes.len(), 16 + 257 * 16);
        assert_eq!(&bytes[..16], b"SOVR\0\0\0\x01\0\0\0\x01\0\0\x01\x01");
        assert_eq!(Overview::deserialize(&bytes), Some(overview));
    }

    #[test]
    fn round_trips_the_largest_overview() {
        let overview = overview(MAXIMUM_SLICES);
        assert_eq!(Overview::deserialize(&overview.serialize()), Some(overview));
    }

    #[test]
    fn rejects_corrupt_fields_and_lengths() {
        let bytes = overview(257).serialize();
        for offset in [0, 7, 11, 12, 15] {
            let mut corrupt = bytes.clone();
            corrupt[offset] ^= 0xFF;
            assert_eq!(Overview::deserialize(&corrupt), None, "offset {offset}");
        }
        for length in [0, 15, bytes.len() - 1, bytes.len() + 1] {
            let mut corrupt = bytes.clone();
            corrupt.resize(length, 0);
            assert_eq!(Overview::deserialize(&corrupt), None, "length {length}");
        }
        let mut zero_count = bytes;
        zero_count[12..16].fill(0);
        assert_eq!(Overview::deserialize(&zero_count), None);
    }

    #[test]
    fn invalid_overviews_have_no_cache_representation() {
        assert_eq!(Overview::default().serialize(), Vec::<u8>::new());
        assert_eq!(overview(MAXIMUM_SLICES + 1).serialize(), Vec::<u8>::new());
    }
}
