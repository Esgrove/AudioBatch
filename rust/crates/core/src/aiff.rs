//! Minimal AIFF writer for normalized and plugin-processed output.
//!
//! Writes uncompressed big-endian PCM in a `FORM` container with one `COMM` and one `SSND` chunk,
//! at 8, 16, or 24 bits.
//! Float samples convert to integers by rounding the float scaled to the 32-bit range and then keeping the high bits.
//! That is how JUCE converts them in the C++ application,
//! and `scripts/compare_outputs.py` expects both applications to write the same sample values.
//! The header is written with placeholder sizes first and patched in `finish`,
//! once the number of frames is known.

use std::io::{Seek, SeekFrom, Write};

/// Bit depths the writer supports.
pub(crate) const SUPPORTED_BIT_DEPTHS: [u32; 3] = [8, 16, 24];

/// Size of the header before the sample data: `FORM`, `COMM`, and the `SSND` chunk header.
const HEADER_BYTES: u64 = 54;

/// Size of the `COMM` chunk body.
const COMM_BYTES: u32 = 18;

/// Invalid audio layout, AIFF size limits, or an output stream failure.
#[derive(Debug, thiserror::Error)]
pub enum AiffError {
    /// The requested PCM bit depth is unsupported.
    #[error("AIFF output does not support {0}-bit audio")]
    UnsupportedBitDepth(u32),
    /// A sample rate of zero cannot describe AIFF audio.
    #[error("AIFF output needs a sample rate")]
    MissingSampleRate,
    /// The channel count cannot fit the AIFF header.
    #[error("AIFF output needs between 1 and 65535 channels")]
    InvalidChannels,
    /// A block does not match the output's channel count.
    #[error("Expected {expected} channels of audio, got {actual}")]
    ChannelCount {
        /// Channel count in the output header.
        expected: u16,
        /// Channel count in the block.
        actual: usize,
    },
    /// A block's planes have different frame counts.
    #[error("Channels of one block differ in length")]
    ChannelLengths,
    /// The audio would exceed the format's size limit.
    #[error("The normalized audio is too long for an AIFF file")]
    TooLong,
    /// Sample data could not be written.
    #[error("Failed while writing normalized audio data")]
    WriteData(#[source] std::io::Error),
    /// Seeking to update the header failed.
    #[error("Failed to finish the AIFF header")]
    FinishHeader(#[source] std::io::Error),
    /// Flushing the completed output failed.
    #[error("Failed to finish the AIFF file")]
    FinishFile(#[source] std::io::Error),
    /// The initial or updated header could not be written.
    #[error("Failed to write the AIFF header")]
    WriteHeader(#[source] std::io::Error),
}

/// Streams planar float samples into an AIFF file.
pub struct AiffWriter<W: Write + Seek> {
    writer: W,
    channels: u16,
    sample_rate: u32,
    bits_per_sample: u16,
    frames_written: u64,
    /// Interleaved bytes of the block being written, kept to avoid an allocation per block.
    scratch: Vec<u8>,
}

impl<W: Write + Seek> AiffWriter<W> {
    /// Starts an AIFF stream on the writer, which must be positioned at the start of the file.
    ///
    /// # Errors
    ///
    /// Returns an error for an unsupported bit depth, no channels, a zero sample rate,
    /// or when the header cannot be written.
    pub fn new(mut writer: W, channels: u32, sample_rate: u32, bits_per_sample: u32) -> Result<Self, AiffError> {
        if !SUPPORTED_BIT_DEPTHS.contains(&bits_per_sample) {
            return Err(AiffError::UnsupportedBitDepth(bits_per_sample));
        }
        if sample_rate == 0 {
            return Err(AiffError::MissingSampleRate);
        }
        let channels = u16::try_from(channels)
            .ok()
            .filter(|&channels| channels > 0)
            .ok_or(AiffError::InvalidChannels)?;
        let bits_per_sample =
            u16::try_from(bits_per_sample).map_err(|_| AiffError::UnsupportedBitDepth(bits_per_sample))?;
        write_header(&mut writer, channels, sample_rate, bits_per_sample, 0)?;
        Ok(Self {
            writer,
            channels,
            sample_rate,
            bits_per_sample,
            frames_written: 0,
            scratch: Vec::new(),
        })
    }

    /// Appends one block of planar samples, one slice per channel, all of the same length.
    ///
    /// # Errors
    ///
    /// Returns an error when the channel count or lengths do not match,
    /// when the file would outgrow the AIFF size fields, or when writing fails.
    pub(crate) fn write_planar(&mut self, planes: &[&[f32]]) -> Result<(), AiffError> {
        self.write_frames(planes, float_to_int32)
    }

    /// Appends one block of planar PCM values at the writer's bit depth, one slice per channel,
    /// such as -32768 to 32767 for 16 bits, for samples the caller has already quantized.
    /// Values outside that range are clamped.
    ///
    /// # Errors
    ///
    /// Returns an error when the channel count or lengths do not match,
    /// when the file would outgrow the AIFF size fields, or when writing fails.
    pub fn write_planar_pcm(&mut self, planes: &[&[i32]]) -> Result<(), AiffError> {
        let bits = u32::from(self.bits_per_sample);
        let maximum = (1_i32 << (bits - 1)) - 1;
        let minimum = -maximum - 1;
        let shift = 32 - bits;
        self.write_frames(planes, |value: i32| value.clamp(minimum, maximum) << shift)
    }

    /// Appends one block of planar samples, converting each to a 32-bit integer whose high bits are written.
    fn write_frames<T: Copy + Default>(
        &mut self,
        planes: &[&[T]],
        to_int32: impl Fn(T) -> i32,
    ) -> Result<(), AiffError> {
        if planes.len() != usize::from(self.channels) {
            return Err(AiffError::ChannelCount {
                expected: self.channels,
                actual: planes.len(),
            });
        }
        let frames = planes.first().map_or(0, |plane| plane.len());
        if !planes.iter().all(|plane| plane.len() == frames) {
            return Err(AiffError::ChannelLengths);
        }
        let total_frames = self.frames_written + frames as u64;
        data_bytes(total_frames, self.channels, self.bits_per_sample)?;

        let bytes_per_sample = usize::from(self.bits_per_sample / 8);
        self.scratch.clear();
        self.scratch.reserve(frames * planes.len() * bytes_per_sample);
        for frame in 0..frames {
            for plane in planes {
                let sample = plane.get(frame).copied().unwrap_or_default();
                let value = to_int32(sample);
                let bytes = value.to_be_bytes();
                self.scratch.extend(bytes.iter().take(bytes_per_sample));
            }
        }
        self.writer.write_all(&self.scratch).map_err(AiffError::WriteData)?;
        self.frames_written = total_frames;
        Ok(())
    }

    /// Returns the number of frames written so far.
    #[must_use]
    pub const fn frames_written(&self) -> u64 {
        self.frames_written
    }

    /// Pads the sample data to an even length, patches the header sizes, and returns the writer.
    ///
    /// # Errors
    ///
    /// Returns an error when writing or seeking fails.
    pub fn finish(mut self) -> Result<W, AiffError> {
        let data = data_bytes(self.frames_written, self.channels, self.bits_per_sample)?;
        if data % 2 == 1 {
            self.writer.write_all(&[0]).map_err(AiffError::WriteData)?;
        }
        self.writer.seek(SeekFrom::Start(0)).map_err(AiffError::FinishHeader)?;
        write_header(
            &mut self.writer,
            self.channels,
            self.sample_rate,
            self.bits_per_sample,
            self.frames_written,
        )?;
        self.writer.seek(SeekFrom::End(0)).map_err(AiffError::FinishHeader)?;
        self.writer.flush().map_err(AiffError::FinishFile)?;
        Ok(self.writer)
    }
}

/// Converts a float sample to a 32-bit integer like JUCE's `convertFloatsToInts`,
/// clipping at full scale and rounding half to even.
#[must_use]
pub(crate) fn float_to_int32(sample: f32) -> i32 {
    let sample = f64::from(sample);
    if sample <= -1.0 {
        i32::MIN
    } else if sample >= 1.0 {
        i32::MAX
    } else {
        (f64::from(i32::MAX) * sample).round_ties_even() as i32
    }
}

/// Returns the number of sample data bytes for the frame count,
/// or an error when it does not fit the 32-bit AIFF size fields.
fn data_bytes(frames: u64, channels: u16, bits_per_sample: u16) -> Result<u32, AiffError> {
    u32::try_from(frames)
        .ok()
        .and_then(|_| frames.checked_mul(u64::from(channels) * u64::from(bits_per_sample / 8)))
        .filter(|&bytes| bytes + HEADER_BYTES < u64::from(u32::MAX))
        .and_then(|bytes| u32::try_from(bytes).ok())
        .ok_or(AiffError::TooLong)
}

/// Writes the `FORM`, `COMM`, and `SSND` headers for the given number of frames.
fn write_header(
    writer: &mut impl Write,
    channels: u16,
    sample_rate: u32,
    bits_per_sample: u16,
    frames: u64,
) -> Result<(), AiffError> {
    let data = data_bytes(frames, channels, bits_per_sample)?;
    let frames = u32::try_from(frames).map_err(|_| AiffError::TooLong)?;
    let padding = data % 2;
    let form_size = 4 + (8 + COMM_BYTES) + (8 + 8 + data + padding);
    let ssnd_size = 8 + data;

    let mut header = Vec::with_capacity(54);
    header.extend_from_slice(b"FORM");
    header.extend_from_slice(&form_size.to_be_bytes());
    header.extend_from_slice(b"AIFF");
    header.extend_from_slice(b"COMM");
    header.extend_from_slice(&COMM_BYTES.to_be_bytes());
    header.extend_from_slice(&channels.to_be_bytes());
    header.extend_from_slice(&frames.to_be_bytes());
    header.extend_from_slice(&bits_per_sample.to_be_bytes());
    header.extend_from_slice(&extended_float(sample_rate));
    header.extend_from_slice(b"SSND");
    header.extend_from_slice(&ssnd_size.to_be_bytes());
    header.extend_from_slice(&0_u32.to_be_bytes());
    header.extend_from_slice(&0_u32.to_be_bytes());
    writer.write_all(&header).map_err(AiffError::WriteHeader)
}

/// Encodes a positive integer as the 80-bit IEEE extended float AIFF stores the sample rate in.
fn extended_float(value: u32) -> [u8; 10] {
    let mut bytes = [0_u8; 10];
    if value == 0 {
        return bytes;
    }
    let shift = value.leading_zeros();
    let exponent = 16_383 + 31 - u16::try_from(shift).unwrap_or_default();
    let mantissa = u64::from(value) << (32 + shift);
    let (exponent_bytes, mantissa_bytes) = bytes.split_at_mut(2);
    exponent_bytes.copy_from_slice(&exponent.to_be_bytes());
    mantissa_bytes.copy_from_slice(&mantissa.to_be_bytes());
    bytes
}

#[cfg(test)]
mod test_sample_encoding {
    use super::*;

    #[test]
    fn clips_at_full_scale() {
        assert_eq!(float_to_int32(1.5), i32::MAX);
        assert_eq!(float_to_int32(1.0), i32::MAX);
        assert_eq!(float_to_int32(-1.0), i32::MIN);
        assert_eq!(float_to_int32(-2.0), i32::MIN);
    }

    #[test]
    fn scales_to_the_32_bit_range() {
        assert_eq!(float_to_int32(0.0), 0);
        assert_eq!(float_to_int32(0.5), 1_073_741_824);
        assert_eq!(float_to_int32(-0.5), -1_073_741_824);
    }

    #[test]
    fn encodes_common_sample_rates() {
        assert_eq!(extended_float(44_100), [0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0]);
        assert_eq!(extended_float(48_000), [0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0]);
        assert_eq!(extended_float(1), [0x3F, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0]);
    }
}

#[cfg(test)]
mod test_aiff_writer {
    use std::io::Cursor;

    use super::*;
    use crate::decode::AudioFileDecoder;

    /// Writes the planes to an AIFF file in a temporary directory and returns its path.
    fn write_file(directory: &tempfile::TempDir, planes: &[&[f32]], bits: u32) -> std::path::PathBuf {
        let path = directory.path().join("out.aif");
        let file = std::fs::File::create(&path).expect("creates file");
        let mut writer = AiffWriter::new(file, planes.len() as u32, 44_100, bits).expect("starts writer");
        writer.write_planar(planes).expect("writes");
        writer.finish().expect("finishes");
        path
    }

    #[test]
    fn rejects_unsupported_bit_depths() {
        assert!(matches!(
            AiffWriter::new(Cursor::new(Vec::new()), 2, 44_100, 32),
            Err(AiffError::UnsupportedBitDepth(32))
        ));
        assert!(matches!(
            AiffWriter::new(Cursor::new(Vec::new()), 0, 44_100, 16),
            Err(AiffError::InvalidChannels)
        ));
        assert!(matches!(
            AiffWriter::new(Cursor::new(Vec::new()), 2, 0, 16),
            Err(AiffError::MissingSampleRate)
        ));
    }

    #[test]
    fn write_errors_keep_context_and_the_io_source() {
        let error = AiffError::WriteData(std::io::Error::new(std::io::ErrorKind::WriteZero, "disk full"));
        assert_eq!(error.to_string(), "Failed while writing normalized audio data");
        assert_eq!(
            std::error::Error::source(&error).expect("I/O cause").to_string(),
            "disk full"
        );
    }

    #[test]
    fn rejects_mismatched_blocks_with_typed_errors() {
        let mut writer = AiffWriter::new(Cursor::new(Vec::new()), 2, 44_100, 16).expect("starts writer");
        assert!(matches!(
            writer.write_planar(&[&[0.0]]),
            Err(AiffError::ChannelCount { expected: 2, actual: 1 })
        ));
        assert!(matches!(
            writer.write_planar(&[&[0.0], &[]]),
            Err(AiffError::ChannelLengths)
        ));
    }

    #[test]
    fn writes_big_endian_interleaved_samples() {
        let left = [0.5_f32, -0.5];
        let right = [0.25_f32, 0.0];
        let mut writer = AiffWriter::new(Cursor::new(Vec::new()), 2, 44_100, 16).expect("starts writer");
        writer.write_planar(&[&left, &right]).expect("writes");
        let bytes = writer.finish().expect("finishes").into_inner();
        assert_eq!(bytes.len(), 54 + 8);
        assert_eq!(
            bytes.get(54..),
            Some([0x40, 0x00, 0x20, 0x00, 0xC0, 0x00, 0x00, 0x00].as_slice())
        );
        assert_eq!(bytes.get(..4), Some(b"FORM".as_slice()));
        assert_eq!(bytes.get(4..8), Some((54_u32 + 8 - 8).to_be_bytes().as_slice()));
        assert_eq!(bytes.get(22..26), Some(2_u32.to_be_bytes().as_slice()));
    }

    #[test]
    fn writes_quantized_values_unchanged_and_clamps_the_rest() {
        let left = [32_767_i32, -32_768, 1, 40_000];
        let right = [-1_i32, 0, -40_000, 256];
        let mut writer = AiffWriter::new(Cursor::new(Vec::new()), 2, 44_100, 16).expect("starts writer");
        writer.write_planar_pcm(&[&left, &right]).expect("writes");
        let bytes = writer.finish().expect("finishes").into_inner();
        let expected: Vec<u8> = [32_767_i16, -1, -32_768, 0, 1, -32_768, 32_767, 256]
            .iter()
            .flat_map(|value| value.to_be_bytes())
            .collect();
        assert_eq!(bytes.get(54..), Some(expected.as_slice()));

        let mut writer = AiffWriter::new(Cursor::new(Vec::new()), 1, 44_100, 24).expect("starts writer");
        writer.write_planar_pcm(&[&[-8_388_608, 8_388_607]]).expect("writes");
        let bytes = writer.finish().expect("finishes").into_inner();
        assert_eq!(bytes.get(54..), Some([0x80, 0, 0, 0x7F, 0xFF, 0xFF].as_slice()));
    }

    #[test]
    fn pads_odd_sample_data() {
        let samples = [0.5_f32];
        let mut writer = AiffWriter::new(Cursor::new(Vec::new()), 1, 44_100, 8).expect("starts writer");
        writer.write_planar(&[&samples]).expect("writes");
        let bytes = writer.finish().expect("finishes").into_inner();
        assert_eq!(bytes.len(), 54 + 2);
        assert_eq!(bytes.get(54..), Some([0x40, 0x00].as_slice()));
        assert_eq!(bytes.get(4..8), Some((54_u32 + 2 - 8).to_be_bytes().as_slice()));
    }

    #[test]
    fn written_file_decodes_to_the_same_samples() {
        let directory = tempfile::tempdir().expect("creates directory");
        let left: Vec<f32> = (0..1000).map(|index| (index as f32 / 1000.0) - 0.5).collect();
        let right: Vec<f32> = left.iter().map(|sample| -sample).collect();
        let path = write_file(&directory, &[&left, &right], 24);

        let mut decoder = AudioFileDecoder::open(&path).expect("opens output");
        assert_eq!(decoder.info().channels, 2);
        assert_eq!(decoder.info().sample_rate, 44_100);
        assert_eq!(decoder.info().bits_per_sample, 24);
        assert_eq!(decoder.info().expected_frames, Some(1000));
        let mut decoded_left = Vec::new();
        while let Some(chunk) = decoder.next_chunk().expect("decodes") {
            decoded_left.extend_from_slice(chunk.first().expect("left channel"));
        }
        assert_eq!(decoded_left.len(), left.len());
        for (decoded, original) in decoded_left.iter().zip(&left) {
            assert!((decoded - original).abs() < 1e-6, "{decoded} != {original}");
        }
    }
}
