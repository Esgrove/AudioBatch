//! The Serato Autotags tag, which holds the detected BPM and the auto and manual gain.

use std::fmt;
use std::fmt::Display;

use anyhow::anyhow;

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
    /// # Errors
    ///
    /// Returns an error when the data is too short or a number cannot be parsed.
    pub fn parse(data: &[u8]) -> anyhow::Result<Self> {
        let (Some(bpm_bytes), Some(auto_gain_bytes)) = (data.get(2..9), data.get(9..16)) else {
            return Err(anyhow!("Data is too short to contain all necessary information"));
        };
        let bpm = parse_number(bpm_bytes, false).map_err(|error| anyhow!("Failed to parse BPM: {error}"))?;
        let auto_gain =
            parse_number(auto_gain_bytes, true).map_err(|error| anyhow!("Failed to parse Auto Gain: {error}"))?;
        // The manual gain field is missing in older files.
        let gain = match data.get(16..22) {
            Some(gain_bytes) => {
                parse_number(gain_bytes, true).map_err(|error| anyhow!("Failed to parse Gain dB: {error}"))?
            }
            None => 0.0,
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

/// Parses a zero-terminated ASCII number, keeping only digits, the decimal point, and optionally a minus sign.
fn parse_number(bytes: &[u8], allow_negative: bool) -> anyhow::Result<f32> {
    let text: String = std::str::from_utf8(bytes)
        .map_err(|_| anyhow!("not UTF-8"))?
        .chars()
        .filter(|character| character.is_numeric() || *character == '.' || (allow_negative && *character == '-'))
        .collect();
    text.trim_end_matches('.')
        .parse()
        .map_err(|error| anyhow!("{error} {text:?}"))
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
    fn formats_all_fields() {
        let data = [HEADER, b"128.00\0", b"-3.257\0", b"0.000\0"].concat();
        let autotags = AutoTags::parse(&data).expect("Should parse valid autotags data");
        assert_eq!(
            autotags.to_string(),
            "BPM: 128.000, Auto Gain: -3.257 dB, Gain: 0.000 dB"
        );
    }
}
