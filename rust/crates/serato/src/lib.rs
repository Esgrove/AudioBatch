//! Parsers for the Serato DJ tags that Serato stores as GEOB frames in audio file tags.
//!
//! Extracted from the track-rename project.
//! The parsers take the raw frame bytes, so they work with any tag library,
//! and they do no terminal printing, so any front end can present the data.
//! `SeratoData::parse` takes `(description, data)` pairs of the GEOB frames
//! and collects the parse errors instead of printing them.

mod analysis;
mod autotags;
mod beatgrid;
mod markers;
mod overview;

use std::fmt;
use std::fmt::Display;
use std::str::FromStr;

use anyhow::anyhow;

pub use analysis::AnalysisVersion;
pub use autotags::AutoTags;
pub use beatgrid::{BeatGrid, BeatGridMarker};
pub use markers::{BpmLock, Color, Cue, Loop, Markers};
pub use overview::{BAND_COUNT, Overview};

/// Serato tag types, identified by the GEOB frame description.
#[derive(Debug, Clone, Copy, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub enum SeratoTag {
    /// Serato analysis version number.
    Analysis,
    /// BPM, auto gain, and manual gain values.
    Autotags,
    /// Beatgrid markers.
    BeatGrid,
    /// Cue points, loops, track color, and BPM lock status.
    Markers,
    /// Waveform overview data.
    Overview,
}

/// All Serato tag data of a file.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SeratoData {
    /// Parsed analysis version tag.
    pub analysis: Option<AnalysisVersion>,
    /// Parsed autotags data.
    pub autotags: Option<AutoTags>,
    /// Parsed beatgrid data.
    pub beatgrid: Option<BeatGrid>,
    /// Parsed cue, loop, color, and BPM lock markers.
    pub markers: Vec<Markers>,
    /// Parsed waveform overview data.
    pub overview: Option<Overview>,
    /// Errors from tags that could not be parsed, one message per tag.
    pub errors: Vec<String>,
}

impl SeratoTag {
    /// Returns the GEOB frame description of the tag.
    #[must_use]
    pub const fn description(self) -> &'static str {
        match self {
            Self::Analysis => "Serato Analysis",
            Self::Autotags => "Serato Autotags",
            Self::BeatGrid => "Serato BeatGrid",
            Self::Markers => "Serato Markers2",
            Self::Overview => "Serato Overview",
        }
    }
}

impl FromStr for SeratoTag {
    type Err = anyhow::Error;

    fn from_str(description: &str) -> Result<Self, Self::Err> {
        match description {
            "Serato Analysis" => Ok(Self::Analysis),
            "Serato Autotags" => Ok(Self::Autotags),
            "Serato BeatGrid" => Ok(Self::BeatGrid),
            "Serato Markers2" => Ok(Self::Markers),
            "Serato Overview" => Ok(Self::Overview),
            _ => Err(anyhow!("Unknown tag description: {description}")),
        }
    }
}

impl Display for SeratoTag {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{}",
            match self {
                Self::Analysis => "SeratoAnalysis",
                Self::Autotags => "SeratoAutotags",
                Self::BeatGrid => "SeratoBeatGrid",
                Self::Markers => "SeratoMarkers",
                Self::Overview => "SeratoOverview",
            }
        )
    }
}

impl SeratoData {
    /// Parses the Serato tags among the given GEOB frames, as `(description, data)` pairs.
    ///
    /// Frames with other descriptions are ignored.
    /// Returns `None` when no Serato tag was found, valid or not.
    #[must_use]
    pub fn parse<'a>(frames: impl IntoIterator<Item = (&'a str, &'a [u8])>) -> Option<Self> {
        let mut data = Self::default();
        let mut found_any = false;
        for (description, bytes) in frames {
            let Ok(tag) = SeratoTag::from_str(description) else {
                continue;
            };
            found_any = true;
            let result = match tag {
                SeratoTag::Analysis => AnalysisVersion::parse(bytes).map(|value| data.analysis = Some(value)),
                SeratoTag::Autotags => AutoTags::parse(bytes).map(|value| data.autotags = Some(value)),
                SeratoTag::BeatGrid => BeatGrid::parse(bytes).map(|value| data.beatgrid = Some(value)),
                SeratoTag::Markers => Markers::parse(bytes).map(|value| data.markers = value),
                SeratoTag::Overview => Overview::parse(bytes).map(|value| data.overview = Some(value)),
            };
            if let Err(error) = result {
                data.errors.push(format!("{tag}: {error}"));
            }
        }
        found_any.then_some(data)
    }

    /// Returns the BPM from the autotags, or from the beatgrid when the autotags have none.
    #[must_use]
    pub fn bpm(&self) -> Option<f32> {
        self.autotags
            .as_ref()
            .map(|autotags| autotags.bpm)
            .filter(|&bpm| bpm > 0.0)
            .or_else(|| self.beatgrid.as_ref().and_then(BeatGrid::bpm))
    }

    /// Returns the cue points.
    pub fn cues(&self) -> impl Iterator<Item = &Cue> {
        self.markers.iter().filter_map(|marker| match marker {
            Markers::Cue(cue) => Some(cue),
            _ => None,
        })
    }

    /// Returns the track color.
    #[must_use]
    pub fn track_color(&self) -> Option<Color> {
        self.markers.iter().find_map(|marker| match marker {
            Markers::Color(color) => Some(*color),
            _ => None,
        })
    }
}

impl Display for SeratoData {
    fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        writeln!(formatter, "Serato tags:")?;
        write_optional(formatter, SeratoTag::Autotags, self.autotags.as_ref())?;
        write_optional(formatter, SeratoTag::Analysis, self.analysis.as_ref())?;
        write_optional(formatter, SeratoTag::BeatGrid, self.beatgrid.as_ref())?;
        write_optional(formatter, SeratoTag::Overview, self.overview.as_ref())?;
        if self.markers.is_empty() {
            writeln!(formatter, "{}: None", SeratoTag::Markers)?;
        } else {
            writeln!(formatter, "{}:", SeratoTag::Markers)?;
            for marker in &self.markers {
                writeln!(formatter, "  {marker}")?;
            }
        }
        for error in &self.errors {
            writeln!(formatter, "Error: {error}")?;
        }
        Ok(())
    }
}

/// Formats a position in milliseconds as `MM:SS.T`, the way Serato shows it.
#[must_use]
pub fn format_position_timestamp(position_in_ms: u32) -> String {
    let minutes = position_in_ms / 60_000;
    let seconds = (position_in_ms % 60_000) / 1000;
    let tenths = (f64::from(position_in_ms % 1000) / 100.0).round();
    format!("{minutes:02}:{seconds:02}.{tenths}")
}

/// Writes one tag line, with "None" when the tag is missing.
fn write_optional(formatter: &mut fmt::Formatter, tag: SeratoTag, value: Option<&impl Display>) -> fmt::Result {
    match value {
        Some(value) => writeln!(formatter, "{tag}: {value}"),
        None => writeln!(formatter, "{tag}: None"),
    }
}

#[cfg(test)]
mod test_serato_tag {
    use super::*;

    #[test]
    fn parses_known_descriptions() {
        for tag in [
            SeratoTag::Analysis,
            SeratoTag::Autotags,
            SeratoTag::BeatGrid,
            SeratoTag::Markers,
            SeratoTag::Overview,
        ] {
            assert_eq!(SeratoTag::from_str(tag.description()).expect("parses"), tag);
        }
    }

    #[test]
    fn rejects_unknown_descriptions() {
        assert!(SeratoTag::from_str("Unknown Tag").is_err());
        assert!(SeratoTag::from_str("").is_err());
    }

    #[test]
    fn displays_short_names() {
        assert_eq!(SeratoTag::Analysis.to_string(), "SeratoAnalysis");
        assert_eq!(SeratoTag::Markers.to_string(), "SeratoMarkers");
    }
}

#[cfg(test)]
mod test_serato_data {
    use super::*;

    const AUTOTAGS: &[u8] = b"\x01\x01128.00\0-3.257\x000.000\0";

    #[test]
    fn parses_known_frames_and_ignores_others() {
        let frames = [
            ("Serato Analysis", &[2_u8, 1][..]),
            ("Serato Autotags", AUTOTAGS),
            ("Other", &[0xFF][..]),
        ];
        let data = SeratoData::parse(frames).expect("found serato tags");
        assert_eq!(
            data.analysis,
            Some(AnalysisVersion {
                major_version: 2,
                minor_version: 1
            })
        );
        assert_eq!(data.bpm(), Some(128.0));
        assert_eq!(data.errors, Vec::<String>::new());
    }

    #[test]
    fn returns_none_without_serato_frames() {
        assert_eq!(SeratoData::parse([("Other", &[0_u8][..])]), None);
    }

    #[test]
    fn collects_errors_instead_of_printing() {
        let data = SeratoData::parse([("Serato Analysis", &[2_u8][..])]).expect("found serato tags");
        assert_eq!(data.analysis, None);
        assert_eq!(data.errors.len(), 1);
        assert!(data.to_string().contains("Error: SeratoAnalysis"));
    }

    #[test]
    fn display_lists_every_section() {
        let output = SeratoData::default().to_string();
        for section in [
            "Serato tags:",
            "SeratoAutotags: None",
            "SeratoMarkers: None",
            "SeratoOverview: None",
        ] {
            assert!(output.contains(section), "missing {section} in {output}");
        }
    }

    #[test]
    fn formats_positions_like_serato() {
        assert_eq!(format_position_timestamp(0), "00:00.0");
        assert_eq!(format_position_timestamp(61_500), "01:01.5");
        assert_eq!(format_position_timestamp(600_000), "10:00.0");
    }
}
