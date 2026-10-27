//! The Serato Autotags tag, which holds the detected BPM and the auto and manual gain.

use std::fmt;
use std::fmt::Display;

use anyhow::anyhow;

/// Offset of the BPM field after the two version bytes.
const BPM_OFFSET: usize = 2;

/// Offset of the auto gain field.
const AUTO_GAIN_OFFSET: usize = 9;

/// Offset of the manual gain field, which is also the size of tags written without it.
const GAIN_OFFSET: usize = 16;

/// Size of a tag with all three fields.
const FULL_BYTES: usize = 22;

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
    /// The checks match the C++ parser, so both apps reject the same damaged tags.
    ///
    /// # Errors
    ///
    /// Returns an error when the size or version is unexpected, a number cannot be parsed,
    /// or the BPM is negative.
    pub fn parse(data: &[u8]) -> anyhow::Result<Self> {
        let (Some(bpm_bytes), Some(auto_gain_bytes)) = (
            data.get(BPM_OFFSET..AUTO_GAIN_OFFSET),
            data.get(AUTO_GAIN_OFFSET..GAIN_OFFSET),
        ) else {
            return Err(anyhow!("Data is too short to contain all necessary information"));
        };
        if !matches!(data.len(), GAIN_OFFSET | FULL_BYTES) || !data.starts_with(&[0x01, 0x01]) {
            return Err(anyhow!("Unexpected autotags size or version"));
        }
        let bpm = fixed_number(bpm_bytes).map_err(|error| anyhow!("Failed to parse BPM: {error}"))?;
        if bpm < 0.0 {
            return Err(anyhow!("Negative BPM: {bpm}"));
        }
        let auto_gain = fixed_number(auto_gain_bytes).map_err(|error| anyhow!("Failed to parse Auto Gain: {error}"))?;
        // The manual gain field is missing in older files.
        let gain = match data.get(GAIN_OFFSET..) {
            Some(gain_bytes) if !gain_bytes.is_empty() => {
                fixed_number(gain_bytes).map_err(|error| anyhow!("Failed to parse Gain dB: {error}"))?
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
fn fixed_number(bytes: &[u8]) -> anyhow::Result<f32> {
    let length = bytes
        .iter()
        .position(|&byte| byte == 0)
        .ok_or_else(|| anyhow!("missing zero terminator"))?;
    let (digits, padding) = bytes.split_at(length);
    if padding.iter().any(|&byte| byte != 0) {
        return Err(anyhow!("data after the zero terminator"));
    }
    let text = std::str::from_utf8(digits).map_err(|_| anyhow!("not UTF-8"))?;
    parse_tag_number(text).map(|value| value as f32)
}

/// Strictly parses a finite ASCII number with no ignored text, like `parseTagNumber` in the C++ app.
fn parse_tag_number(text: &str) -> anyhow::Result<f64> {
    let trimmed = text.trim_matches([' ', '\t']);
    if trimmed.is_empty()
        || trimmed.starts_with('+')
        || !trimmed.chars().all(|character| "0123456789.eE+-".contains(character))
    {
        return Err(anyhow!("not a number {text:?}"));
    }
    let value: f64 = trimmed.parse().map_err(|error| anyhow!("{error} {text:?}"))?;
    let significand = trimmed.split(['e', 'E']).next().unwrap_or_default();
    let underflowed = value == 0.0 && significand.contains(|character: char| ('1'..='9').contains(&character));
    if !value.is_finite() || underflowed {
        return Err(anyhow!("out of range {text:?}"));
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
        assert!(AutoTags::parse(&[0x01, 0x01, 0x31, 0x32, 0x38]).is_err());
        assert!(AutoTags::parse(&[]).is_err());
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
