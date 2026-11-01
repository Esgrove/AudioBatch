//! The Serato Markers2 tag: cue points, saved loops, the track color, and the BPM lock state.
//!
//! The tag holds a base64 encoded payload of named entries.
//! Each entry is parsed into a `Markers` value with public fields,
//! so front ends can draw cue points and loops in their own colors.

use std::fmt;
use std::fmt::Display;
use std::io::{self, BufRead, Cursor, Read};
use std::str;

use base64::Engine as _;
use base64::alphabet;
#[cfg(test)]
use base64::engine::general_purpose;
use base64::engine::{GeneralPurpose, GeneralPurposeConfig};
use byteorder::{BigEndian, ReadBytesExt};

use crate::format_position_timestamp;

/// Fixed number of slots in the legacy markers layout.
const LEGACY_SLOTS: usize = 14;
/// Size of a legacy slot in bytes.
const LEGACY_RECORD_BYTES: usize = 22;
/// Largest supported marker payload.
const MAXIMUM_PAYLOAD_BYTES: usize = 2 + crate::MAXIMUM_SLICES * crate::BAND_COUNT;
/// Bound on records in one marker payload.
const MAXIMUM_RECORDS: usize = 4096;
/// Bound on owned marker labels.
const MAXIMUM_LABEL_BYTES: usize = 32_768;
/// Serato can leave nonzero unused bits in its final Base64 sextet.
const MARKERS_BASE64: GeneralPurpose = GeneralPurpose::new(
    &alphabet::STANDARD,
    GeneralPurposeConfig::new().with_decode_allow_trailing_bits(true),
);

/// A malformed Markers2 wrapper, payload, or known entry.
#[derive(Debug, thiserror::Error)]
pub enum MarkersError {
    /// A legacy header, slot, or size is invalid.
    #[error("Invalid legacy markers layout")]
    InvalidLegacy,
    /// A lock byte is not a boolean.
    #[error("Invalid marker lock byte: {value}")]
    InvalidLock {
        /// Rejected lock byte.
        value: u8,
    },
    /// A loop ends before it starts.
    #[error("Loop ends before its start")]
    ReversedLoop,
    /// A record name is not bounded ASCII.
    #[error("Invalid marker record type")]
    InvalidRecordType,
    /// The payload, record count, or label exceeds its bound.
    #[error("Marker data exceeds its size limit")]
    SizeLimit,
    /// A required marker label or record terminator is missing.
    #[error("No null terminator found")]
    MissingTerminator,
    /// The terminator occurs before the wrapper header ends.
    #[error("Invalid Markers2 base64 data range")]
    InvalidBase64Range {
        /// Byte offset of the terminator.
        end: usize,
    },
    /// The payload is not valid base64.
    #[error("Failed to decode base64 data")]
    Base64(#[source] base64::DecodeError),
    /// The payload version is not supported.
    #[error("Invalid payload version: {version:?}")]
    InvalidVersion {
        /// Version bytes in the decoded payload.
        version: (u8, u8),
    },
    /// Nonzero bytes follow the final entry.
    #[error("Unexpected data after the final Markers2 entry")]
    TrailingData,
    /// The stated entry length overflows.
    #[error("Marker entry length overflows")]
    EntryLengthOverflow {
        /// Byte offset of the entry data.
        start: usize,
        /// Entry length stated in the payload.
        length: usize,
    },
    /// The stated entry extends beyond the payload.
    #[error("Marker entry exceeds payload")]
    EntryExceedsPayload {
        /// Byte offset of the entry data.
        start: usize,
        /// Entry length stated in the payload.
        length: usize,
    },
    /// The BPM lock entry has the wrong length.
    #[error("Invalid data length for BpmLock")]
    BpmLockLength {
        /// Entry length in bytes.
        length: usize,
    },
    /// The track color entry has the wrong length.
    #[error("Invalid data length for Color")]
    ColorLength {
        /// Entry length in bytes.
        length: usize,
        /// Fixed-size conversion failure.
        #[source]
        source: std::array::TryFromSliceError,
    },
    /// A cue entry is too short.
    #[error("Invalid data length for CueEntry")]
    CueLength {
        /// Entry length in bytes.
        length: usize,
    },
    /// A loop entry is too short.
    #[error("Invalid data length for Loop")]
    LoopLength {
        /// Entry length in bytes.
        length: usize,
    },
    /// A binary payload read failed.
    #[error("{0}")]
    Read(#[from] io::Error),
    /// An entry name is not valid UTF-8.
    #[error("{0}")]
    EntryName(#[from] std::string::FromUtf8Error),
    /// A cue or loop name is not valid UTF-8.
    #[error("{0}")]
    Name(#[from] str::Utf8Error),
}

/// One entry of the Markers2 tag.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Markers {
    /// Whether the beatgrid is locked.
    BpmLock(BpmLock),
    /// Track color.
    Color(Color),
    /// A cue point.
    Cue(Cue),
    /// A saved loop.
    Loop(Loop),
}

/// BPM lock status. True means the beatgrid is locked.
///
/// <https://support.serato.com/hc/en-us/articles/235214887-Lock-Beatgrids>
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BpmLock {
    /// True when the lock is enabled.
    pub enabled: bool,
}

/// RGB color of the track, a cue point, or a loop.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Color {
    /// Red component.
    pub red: u8,
    /// Green component.
    pub green: u8,
    /// Blue component.
    pub blue: u8,
}

/// A cue point.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Cue {
    /// Zero-based cue number.
    pub index: u8,
    /// Position in milliseconds.
    pub position: u32,
    /// Display color.
    pub color: Color,
    /// Exact source label, possibly empty or whitespace.
    pub name: String,
}

/// A saved loop.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Loop {
    /// Zero-based loop number.
    pub index: u8,
    /// Signed start position in milliseconds, including loops that begin before the audio.
    pub start_position: i32,
    /// Signed end position in milliseconds.
    pub end_position: i32,
    /// Display color.
    pub color: Color,
    /// True when the loop is locked.
    pub locked: bool,
    /// Name, possibly empty.
    pub name: String,
}

impl Markers {
    /// Parses the fixed fourteen-slot legacy tag without inventing names or a BPM lock.
    ///
    /// Positions and RGB colours use four seven-bit groups carrying a 24-bit value.
    /// Unset slots still consume their cue or loop index, and unknown slot types are skipped.
    ///
    /// # Errors
    ///
    /// Returns an error for a wrong header or length, invalid slot flags, or reversed loop boundaries.
    pub fn parse_legacy(data: &[u8]) -> Result<Vec<Self>, MarkersError> {
        if data.len() != 6 + LEGACY_SLOTS * LEGACY_RECORD_BYTES + 4 || !data.starts_with(&[2, 5, 0, 0, 0, 14]) {
            return Err(MarkersError::InvalidLegacy);
        }
        let mut markers = Vec::new();
        let mut cue_index = 0;
        let mut loop_index = 0;
        let records = data
            .get(6..data.len() - 4)
            .ok_or(MarkersError::InvalidLegacy)?
            .as_chunks::<LEGACY_RECORD_BYTES>()
            .0;
        for &[
            flag,
            p0,
            p1,
            p2,
            p3,
            end_flag,
            e0,
            e1,
            e2,
            e3,
            _,
            _,
            _,
            _,
            _,
            _,
            c0,
            c1,
            c2,
            c3,
            kind,
            lock,
        ] in records
        {
            if !matches!(flag, 0 | 127) || lock > 1 {
                return Err(MarkersError::InvalidLegacy);
            }
            let index = if kind == 1 {
                let index = cue_index;
                cue_index += 1;
                index
            } else {
                let index = loop_index;
                loop_index += 1;
                index
            };
            if flag == 127 {
                continue;
            }
            let position = legacy_number([p0, p1, p2, p3]);
            let color = legacy_color([c0, c1, c2, c3]);
            match kind {
                1 => markers.push(Self::Cue(Cue {
                    index,
                    position,
                    color,
                    name: String::new(),
                })),
                3 => {
                    if end_flag != 0 {
                        return Err(MarkersError::InvalidLegacy);
                    }
                    let start_position = i32::from_be_bytes(position.to_be_bytes());
                    let end_position = i32::from_be_bytes(legacy_number([e0, e1, e2, e3]).to_be_bytes());
                    if end_position < start_position {
                        return Err(MarkersError::ReversedLoop);
                    }
                    markers.push(Self::Loop(Loop {
                        index,
                        start_position,
                        end_position,
                        color,
                        locked: lock != 0,
                        name: String::new(),
                    }));
                }
                _ => {}
            }
        }
        let footer = data.last_chunk::<4>().ok_or(MarkersError::InvalidLegacy)?;
        markers.push(Self::Color(legacy_color(*footer)));
        Ok(markers)
    }

    /// Parses every entry of base64 encoded Serato Markers2 tag data.
    ///
    /// Entry types this parser does not know, such as Serato's flips, are skipped by their length,
    /// and zero padding after the empty terminating name is accepted.
    ///
    /// # Errors
    ///
    /// Returns an error when the base64 payload or one of its known entries is malformed,
    /// or when anything but zero bytes follows the terminating name.
    pub fn parse(data: &[u8]) -> Result<Vec<Self>, MarkersError> {
        if data.len() > MAXIMUM_PAYLOAD_BYTES {
            return Err(MarkersError::SizeLimit);
        }
        if !data.starts_with(&[1, 1]) {
            return Err(MarkersError::InvalidVersion {
                version: (
                    data.first().copied().unwrap_or_default(),
                    data.get(1).copied().unwrap_or_default(),
                ),
            });
        }
        let base64_end = data.iter().position(|&byte| byte == b'\x00').unwrap_or(data.len());
        if data
            .get(base64_end..)
            .ok_or(MarkersError::InvalidBase64Range { end: base64_end })?
            .iter()
            .any(|byte| *byte != 0)
        {
            return Err(MarkersError::TrailingData);
        }
        let base64_data = data
            .get(2..base64_end)
            .ok_or(MarkersError::InvalidBase64Range { end: base64_end })?;

        // Serato wraps the base64 text with line feeds and leaves out the padding.
        let mut cleaned: Vec<u8> = base64_data
            .iter()
            .copied()
            .filter(|byte| !byte.is_ascii_whitespace() && *byte != b'\x0b')
            .collect();
        match cleaned.len() % 4 {
            1 => cleaned.extend_from_slice(b"A=="),
            2 => cleaned.extend_from_slice(b"=="),
            3 => cleaned.extend_from_slice(b"="),
            _ => {}
        }
        let payload = MARKERS_BASE64.decode(&cleaned).map_err(MarkersError::Base64)?;

        let mut cursor = Cursor::new(payload);
        let version = (cursor.read_u8()?, cursor.read_u8()?);
        if version != (0x01, 0x01) {
            return Err(MarkersError::InvalidVersion { version });
        }

        let mut entries = Vec::new();
        let mut records = 0;
        loop {
            if cursor.position() == cursor.get_ref().len() as u64 {
                if records == 0 {
                    return Err(MarkersError::MissingTerminator);
                }
                break;
            }
            let name_bytes = read_until_null(&mut cursor)?;
            if name_bytes.is_empty() {
                let rest = cursor.get_ref().get(cursor.position() as usize..).unwrap_or_default();
                if rest.iter().any(|&byte| byte != 0) {
                    return Err(MarkersError::TrailingData);
                }
                break;
            }
            if name_bytes.len() > 64 || !name_bytes.is_ascii() {
                return Err(MarkersError::InvalidRecordType);
            }
            records += 1;
            if records > MAXIMUM_RECORDS {
                return Err(MarkersError::SizeLimit);
            }
            let entry_name = String::from_utf8(name_bytes)?;
            let entry_length = cursor.read_u32::<BigEndian>()? as usize;
            let start = cursor.position() as usize;
            let end = start
                .checked_add(entry_length)
                .ok_or(MarkersError::EntryLengthOverflow {
                    start,
                    length: entry_length,
                })?;
            let entry_data = cursor
                .get_ref()
                .get(start..end)
                .ok_or(MarkersError::EntryExceedsPayload {
                    start,
                    length: entry_length,
                })?;
            entries.extend(Self::load(&entry_name, entry_data)?);
            cursor.set_position(end as u64);
        }
        Ok(entries)
    }

    /// Parses a single entry based on its name, or returns `None` for an entry type this parser does not know.
    fn load(entry_name: &str, data: &[u8]) -> Result<Option<Self>, MarkersError> {
        Ok(Some(match entry_name {
            "BPMLOCK" => Self::BpmLock(BpmLock::load(data)?),
            "COLOR" => Self::Color(Color::load(data)?),
            "CUE" => Self::Cue(Cue::load(data)?),
            "LOOP" => Self::Loop(Loop::load(data)?),
            _ => return Ok(None),
        }))
    }
}

impl Display for Markers {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BpmLock(bpm_lock) => write!(formatter, "{bpm_lock}"),
            Self::Color(color) => write!(formatter, "Color: {color}"),
            Self::Cue(cue) => write!(formatter, "{cue}"),
            Self::Loop(saved_loop) => write!(formatter, "{saved_loop}"),
        }
    }
}

impl BpmLock {
    /// Parses a BPM lock entry, a single boolean byte.
    const fn load(data: &[u8]) -> Result<Self, MarkersError> {
        match data {
            [value @ 0..=1] => Ok(Self { enabled: *value != 0 }),
            [value] => Err(MarkersError::InvalidLock { value: *value }),
            _ => Err(MarkersError::BpmLockLength { length: data.len() }),
        }
    }
}

impl Display for BpmLock {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(formatter, "BPM Lock: {}", self.enabled)
    }
}

impl Color {
    /// Creates a color from RGB bytes.
    #[must_use]
    pub const fn new(bytes: [u8; 3]) -> Self {
        let [red, green, blue] = bytes;
        Self { red, green, blue }
    }

    /// Creates a color from ARGB bytes, ignoring the alpha channel.
    #[must_use]
    pub const fn new_argb(bytes: [u8; 4]) -> Self {
        let [_, red, green, blue] = bytes;
        Self { red, green, blue }
    }

    /// Parses a track color entry of four bytes, where the first byte is ignored.
    fn load(data: &[u8]) -> Result<Self, MarkersError> {
        let bytes: [u8; 4] = data.try_into().map_err(|source| MarkersError::ColorLength {
            length: data.len(),
            source,
        })?;
        Ok(Self::new_argb(bytes))
    }

    /// Returns the color as a `0xRRGGBB` value.
    #[must_use]
    pub const fn to_rgb_u32(self) -> u32 {
        ((self.red as u32) << 16) | ((self.green as u32) << 8) | self.blue as u32
    }
}

impl Display for Color {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(formatter, "[{},{},{}]", self.red, self.green, self.blue)
    }
}

impl Cue {
    /// Parses a cue point entry.
    ///
    /// | Offset |            Length | Raw Value     | Decoded   | Type                    | Description
    /// | ------ | ----------------- | ------------- | --------- | ----------------------- | -----------
    /// | `00`   |              `01` | `00`          |           |                         |
    /// | `01`   |              `01` | `00`          | 0         | `uint8_t`               | Index
    /// | `02`   |              `04` | `00 00 00 00` | 0         | `uint32_t`              | Position in ms
    /// | `06`   |              `01` | `00`          |           |                         |
    /// | `07`   |              `03` | `cc 00 00`    | `#CC0000` | 3-byte RGB value        | Color
    /// | `0a`   |              `02` | `00 00`       |           |                         |
    /// | `0c`   | `01` <= X <= `33` | `00`          | ``        | UTF-8 (null-terminated) | Name
    ///
    fn load(data: &[u8]) -> Result<Self, MarkersError> {
        if data.len() < 13 {
            return Err(MarkersError::CueLength { length: data.len() });
        }
        let mut cursor = Cursor::new(data);
        cursor.set_position(1);
        let index = cursor.read_u8()?;
        let position = cursor.read_u32::<BigEndian>()?;
        cursor.set_position(cursor.position() + 1);
        let mut color = [0; 3];
        cursor.read_exact(&mut color)?;
        cursor.set_position(cursor.position() + 2);
        let name = marker_name(data.get(12..).ok_or(MarkersError::CueLength { length: data.len() })?)?;
        Ok(Self {
            index,
            position,
            color: Color::new(color),
            name,
        })
    }
}

impl Display for Cue {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        let seconds = f64::from(self.position) / 1000.0;
        let name = if self.name.is_empty() {
            format_position_timestamp(self.position)
        } else {
            self.name.clone()
        };
        write!(
            formatter,
            "Cue {}: {:<12} {seconds:>7.3}s",
            u16::from(self.index) + 1,
            name
        )
    }
}

impl Loop {
    /// Parses a saved loop entry.
    ///
    /// | Offset   |              Length | Raw Value     | Decoded   | Type                    | Description
    /// | -------- | ------------------- | ------------- | --------- | ----------------------- | -----------
    /// | `00`     |                `01` | `00`          |           |                         |
    /// | `01`     |                `01` | `00`          | 0         | `uint8_t`               | Index
    /// | `02`     |                `04` | `00 00 00 00` | 0         | `int32_t`               | Start Position in milliseconds
    /// | `06`     |                `04` | `00 00 08 26` | 2086      | `int32_t`               | End Position in milliseconds
    /// | `0a`     |                `04` | `ff ff ff ff` |           |                         |
    /// | `0e`     |                `04` | `00 27 aa e1` | `#27aae1` | 4-byte ARGB value       | Color
    /// | `12`     |                `01` | `00`          |           |                         |
    /// | `13`     |                `01` | `00`          | False     | `uint8_t` (boolean)     | Locked
    /// | `14`     | `01` <= X <= `7fec` | `00`          | ``        | UTF-8 (null-terminated) | Name
    ///
    fn load(data: &[u8]) -> Result<Self, MarkersError> {
        if data.len() < 21 {
            return Err(MarkersError::LoopLength { length: data.len() });
        }
        let mut cursor = Cursor::new(data);
        cursor.set_position(1);
        let index = cursor.read_u8()?;
        let start_position = cursor.read_i32::<BigEndian>()?;
        let end_position = cursor.read_i32::<BigEndian>()?;
        if end_position < start_position {
            return Err(MarkersError::ReversedLoop);
        }
        cursor.set_position(cursor.position() + 4);
        let mut color = [0; 4];
        cursor.read_exact(&mut color)?;
        cursor.set_position(cursor.position() + 1);
        let locked = BpmLock::load(&[cursor.read_u8()?])?.enabled;
        let name = marker_name(data.get(20..).ok_or(MarkersError::LoopLength { length: data.len() })?)?;
        Ok(Self {
            index,
            start_position,
            end_position,
            color: Color::new_argb(color),
            locked,
            name,
        })
    }
}

impl Display for Loop {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        let name = if self.name.is_empty() {
            format!(
                "{}{}",
                if self.start_position < 0 { "-" } else { "" },
                format_position_timestamp(self.start_position.unsigned_abs())
            )
        } else {
            self.name.clone()
        };
        write!(
            formatter,
            "Loop {}: {name} [{:.2}s - {:.2}s] {}",
            u16::from(self.index) + 1,
            f64::from(self.start_position) / 1000.0,
            f64::from(self.end_position) / 1000.0,
            if self.locked { "locked" } else { "unlocked" }
        )
    }
}

/// Reads bytes until a null byte, excluding the terminator.
fn read_until_null<R: BufRead>(reader: &mut R) -> io::Result<Vec<u8>> {
    let mut buffer = Vec::new();
    reader.read_until(b'\x00', &mut buffer)?;
    if buffer.last() != Some(&b'\x00') {
        return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "missing zero terminator"));
    }
    buffer.pop();
    Ok(buffer)
}

/// Reads an exact UTF-8 label with a required terminator and only zero padding.
fn marker_name(bytes: &[u8]) -> Result<String, MarkersError> {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .ok_or(MarkersError::MissingTerminator)?;
    if end > MAXIMUM_LABEL_BYTES {
        return Err(MarkersError::SizeLimit);
    }
    let (label, padding) = bytes.split_at_checked(end).ok_or(MarkersError::MissingTerminator)?;
    if padding.iter().any(|byte| *byte != 0) {
        return Err(MarkersError::TrailingData);
    }
    Ok(str::from_utf8(label)?.to_owned())
}

/// Recovers a legacy 24-bit value from four seven-bit groups.
fn legacy_number([high, second, third, low]: [u8; 4]) -> u32 {
    (u32::from(high & 7) << 21) | (u32::from(second & 127) << 14) | (u32::from(third & 127) << 7) | u32::from(low & 127)
}

/// Decodes a legacy RGB value without narrowing any colour component.
fn legacy_color(bytes: [u8; 4]) -> Color {
    let [_, red, green, blue] = legacy_number(bytes).to_be_bytes();
    Color { red, green, blue }
}

#[cfg(test)]
pub mod test_legacy {
    use super::*;

    /// Encodes independent byte-layout expectations, with two cue slots and one loop slot in use.
    pub fn payload() -> Vec<u8> {
        let mut bytes = vec![2, 5, 0, 0, 0, 14];
        for slot in 0..14 {
            let mut record = [0; 22];
            record[0] = 127;
            record[20] = if slot < 5 { 1 } else { 3 };
            match slot {
                0 => {
                    record[0] = 0;
                    record[1..5].copy_from_slice(&[0, 0, 3, 104]);
                    record[16..20].copy_from_slice(&[4, 72, 104, 86]);
                }
                2 => {
                    record[0] = 0;
                    record[1..5].copy_from_slice(&[0, 0, 7, 104]);
                    record[16..20].copy_from_slice(&[0, 0, 0, 127]);
                }
                6 => {
                    record[0] = 0;
                    record[1..5].copy_from_slice(&[0, 0, 3, 116]);
                    record[6..10].copy_from_slice(&[0, 0, 15, 80]);
                    record[16..20].copy_from_slice(&[0, 0, 0, 127]);
                    record[21] = 1;
                }
                _ => {}
            }
            bytes.extend_from_slice(&record);
        }
        bytes.extend_from_slice(&[7, 124, 0, 1]);
        bytes
    }

    #[test]
    fn parses_seven_bit_groups_and_counts_unset_slots() {
        let markers = Markers::parse_legacy(&payload()).expect("legacy layout");
        assert_eq!(
            markers,
            [
                Markers::Cue(Cue {
                    index: 0,
                    position: 488,
                    color: Color::new([0x92, 0x34, 0x56]),
                    name: String::new()
                }),
                Markers::Cue(Cue {
                    index: 2,
                    position: 1000,
                    color: Color::new([0, 0, 127]),
                    name: String::new()
                }),
                Markers::Loop(Loop {
                    index: 1,
                    start_position: 500,
                    end_position: 2000,
                    color: Color::new([0, 0, 127]),
                    locked: true,
                    name: String::new(),
                }),
                Markers::Color(Color::new([0xff, 0, 1])),
            ]
        );
    }

    #[test]
    fn rejects_legacy_header_flags_lengths_and_reversed_boundaries() {
        for (offset, value) in [(0, 1), (1, 4), (5, 13), (6, 1), (6 + 21, 2), (6 + 6 * 22 + 5, 127)] {
            let mut bytes = payload();
            bytes[offset] = value;
            assert!(Markers::parse_legacy(&bytes).is_err(), "{offset}");
        }
        let bytes = payload();
        assert!(Markers::parse_legacy(&bytes[..bytes.len() - 1]).is_err());
        assert!(Markers::parse_legacy(&[bytes.as_slice(), &[0]].concat()).is_err());
        let mut bytes = bytes;
        bytes[6 + 6 * 22 + 6..6 + 6 * 22 + 10].copy_from_slice(&[0, 0, 0, 1]);
        assert!(Markers::parse_legacy(&bytes).is_err());
    }

    #[test]
    fn skips_valid_unknown_slots_without_discarding_other_markers() {
        let mut bytes = payload();
        bytes[6 + 20] = 99;
        let markers = Markers::parse_legacy(&bytes).expect("unknown legacy type");
        assert_eq!(markers.len(), 3);
        assert!(matches!(markers[0], Markers::Cue(Cue { index: 1, .. })));
    }
}

#[cfg(test)]
mod test_color {
    use super::*;

    #[test]
    fn errors_preserve_decoding_sources_and_display_text() {
        let error = Markers::parse(b"\x01\x01!\0").expect_err("invalid base64");
        assert!(matches!(error, MarkersError::Base64(_)));
        assert_eq!(error.to_string(), "Failed to decode base64 data");
        assert!(std::error::Error::source(&error).is_some());
        let error = Color::load(&[0; 3]).expect_err("short color");
        assert!(matches!(error, MarkersError::ColorLength { length: 3, .. }));
        assert_eq!(error.to_string(), "Invalid data length for Color");
        assert!(std::error::Error::source(&error).is_some());
        let error = Markers::parse(b"\x01\x01AQ==\0").expect_err("short version");
        assert!(matches!(error, MarkersError::Read(_)));
        assert_eq!(error.to_string(), "failed to fill whole buffer");
        assert!(std::error::Error::source(&error).is_some());
    }

    #[test]
    fn creates_colors_from_rgb_and_argb() {
        assert_eq!(
            Color::new([255, 128, 0]),
            Color {
                red: 255,
                green: 128,
                blue: 0
            }
        );
        assert_eq!(
            Color::new_argb([200, 10, 20, 30]),
            Color {
                red: 10,
                green: 20,
                blue: 30
            }
        );
    }

    #[test]
    fn converts_to_packed_rgb() {
        assert_eq!(Color::new([0x12, 0x34, 0x56]).to_rgb_u32(), 0x0012_3456);
    }

    #[test]
    fn loads_four_byte_entries_only() {
        assert_eq!(
            Color::load(&[0x00, 0xFF, 0x80, 0x40]).expect("parses"),
            Color::new([0xFF, 0x80, 0x40])
        );
        assert!(Color::load(&[0x00, 0xFF, 0x80]).is_err());
        assert!(Color::load(&[0x00, 0xFF, 0x80, 0x40, 0x00]).is_err());
    }

    #[test]
    fn displays_plain_rgb() {
        assert_eq!(Markers::Color(Color::new([1, 2, 3])).to_string(), "Color: [1,2,3]");
    }
}

#[cfg(test)]
mod test_cue_and_loop {
    use super::*;

    /// Returns the bytes of a CUE entry of the Markers2 tag.
    fn cue_bytes(index: u8, position: u32, color: [u8; 3], name: &[u8]) -> Vec<u8> {
        [
            &[0x00, index][..],
            &position.to_be_bytes(),
            &[0x00],
            &color,
            &[0x00, 0x00],
            name,
        ]
        .concat()
    }

    /// Returns the bytes of a LOOP entry of the Markers2 tag, locked or not.
    fn loop_bytes(locked: bool) -> Vec<u8> {
        [
            &[0x00, 0x00][..],
            &500_u32.to_be_bytes(),
            &2000_u32.to_be_bytes(),
            &[0xFF; 4],
            &[0x00, 0x27, 0xAA, 0xE1],
            &[0x00],
            &[u8::from(locked)],
            b"\0",
        ]
        .concat()
    }

    #[test]
    fn unnamed_cue_preserves_empty_label_and_displays_timestamp() {
        let cue = Cue::load(&cue_bytes(0, 61_500, [0xCC, 0x00, 0x00], b"\0")).expect("parses");
        assert_eq!(cue.position, 61_500);
        assert_eq!(cue.color, Color::new([0xCC, 0x00, 0x00]));
        assert_eq!(cue.name, "");
        assert!(cue.to_string().starts_with("Cue 1: 01:01.5"));
    }

    #[test]
    fn labels_preserve_whitespace_and_accept_only_zero_padding() {
        for label in [b"  Drop \t\0".as_slice(), b" \t\0", b"Drop\0\0\0"] {
            let cue = Cue::load(&cue_bytes(0, 0, [1, 2, 3], label)).expect("terminated label");
            let end = label.iter().position(|byte| *byte == 0).expect("terminator");
            assert_eq!(cue.name.as_bytes(), &label[..end]);
        }
        for label in [b"Drop".as_slice(), b"Drop\0hidden", b"\xff\0"] {
            assert!(Cue::load(&cue_bytes(0, 0, [1, 2, 3], label)).is_err());
            let mut bytes = loop_bytes(false);
            bytes.pop();
            bytes.extend_from_slice(label);
            assert!(Loop::load(&bytes).is_err());
        }
    }

    #[test]
    fn rejects_nonboolean_locks_and_reversed_loops() {
        for value in [2, 127, 255] {
            assert!(BpmLock::load(&[value]).is_err());
            let mut bytes = loop_bytes(false);
            bytes[19] = value;
            assert!(Loop::load(&bytes).is_err());
        }
        let mut bytes = loop_bytes(false);
        bytes[6..10].copy_from_slice(&499_i32.to_be_bytes());
        assert!(Loop::load(&bytes).is_err());
    }

    #[test]
    fn named_cue_keeps_its_name() {
        let cue = Cue::load(&cue_bytes(2, 2000, [0x00, 0xFF, 0x00], b"Drop\0")).expect("parses");
        assert_eq!(cue.index, 2);
        assert_eq!(cue.name, "Drop");
        assert!(Markers::Cue(cue).to_string().starts_with("Cue 3: Drop"));
    }

    #[test]
    fn short_entries_are_rejected() {
        assert!(Cue::load(&[0x00; 4]).is_err());
        assert!(Loop::load(&[0x00; 10]).is_err());
        assert!(BpmLock::load(&[]).is_err());
        assert!(BpmLock::load(&[1, 0]).is_err());
    }

    #[test]
    fn loops_parse_lock_state() {
        let locked = Loop::load(&loop_bytes(true)).expect("parses");
        assert_eq!(locked.start_position, 500);
        assert_eq!(locked.end_position, 2000);
        assert_eq!(locked.color, Color::new([0x27, 0xAA, 0xE1]));
        assert!(locked.locked);
        assert!(Markers::Loop(locked).to_string().ends_with(" locked"));
        let unlocked = Loop::load(&loop_bytes(false)).expect("parses");
        assert!(Markers::Loop(unlocked).to_string().ends_with("unlocked"));
    }

    #[test]
    fn saved_loop_preserves_signed_positions_and_display() {
        let mut bytes = loop_bytes(false);
        bytes[2..6].copy_from_slice(&(-5_i32).to_be_bytes());
        bytes[6..10].copy_from_slice(&7555_i32.to_be_bytes());
        let saved_loop = Loop::load(&bytes).expect("loop starts before audio");
        assert_eq!(saved_loop.start_position, -5);
        assert_eq!(saved_loop.end_position, 7555);
        assert_eq!(saved_loop.to_string(), "Loop 1: -00:00.0 [-0.01s - 7.55s] unlocked");
        bytes[2..6].copy_from_slice(&i32::MIN.to_be_bytes());
        assert!(Loop::load(&bytes).expect("signed minimum").to_string().contains('-'));
    }
}

#[cfg(test)]
mod test_markers_parse {
    use super::*;

    /// Returns a Markers2 tag body: the version bytes, the base64 payload, and a null.
    fn encode(payload: &[u8]) -> Vec<u8> {
        let mut data = vec![0x01, 0x01];
        data.extend(general_purpose::STANDARD.encode(payload).bytes());
        data.push(0);
        data
    }

    #[test]
    fn parses_entries_from_base64_payload() {
        let mut payload = vec![0x01, 0x01];
        payload.extend_from_slice(b"BPMLOCK\0");
        payload.extend_from_slice(&1_u32.to_be_bytes());
        payload.push(1);
        payload.extend_from_slice(b"COLOR\0");
        payload.extend_from_slice(&4_u32.to_be_bytes());
        payload.extend_from_slice(&[0x00, 0x11, 0x22, 0x33]);
        let markers = Markers::parse(&encode(&payload)).expect("parses");
        assert_eq!(
            markers,
            vec![
                Markers::BpmLock(BpmLock { enabled: true }),
                Markers::Color(Color::new([0x11, 0x22, 0x33])),
            ]
        );
    }

    #[test]
    fn skips_unknown_entries_and_accepts_zero_padding() {
        let mut payload = vec![0x01, 0x01];
        payload.extend_from_slice(b"FLIP\0");
        payload.extend_from_slice(&3_u32.to_be_bytes());
        payload.extend_from_slice(&[1, 2, 3]);
        payload.extend_from_slice(b"BPMLOCK\0");
        payload.extend_from_slice(&1_u32.to_be_bytes());
        payload.push(0);
        payload.extend_from_slice(&[0, 0, 0]);
        let markers = Markers::parse(&encode(&payload)).expect("parses");
        assert_eq!(markers, vec![Markers::BpmLock(BpmLock { enabled: false })]);
    }

    #[test]
    fn rejects_data_after_the_terminator() {
        let payload = [0x01, 0x01, 0x00, 0x00, 0x07];
        assert!(Markers::parse(&encode(&payload)).is_err());
    }

    #[test]
    fn rejects_entry_longer_than_payload() {
        let payload = [0x01, 0x01, b'C', b'U', b'E', 0x00, 0xff, 0xff, 0xff, 0xff];
        let error = Markers::parse(&encode(&payload)).expect_err("oversized entry");
        assert!(error.to_string().contains("Marker entry exceeds payload"));
    }

    #[test]
    fn rejects_missing_payload() {
        assert!(Markers::parse(b"\0").is_err());
        assert!(Markers::parse(b"\x01\0").is_err());
        assert!(Markers::parse(&encode(&[0x02, 0x01])).is_err());
    }

    #[test]
    fn envelope_and_record_names_are_strict() {
        let valid = encode(&[1, 1, 0]);
        let mut version = valid.clone();
        version[0] = 2;
        assert!(Markers::parse(&version).is_err());
        assert!(Markers::parse(&[valid.as_slice(), b"hidden"].concat()).is_err());
        for payload in [b"\x01\x01".as_slice(), b"\x01\x01FLIP", b"\x01\x01\xc3\xa9\0\0\0\0\0"] {
            assert!(Markers::parse(&encode(payload)).is_err());
        }
    }

    #[test]
    fn supports_documented_base64_whitespace_and_omitted_padding() {
        for data in [
            b"\x01\x01AQEA".as_slice(),
            b"\x01\x01 A Q\tE\rA\n\x0b\0\0",
            b"\x01\x01AQEAAB==\0",
            b"\x01\x01AQEAA\0",
        ] {
            assert_eq!(Markers::parse(data).expect("valid envelope"), []);
        }
    }

    #[test]
    fn reads_bytes_until_null() {
        let mut cursor = Cursor::new(vec![0x48, 0x65, 0x00, 0xFF]);
        assert_eq!(read_until_null(&mut cursor).expect("reads"), vec![0x48, 0x65]);
        let mut cursor = Cursor::new(Vec::new());
        assert!(read_until_null(&mut cursor).is_err());
    }
}
