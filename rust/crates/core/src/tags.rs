//! Display tags and Serato data of an audio file, read with lofty.
//!
//! The common fields come from lofty's format-independent tag.
//! Serato stores its data as `GEOB` frames, which only the concrete `ID3v2` tag keeps,
//! so MP3, AIFF, and WAV files are read a second time through their own file types for those frames.

use std::borrow::Cow;
use std::fs::File;
use std::path::Path;

use anyhow::{Context, Result};
use lofty::config::ParseOptions;
use lofty::file::AudioFile;
use lofty::id3::v2::{Frame, GeneralEncapsulatedObject, Id3v2Tag};
use lofty::iff::aiff::AiffFile;
use lofty::iff::wav::WavFile;
use lofty::mpeg::MpegFile;
use lofty::prelude::{Accessor, ItemKey, TaggedFileExt};
use serato_tags::SeratoData;

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
    tags.serato = read_id3v2(path).as_ref().and_then(serato_data);
    Ok(tags)
}

/// Reads the concrete `ID3v2` tag of an MP3, AIFF, or WAV file.
pub(crate) fn read_id3v2(path: &Path) -> Option<Id3v2Tag> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    let mut file = File::open(path).ok()?;
    let options = ParseOptions::new();
    match extension.as_str() {
        "mp3" => MpegFile::read_from(&mut file, options).ok()?.id3v2().cloned(),
        "aif" | "aiff" | "aifc" => AiffFile::read_from(&mut file, options).ok()?.id3v2().cloned(),
        "wav" | "wave" => WavFile::read_from(&mut file, options).ok()?.id3v2().cloned(),
        _ => None,
    }
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
