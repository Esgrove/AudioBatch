//! Gapless playback information of MP4 audio, which symphonia reads but does not apply.
//!
//! AAC encoders add priming samples at the start of the stream and padding at the end.
//! An MP4 file records the playable part in the edit list of the audio track,
//! and files from Apple encoders also in the `iTunSMPB` iTunes atom.
//! `read_mp4_gapless` walks the boxes of the file to find either one,
//! so the decoder can leave out the priming and padding the way Core Audio does for the C++ application.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// Largest box body read into memory while looking for the gapless information.
const MAXIMUM_BOX_BODY_BYTES: u64 = 1 << 20;

/// Media time of an empty edit, which inserts silence instead of playing media.
const EMPTY_EDIT_MEDIA_TIME: i64 = -1;

/// The playable part of an MP4 audio track in frames of the track sample rate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct Gapless {
    /// Priming frames at the start of the stream that are not played.
    pub priming: u64,
    /// Playable frames after the priming, when the file states it.
    pub frames: Option<u64>,
}

/// The header of one box: its type and the byte range of its body.
#[derive(Debug, Clone, Copy)]
struct BoxHeader {
    kind: [u8; 4],
    body_start: u64,
    body_end: u64,
}

/// The parts of one track that matter for its gapless information.
#[derive(Debug, Default)]
struct TrackBoxes {
    is_audio: bool,
    media_timescale: u32,
    media_duration: u64,
    first_edit: Option<(u64, i64)>,
}

/// Reads the gapless information of the first audio track of an MP4 file,
/// or `None` when the file has neither an edit list that trims the stream nor an `iTunSMPB` atom.
pub(super) fn read_mp4_gapless(path: &Path, sample_rate: u32) -> Option<Gapless> {
    let mut file = File::open(path).ok()?;
    let length = file.metadata().ok()?.len();
    let movie = child_boxes(&mut file, 0, length)
        .into_iter()
        .find(|header| &header.kind == b"moov")?;

    let mut movie_timescale = 0;
    let mut audio_track = None;
    let mut smpb = None;
    for header in child_boxes(&mut file, movie.body_start, movie.body_end) {
        match &header.kind {
            b"mvhd" => {
                movie_timescale = read_body(&mut file, header)
                    .and_then(|body| movie_header_timescale(&body))
                    .unwrap_or(0);
            }
            b"trak" if audio_track.is_none() => {
                let track = read_track(&mut file, header);
                if track.is_audio {
                    audio_track = Some(track);
                }
            }
            b"udta" => smpb = smpb.or_else(|| read_itunes_smpb(&mut file, header)),
            _ => {}
        }
    }

    let track = audio_track?;
    edit_list_gapless(&track, movie_timescale, sample_rate).or(smpb)
}

/// Converts the first edit of an audio track into gapless information,
/// or `None` when the edit plays the whole stream from its start.
fn edit_list_gapless(track: &TrackBoxes, movie_timescale: u32, sample_rate: u32) -> Option<Gapless> {
    let (segment_duration, media_time) = track.first_edit?;
    if track.media_timescale == 0 || movie_timescale == 0 {
        return None;
    }
    let priming = rescale(u64::try_from(media_time).ok()?, track.media_timescale, sample_rate);
    let available = rescale(track.media_duration, track.media_timescale, sample_rate).checked_sub(priming);
    let frames = (segment_duration > 0).then(|| {
        let edited = rescale(segment_duration, movie_timescale, sample_rate);
        // The movie timescale is often coarser than the sample rate, so the edit can be a few frames off.
        let tolerance = u64::from(sample_rate.div_ceil(movie_timescale));
        available
            .filter(|frames| frames.abs_diff(edited) <= tolerance)
            .unwrap_or(edited)
    });
    if priming == 0 && frames.is_none_or(|frames| Some(frames) >= available) {
        return None;
    }
    Some(Gapless { priming, frames })
}

/// Reads the handler, the media timescale and duration, and the first edit of a `trak` box.
fn read_track(file: &mut File, track: BoxHeader) -> TrackBoxes {
    let mut boxes = TrackBoxes::default();
    for header in child_boxes(file, track.body_start, track.body_end) {
        match &header.kind {
            b"edts" => {
                boxes.first_edit = child_boxes(file, header.body_start, header.body_end)
                    .into_iter()
                    .find(|child| &child.kind == b"elst")
                    .and_then(|child| read_body(file, child))
                    .and_then(|body| first_edit(&body));
            }
            b"mdia" => {
                for child in child_boxes(file, header.body_start, header.body_end) {
                    match &child.kind {
                        b"hdlr" => {
                            boxes.is_audio =
                                read_body(file, child).is_some_and(|body| body.get(8..12) == Some(b"soun".as_slice()));
                        }
                        b"mdhd" => {
                            if let Some((timescale, duration)) =
                                read_body(file, child).and_then(|body| media_header(&body))
                            {
                                boxes.media_timescale = timescale;
                                boxes.media_duration = duration;
                            }
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }
    boxes
}

/// Reads the priming and playable frames from the `iTunSMPB` atom under `udta/meta/ilst`.
fn read_itunes_smpb(file: &mut File, user_data: BoxHeader) -> Option<Gapless> {
    let meta = child_boxes(file, user_data.body_start, user_data.body_end)
        .into_iter()
        .find(|header| &header.kind == b"meta")?;
    // The iTunes meta box is a full box with four bytes of version and flags before its children,
    // while the QuickTime form has none.
    let mut version = [0_u8; 4];
    file.seek(SeekFrom::Start(meta.body_start)).ok()?;
    file.read_exact(&mut version).ok()?;
    let children_start = if version == [0; 4] {
        meta.body_start + 4
    } else {
        meta.body_start
    };
    let item_list = child_boxes(file, children_start, meta.body_end)
        .into_iter()
        .find(|header| &header.kind == b"ilst")?;
    for item in child_boxes(file, item_list.body_start, item_list.body_end) {
        if &item.kind != b"----" {
            continue;
        }
        let mut name = None;
        let mut value = None;
        for child in child_boxes(file, item.body_start, item.body_end) {
            match &child.kind {
                b"name" => name = read_body(file, child).and_then(|body| body.get(4..).map(<[u8]>::to_vec)),
                b"data" => value = read_body(file, child).and_then(|body| body.get(8..).map(<[u8]>::to_vec)),
                _ => {}
            }
        }
        if name.as_deref() == Some(b"iTunSMPB".as_slice()) {
            return parse_itunes_smpb(&String::from_utf8_lossy(&value?));
        }
    }
    None
}

/// Parses the hexadecimal fields of an `iTunSMPB` value: a reserved field, the priming, the padding,
/// and the playable frame count.
fn parse_itunes_smpb(value: &str) -> Option<Gapless> {
    let mut fields = value.split_whitespace().skip(1);
    let priming = u64::from_str_radix(fields.next()?, 16).ok()?;
    let _padding = u64::from_str_radix(fields.next()?, 16).ok()?;
    let frames = u64::from_str_radix(fields.next()?, 16).ok()?;
    if priming == 0 && frames == 0 {
        return None;
    }
    Some(Gapless {
        priming,
        frames: (frames > 0).then_some(frames),
    })
}

/// Returns the duration and media time of the first edit that plays media, skipping empty edits.
fn first_edit(body: &[u8]) -> Option<(u64, i64)> {
    let (&version, rest) = body.split_first()?;
    let count = u32::from_be_bytes(*rest.get(3..7)?.first_chunk()?);
    let entries = rest.get(7..)?;
    let entry_size = if version == 1 { 20 } else { 12 };
    (0..count as usize)
        .map_while(|index| entries.get(index * entry_size..(index + 1) * entry_size))
        .map(|entry| {
            if version == 1 {
                let (duration, time) = entry.split_at(8);
                Some((
                    u64::from_be_bytes(*duration.first_chunk()?),
                    i64::from_be_bytes(*time.first_chunk()?),
                ))
            } else {
                let (duration, time) = entry.split_at(4);
                Some((
                    u64::from(u32::from_be_bytes(*duration.first_chunk()?)),
                    i64::from(i32::from_be_bytes(*time.first_chunk()?)),
                ))
            }
        })
        .find(|edit| edit.is_none_or(|(_, media_time)| media_time != EMPTY_EDIT_MEDIA_TIME))
        .flatten()
}

/// Returns the timescale of a movie header box body.
fn movie_header_timescale(body: &[u8]) -> Option<u32> {
    let offset = if body.first() == Some(&1) { 20 } else { 12 };
    Some(u32::from_be_bytes(*body.get(offset..)?.first_chunk()?))
}

/// Returns the timescale and duration of a media header box body.
fn media_header(body: &[u8]) -> Option<(u32, u64)> {
    if body.first() == Some(&1) {
        let timescale = u32::from_be_bytes(*body.get(20..)?.first_chunk()?);
        let duration = u64::from_be_bytes(*body.get(24..)?.first_chunk()?);
        Some((timescale, duration))
    } else {
        let timescale = u32::from_be_bytes(*body.get(12..)?.first_chunk()?);
        let duration = u32::from_be_bytes(*body.get(16..)?.first_chunk()?);
        Some((timescale, u64::from(duration)))
    }
}

/// Converts a value from one timescale to another, rounding to the nearest unit.
fn rescale(value: u64, from: u32, to: u32) -> u64 {
    let scaled = (u128::from(value) * u128::from(to) + u128::from(from) / 2) / u128::from(from.max(1));
    u64::try_from(scaled).unwrap_or(u64::MAX)
}

/// Lists the boxes between two byte offsets of the file, stopping at the first malformed header.
fn child_boxes(file: &mut File, start: u64, end: u64) -> Vec<BoxHeader> {
    let mut headers = Vec::new();
    let mut position = start;
    while let Some(header) = read_box_header(file, position, end) {
        position = header.body_end;
        headers.push(header);
    }
    headers
}

/// Reads the box header at the given offset, or `None` when no complete box fits before the end.
fn read_box_header(file: &mut File, position: u64, end: u64) -> Option<BoxHeader> {
    let mut header = [0_u8; 8];
    file.seek(SeekFrom::Start(position)).ok()?;
    file.read_exact(&mut header).ok()?;
    let (size, kind) = header.split_at(4);
    let size = u64::from(u32::from_be_bytes(*size.first_chunk()?));
    let kind = *kind.first_chunk()?;
    let (body_start, total) = match size {
        0 => (position + 8, end - position),
        1 => {
            let mut large = [0_u8; 8];
            file.read_exact(&mut large).ok()?;
            (position + 16, u64::from_be_bytes(large))
        }
        _ => (position + 8, size),
    };
    let body_end = position.checked_add(total)?;
    (body_end <= end && body_start <= body_end).then_some(BoxHeader {
        kind,
        body_start,
        body_end,
    })
}

/// Reads the body of a small box, or `None` when it is larger than the gapless information needs.
fn read_body(file: &mut File, header: BoxHeader) -> Option<Vec<u8>> {
    let length = header.body_end - header.body_start;
    if length > MAXIMUM_BOX_BODY_BYTES {
        return None;
    }
    let mut body = vec![0; usize::try_from(length).ok()?];
    file.seek(SeekFrom::Start(header.body_start)).ok()?;
    file.read_exact(&mut body).ok()?;
    Some(body)
}

#[cfg(test)]
mod test_mp4_gapless {
    use super::*;

    #[test]
    fn edit_list_of_the_aac_fixture_trims_priming_and_padding() {
        let path = test_fixtures::fixture(test_fixtures::AAC_FIXTURE);
        assert_eq!(
            read_mp4_gapless(&path, 44_100),
            Some(Gapless {
                priming: 1024,
                frames: Some(44_100),
            })
        );
        assert_eq!(
            read_mp4_gapless(&test_fixtures::fixture("sine_stereo_44100_16.wav"), 44_100),
            None
        );
    }

    #[test]
    fn edit_list_that_plays_everything_needs_no_trim() {
        let track = TrackBoxes {
            is_audio: true,
            media_timescale: 48_000,
            media_duration: 96_000,
            first_edit: Some((2000, 0)),
        };
        assert_eq!(edit_list_gapless(&track, 1000, 48_000), None);
    }

    #[test]
    fn coarse_movie_timescale_uses_the_media_duration() {
        let track = TrackBoxes {
            is_audio: true,
            media_timescale: 44_100,
            media_duration: 44_100 + 2112 + 7,
            first_edit: Some((1000, 2112)),
        };
        assert_eq!(
            edit_list_gapless(&track, 1000, 44_100),
            Some(Gapless {
                priming: 2112,
                frames: Some(44_107),
            })
        );
    }

    #[test]
    fn empty_edits_are_skipped() {
        let mut body = vec![0, 0, 0, 0, 0, 0, 0, 2];
        body.extend_from_slice(&500_u32.to_be_bytes());
        body.extend_from_slice(&(-1_i32).to_be_bytes());
        body.extend_from_slice(&[0, 1, 0, 0]);
        body.extend_from_slice(&1000_u32.to_be_bytes());
        body.extend_from_slice(&1024_i32.to_be_bytes());
        body.extend_from_slice(&[0, 1, 0, 0]);
        assert_eq!(first_edit(&body), Some((1000, 1024)));
    }

    #[test]
    fn itunes_smpb_value_is_parsed() {
        let value = " 00000000 00000840 000001CA 00000000000D7550 00000000 00000000";
        assert_eq!(
            parse_itunes_smpb(value),
            Some(Gapless {
                priming: 2112,
                frames: Some(882_000),
            })
        );
        assert_eq!(parse_itunes_smpb(" 00000000 00000000 00000000 0000000000000000"), None);
        assert_eq!(parse_itunes_smpb("garbage"), None);
    }
}
