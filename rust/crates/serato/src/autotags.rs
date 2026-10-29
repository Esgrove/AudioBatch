//! The Serato Autotags tag, which holds the detected BPM and the auto and manual gain.

use std::fmt;
use std::fmt::Display;

/// Offset of the BPM field after the two version bytes.
const BPM_OFFSET: usize = 2;

/// Offset of the auto gain field.
const AUTO_GAIN_OFFSET: usize = 9;

/// Offset of the manual gain field, which is also the size of tags written without it.
const GAIN_OFFSET: usize = 16;

/// Size of a tag with all three fields.
const FULL_BYTES: usize = 22;

/// An ASCII number field in an Autotags payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AutoTagsField {
    /// Beats per minute.
    Bpm,
    /// Calculated gain.
    AutoGain,
    /// Manual gain.
    Gain,
}

/// A malformed Autotags header or numeric field.
#[derive(Debug, thiserror::Error)]
pub enum AutoTagsError {
    /// The required fields are incomplete.
    #[error("Data is too short to contain all necessary information")]
    TooShort {
        /// Payload length in bytes.
        length: usize,
    },
    /// The size or version does not match the known layouts.
    #[error("Unexpected autotags size or version")]
    SizeOrVersion {
        /// Payload length in bytes.
        length: usize,
        /// Version bytes in the header.
        version: [u8; 2],
    },
    /// An ASCII field could not be decoded.
    #[error("Failed to parse {field}: {source}")]
    InvalidField {
        /// Field that could not be decoded.
        field: AutoTagsField,
        /// Concrete numeric decoding failure.
        #[source]
        source: AutoTagsNumberError,
    },
    /// The BPM is negative.
    #[error("Negative BPM: {bpm}")]
    NegativeBpm {
        /// Rejected BPM.
        bpm: f32,
    },
}

/// A malformed fixed-width ASCII number in an Autotags field.
#[derive(Debug, thiserror::Error)]
pub enum AutoTagsNumberError {
    /// The field has no zero padding.
    #[error("missing zero terminator")]
    MissingTerminator,
    /// Nonzero bytes follow the first zero.
    #[error("data after the zero terminator")]
    DataAfterTerminator,
    /// The field is not valid UTF-8.
    #[error("not UTF-8")]
    Utf8(#[source] std::str::Utf8Error),
    /// The field does not use the accepted numeric syntax.
    #[error("not a number {text:?}")]
    InvalidSyntax {
        /// Original field text, including whitespace.
        text: String,
    },
    /// The floating-point parser rejected the field.
    #[error("{source} {text:?}")]
    Parse {
        /// Original field text, including whitespace.
        text: String,
        /// Floating-point parser failure.
        #[source]
        source: std::num::ParseFloatError,
    },
    /// The field overflowed or underflowed.
    #[error("out of range {text:?}")]
    OutOfRange {
        /// Original field text, including whitespace.
        text: String,
    },
}

/// Serato autotags with BPM and gain values.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct AutoTags {
    /// Beats per minute.
    pub bpm: f32,
    /// Calculated auto gain in dB.
    pub auto_gain: f32,
    /// Manually adjusted gain in dB.
    pub gain: f32,
}

impl Display for AutoTagsField {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Bpm => "BPM",
            Self::AutoGain => "Auto Gain",
            Self::Gain => "Gain dB",
        })
    }
}

impl AutoTags {
    /// Parses autotags data, which contains the BPM, auto gain, and manual gain values as ASCII numbers.
    ///
    /// | Offset | Length | Raw Value              | Decoded Value | Type                    | Description
    /// | ------ | ------ | ---------------------- | ------------- | ----------------------- | -----------
    /// |   `00` |   `02` | `01 01`                |               | *?* (2 bytes)           |
    /// |   `02` |   `07` | `31 31 35 2e 30 30 00` |      `115.00` | ASCII (zero-terminated) | BPM
    /// |   `09` |   `07` | `2d 33 2e 32 35 37 00` |      `-3.257` | ASCII (zero-terminated) | Auto Gain
    /// |   `16` |   `06` | `30 2e 30 30 30 00`    |       `0.000` | ASCII (zero-terminated) | Gain dB
    ///
    /// The checks are strict, so a damaged tag fails instead of yielding wrong values.
    ///
    /// # Errors
    ///
    /// Returns an error when the size or version is unexpected, a number cannot be parsed,
    /// or the BPM is negative.
    pub fn parse(data: &[u8]) -> Result<Self, AutoTagsError> {
        let (Some(bpm_bytes), Some(auto_gain_bytes)) = (
            data.get(BPM_OFFSET..AUTO_GAIN_OFFSET),
            data.get(AUTO_GAIN_OFFSET..GAIN_OFFSET),
        ) else {
            return Err(AutoTagsError::TooShort { length: data.len() });
        };
        if !matches!(data.len(), GAIN_OFFSET | FULL_BYTES) || !data.starts_with(&[0x01, 0x01]) {
            return Err(AutoTagsError::SizeOrVersion {
                length: data.len(),
                version: data.first_chunk::<2>().copied().unwrap_or_default(),
            });
        }
        let bpm = fixed_number(bpm_bytes).map_err(|source| AutoTagsError::InvalidField {
            field: AutoTagsField::Bpm,
            source,
        })?;
        if bpm < 0.0 {
            return Err(AutoTagsError::NegativeBpm { bpm });
        }
        let auto_gain = fixed_number(auto_gain_bytes).map_err(|source| AutoTagsError::InvalidField {
            field: AutoTagsField::AutoGain,
            source,
        })?;
        // The manual gain field is missing in older files.
        let gain = match data.get(GAIN_OFFSET..) {
            Some(gain_bytes) if !gain_bytes.is_empty() => {
                fixed_number(gain_bytes).map_err(|source| AutoTagsError::InvalidField {
                    field: AutoTagsField::Gain,
                    source,
                })?
            }
            _ => 0.0,
        };
        Ok(Self { bpm, auto_gain, gain })
    }
}

impl Display for AutoTags {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(
            formatter,
            "BPM: {:.3}, Auto Gain: {:.3} dB, Gain: {:.3} dB",
            self.bpm, self.auto_gain, self.gain
        )
    }
}

/// Parses a fixed-width ASCII number padded with zero bytes, rejecting anything after the padding.
#[expect(
    clippy::cast_possible_truncation,
    reason = "the fields are stored as f32 and Serato writes at most three decimals"
)]
fn fixed_number(bytes: &[u8]) -> Result<f32, AutoTagsNumberError> {
    let length = bytes
        .iter()
        .position(|&byte| byte == 0)
        .ok_or(AutoTagsNumberError::MissingTerminator)?;
    let (digits, padding) = bytes.split_at(length);
    if padding.iter().any(|&byte| byte != 0) {
        return Err(AutoTagsNumberError::DataAfterTerminator);
    }
    let text = std::str::from_utf8(digits).map_err(AutoTagsNumberError::Utf8)?;
    parse_tag_number(text).map(|value| value as f32)
}

/// Strictly parses a finite ASCII number with no ignored text.
fn parse_tag_number(text: &str) -> Result<f64, AutoTagsNumberError> {
    let trimmed = text.trim_matches([' ', '\t']);
    if trimmed.is_empty()
        || trimmed.starts_with('+')
        || !trimmed.chars().all(|character| "0123456789.eE+-".contains(character))
    {
        return Err(AutoTagsNumberError::InvalidSyntax { text: text.to_owned() });
    }
    let value: f64 = trimmed.parse().map_err(|source| AutoTagsNumberError::Parse {
        text: text.to_owned(),
        source,
    })?;
    let significand = trimmed.split(['e', 'E']).next().unwrap_or_default();
    let underflowed = value == 0.0 && significand.contains(|character: char| ('1'..='9').contains(&character));
    if !value.is_finite() || underflowed {
        return Err(AutoTagsNumberError::OutOfRange { text: text.to_owned() });
    }
    Ok(value)
}

#[cfg(test)]
mod test_autotags {
    use super::*;

    /// Autotags payload version header.
    const HEADER: &[u8] = &[0x01, 0x01];

    #[test]
    fn parses_full_autotags_data() {
        let data = [HEADER, b"128.00\0", b"-3.257\0", b"0.000\0"].concat();
        let autotags = AutoTags::parse(&data).expect("Should parse valid autotags data");
        assert!((autotags.bpm - 128.0).abs() < 0.001);
        assert!((autotags.auto_gain + 3.257).abs() < 0.001);
        assert!(autotags.gain.abs() < 0.001);
    }

    #[test]
    fn parses_without_gain_field() {
        let data = [HEADER, b"128.00\0", b"-3.257\0"].concat();
        let autotags = AutoTags::parse(&data).expect("Should parse autotags without gain");
        assert!((autotags.bpm - 128.0).abs() < 0.001);
        assert!(autotags.gain.abs() < 0.001);
    }

    #[test]
    fn parses_positive_auto_gain() {
        let data = [HEADER, b"115.00\0", b"2.500\0\0", b"1.200\0"].concat();
        let autotags = AutoTags::parse(&data).expect("Should parse positive auto gain");
        assert!((autotags.bpm - 115.0).abs() < 0.001);
        assert!((autotags.auto_gain - 2.5).abs() < 0.001);
        assert!((autotags.gain - 1.2).abs() < 0.001);
    }

    #[test]
    fn fails_on_short_data() {
        let error = AutoTags::parse(&[0x01, 0x01, 0x31, 0x32, 0x38]).expect_err("short payload");
        assert!(matches!(error, AutoTagsError::TooShort { length: 5 }));
        assert_eq!(
            error.to_string(),
            "Data is too short to contain all necessary information"
        );
        assert!(AutoTags::parse(&[]).is_err());
    }

    #[test]
    fn invalid_fields_preserve_the_field_and_source() {
        let data = [HEADER, b"\xff28.00\0", b"-3.257\0"].concat();
        let error = AutoTags::parse(&data).expect_err("invalid UTF-8");
        assert_eq!(error.to_string(), "Failed to parse BPM: not UTF-8");
        assert!(std::error::Error::source(&error).is_some());
        let AutoTagsError::InvalidField {
            field: AutoTagsField::Bpm,
            source,
        } = error
        else {
            panic!("expected the BPM field");
        };
        assert!(matches!(source, AutoTagsNumberError::Utf8(_)));
        assert!(std::error::Error::source(&source).is_some());
        let error = parse_tag_number("1e").expect_err("incomplete exponent");
        assert!(matches!(error, AutoTagsNumberError::Parse { .. }));
        assert!(std::error::Error::source(&error).is_some());
        assert_eq!(error.to_string(), "invalid float literal \"1e\"");
    }

    #[test]
    fn rejects_damaged_numbers_and_sizes() {
        let valid = [HEADER, b"128.00\0", b"-3.257\0", b"0.000\0"].concat();
        let mut wrong_version = valid.clone();
        wrong_version[1] = 0x02;
        assert!(AutoTags::parse(&wrong_version).is_err());
        assert!(AutoTags::parse(&[valid.as_slice(), b"\0"].concat()).is_err());
        assert!(AutoTags::parse(&[HEADER, b"12x.00\0", b"-3.257\0"].concat()).is_err());
        assert!(AutoTags::parse(&[HEADER, b"-12.00\0", b"-3.257\0"].concat()).is_err());
        assert!(AutoTags::parse(&[HEADER, b"128\0\0\x017", b"-3.257\0"].concat()).is_err());
        assert!(AutoTags::parse(&[HEADER, b"+128.0\0", b"-3.257\0"].concat()).is_err());
        assert!(AutoTags::parse(&[HEADER, b"128.000", b"-3.257\0"].concat()).is_err());
    }

    #[test]
    fn formats_all_fields() {
        let data = [HEADER, b"128.00\0", b"-3.257\0", b"0.000\0"].concat();
        let autotags = AutoTags::parse(&data).expect("Should parse valid autotags data");
        assert_eq!(
            autotags.to_string(),
            "BPM: 128.000, Auto Gain: -3.257 dB, Gain: 0.000 dB"
        );
    }
}
