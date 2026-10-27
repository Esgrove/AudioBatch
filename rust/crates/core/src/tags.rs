//! Display tags and Serato data of an audio file, read with lofty.
//!
//! The common fields come from lofty's format-independent tag.
//! Serato stores its data as `GEOB` frames, which only the concrete `ID3v2` tag keeps,
//! so MP3, AIFF, and WAV files are read a second time through their own file types for those frames.
//! FLAC files keep the same data as native `SERATO_*` Vorbis comments,
//! which are unwrapped into the frame payloads.

use std::borrow::Cow;
use std::fs::File;
use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, Result};
use lofty::config::ParseOptions;
use lofty::file::AudioFile;
use lofty::flac::FlacFile;
use lofty::id3::v2::{Frame, GeneralEncapsulatedObject, Id3v2Tag};
use lofty::iff::aiff::AiffFile;
use lofty::iff::wav::WavFile;
use lofty::mpeg::MpegFile;
use lofty::prelude::{Accessor, ItemKey, TaggedFileExt};
use serato_tags::native::{decode_native_value, native_field_description};
use serato_tags::{Overview, SeratoData, SeratoTag};

/// What reading the Serato Overview tag of a file found, the read results the tag cache keeps.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SeratoOverviewState {
    /// The file has a valid overview.
    Present(Arc<Overview>),
    /// The file can hold Serato tags but has no overview.
    Absent,
    /// Serato tags are not read from this file format.
    Unsupported,
    /// The file has an overview that could not be parsed.
    Invalid,
}

/// Tag fields shown in the GUI information panel.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct FileTags {
    /// Track title.
    pub title: Option<String>,
    /// Track artist.
    pub artist: Option<String>,
    /// Album title.
    pub album: Option<String>,
    /// Genre.
    pub genre: Option<String>,
    /// Release year.
    pub year: Option<u16>,
    /// Tempo as written in the tags.
    pub bpm: Option<String>,
    /// Musical key as written in the tags.
    pub key: Option<String>,
    /// Comment.
    pub comment: Option<String>,
    /// True when the file has embedded album art.
    pub has_picture: bool,
    /// Serato DJ data, when the file has any.
    pub serato: Option<SeratoData>,
}

impl SeratoOverviewState {
    /// Returns the overview state of a file from the tags read from it.
    /// A valid duplicate wins over an invalid one.
    #[must_use]
    pub fn from_tags(path: &Path, tags: &FileTags) -> Self {
        if !supports_serato_data(path) {
            return Self::Unsupported;
        }
        match &tags.serato {
            Some(SeratoData {
                overview: Some(overview),
                ..
            }) => Self::Present(Arc::new(overview.clone())),
            Some(serato) if serato.failed_to_parse(SeratoTag::Overview) => Self::Invalid,
            _ => Self::Absent,
        }
    }

    /// Returns the overview when the file has a valid one.
    #[must_use]
    pub const fn overview(&self) -> Option<&Arc<Overview>> {
        match self {
            Self::Present(overview) => Some(overview),
            Self::Absent | Self::Unsupported | Self::Invalid => None,
        }
    }
}

impl FileTags {
    /// Returns true when no field is set.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}

/// Reads the display tags and the Serato data of a file.
///
/// A file without tags returns empty tags.
///
/// # Errors
///
/// Returns an error when the file cannot be opened or parsed.
pub fn read_tags(path: &Path) -> Result<FileTags> {
    let tagged = lofty::read_from_path(path).with_context(|| format!("Failed to read tags of {}", path.display()))?;
    let mut tags = FileTags::default();
    if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
        tags.title = tag.title().map(Cow::into_owned);
        tags.artist = tag.artist().map(Cow::into_owned);
        tags.album = tag.album().map(Cow::into_owned);
        tags.genre = tag.genre().map(Cow::into_owned);
        // Vorbis comments may hold the comment as DESCRIPTION, as ffmpeg writes it, so that is the fallback.
        tags.comment = tag
            .comment()
            .map(Cow::into_owned)
            .or_else(|| tag.get_string(ItemKey::Description).map(str::to_string));
        tags.year = tag.date().map(|date| date.year);
        tags.bpm = tag
            .get_string(ItemKey::Bpm)
            .or_else(|| tag.get_string(ItemKey::IntegerBpm))
            .map(str::to_string);
        tags.key = tag.get_string(ItemKey::InitialKey).map(str::to_string);
        tags.has_picture = !tag.pictures().is_empty();
    }
    tags.serato = if lowercase_extension(path).as_deref() == Some("flac") {
        read_flac_serato_data(path)
    } else {
        read_id3v2(path).as_ref().and_then(serato_data)
    };
    Ok(tags)
}

/// Reads the concrete `ID3v2` tag of an MP3, AIFF, or WAV file.
pub(crate) fn read_id3v2(path: &Path) -> Option<Id3v2Tag> {
    let extension = lowercase_extension(path)?;
    let mut file = File::open(path).ok()?;
    let options = ParseOptions::new();
    match extension.as_str() {
        "mp3" => MpegFile::read_from(&mut file, options).ok()?.id3v2().cloned(),
        "aif" | "aiff" | "aifc" => AiffFile::read_from(&mut file, options).ok()?.id3v2().cloned(),
        "wav" | "wave" => WavFile::read_from(&mut file, options).ok()?.id3v2().cloned(),
        _ => None,
    }
}

/// Returns true for the formats Serato data is read from:
/// the `ID3v2` tag of MP3, AIFF, and WAV files, and the Vorbis comments of FLAC files.
fn supports_serato_data(path: &Path) -> bool {
    lowercase_extension(path).is_some_and(|extension| {
        matches!(
            extension.as_str(),
            "mp3" | "aif" | "aiff" | "aifc" | "wav" | "wave" | "flac"
        )
    })
}

/// Parses the Serato data among the native `SERATO_*` Vorbis comments of a FLAC file.
/// A value that is not a valid wrapper is passed on empty, so its tag reads as invalid rather than absent.
fn read_flac_serato_data(path: &Path) -> Option<SeratoData> {
    let mut file = File::open(path).ok()?;
    let flac = FlacFile::read_from(&mut file, ParseOptions::new()).ok()?;
    let objects: Vec<(&'static str, Vec<u8>)> = flac
        .vorbis_comments()?
        .items()
        .filter_map(|(key, value)| {
            let description = native_field_description(key)?;
            Some((description, decode_native_value(value, description).unwrap_or_default()))
        })
        .collect();
    SeratoData::parse(
        objects
            .iter()
            .map(|(description, data)| (*description, data.as_slice())),
    )
}

/// Returns the lowercase extension of a path.
fn lowercase_extension(path: &Path) -> Option<String> {
    Some(path.extension()?.to_str()?.to_ascii_lowercase())
}

/// Parses the Serato data among the `GEOB` frames of an `ID3v2` tag.
fn serato_data(tag: &Id3v2Tag) -> Option<SeratoData> {
    let objects: Vec<(String, Vec<u8>)> = tag
        .into_iter()
        .filter(|frame| frame.id_str() == "GEOB")
        .filter_map(|frame| match frame {
            Frame::Binary(binary) => GeneralEncapsulatedObject::parse(&binary.data, frame.flags()).ok(),
            _ => None,
        })
        .map(|object| (object.descriptor.unwrap_or_default(), object.data))
        .collect();
    SeratoData::parse(
        objects
            .iter()
            .map(|(description, data)| (description.as_str(), data.as_slice())),
    )
}

#[cfg(test)]
mod test_read_tags {
    use super::*;

    #[test]
    fn reads_tags_from_tagged_flac() {
        let tags = read_tags(&test_fixtures::fixture("tagged_stereo_44100_16.flac")).expect("reads");
        assert_eq!(tags.title.as_deref(), Some("Test Tone"));
        assert_eq!(tags.artist.as_deref(), Some("AudioBatch"));
        assert_eq!(tags.album.as_deref(), Some("Fixtures"));
        assert_eq!(tags.comment.as_deref(), Some("Custom comment"));
        assert_eq!(tags.serato, None);
    }

    #[test]
    fn untagged_file_has_empty_tags() {
        let tags = read_tags(&test_fixtures::fixture("sine_stereo_44100_16.wav")).expect("reads");
        assert!(tags.is_empty(), "{tags:?}");
    }

    #[test]
    fn mp3_without_serato_frames_has_no_serato_data() {
        let tags = read_tags(&test_fixtures::fixture("sine_stereo_44100_320k.mp3")).expect("reads");
        assert_eq!(tags.serato, None);
    }

    #[test]
    fn broken_file_is_an_error() {
        assert!(read_tags(&test_fixtures::fixture("broken.wav")).is_err());
    }
}

#[cfg(test)]
mod test_serato_overview_state {
    use super::*;

    use lofty::config::WriteOptions;
    use lofty::ogg::tag::VorbisComments;
    use lofty::prelude::TagExt;
    use serato_tags::colouring::{OverviewColour, column_colours};
    use tempfile::TempDir;

    /// Returns tags whose Serato data comes from the given GEOB frames.
    fn tags_with_frames(frames: &[(&str, &[u8])]) -> FileTags {
        FileTags {
            serato: SeratoData::parse(frames.iter().copied()),
            ..FileTags::default()
        }
    }

    #[test]
    fn formats_without_serato_data_are_unsupported() {
        let tags = tags_with_frames(&[(
            "Serato Overview",
            &[1, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2],
        )]);
        assert_eq!(
            SeratoOverviewState::from_tags(Path::new("song.ogg"), &tags),
            SeratoOverviewState::Unsupported
        );
        assert_eq!(
            SeratoOverviewState::from_tags(Path::new("song"), &FileTags::default()),
            SeratoOverviewState::Unsupported
        );
    }

    #[test]
    fn valid_overview_is_present() {
        let tags = tags_with_frames(&[(
            "Serato Overview",
            &[1, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2],
        )]);
        let state = SeratoOverviewState::from_tags(Path::new("song.MP3"), &tags);
        assert_eq!(state.overview().map(|overview| overview.slices().len()), Some(1));
    }

    #[test]
    fn rainbow_fixtures_have_six_colour_bands_in_every_container() {
        let band_count = test_fixtures::SERATO_OVERVIEW_BANDS.len();
        let expected: Vec<Option<u32>> = test_fixtures::SERATO_OVERVIEW_BANDS.into_iter().map(Some).collect();
        for name in test_fixtures::SERATO_OVERVIEW_FIXTURES {
            let path = test_fixtures::fixture(name);
            let tags = read_tags(&path).expect("reads");
            let state = SeratoOverviewState::from_tags(&path, &tags);
            let overview = state
                .overview()
                .unwrap_or_else(|| panic!("{name} has no overview: {state:?}"));
            assert_eq!(overview.slices().len(), test_fixtures::SERATO_OVERVIEW_SLICES, "{name}");
            let colours: Vec<Option<u32>> = column_colours(overview, 0.0, 1.0, band_count)
                .into_iter()
                .map(|colour| colour.map(OverviewColour::to_rgb_u32))
                .collect();
            assert_eq!(colours, expected, "{name}");
        }
    }

    #[test]
    fn malformed_flac_comment_is_invalid() {
        let directory = TempDir::new().expect("temp dir");
        let path = test_fixtures::copy_fixture(&directory, "sine_stereo_44100_16.flac", None);
        let mut comments = VorbisComments::default();
        comments.insert("SERATO_OVERVIEW".to_string(), "!invalid!".to_string());
        comments
            .save_to_path(&path, WriteOptions::default())
            .expect("writes the comment");
        let tags = read_tags(&path).expect("reads");
        assert_eq!(
            SeratoOverviewState::from_tags(&path, &tags),
            SeratoOverviewState::Invalid
        );
    }

    #[test]
    fn valid_duplicate_wins_over_an_invalid_one() {
        let tags = tags_with_frames(&[
            ("Serato Overview", &[1, 5, 0]),
            (
                "Serato Overview",
                &[1, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2],
            ),
        ]);
        let state = SeratoOverviewState::from_tags(Path::new("song.flac"), &tags);
        assert_eq!(state.overview().map(|overview| overview.slices().len()), Some(1));
    }

    #[test]
    fn missing_and_broken_overviews_are_absent_and_invalid() {
        let path = Path::new("song.aiff");
        assert_eq!(
            SeratoOverviewState::from_tags(path, &FileTags::default()),
            SeratoOverviewState::Absent
        );
        let other_tag = tags_with_frames(&[("Serato Analysis", &[2, 1])]);
        assert_eq!(
            SeratoOverviewState::from_tags(path, &other_tag),
            SeratoOverviewState::Absent
        );
        let broken = tags_with_frames(&[("Serato Overview", &[1, 5, 0])]);
        assert_eq!(
            SeratoOverviewState::from_tags(path, &broken),
            SeratoOverviewState::Invalid
        );
        assert_eq!(SeratoOverviewState::Invalid.overview(), None);
    }
}
