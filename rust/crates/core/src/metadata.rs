//! Copies tags, album art, and Serato data from a source file onto processed AIFF output.
//!
//! Ports `copyMetadata` from the C++ `AudioMetadata.cpp`, built on lofty instead of `TagLib`.
//! A source with its own `ID3v2` tag, such as an MP3, AIFF, or WAV file, has that tag copied frame by frame,
//! so custom frames and Serato `GEOB` frames survive unchanged.
//! Other sources go through lofty's format-independent tag.
//! The Vorbis comments of FLAC and Ogg files are read directly, so the vendor string is not copied as a tag.
//! Vorbis comment fields without an `ID3v2` mapping become `TXXX` frames,
//! and native Serato fields of FLAC and Ogg files are also written as the `GEOB` frames Serato reads from AIFF files.
//! The Serato overview is dropped, because it pictures the audio before processing.
//! The tag is written as `ID3v2.4`.

use std::borrow::Cow;
use std::fs::File;
use std::path::Path;

use anyhow::{Context, Result};
use lofty::TextEncoding;
use lofty::config::{ParseOptions, WriteOptions};
use lofty::file::AudioFile;
use lofty::flac::FlacFile;
use lofty::id3::v2::{BinaryFrame, ExtendedTextFrame, Frame, FrameId, GeneralEncapsulatedObject, Id3v2Tag};
use lofty::ogg::tag::VorbisComments;
use lofty::ogg::{OggPictureStorage, OpusFile, VorbisFile};
use lofty::prelude::{ItemKey, TagExt, TaggedFileExt};
use lofty::tag::{Tag, TagType};
use serato_tags::SeratoTag;
use serato_tags::native::{GEOB_MIME_TYPE, decode_native_value, native_field_description};

use crate::tags::read_id3v2;

/// Native field name Serato uses for the overview in Vorbis comments.
const OVERVIEW_FIELD: &str = "SERATO_OVERVIEW";

/// Vorbis comment field for the encoding software.
const ENCODER_FIELD: &str = "ENCODER";

/// Separator between the values of a multi-valued `ID3v2.4` text frame.
const MULTI_VALUE_SEPARATOR: char = '\0';

/// Copies the tags, embedded pictures, and Serato data of the source onto the destination,
/// leaving out the Serato overview.
///
/// A source without readable metadata is not an error, because some inputs have none that lofty can read.
///
/// # Errors
///
/// Returns an error only when metadata was read but could not be written,
/// so the caller can keep the original and its tags.
pub fn copy_metadata(source: &Path, destination: &Path) -> Result<()> {
    let Some(mut tag) = read_source_tag(source) else {
        return Ok(());
    };
    tag.retain(|frame| !is_serato_overview(frame));
    if tag.is_empty() {
        return Ok(());
    }
    tag.save_to_path(destination, WriteOptions::default())
        .with_context(|| format!("Failed to write metadata to {}", destination.display()))
}

/// Reads the metadata of the source as an `ID3v2` tag, or `None` when it has none that lofty can read.
fn read_source_tag(source: &Path) -> Option<Id3v2Tag> {
    if let Some(tag) = read_id3v2(source).filter(|tag| !tag.is_empty()) {
        return Some(tag);
    }
    if let Some(comments) = read_vorbis_comments(source) {
        let mut generic = Tag::from(comments.clone());
        // lofty fills the encoder from the vendor string, which is not a comment and TagLib does not copy.
        if comments.get(ENCODER_FIELD).is_none() {
            generic.remove_key(ItemKey::EncoderSoftware);
        }
        let mut converted = Id3v2Tag::from(generic);
        add_vorbis_fields(&mut converted, &comments);
        return Some(converted);
    }
    let tagged = lofty::read_from_path(source).ok()?;
    let tag = tagged.primary_tag().or_else(|| tagged.first_tag())?;
    Some(Id3v2Tag::from(tag.clone()))
}

/// Reads the Vorbis comments of a FLAC, Ogg Vorbis, or Opus file, with the FLAC pictures added to them.
fn read_vorbis_comments(path: &Path) -> Option<VorbisComments> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    let mut file = File::open(path).ok()?;
    let options = ParseOptions::new();
    match extension.as_str() {
        "flac" => {
            let flac = FlacFile::read_from(&mut file, options).ok()?;
            let mut comments = flac.vorbis_comments().cloned().unwrap_or_default();
            for (picture, information) in flac.pictures() {
                comments.insert_picture(picture.clone(), Some(*information)).ok()?;
            }
            Some(comments)
        }
        "ogg" | "oga" => Some(
            VorbisFile::read_from(&mut file, options)
                .ok()?
                .vorbis_comments()
                .clone(),
        ),
        "opus" => Some(OpusFile::read_from(&mut file, options).ok()?.vorbis_comments().clone()),
        _ => None,
    }
}

/// Adds the Vorbis comment fields that the generic conversion drops as `TXXX` frames,
/// and the native Serato fields as `GEOB` frames.
fn add_vorbis_fields(tag: &mut Id3v2Tag, comments: &VorbisComments) {
    let mut unmapped: Vec<(String, String)> = Vec::new();
    for (key, value) in comments.items() {
        if let Some(description) = native_field_description(key)
            && let Some(payload) = decode_native_value(value, description)
        {
            tag.insert(serato_frame(description, payload));
        }
        let mapped = ItemKey::from_key(TagType::VorbisComments, key)
            .and_then(|item_key| item_key.map_key(TagType::Id3v2))
            .is_some();
        if mapped {
            continue;
        }
        let key = key.to_ascii_uppercase();
        if let Some((_, values)) = unmapped.iter_mut().find(|(existing, _)| *existing == key) {
            values.push(MULTI_VALUE_SEPARATOR);
            values.push_str(value);
        } else {
            unmapped.push((key, value.to_string()));
        }
    }
    for (description, content) in unmapped {
        tag.insert(Frame::UserText(ExtendedTextFrame::new(
            TextEncoding::UTF8,
            Cow::Owned(description),
            Cow::Owned(content),
        )));
    }
}

/// Builds a Serato `GEOB` frame with the given description and payload.
fn serato_frame(description: &str, payload: Vec<u8>) -> Frame<'static> {
    let object = GeneralEncapsulatedObject::new(
        TextEncoding::Latin1,
        Some(GEOB_MIME_TYPE.to_string()),
        None,
        Some(description.to_string()),
        payload,
    );
    let bytes = object.as_bytes(WriteOptions::default()).unwrap_or_default();
    Frame::Binary(BinaryFrame::new(FrameId::Valid(Cow::Borrowed("GEOB")), bytes))
}

/// Returns true for the frames that hold the Serato overview, as a `GEOB` frame or a native field copy.
fn is_serato_overview(frame: &Frame<'_>) -> bool {
    match frame {
        Frame::Binary(binary) if frame.id_str() == "GEOB" => {
            GeneralEncapsulatedObject::parse(&binary.data, frame.flags())
                .is_ok_and(|object| object.descriptor.as_deref() == Some(SeratoTag::Overview.description()))
        }
        Frame::UserText(text) => text.description.eq_ignore_ascii_case(OVERVIEW_FIELD),
        _ => false,
    }
}

#[cfg(test)]
mod test_copy_metadata {
    use std::fs;

    use base64::Engine as _;
    use lofty::iff::aiff::AiffFile;
    use lofty::picture::{MimeType, Picture, PictureInformation, PictureType};
    use lofty::prelude::Accessor;

    use super::*;
    use crate::aiff::AiffWriter;

    /// Writes a short silent AIFF file to receive metadata.
    fn write_destination(path: &Path) {
        let file = fs::File::create(path).expect("creates destination");
        let mut writer = AiffWriter::new(file, 1, 44_100, 16).expect("starts writer");
        writer.write_planar(&[&[0.0_f32; 16]]).expect("writes");
        writer.finish().expect("finishes");
    }

    /// Reads the `ID3v2` tag of an AIFF file.
    fn read_aiff_tag(path: &Path) -> Option<Id3v2Tag> {
        let mut file = fs::File::open(path).expect("opens");
        AiffFile::read_from(&mut file, ParseOptions::new())
            .expect("parses")
            .id3v2()
            .cloned()
    }

    /// Returns the descriptions of the `GEOB` frames of a tag.
    fn geob_descriptions(tag: &Id3v2Tag) -> Vec<String> {
        tag.into_iter()
            .filter_map(|frame| match frame {
                Frame::Binary(binary) if frame.id_str() == "GEOB" => {
                    GeneralEncapsulatedObject::parse(&binary.data, frame.flags()).ok()
                }
                _ => None,
            })
            .filter_map(|object| object.descriptor)
            .collect()
    }

    /// Encodes a payload as a native Serato Vorbis comment value.
    fn native_value(description: &str, payload: &[u8]) -> String {
        let mut bytes = Vec::new();
        for field in [GEOB_MIME_TYPE, "", description] {
            bytes.extend_from_slice(field.as_bytes());
            bytes.push(0);
        }
        bytes.extend_from_slice(payload);
        base64::engine::general_purpose::STANDARD.encode(bytes)
    }

    #[test]
    fn copies_flac_tags_and_serato_fields() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.flac");
        fs::copy(test_fixtures::fixture("tagged_stereo_44100_16.flac"), &source).expect("copies fixture");
        let tagged = lofty::read_from_path(&source).expect("reads fixture tags");
        let mut comments = VorbisComments::from(tagged.primary_tag().expect("has comments").clone());
        comments.push("SERATO_ANALYSIS".to_string(), native_value("Serato Analysis", &[2, 1]));
        comments.push("SERATO_OVERVIEW".to_string(), native_value("Serato Overview", &[1, 5]));
        comments.push("MY_FIELD".to_string(), "custom".to_string());
        let picture = Picture::unchecked(vec![1, 2, 3])
            .pic_type(PictureType::CoverFront)
            .mime_type(MimeType::Png)
            .build();
        comments
            .insert_picture(picture, Some(PictureInformation::default()))
            .expect("adds picture");
        comments.save_to_path(&source, WriteOptions::default()).expect("saves");
        let destination = directory.path().join("song.aif");
        write_destination(&destination);

        copy_metadata(&source, &destination).expect("copies");

        let tag = read_aiff_tag(&destination).expect("has a tag");
        assert_eq!(tag.title().as_deref(), Some("Test Tone"));
        assert_eq!(tag.artist().as_deref(), Some("AudioBatch"));
        assert_eq!(tag.get_user_text("MY_FIELD"), Some("custom"));
        assert_eq!(tag.get_user_text("SERATO_OVERVIEW"), None);
        assert!(tag.get_user_text("SERATO_ANALYSIS").is_some());
        assert_eq!(geob_descriptions(&tag), vec!["Serato Analysis".to_string()]);
        assert_eq!(tag.into_iter().filter(|frame| frame.id_str() == "APIC").count(), 1);
    }

    #[test]
    fn copies_id3_frames_without_the_overview() {
        let directory = tempfile::tempdir().expect("creates directory");
        let source = directory.path().join("song.aif");
        write_destination(&source);
        let mut source_tag = Id3v2Tag::new();
        source_tag.set_title("Original".to_string());
        source_tag.insert(serato_frame("Serato Markers2", vec![1, 1, 0]));
        source_tag.insert(serato_frame("Serato Overview", vec![1, 5]));
        source_tag
            .save_to_path(&source, WriteOptions::default())
            .expect("saves source tag");
        let destination = directory.path().join("out.aif");
        write_destination(&destination);

        copy_metadata(&source, &destination).expect("copies");

        let tag = read_aiff_tag(&destination).expect("has a tag");
        assert_eq!(tag.title().as_deref(), Some("Original"));
        assert_eq!(geob_descriptions(&tag), vec!["Serato Markers2".to_string()]);
    }

    #[test]
    fn untagged_source_leaves_the_destination_untagged() {
        let directory = tempfile::tempdir().expect("creates directory");
        let destination = directory.path().join("out.aif");
        write_destination(&destination);

        copy_metadata(&test_fixtures::fixture("sine_stereo_44100_16.wav"), &destination).expect("copies");

        assert!(read_aiff_tag(&destination).is_none_or(|tag| tag.is_empty()));
    }

    #[test]
    fn flac_vendor_string_is_not_copied() {
        let directory = tempfile::tempdir().expect("creates directory");
        let destination = directory.path().join("out.aif");
        write_destination(&destination);

        copy_metadata(&test_fixtures::fixture("sine_stereo_48000_24.flac"), &destination).expect("copies");

        assert!(read_aiff_tag(&destination).is_none_or(|tag| tag.is_empty()));
    }
}
