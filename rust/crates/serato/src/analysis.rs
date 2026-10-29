//! The Serato Analysis tag, which holds the version of the Serato analysis that wrote the other tags.

use std::fmt;
use std::fmt::Display;

/// A malformed Serato analysis version payload.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("Data is too short to contain version information")]
pub struct AnalysisVersionError {
    /// Number of bytes in the payload.
    pub length: usize,
}

/// Serato analysis version number.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct AnalysisVersion {
    /// Major version.
    pub major_version: u8,
    /// Minor version.
    pub minor_version: u8,
}

impl AnalysisVersion {
    /// Parses the analysis tag, which contains the Serato analysis version number, for example 2.1.
    ///
    /// | Offset | Length | Raw Value | Decoded Value | Type            | Description
    /// | ------ | ------ | --------- | ------------- | --------------- | -----------
    /// |   `00` |   `01` |      `02` |           `2` | `unsigned char` | Major Version
    /// |   `01` |   `01` |      `01` |           `1` | `unsigned char` | Minor Version
    ///
    /// # Errors
    ///
    /// Returns an error when the data has fewer than two bytes.
    pub const fn parse(data: &[u8]) -> Result<Self, AnalysisVersionError> {
        let [major_version, minor_version, ..] = data else {
            return Err(AnalysisVersionError { length: data.len() });
        };
        Ok(Self {
            major_version: *major_version,
            minor_version: *minor_version,
        })
    }
}

impl Display for AnalysisVersion {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        write!(formatter, "Version {}.{}", self.major_version, self.minor_version)
    }
}

#[cfg(test)]
mod test_analysis_version {
    use super::*;

    #[test]
    fn parses_valid_two_byte_data() {
        let version = AnalysisVersion::parse(&[2, 1]).expect("Should parse version 2.1");
        assert_eq!(version.major_version, 2);
        assert_eq!(version.minor_version, 1);
    }

    #[test]
    fn parses_data_longer_than_two_bytes() {
        let version = AnalysisVersion::parse(&[3, 7, 0xFF, 0xAB]).expect("Should parse when data has extra bytes");
        assert_eq!(version.major_version, 3);
        assert_eq!(version.minor_version, 7);
    }

    #[test]
    fn fails_on_short_data() {
        let error = AnalysisVersion::parse(&[1]).expect_err("short version");
        assert_eq!(error, AnalysisVersionError { length: 1 });
        assert_eq!(error.to_string(), "Data is too short to contain version information");
        assert!(AnalysisVersion::parse(&[]).is_err());
    }

    #[test]
    fn displays_version() {
        let version = AnalysisVersion::parse(&[1, 5]).expect("Should parse valid data");
        assert_eq!(version.to_string(), "Version 1.5");
    }
}
