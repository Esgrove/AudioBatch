//! Audio file decoding on top of symphonia, shared by analysis and the preview player.
//!
//! `AudioFileDecoder` probes a file, picks its default audio track,
//! and decodes it packet by packet into planar `f32` samples, with encoder delay and padding trimmed.
//! Symphonia trims MP3 files with a LAME tag itself.
//! The decoder trims MP4 files from their edit list, which symphonia leaves alone,
//! and leaves the synthesis delay out of other MP3 files.
//! Both trims match the decoders the C++ application uses, so both applications measure and write the same samples.
//! `DecodeFailureTracker` holds the shared rules for packets that fail to decode:
//! isolated failures are tolerated and replaced with silence,
//! while several failures in a row stop decoding with an error,
//! so no pass silently works on truncated audio.

use std::fs::File;
use std::io::ErrorKind;
use std::path::Path;

use anyhow::{Context, Result, anyhow, bail};
use symphonia::core::codecs::CodecParameters;
use symphonia::core::codecs::audio::well_known::{
    CODEC_ID_AAC, CODEC_ID_ALAC, CODEC_ID_MP1, CODEC_ID_MP2, CODEC_ID_MP3, CODEC_ID_PCM_F32BE,
    CODEC_ID_PCM_F32BE_PLANAR, CODEC_ID_PCM_F32LE, CODEC_ID_PCM_F32LE_PLANAR, CODEC_ID_PCM_F64BE,
    CODEC_ID_PCM_F64BE_PLANAR, CODEC_ID_PCM_F64LE, CODEC_ID_PCM_F64LE_PLANAR, CODEC_ID_VORBIS,
};
use symphonia::core::codecs::audio::{AudioCodecParameters, AudioDecoder, AudioDecoderOptions};
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::probe::Hint;
use symphonia::core::formats::{FormatOptions, FormatReader, SeekMode, SeekTo, TrackType};
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::units::{Duration, Time, TimeBase};
use symphonia_common::apple::audio::alac::MagicCookie;

mod gapless;

/// Bit depth assumed for lossy files, whose formats store no source bit depth.
/// It is also the depth their normalized output is written in.
pub const DEFAULT_LOSSY_BITS_PER_SAMPLE: u32 = 16;

/// Samples of layer III synthesis delay at the start of a decoded MP3 stream.
/// A LAME tag includes them in the delay it records.
/// Without one, JUCE's MP3 reader and Core Audio leave them out, so this decoder does too.
const MP3_DECODER_DELAY_FRAMES: u64 = 529;

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
    /// Source bit depth, the lossy default for formats that store none, or 0 for other lossy codecs.
    pub bits_per_sample: u32,
    /// Container format name, for example "WAV file".
    pub format_name: String,
    /// Playable length in frames, when the container states it.
    pub expected_frames: Option<u64>,
}

/// Tracks packet decode failures for one decode pass over a file.
///
/// Isolated failures are tolerated, because some decoders fail mid-stream on files that others handle fine.
/// A failed packet that reaches the stated end of the stream is tolerated without being counted,
/// because MP3 files often overshoot the decodable audio.
/// Several failures in a row mean the rest of the file cannot be decoded, and the pass must stop.
#[derive(Debug, Default, Clone)]
pub struct DecodeFailureTracker {
    total_frames: Option<u64>,
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
    /// Frames at the start of the stream that are left out, such as AAC priming.
    leading_frames: u64,
    /// Leading frames still to drop from the next decoded packets.
    frames_to_skip: u64,
    /// Total frames to produce, when the container states where the playable audio ends.
    frame_limit: Option<u64>,
    /// Time base of the packet timestamps and durations, which is not always one over the sample rate in MP4.
    time_base: Option<TimeBase>,
    /// True when packet trimming comes only from symphonia's estimated length,
    /// which it guesses from the file size for an MP3 file without a frame count header.
    ignore_packet_trim: bool,
}

impl DecodeFailureTracker {
    /// Number of consecutive failed packets after which decoding stops.
    pub const MAXIMUM_CONSECUTIVE_FAILURES: u32 = 3;

    /// Creates a tracker for a stream with the given length in frames, when the container states it.
    #[must_use]
    pub const fn new(total_frames: Option<u64>) -> Self {
        Self {
            total_frames,
            failed_packets: 0,
            consecutive_failures: 0,
            first_failure_position: None,
            stopped_at: None,
        }
    }

    /// Records a packet that decoded.
    pub const fn record_success(&mut self) {
        self.consecutive_failures = 0;
    }

    /// Records a packet of the given length that failed to decode at the given frame position.
    /// Returns false when decoding must stop, after which `error_message` describes the failure.
    pub fn record_failure(&mut self, position: u64, frames: u64) -> bool {
        if self
            .total_frames
            .is_some_and(|total| position.saturating_add(frames) >= total)
        {
            return true;
        }
        self.failed_packets += 1;
        self.consecutive_failures += 1;
        self.first_failure_position.get_or_insert(position);
        if self.consecutive_failures >= Self::MAXIMUM_CONSECUTIVE_FAILURES {
            self.stopped_at = Some(position);
            return false;
        }
        true
    }

    /// Returns the number of failed packets before the final one.
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
    pub fn error_message(&self) -> String {
        let position = self.stopped_at.unwrap_or_default();
        self.total_frames.map_or_else(
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
        let time_base = track.time_base;
        let mut expected_frames = track.num_frames;
        let has_recorded_delay = track.delay.is_some();
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

        let mp3_without_delay = parameters.codec == CODEC_ID_MP3 && !has_recorded_delay;
        let mp4_gapless = (format.format_info().short_name == "isomp4")
            .then(|| gapless::read_mp4_gapless(path, sample_rate))
            .flatten();
        let mut leading_frames = 0;
        let mut frame_limit = None;
        if let Some(mp4_gapless) = mp4_gapless {
            leading_frames = mp4_gapless.priming;
            frame_limit = mp4_gapless.frames;
            expected_frames =
                frame_limit.or_else(|| expected_frames.map(|frames| frames.saturating_sub(leading_frames)));
        } else if mp3_without_delay {
            leading_frames = MP3_DECODER_DELAY_FRAMES;
            expected_frames = expected_frames.map(|frames| frames.saturating_sub(leading_frames));
        }

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
            failures: DecodeFailureTracker::new(expected_frames),
            position: 0,
            leading_frames,
            frames_to_skip: leading_frames,
            frame_limit,
            time_base,
            ignore_packet_trim: mp3_without_delay,
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
            if self.frame_limit.is_some_and(|limit| self.position >= limit) {
                return Ok(None);
            }
            let mut packet = match self.format.next_packet() {
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
            if self.ignore_packet_trim {
                packet.dur = Duration::new(packet.dur.get() + packet.trim_end.get());
                packet.trim_end = Duration::ZERO;
            }

            match self.decoder.decode(&packet) {
                Ok(buffer) => {
                    self.failures.record_success();
                    if buffer.frames() == 0 {
                        continue;
                    }
                    buffer.copy_to_vecs_planar::<f32>(&mut self.planar);
                    let frames = self.trim_chunk();
                    if frames == 0 {
                        continue;
                    }
                    self.position += frames;
                    return Ok(Some(&self.planar));
                }
                Err(SymphoniaError::DecodeError(_) | SymphoniaError::IoError(_)) => {
                    let packet_frames = self.frames_of(i64::try_from(packet.dur.get()).unwrap_or(i64::MAX));
                    if !self.failures.record_failure(self.position, packet_frames) {
                        bail!(self.failures.error_message());
                    }
                    // Silence for a failed final packet stops at the stated length, so the file keeps its length.
                    let remaining = self
                        .info
                        .expected_frames
                        .map_or(u64::MAX, |total| total.saturating_sub(self.position));
                    let silent_frames = packet_frames
                        .min(u64::from(self.info.sample_rate) * MAX_FAILED_PACKET_SECONDS)
                        .min(remaining) as usize;
                    if silent_frames == 0 {
                        continue;
                    }
                    for channel in &mut self.planar {
                        channel.clear();
                        channel.resize(silent_frames, 0.0);
                    }
                    let frames = self.trim_chunk();
                    if frames == 0 {
                        continue;
                    }
                    self.position += frames;
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
        let stream_seconds = seconds.max(0.0) + self.leading_frames as f64 / f64::from(self.info.sample_rate);
        let time = Time::try_from_secs_f64(stream_seconds).ok_or_else(|| anyhow!("Invalid seek time {seconds}"))?;
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
        self.failures = DecodeFailureTracker::new(self.info.expected_frames);
        let stream_position = self.frames_of(seeked.actual_ts.get());
        self.frames_to_skip = self.leading_frames.saturating_sub(stream_position);
        self.position = stream_position.saturating_sub(self.leading_frames);
        Ok(self.position)
    }

    /// Converts a timestamp or duration in the track's time base to frames at the sample rate.
    fn frames_of(&self, timestamp: i64) -> u64 {
        let Some(time_base) = self.time_base else {
            return u64::try_from(timestamp).unwrap_or(0);
        };
        let numerator = i128::from(timestamp) * i128::from(time_base.numer.get()) * i128::from(self.info.sample_rate);
        let denominator = i128::from(time_base.denom.get());
        u64::try_from((numerator + denominator / 2) / denominator).unwrap_or(if numerator < 0 { 0 } else { u64::MAX })
    }

    /// Drops the leading frames still to skip and the frames past the frame limit from the latest packet,
    /// and returns how many frames remain.
    fn trim_chunk(&mut self) -> u64 {
        let frames = self.planar.first().map_or(0, Vec::len) as u64;
        let skipped = self.frames_to_skip.min(frames);
        let remaining = self
            .frame_limit
            .map_or(u64::MAX, |limit| limit.saturating_sub(self.position));
        let kept = (frames - skipped).min(remaining);
        if skipped > 0 || kept < frames {
            let start = usize::try_from(skipped).unwrap_or(usize::MAX);
            let length = usize::try_from(kept).unwrap_or(usize::MAX);
            for channel in &mut self.planar {
                channel.drain(..start);
                channel.truncate(length);
            }
        }
        self.frames_to_skip -= skipped;
        kept
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
/// MPEG audio, AAC, and Vorbis get the default of 16, because the formats store no sample depth.
/// ALAC states its depth in the magic cookie of its codec configuration,
/// and gets the default too when the cookie does not parse.
/// Floating point PCM states no bit depth in its parameters, so it gets the width of its sample type.
/// Other lossy codecs report 0, since their decoded depth says nothing about the source.
fn source_bits_per_sample(parameters: &AudioCodecParameters) -> u32 {
    if [CODEC_ID_MP1, CODEC_ID_MP2, CODEC_ID_MP3, CODEC_ID_AAC, CODEC_ID_VORBIS].contains(&parameters.codec) {
        return DEFAULT_LOSSY_BITS_PER_SAMPLE;
    }
    if parameters.codec == CODEC_ID_ALAC {
        return parameters
            .extra_data
            .as_deref()
            .and_then(|extra_data| MagicCookie::read(extra_data).ok())
            .map_or(DEFAULT_LOSSY_BITS_PER_SAMPLE, |cookie| u32::from(cookie.bit_depth));
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
        assert!(tracker.record_failure(100, 10));
        tracker.record_success();
        assert!(tracker.record_failure(200, 10));
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
        let mut tracker = DecodeFailureTracker::new(Some(1000));
        assert!(tracker.record_failure(10, 10));
        assert!(tracker.record_failure(20, 10));
        assert!(!tracker.record_failure(30, 10));
        assert_eq!(tracker.error_message(), "Audio decoding failed at sample 30 of 1000");

        let mut tracker = DecodeFailureTracker::default();
        for position in [10, 20, 30] {
            tracker.record_failure(position, 10);
        }
        assert_eq!(tracker.error_message(), "Audio decoding failed at sample 30");
    }

    #[test]
    fn failures_reaching_the_end_are_tolerated_without_counting() {
        let mut tracker = DecodeFailureTracker::new(Some(1000));
        for _ in 0..=DecodeFailureTracker::MAXIMUM_CONSECUTIVE_FAILURES {
            assert!(tracker.record_failure(990, 10));
        }
        assert_eq!(tracker.failed_packet_count(), 0);
        assert!(tracker.notice().is_none());
        assert!(tracker.record_failure(980, 10));
        assert_eq!(tracker.failed_packet_count(), 1);
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
    use std::num::NonZero;

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
    fn aac_priming_and_padding_are_trimmed() {
        let mut decoder = AudioFileDecoder::open(&test_fixtures::fixture(test_fixtures::AAC_FIXTURE)).expect("opens");
        assert_eq!(decoder.info().expected_frames, Some(44_100));
        while decoder.next_chunk().expect("decodes").is_some() {}
        assert_eq!(decoder.position(), 44_100);
    }

    #[test]
    fn mp3_without_frame_count_decodes_every_frame_without_the_synthesis_delay() {
        let mut decoder =
            AudioFileDecoder::open(&test_fixtures::fixture(test_fixtures::NO_XING_MP3_FIXTURE)).expect("opens");
        while decoder.next_chunk().expect("decodes").is_some() {}
        assert_eq!(decoder.position(), test_fixtures::NO_XING_MP3_LENGTH);
    }

    #[test]
    fn seeking_counts_from_the_end_of_the_priming() {
        let mut decoder = AudioFileDecoder::open(&test_fixtures::fixture(test_fixtures::AAC_FIXTURE)).expect("opens");
        let position = decoder.seek(0.5).expect("seeks");
        assert!(position <= 22_050, "{position}");
        let mut frames = position;
        while let Some(chunk) = decoder.next_chunk().expect("decodes") {
            frames += chunk.first().map_or(0, Vec::len) as u64;
        }
        assert_eq!(frames, 44_100);
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
    fn timestamps_convert_from_the_track_time_base() {
        let mut decoder = AudioFileDecoder::open(&test_fixtures::fixture(test_fixtures::AAC_FIXTURE)).expect("opens");
        assert_eq!(decoder.frames_of(44_100), 44_100);
        decoder.time_base = Some(TimeBase::new(
            NonZero::new(1).expect("nonzero"),
            NonZero::new(1000).expect("nonzero"),
        ));
        assert_eq!(decoder.frames_of(500), 22_050);
        assert_eq!(decoder.frames_of(-5), 0);
    }

    #[test]
    fn format_names_follow_the_cpp_application() {
        assert_eq!(format_display_name("wave", Path::new("a.wav")), "WAV file");
        assert_eq!(format_display_name("aiff", Path::new("a.aif")), "AIFF file");
        assert_eq!(format_display_name("mp3", Path::new("a.mp3")), "MP3 file");
        assert_eq!(format_display_name("ogg", Path::new("a.ogg")), "Ogg-Vorbis file");
    }

    #[test]
    fn alac_reports_the_depth_of_its_magic_cookie() {
        let mut cookie = vec![0, 0, 0x10, 0, 0, 24, 40, 10, 14, 2, 0, 255];
        cookie.extend_from_slice(&[0; 8]);
        cookie.extend_from_slice(&48_000_u32.to_be_bytes());
        let mut parameters = AudioCodecParameters::new();
        parameters
            .for_codec(CODEC_ID_ALAC)
            .with_extra_data(cookie.into_boxed_slice());
        assert_eq!(source_bits_per_sample(&parameters), 24);

        parameters.extra_data = None;
        assert_eq!(source_bits_per_sample(&parameters), DEFAULT_LOSSY_BITS_PER_SAMPLE);
    }

    #[test]
    fn lossy_codecs_report_the_default_depth() {
        for codec in [CODEC_ID_MP3, CODEC_ID_AAC, CODEC_ID_VORBIS] {
            let mut parameters = AudioCodecParameters::new();
            parameters.for_codec(codec).with_bits_per_sample(32);
            assert_eq!(source_bits_per_sample(&parameters), DEFAULT_LOSSY_BITS_PER_SAMPLE);
        }
    }
}
