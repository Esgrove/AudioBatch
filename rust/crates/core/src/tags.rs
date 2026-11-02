//! Display tags, Serato data, and independent single-cover artwork reads through lofty.
//!
//! The common fields come from lofty's format-independent tag.
//! Serato stores its data as `GEOB` frames, which only the concrete `ID3v2` tag keeps,
//! so MP3, AIFF, and WAV files are read a second time through their own file types for those frames.
//! FLAC files keep the same data as native `SERATO_*` Vorbis comments,
//! which are unwrapped into the frame payloads.

use std::borrow::Cow;
use std::fs::File;
use std::io::{self, Cursor, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use lofty::config::ParseOptions;
use lofty::file::{AudioFile, TaggedFile};
use lofty::flac::FlacFile;
use lofty::id3::v2::util::synchsafe::UnsynchronizedStream;
use lofty::id3::v2::{Frame, GeneralEncapsulatedObject, Id3v2Tag};
use lofty::iff::aiff::AiffFile;
use lofty::iff::wav::WavFile;
use lofty::mpeg::MpegFile;
use lofty::picture::PictureType;
use lofty::prelude::{Accessor, ItemKey, TaggedFileExt};
use lofty::probe::Probe;
use serato_tags::native::{decode_native_value, native_field_description};
use serato_tags::{Overview, SeratoData, SeratoTag};

/// Largest encoded embedded picture passed to the thumbnail decoder, 16 MiB.
pub const ARTWORK_MAXIMUM_BYTES: usize = 16 * 1024 * 1024;

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

/// A failure to open or parse the display tags of an audio file.
#[derive(Debug, thiserror::Error)]
#[error("Failed to read tags of {}", path.display())]
pub struct TagsError {
    /// File whose tags could not be read.
    pub path: PathBuf,
    /// Tag reader's concrete parse or I/O failure.
    #[source]
    pub source: lofty::error::FileParseError,
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
    /// True when the reader found embedded album art.
    /// A false value from `read_display_tags` does not establish absence because that reader skips pictures.
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
pub fn read_tags(path: &Path) -> Result<FileTags, TagsError> {
    read_tags_with_options(path, ParseOptions::new())
}

/// Reads the display fields and Serato cues without audio properties or embedded pictures.
///
/// `has_picture` is always false because pictures are skipped, not because their absence was established.
/// Use the independent artwork reader rather than this flag to decide whether artwork is available.
/// Use `read_tags` when the picture presence flag is required.
///
/// # Errors
///
/// Returns an error when the file cannot be opened or parsed.
pub fn read_display_tags(path: &Path) -> Result<FileTags, TagsError> {
    read_tags_with_options(path, display_parse_options())
}

/// Reads one encoded embedded picture, preferring the front cover and otherwise the first available picture.
///
/// Lofty reads the tags without audio properties, then only the selected picture is retained, without copying its bytes.
/// Empty pictures and those above `ARTWORK_MAXIMUM_BYTES` are skipped.
/// A file that cannot be read or has no suitable picture returns none, since missing artwork is never an error.
#[must_use]
pub fn read_artwork(path: &Path) -> Option<Vec<u8>> {
    let _span = tracing::info_span!("read_artwork", file = %path.display()).entered();
    let tagged = Probe::open(path)
        .ok()?
        .options(ParseOptions::new().read_properties(false))
        .read()
        .ok()?;
    select_artwork(tagged)
}

/// Reads the concrete `ID3v2` tag of an MP3, AIFF, or WAV file.
pub(crate) fn read_id3v2(path: &Path) -> Option<Id3v2Tag> {
    let _span = tracing::info_span!("read_id3v2", file = %path.display()).entered();
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

/// Returns the parsing options for text and Serato data, leaving properties and cover bytes unread.
fn display_parse_options() -> ParseOptions {
    ParseOptions::new().read_properties(false).read_cover_art(false)
}

/// Reads display fields using the requested picture policy, and Serato data without cover bytes.
fn read_tags_with_options(path: &Path, options: ParseOptions) -> Result<FileTags, TagsError> {
    let _span = tracing::info_span!("read_tags", file = %path.display()).entered();
    let tagged = Probe::open(path)
        .and_then(|probe| probe.options(options).read())
        .map_err(|source| TagsError {
            path: path.to_path_buf(),
            source,
        })?;
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
    tags.serato = read_serato_data(path).map_err(|source| TagsError {
        path: path.to_path_buf(),
        source,
    })?;
    Ok(tags)
}

/// Reads a concrete `ID3v2` tag without suppressing a second-open or container parsing failure.
fn read_display_id3v2(path: &Path) -> Result<Option<Id3v2Tag>, lofty::error::FileParseError> {
    let mut file = File::open(path)?;
    let options = display_parse_options();
    Ok(match lowercase_extension(path).as_deref() {
        Some("mp3") => MpegFile::read_from(&mut file, options)?.id3v2().cloned(),
        Some("aif" | "aiff" | "aifc") => AiffFile::read_from(&mut file, options)?.id3v2().cloned(),
        Some("wav" | "wave") => WavFile::read_from(&mut file, options)?.id3v2().cloned(),
        _ => None,
    })
}

/// Reads Serato values independently of generic-tag conversion and preserves physical GEOB duplicate order.
fn read_serato_data(path: &Path) -> Result<Option<SeratoData>, lofty::error::FileParseError> {
    if !supports_serato_data(path) {
        return Ok(None);
    }
    if lowercase_extension(path).as_deref() == Some("flac") {
        return read_flac_serato_data(path);
    }
    let Some(tag) = read_display_id3v2(path)? else {
        return Ok(None);
    };
    // A single retained GEOB has no ordering ambiguity, including byte-identical repeated copies.
    if (&tag)
        .into_iter()
        .filter(|frame| frame.id_str() == "GEOB")
        .take(2)
        .count()
        < 2
    {
        return Ok(serato_data(&tag));
    }
    // Lofty's FrameList replaces byte-identical binary frames and moves the surviving copy to the end.
    // Recover source order before first-valid selection, while retaining Lofty's frame decoding.
    let objects = read_ordered_geob(path)?;
    if objects.is_empty() {
        return Err(invalid_id3("Cannot locate the source GEOB frames").into());
    }
    Ok(parse_serato_objects(&objects))
}

/// Parses retained GEOB objects when no duplicate-order recovery is required.
fn serato_data(tag: &Id3v2Tag) -> Option<SeratoData> {
    let objects: Vec<_> = tag
        .into_iter()
        .filter_map(|frame| {
            if frame.id_str() != "GEOB" {
                return None;
            }
            let Frame::Binary(binary) = frame else {
                return None;
            };
            let object = GeneralEncapsulatedObject::parse(&binary.data, frame.flags()).ok()?;
            Some((object.descriptor.unwrap_or_default(), object.data))
        })
        .collect();
    parse_serato_objects(&objects)
}

/// Selects the first valid object of each kind from a source-ordered object list.
fn parse_serato_objects(objects: &[(String, Vec<u8>)]) -> Option<SeratoData> {
    SeratoData::parse(
        objects
            .iter()
            .map(|(description, bytes)| (description.as_str(), bytes.as_slice())),
    )
}

/// Creates an extraction failure, which must not become a stable absent tag result.
fn invalid_id3(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

/// Decodes a checked four-byte syncsafe ID3 size.
fn syncsafe_size(bytes: [u8; 4]) -> io::Result<usize> {
    if bytes.iter().any(|byte| byte & 0x80 != 0) {
        return Err(invalid_id3("Invalid syncsafe ID3 size"));
    }
    Ok(bytes.into_iter().fold(0, |size, byte| (size << 7) | usize::from(byte)))
}

/// Locates the first ID3 chunk without reading PCM, rejecting truncated container chunks.
fn read_ordered_geob(path: &Path) -> Result<Vec<(String, Vec<u8>)>, lofty::error::FileParseError> {
    let mut file = File::open(path)?;
    let length = file.metadata()?.len();
    if lowercase_extension(path).as_deref() == Some("mp3") {
        return read_geob_tag(&mut file, length);
    }
    let mut header = [0; 12];
    file.read_exact(&mut header)?;
    let little_endian = &header[..4] == b"RIFF";
    if !(little_endian && &header[8..] == b"WAVE"
        || &header[..4] == b"FORM" && matches!(&header[8..], b"AIFF" | b"AIFC"))
    {
        return Err(invalid_id3("Invalid ID3 audio container").into());
    }
    while file.stream_position()?.checked_add(8).is_some_and(|end| end <= length) {
        let mut chunk = [0; 8];
        file.read_exact(&mut chunk)?;
        let size = chunk[4..].try_into().expect("four-byte chunk size");
        let size = u64::from(if little_endian {
            u32::from_le_bytes(size)
        } else {
            u32::from_be_bytes(size)
        });
        let start = file.stream_position()?;
        let end = start
            .checked_add(size)
            .filter(|end| *end <= length)
            .ok_or_else(|| invalid_id3("Truncated audio container chunk"))?;
        if chunk[..4].eq_ignore_ascii_case(b"ID3 ") {
            return read_geob_tag(&mut file, size);
        }
        file.seek(SeekFrom::Start(end + size % 2))?;
    }
    Ok(Vec::new())
}

/// Reads bounded ID3 framing in source order, leaving GEOB decoding and flag handling to Lofty.
fn read_geob_tag(file: &mut File, available: u64) -> Result<Vec<(String, Vec<u8>)>, lofty::error::FileParseError> {
    let mut header = [0; 10];
    if available < 10 {
        return Ok(Vec::new());
    }
    file.read_exact(&mut header)?;
    if &header[..3] != b"ID3" {
        return Ok(Vec::new());
    }
    let version = header[3];
    if !matches!(version, 2..=4) {
        return Err(invalid_id3("Unsupported ID3 version").into());
    }
    let length = syncsafe_size(header[6..].try_into().expect("four-byte ID3 size"))?;
    // Bounds the temporary tag body, including unrelated frames, before allocating it.
    if length > 64 * 1024 * 1024 || length as u64 > available - 10 {
        return Err(invalid_id3("Oversized or truncated ID3 tag").into());
    }
    let mut body = vec![0; length];
    file.read_exact(&mut body)?;
    if version < 4 && header[5] & 0x80 != 0 {
        let mut decoded = Vec::with_capacity(body.len());
        UnsynchronizedStream::new(body.as_slice()).read_to_end(&mut decoded)?;
        body = decoded;
        header[5] &= !0x80;
    }
    let mut remaining = body.as_slice();
    if version >= 3 && header[5] & 0x40 != 0 {
        let size: [u8; 4] = remaining
            .get(..4)
            .ok_or_else(|| invalid_id3("Truncated extended ID3 header"))?
            .try_into()
            .expect("four-byte extended header size");
        let skip = if version == 4 {
            syncsafe_size(size)?
        } else {
            u32::from_be_bytes(size) as usize + 4
        };
        remaining = remaining
            .get(skip..)
            .ok_or_else(|| invalid_id3("Truncated extended ID3 header"))?;
    }
    let mut objects = Vec::new();
    let frame_header_length = if version == 2 { 6 } else { 10 };
    while remaining.first().is_some_and(|byte| *byte != 0) {
        let frame_header = remaining
            .get(..frame_header_length)
            .ok_or_else(|| invalid_id3("Truncated ID3 frame header"))?;
        let length = match version {
            2 => frame_header
                .get(3..6)
                .expect("validated six-byte frame header")
                .iter()
                .fold(0, |size, byte| (size << 8) | usize::from(*byte)),
            3 => u32::from_be_bytes(
                frame_header
                    .get(4..8)
                    .expect("validated ten-byte frame header")
                    .try_into()
                    .expect("four-byte frame size"),
            ) as usize,
            _ => syncsafe_size(
                frame_header
                    .get(4..8)
                    .expect("validated ten-byte frame header")
                    .try_into()
                    .expect("four-byte frame size"),
            )?,
        };
        let end = frame_header_length + length;
        let frame = remaining.get(..end).ok_or_else(|| invalid_id3("Truncated ID3 frame"))?;
        if (version == 2 && frame.starts_with(b"GEO")) || (version != 2 && frame.starts_with(b"GEOB")) {
            if length > 2 + serato_tags::MAXIMUM_SLICES * serato_tags::BAND_COUNT + 1024 {
                return Err(invalid_id3("Oversized GEOB frame").into());
            }
            objects.extend(decode_geob_frame(header, frame)?);
        }
        remaining = remaining.get(end..).expect("validated complete frame");
    }
    Ok(objects)
}

/// Decodes one source frame in an isolated ID3 chunk so Lofty's deduplication cannot change source order.
fn decode_geob_frame(
    mut header: [u8; 10],
    frame: &[u8],
) -> Result<Vec<(String, Vec<u8>)>, lofty::error::FileParseError> {
    header[5] &= 0x80;
    let length = u32::try_from(frame.len()).map_err(|_| invalid_id3("Oversized GEOB frame"))?;
    header[6..].copy_from_slice(&[
        ((length >> 21) & 0x7f) as u8,
        ((length >> 14) & 0x7f) as u8,
        ((length >> 7) & 0x7f) as u8,
        (length & 0x7f) as u8,
    ]);
    let chunk_length = 10 + length;
    let mut bytes = b"RIFF".to_vec();
    bytes.extend_from_slice(&(12 + chunk_length + chunk_length % 2).to_le_bytes());
    bytes.extend_from_slice(b"WAVEID3 ");
    bytes.extend_from_slice(&chunk_length.to_le_bytes());
    bytes.extend_from_slice(&header);
    bytes.extend_from_slice(frame);
    if chunk_length % 2 != 0 {
        bytes.push(0);
    }
    let wav = WavFile::read_from(&mut Cursor::new(bytes), display_parse_options())?;
    Ok(wav
        .id3v2()
        .into_iter()
        .flat_map(IntoIterator::into_iter)
        .filter_map(|frame| {
            let Frame::Binary(binary) = frame else {
                return None;
            };
            let object = GeneralEncapsulatedObject::parse(&binary.data, frame.flags()).ok()?;
            Some((object.descriptor.unwrap_or_default(), object.data))
        })
        .collect())
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
fn read_flac_serato_data(path: &Path) -> Result<Option<SeratoData>, lofty::error::FileParseError> {
    let mut file = File::open(path)?;
    let flac = FlacFile::read_from(&mut file, display_parse_options())?;
    let Some(comments) = flac.vorbis_comments() else {
        return Ok(None);
    };
    let objects: Vec<(&'static str, Vec<u8>)> = comments
        .items()
        .filter_map(|(key, value)| {
            let description = native_field_description(key)?;
            Some((description, decode_native_value(value, description).unwrap_or_default()))
        })
        .collect();
    Ok(SeratoData::parse(
        objects
            .iter()
            .map(|(description, data)| (*description, data.as_slice())),
    ))
}

/// Moves out one suitable front cover or fallback, searching the primary tag first within each preference group.
fn select_artwork(mut tagged: TaggedFile) -> Option<Vec<u8>> {
    let primary = tagged.primary_tag().map(lofty::tag::Tag::tag_type);
    let pictures = || {
        primary
            .and_then(|tag_type| tagged.tag(tag_type))
            .into_iter()
            .chain(tagged.tags().iter().filter(|tag| Some(tag.tag_type()) != primary))
            .flat_map(|tag| {
                tag.pictures()
                    .iter()
                    .enumerate()
                    .map(move |(index, picture)| (tag.tag_type(), index, picture))
            })
            .filter(|(_, _, picture)| !picture.data().is_empty() && picture.data().len() <= ARTWORK_MAXIMUM_BYTES)
    };
    let (tag_type, index, _) = pictures()
        .find(|(_, _, picture)| picture.pic_type() == PictureType::CoverFront)
        .or_else(|| pictures().next())?;
    Some(tagged.remove(tag_type)?.remove_picture(index).into_data())
}

/// Returns the lowercase extension of a path.
fn lowercase_extension(path: &Path) -> Option<String> {
    Some(path.extension()?.to_str()?.to_ascii_lowercase())
}

#[cfg(test)]
mod test_read_tags {
    use super::*;

    #[test]
    fn read_failure_keeps_the_path_and_parse_source() {
        let directory = tempfile::tempdir().expect("temp dir");
        let path = directory.path().join("missing.wav");
        let error = read_tags(&path).expect_err("missing file");
        assert_eq!(error.path, path);
        assert_eq!(error.to_string(), format!("Failed to read tags of {}", path.display()));
        assert!(std::error::Error::source(&error).is_some());
    }

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
mod test_artwork {
    use lofty::config::WriteOptions;
    use lofty::file::FileType;
    use lofty::picture::{MimeType, Picture};
    use lofty::prelude::TagExt as _;
    use lofty::tag::{Tag, TagType};

    use super::*;

    /// Returns a picture of a type with placeholder data, which artwork selection never decodes.
    fn picture(pic_type: PictureType, data: Vec<u8>) -> Picture {
        Picture::unchecked(data)
            .pic_type(pic_type)
            .mime_type(MimeType::Png)
            .build()
    }

    #[test]
    fn artwork_and_display_options_skip_audio_properties() {
        assert_eq!(
            display_parse_options(),
            ParseOptions::new().read_properties(false).read_cover_art(false)
        );
        let tagged = Probe::open(test_fixtures::fixture("tagged_stereo_44100_16.flac"))
            .expect("opens fixture")
            .options(ParseOptions::new().read_properties(false))
            .read()
            .expect("reads tags");
        assert!(tagged.properties().duration().is_zero());
        assert!(tagged.primary_tag().is_some());
    }

    #[test]
    fn display_reads_skip_pictures_without_changing_full_tag_contract() {
        let directory = tempfile::tempdir().expect("temp dir");
        for (name, tag_type) in [
            ("tagged_stereo_44100_16.flac", TagType::VorbisComments),
            ("sine_stereo_44100_320k.mp3", TagType::Id3v2),
            ("sine_stereo_44100_16.wav", TagType::Id3v2),
            ("serato_rainbow_stereo_44100_16.aif", TagType::Id3v2),
            ("sine_stereo_48000.ogg", TagType::VorbisComments),
            ("sine_stereo_44100_aac.m4a", TagType::Mp4Ilst),
        ] {
            let path = test_fixtures::copy_fixture(&directory, name, None);
            let mut tag = Tag::new(tag_type);
            tag.set_title("Artwork test title".to_string());
            tag.push_picture(picture(PictureType::CoverFront, vec![1, 2, 3]));
            tag.save_to_path(&path, WriteOptions::default()).expect("writes tags");
            let full = read_tags(&path).expect("reads full tags");
            let display = read_display_tags(&path).expect("reads display tags");
            assert!(full.has_picture, "{name}");
            assert!(
                !display.has_picture,
                "display reader deliberately skips pictures in {name}"
            );
            assert_eq!(display.title.as_deref(), Some("Artwork test title"));
            assert_eq!(display.serato, full.serato);
            assert_eq!(read_artwork(&path), Some(vec![1, 2, 3]));
            if tag_type == TagType::Id3v2 {
                let copy_tag = read_id3v2(&path).expect("reads metadata-copy tag");
                assert!(copy_tag.into_iter().any(|frame| frame.id_str() == "APIC"), "{name}");
                let display_tag = read_display_id3v2(&path).expect("reads display tag").expect("ID3 tag");
                assert!(!display_tag.into_iter().any(|frame| frame.id_str() == "APIC"), "{name}");
            }
        }
    }

    #[test]
    fn display_reader_keeps_serato_data_without_cover_parsing() {
        for name in test_fixtures::SERATO_OVERVIEW_FIXTURES {
            let path = test_fixtures::fixture(name);
            let full = read_tags(&path).expect("reads full tags");
            let display = read_display_tags(&path).expect("reads display tags");
            assert!(display.serato.is_some(), "{name}");
            assert_eq!(display.serato, full.serato, "{name}");
        }
    }

    /// Builds a tagged FLAC in memory to test picture selection independently of container restrictions.
    fn flac_with_pictures(pictures: impl IntoIterator<Item = Picture>) -> TaggedFile {
        let mut tag = Tag::new(TagType::VorbisComments);
        for picture in pictures {
            tag.push_picture(picture);
        }
        TaggedFile::new(FileType::Flac, lofty::properties::FileProperties::default(), vec![tag])
    }

    #[test]
    fn returns_only_the_first_front_cover_even_after_many_other_pictures() {
        let mut pictures: Vec<_> = (0..10).map(|_| picture(PictureType::CoverBack, vec![1])).collect();
        pictures.push(picture(PictureType::CoverFront, vec![2]));
        pictures.push(picture(PictureType::CoverFront, vec![3]));
        assert_eq!(select_artwork(flac_with_pictures(pictures)), Some(vec![2]));
    }

    #[test]
    fn missing_front_cover_falls_back_to_the_first_picture() {
        let pictures = [
            picture(PictureType::Artist, vec![4]),
            picture(PictureType::CoverBack, vec![5]),
        ];
        assert_eq!(select_artwork(flac_with_pictures(pictures)), Some(vec![4]));
        assert!(select_artwork(flac_with_pictures([])).is_none());
    }

    #[test]
    fn empty_and_oversized_pictures_are_skipped() {
        let pictures = [
            picture(PictureType::CoverFront, Vec::new()),
            picture(PictureType::CoverFront, vec![0; ARTWORK_MAXIMUM_BYTES + 1]),
            picture(PictureType::CoverBack, vec![5]),
        ];
        assert_eq!(select_artwork(flac_with_pictures(pictures)), Some(vec![5]));
    }

    #[test]
    fn front_cover_in_a_secondary_tag_beats_primary_tag_fallback() {
        let mut primary = Tag::new(TagType::VorbisComments);
        primary.push_picture(picture(PictureType::CoverBack, vec![1]));
        let mut secondary = Tag::new(TagType::Id3v2);
        secondary.push_picture(picture(PictureType::CoverFront, vec![2]));
        let tagged = TaggedFile::new(
            FileType::Flac,
            lofty::properties::FileProperties::default(),
            vec![secondary, primary],
        );
        assert_eq!(select_artwork(tagged), Some(vec![2]));
    }

    #[test]
    fn primary_tag_front_cover_wins_over_secondary_tag_front_cover() {
        let mut primary = Tag::new(TagType::VorbisComments);
        primary.push_picture(picture(PictureType::CoverFront, vec![1]));
        let mut secondary = Tag::new(TagType::Id3v2);
        secondary.push_picture(picture(PictureType::CoverFront, vec![2]));
        let tagged = TaggedFile::new(
            FileType::Flac,
            lofty::properties::FileProperties::default(),
            vec![secondary, primary],
        );
        assert_eq!(select_artwork(tagged), Some(vec![1]));
    }

    #[test]
    fn reads_one_picture_of_a_flac_file() {
        let directory = tempfile::tempdir().expect("temp dir");
        let path = directory.path().join("artwork.flac");
        std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.flac"), &path).expect("copies the fixture");
        assert!(read_artwork(&path).is_none());

        let mut tag = Tag::new(TagType::VorbisComments);
        tag.push_picture(picture(PictureType::CoverBack, vec![7, 7]));
        tag.push_picture(picture(PictureType::CoverFront, vec![9, 9, 9]));
        tag.save_to_path(&path, WriteOptions::default())
            .expect("writes the pictures");
        assert_eq!(read_artwork(&path), Some(vec![9, 9, 9]));
    }

    #[test]
    fn missing_or_broken_files_have_no_artwork() {
        for name in ["missing.flac", "broken.wav"] {
            assert!(read_artwork(&test_fixtures::fixture(name)).is_none(), "{name}");
        }
    }
}

#[cfg(test)]
mod test_serato_overview_state {
    use super::*;

    use std::error::Error as _;

    use base64::Engine as _;
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

    /// Encodes independent ID3v2.3/v2.4 framing without a library's duplicate normalization.
    fn raw_id3(payloads: &[&[u8]], version: u8) -> Vec<u8> {
        let mut frames = Vec::new();
        for payload in payloads {
            let mut body = b"\0application/octet-stream\0\0Serato Overview\0".to_vec();
            body.extend_from_slice(payload);
            frames.extend_from_slice(b"GEOB");
            let length = u32::try_from(body.len()).expect("small test frame");
            frames.extend_from_slice(&if version == 3 {
                length.to_be_bytes()
            } else {
                syncsafe(length)
            });
            frames.extend_from_slice(&[0, 0]);
            frames.extend_from_slice(&body);
        }
        let mut tag = vec![b'I', b'D', b'3', version, 0, 0];
        tag.extend_from_slice(&syncsafe(u32::try_from(frames.len()).expect("small test tag")));
        tag.extend_from_slice(&frames);
        tag
    }

    /// Encodes a test size without relying on production ID3 parsing.
    fn syncsafe(length: u32) -> [u8; 4] {
        [
            ((length >> 21) & 127) as u8,
            ((length >> 14) & 127) as u8,
            ((length >> 7) & 127) as u8,
            (length & 127) as u8,
        ]
    }

    /// Places raw frames in a copied container, retaining committed audio bytes unchanged.
    fn write_raw_id3(path: &Path, payloads: &[&[u8]], version: u8) {
        let source = std::fs::read(path).expect("reads fixture copy");
        let tag = raw_id3(payloads, version);
        let output = if path.extension().is_some_and(|extension| extension == "mp3") {
            let skip = if source.starts_with(b"ID3") {
                10 + source[6..10]
                    .iter()
                    .fold(0, |length, byte| (length << 7) | usize::from(*byte))
            } else {
                0
            };
            [tag, source[skip..].to_vec()].concat()
        } else {
            let little_endian = source.starts_with(b"RIFF");
            let mut output = source;
            output.extend_from_slice(b"ID3 ");
            let length = u32::try_from(tag.len()).expect("small test tag");
            output.extend_from_slice(&if little_endian {
                length.to_le_bytes()
            } else {
                length.to_be_bytes()
            });
            output.extend_from_slice(&tag);
            if !tag.len().is_multiple_of(2) {
                output.push(0);
            }
            let length = u32::try_from(output.len() - 8).expect("small fixture");
            output[4..8].copy_from_slice(&if little_endian {
                length.to_le_bytes()
            } else {
                length.to_be_bytes()
            });
            output
        };
        std::fs::write(path, &output).expect("writes raw ID3 fixture");
    }

    /// Constructs the same native wrapper as the independently specified C++ extraction tests.
    fn native_overview(payload: &[u8]) -> String {
        let mut bytes = b"application/octet-stream\0\0Serato Overview\0".to_vec();
        bytes.extend_from_slice(payload);
        base64::engine::general_purpose::STANDARD.encode(bytes)
    }

    /// Preserves physical first-valid GEOB order across supported containers despite library frame deduplication.
    #[test]
    fn physical_geob_duplicates_keep_first_valid_in_every_container() {
        let directory = TempDir::new().expect("temp dir");
        let first = [1, 5, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37];
        let mut second = first;
        second[2] = 180;
        let invalid = [1, 5, 0];
        for name in [
            "sine_stereo_44100_320k.mp3",
            "sine_stereo_44100_16.aif",
            "sine_stereo_44100_16.wav",
        ] {
            for version in [3, 4] {
                for payloads in [
                    vec![first.as_slice(), &second, &first],
                    vec![invalid.as_slice(), &first, &second, &first],
                    vec![first.as_slice(), &invalid, &second],
                ] {
                    let path = test_fixtures::copy_fixture(&directory, name, None);
                    write_raw_id3(&path, &payloads, version);
                    let tags = read_display_tags(&path).expect("reads raw duplicate tags");
                    let state = SeratoOverviewState::from_tags(&path, &tags);
                    assert_eq!(
                        state.overview().expect("first valid overview").slices(),
                        &[[37; 16]],
                        "{name} v{version}"
                    );
                }
            }
        }
    }

    /// Selects the first valid native duplicate and rejects malformed trailing base64 padding.
    #[test]
    fn native_duplicates_keep_first_valid_and_reject_padding_after_padding() {
        let directory = TempDir::new().expect("temp dir");
        let path = test_fixtures::copy_fixture(&directory, "sine_stereo_44100_16.flac", None);
        let first = [1, 5, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37];
        let mut second = first;
        second[2] = 180;
        let first_encoded = native_overview(&first);
        let second_encoded = native_overview(&second);
        for values in [
            vec![first_encoded.as_str(), &second_encoded, &first_encoded],
            vec!["!invalid!", &first_encoded, &second_encoded, &first_encoded],
            vec![first_encoded.as_str(), "!invalid!", &second_encoded],
        ] {
            let mut comments = VorbisComments::default();
            for value in values {
                comments.push("SERATO_OVERVIEW".to_string(), value.to_string());
            }
            comments
                .save_to_path(&path, WriteOptions::default())
                .expect("writes duplicates");
            let tags = read_display_tags(&path).expect("reads");
            let state = SeratoOverviewState::from_tags(&path, &tags);
            assert_eq!(state.overview().expect("first valid overview").slices(), &[[37; 16]]);
        }
        for (payload, expected) in [
            ([first.as_slice(), &[0]].concat(), true),
            ([first.as_slice(), &[1]].concat(), false),
            ([first.as_slice(), &[0, 0]].concat(), false),
        ] {
            let mut comments = VorbisComments::default();
            comments.insert("SERATO_OVERVIEW".to_string(), native_overview(&payload));
            comments
                .save_to_path(&path, WriteOptions::default())
                .expect("writes padding");
            let tags = read_display_tags(&path).expect("reads padding");
            assert_eq!(
                SeratoOverviewState::from_tags(&path, &tags).overview().is_some(),
                expected
            );
        }
        let mut comments = VorbisComments::default();
        comments.insert(
            "SERATO_OVERVIEW".to_string(),
            native_overview(&[first.as_slice(), &[0]].concat()) + "A",
        );
        comments
            .save_to_path(&path, WriteOptions::default())
            .expect("writes invalid padding");
        let tags = read_display_tags(&path).expect("reads invalid padding");
        assert_eq!(
            SeratoOverviewState::from_tags(&path, &tags),
            SeratoOverviewState::Invalid
        );
    }

    /// Retains Lofty's concrete frame failure through ordered extraction instead of replacing it with a text-only error.
    #[test]
    fn ordered_geob_decode_failure_retains_the_concrete_lofty_source() {
        let directory = TempDir::new().expect("temporary directory");
        let path = directory.path().join("malformed.mp3");
        let mut tag = b"ID3\x04\0\0\0\0\0\x0bGEOB\0\0\0\x01\0\x08".to_vec();
        // A compressed frame must contain a four-byte length indicator, but this one has only one byte.
        tag.push(0);
        std::fs::write(&path, tag).expect("writes malformed compressed frame");
        let error = read_ordered_geob(&path).expect_err("Lofty rejects an undersized compressed frame");
        let mut cause = error.source();
        let mut retains_frame_error = false;
        while let Some(source) = cause {
            if source.is::<lofty::id3::v2::error::FrameParseError>() {
                retains_frame_error = true;
                break;
            }
            cause = source.source();
        }
        assert!(
            retains_frame_error,
            "ordered extraction must retain the concrete Lofty frame error"
        );
    }

    /// Keeps missing or unreadable source data as errors rather than caching a stable absent outcome.
    #[test]
    fn extraction_read_failures_never_return_absent() {
        let directory = TempDir::new().expect("temp dir");
        for name in ["missing.mp3", "missing.aif", "missing.wav", "missing.flac"] {
            assert!(read_serato_data(&directory.path().join(name)).is_err(), "{name}");
        }
        assert!(read_serato_data(&test_fixtures::fixture("broken.wav")).is_err());
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
