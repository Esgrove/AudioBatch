//! Audio file decoding on top of symphonia, shared by analysis and the preview player.
//!
//! `AudioFileDecoder` probes a file, picks its default audio track,
//! and decodes it packet by packet into planar `f32` samples, with encoder delay and padding trimmed.
//! `DecodeFailureTracker` holds the shared rules for packets that fail to decode,
//! ported from the C++ `DecodeFailureTracker.h`:
//! isolated failures are tolerated and replaced with silence,
//! while several failures in a row stop decoding with an error,
//! so no pass silently works on truncated audio.

use std::fs::File;
use std::io::ErrorKind;
use std::path::Path;

use anyhow::{Context, Result, anyhow, bail};
use symphonia::core::codecs::CodecParameters;
use symphonia::core::codecs::audio::well_known::{
    CODEC_ID_MP1, CODEC_ID_MP2, CODEC_ID_MP3, CODEC_ID_PCM_F32BE, CODEC_ID_PCM_F32BE_PLANAR, CODEC_ID_PCM_F32LE,
    CODEC_ID_PCM_F32LE_PLANAR, CODEC_ID_PCM_F64BE, CODEC_ID_PCM_F64BE_PLANAR, CODEC_ID_PCM_F64LE,
    CODEC_ID_PCM_F64LE_PLANAR, CODEC_ID_VORBIS,
};
use symphonia::core::codecs::audio::{AudioCodecParameters, AudioDecoder, AudioDecoderOptions};
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::probe::Hint;
use symphonia::core::formats::{FormatOptions, FormatReader, SeekMode, SeekTo, TrackType};
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::units::Time;

/// Bit depth assumed for MPEG audio and Vorbis files, which store no source bit depth.
pub const DEFAULT_MP3_BITS_PER_SAMPLE: u32 = 16;

/// Longest run of silence inserted for one failed packet, in seconds.
/// Guards against a corrupt packet duration turning into a huge allocation.
const MAX_FAILED_PACKET_SECONDS: u64 = 1;

/// Stream properties of an opened audio file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StreamInfo {
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// Number of channels.
    pub channels: u32,
    /// Source bit depth, the MP3 default for MPEG audio, or 0 when the format has none.
    pub bits_per_sample: u32,
    /// Container format name in the style of the C++ application, for example "WAV file".
    pub format_name: String,
    /// Playable length in frames, when the container states it.
    pub expected_frames: Option<u64>,
}

/// Tracks packet decode failures for one decode pass over a file.
///
/// Isolated failures are tolerated, because some decoders fail mid-stream on files that others handle fine.
/// Failures at the end of the stream are tolerated too, because MP3 files often overshoot the decodable audio.
/// Several failures in a row mean the rest of the file cannot be decoded, and the pass must stop.
#[derive(Debug, Default, Clone)]
pub struct DecodeFailureTracker {
    failed_packets: u32,
    consecutive_failures: u32,
    first_failure_position: Option<u64>,
    stopped_at: Option<u64>,
}

/// Decodes one audio file into planar `f32` samples, packet by packet.
pub struct AudioFileDecoder {
    format: Box<dyn FormatReader>,
    decoder: Box<dyn AudioDecoder>,
    track_id: u32,
    info: StreamInfo,
    /// Samples of the most recent packet, one vector per channel.
    planar: Vec<Vec<f32>>,
    failures: DecodeFailureTracker,
    /// Frames produced so far, including silence for failed packets.
    position: u64,
}

impl DecodeFailureTracker {
    /// Number of consecutive failed packets after which decoding stops.
    pub const MAXIMUM_CONSECUTIVE_FAILURES: u32 = 3;

    /// Records a packet that decoded.
    pub const fn record_success(&mut self) {
        self.consecutive_failures = 0;
    }

    /// Records a packet that failed to decode at the given frame position.
    /// Returns false when decoding must stop, after which `error_message` describes the failure.
    pub fn record_failure(&mut self, position: u64) -> bool {
        self.failed_packets += 1;
        self.consecutive_failures += 1;
        self.first_failure_position.get_or_insert(position);
        if self.consecutive_failures >= Self::MAXIMUM_CONSECUTIVE_FAILURES {
            self.stopped_at = Some(position);
            return false;
        }
        true
    }

    /// Returns the number of failed packets.
    #[must_use]
    pub const fn failed_packet_count(&self) -> u32 {
        self.failed_packets
    }

    /// Returns the frame position of the first failed packet, if any failed.
    #[must_use]
    pub const fn first_failure_position(&self) -> Option<u64> {
        self.first_failure_position
    }

    /// Returns a user-facing notice about tolerated failures, or `None` when every packet decoded.
    #[must_use]
    pub fn notice(&self) -> Option<String> {
        self.first_failure_position.map(|position| {
            format!(
                "{} audio packet(s) failed to decode, first at sample {position}",
                self.failed_packets
            )
        })
    }

    /// Returns the user-facing error after `record_failure` returned false.
    #[must_use]
    pub fn error_message(&self, total_frames: Option<u64>) -> String {
        let position = self.stopped_at.unwrap_or_default();
        total_frames.map_or_else(
            || format!("Audio decoding failed at sample {position}"),
            |total| format!("Audio decoding failed at sample {position} of {total}"),
        )
    }
}

impl AudioFileDecoder {
    /// Opens a file and prepares a decoder for its default audio track.
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be opened, has no supported audio track,
    /// or has stream parameters the analysis cannot use.
    pub fn open(path: &Path) -> Result<Self> {
        let file = File::open(path).with_context(|| format!("Failed to open {}", path.display()))?;
        let source = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());
        let mut hint = Hint::new();
        if let Some(extension) = path.extension().and_then(|extension| extension.to_str()) {
            hint.with_extension(extension);
        }
        let format = symphonia::default::get_probe()
            .probe(&hint, source, FormatOptions::default(), MetadataOptions::default())
            .map_err(|_| anyhow!("Unsupported or unreadable audio file"))?;

        let track = format
            .default_track(TrackType::Audio)
            .ok_or_else(|| anyhow!("Unsupported or unreadable audio file"))?;
        let track_id = track.id;
        let expected_frames = track.num_frames;
        let parameters = match &track.codec_params {
            Some(CodecParameters::Audio(parameters)) => parameters.clone(),
            _ => bail!("Unsupported or unreadable audio file"),
        };

        let sample_rate = parameters.sample_rate.unwrap_or(0);
        let channels = parameters
            .channels
            .as_ref()
            .map_or(0, |channels| u32::try_from(channels.count()).unwrap_or(0));
        if sample_rate == 0 || channels == 0 {
            bail!("Unsupported audio stream parameters");
        }

        let bits_per_sample = source_bits_per_sample(&parameters);

        let decoder = symphonia::default::get_codecs()
            .make_audio_decoder(&parameters, &AudioDecoderOptions::default())
            .map_err(|_| anyhow!("Unsupported audio codec"))?;

        let info = StreamInfo {
            sample_rate,
            channels,
            bits_per_sample,
            format_name: format_display_name(format.format_info().short_name, path),
            expected_frames,
        };

        Ok(Self {
            format,
            decoder,
            track_id,
            info,
            planar: vec![Vec::new(); channels as usize],
            failures: DecodeFailureTracker::default(),
            position: 0,
        })
    }

    /// Returns the stream properties.
    #[must_use]
    pub const fn info(&self) -> &StreamInfo {
        &self.info
    }

    /// Returns the number of frames produced so far.
    #[must_use]
    pub const fn position(&self) -> u64 {
        self.position
    }

    /// Returns the failure record of this pass.
    #[must_use]
    pub const fn failures(&self) -> &DecodeFailureTracker {
        &self.failures
    }

    /// Decodes the next packet of the track and returns its samples, one slice per channel,
    /// or `None` at the end of the stream.
    /// A packet that fails to decode is replaced with silence of the same length,
    /// following the `DecodeFailureTracker` rules.
    ///
    /// # Errors
    ///
    /// Returns an error when the container cannot be read further,
    /// or when too many packets in a row fail to decode.
    pub fn next_chunk(&mut self) -> Result<Option<&[Vec<f32>]>> {
        loop {
            let packet = match self.format.next_packet() {
                Ok(Some(packet)) => packet,
                // A truncated final packet and a chained stream both end the track for this purpose.
                Ok(None) | Err(SymphoniaError::ResetRequired) => return Ok(None),
                Err(SymphoniaError::IoError(error)) if error.kind() == ErrorKind::UnexpectedEof => {
                    return Ok(None);
                }
                Err(error) => bail!("Audio decoding failed at sample {}: {error}", self.position),
            };
            if packet.track_id != self.track_id {
                continue;
            }

            match self.decoder.decode(&packet) {
                Ok(buffer) => {
                    self.failures.record_success();
                    if buffer.frames() == 0 {
                        continue;
                    }
                    buffer.copy_to_vecs_planar::<f32>(&mut self.planar);
                    self.position += buffer.frames() as u64;
                    return Ok(Some(&self.planar));
                }
                Err(SymphoniaError::DecodeError(_) | SymphoniaError::IoError(_)) => {
                    if !self.failures.record_failure(self.position) {
                        bail!(self.failures.error_message(self.info.expected_frames));
                    }
                    let silent_frames = packet
                        .dur
                        .get()
                        .min(u64::from(self.info.sample_rate) * MAX_FAILED_PACKET_SECONDS)
                        as usize;
                    if silent_frames == 0 {
                        continue;
                    }
                    for channel in &mut self.planar {
                        channel.clear();
                        channel.resize(silent_frames, 0.0);
                    }
                    self.position += silent_frames as u64;
                    return Ok(Some(&self.planar));
                }
                Err(error) => bail!("Audio decoding failed at sample {}: {error}", self.position),
            }
        }
    }

    /// Seeks to the given time in seconds and returns the new frame position.
    /// The decoder is reset, so the next chunk starts at or slightly before the requested time.
    ///
    /// # Errors
    ///
    /// Returns an error when the container cannot seek.
    pub fn seek(&mut self, seconds: f64) -> Result<u64> {
        let time = Time::try_from_secs_f64(seconds.max(0.0)).ok_or_else(|| anyhow!("Invalid seek time {seconds}"))?;
        let seeked = self
            .format
            .seek(
                SeekMode::Accurate,
                SeekTo::Time {
                    time,
                    track_id: Some(self.track_id),
                },
            )
            .map_err(|error| anyhow!("Seek failed: {error}"))?;
        self.decoder.reset();
        self.failures = DecodeFailureTracker::default();
        self.position = u64::try_from(seeked.actual_ts.get()).unwrap_or(0);
        Ok(self.position)
    }
}

/// Returns true when the path has the extension of an MPEG audio file.
#[must_use]
pub fn is_mp3_path(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("mp3"))
}

/// Determines the best available source bit depth of a track.
///
/// MPEG audio and Vorbis get the default of 16, because the formats store no sample depth,
/// which is also what the C++ application reports for them.
/// Floating point PCM states no bit depth in its parameters, so it gets the width of its sample type.
/// Other lossy codecs report 0, since their decoded depth says nothing about the source.
fn source_bits_per_sample(parameters: &AudioCodecParameters) -> u32 {
    if [CODEC_ID_MP1, CODEC_ID_MP2, CODEC_ID_MP3, CODEC_ID_VORBIS].contains(&parameters.codec) {
        return DEFAULT_MP3_BITS_PER_SAMPLE;
    }
    if let Some(bits) = parameters.bits_per_sample {
        return bits;
    }
    let float32 = [
        CODEC_ID_PCM_F32LE,
        CODEC_ID_PCM_F32LE_PLANAR,
        CODEC_ID_PCM_F32BE,
        CODEC_ID_PCM_F32BE_PLANAR,
    ];
    let float64 = [
        CODEC_ID_PCM_F64LE,
        CODEC_ID_PCM_F64LE_PLANAR,
        CODEC_ID_PCM_F64BE,
        CODEC_ID_PCM_F64BE_PLANAR,
    ];
    if float32.contains(&parameters.codec) {
        32
    } else if float64.contains(&parameters.codec) {
        64
    } else {
        0
    }
}

/// Returns the container name in the style of the JUCE format names the C++ application reports,
/// so the CLI output of the two versions can be compared directly.
fn format_display_name(short_name: &str, path: &Path) -> String {
    let extension = path
        .extension()
        .and_then(|extension| extension.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default();
    let name = match (short_name, extension.as_str()) {
        ("wave" | "wav", _) | (_, "wav" | "wave" | "bwf") => "WAV",
        ("aiff", _) | (_, "aif" | "aiff" | "aifc") => "AIFF",
        ("flac", _) | (_, "flac") => "FLAC",
        ("ogg", _) | (_, "ogg" | "oga") => "Ogg-Vorbis",
        (_, "mp3") => "MP3",
        (_, "m4a" | "mp4" | "aac") => "M4A",
        (_, "caf") => "CAF",
        _ => return format!("{} file", short_name.to_ascii_uppercase()),
    };
    format!("{name} file")
}

#[cfg(test)]
mod test_decode_failure_tracker {
    use super::*;

    #[test]
    fn isolated_failures_are_tolerated() {
        let mut tracker = DecodeFailureTracker::default();
        assert!(tracker.record_failure(100));
        tracker.record_success();
        assert!(tracker.record_failure(200));
        tracker.record_success();
        assert_eq!(tracker.failed_packet_count(), 2);
        assert_eq!(tracker.first_failure_position(), Some(100));
        assert!(
            tracker
                .notice()
                .is_some_and(|notice| notice.contains("first at sample 100"))
        );
    }

    #[test]
    fn consecutive_failures_stop_decoding() {
        let mut tracker = DecodeFailureTracker::default();
        assert!(tracker.record_failure(10));
        assert!(tracker.record_failure(20));
        assert!(!tracker.record_failure(30));
        assert_eq!(
            tracker.error_message(Some(1000)),
            "Audio decoding failed at sample 30 of 1000"
        );
        assert_eq!(tracker.error_message(None), "Audio decoding failed at sample 30");
    }

    #[test]
    fn clean_pass_has_no_notice() {
        let mut tracker = DecodeFailureTracker::default();
        tracker.record_success();
        assert!(tracker.notice().is_none());
        assert_eq!(tracker.failed_packet_count(), 0);
    }
}

#[cfg(test)]
mod test_audio_file_decoder {
    use super::*;

    #[test]
    fn opens_every_tone_fixture() {
        for tone in test_fixtures::TONE_FIXTURES {
            let decoder = AudioFileDecoder::open(&tone.path()).expect(tone.file_name);
            assert_eq!(decoder.info().channels, tone.channels, "{}", tone.file_name);
            assert_eq!(decoder.info().sample_rate, tone.sample_rate, "{}", tone.file_name);
        }
    }

    #[test]
    fn broken_file_is_rejected() {
        let error = AudioFileDecoder::open(&test_fixtures::fixture("broken.wav"))
            .err()
            .expect("broken file should fail");
        assert!(error.to_string().contains("Unsupported"), "{error}");
    }

    #[test]
    fn decodes_full_length() {
        let mut decoder =
            AudioFileDecoder::open(&test_fixtures::fixture("sine_stereo_44100_16.wav")).expect("fixture opens");
        let mut frames = 0;
        while let Some(chunk) = decoder.next_chunk().expect("decodes") {
            assert_eq!(chunk.len(), 2);
            frames += chunk.first().map_or(0, Vec::len);
        }
        assert_eq!(frames, 44_100);
        assert_eq!(decoder.position(), 44_100);
    }

    #[test]
    fn seek_moves_the_position() {
        let mut decoder =
            AudioFileDecoder::open(&test_fixtures::fixture("sine_stereo_48000_24.flac")).expect("fixture opens");
        let position = decoder.seek(0.5).expect("seeks");
        assert!(position <= 24_000, "{position}");
        assert!(decoder.next_chunk().expect("decodes").is_some());
    }

    #[test]
    fn format_names_follow_the_cpp_application() {
        assert_eq!(format_display_name("wave", Path::new("a.wav")), "WAV file");
        assert_eq!(format_display_name("aiff", Path::new("a.aif")), "AIFF file");
        assert_eq!(format_display_name("mp3", Path::new("a.mp3")), "MP3 file");
        assert_eq!(format_display_name("ogg", Path::new("a.ogg")), "Ogg-Vorbis file");
    }
}
