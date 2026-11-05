//! AAC-LC reconstruction through macOS Core Audio, using the caller's already-open file.
//!
//! PNS replaces coded noise bands with decoder-generated random spectra before inverse MDCT and overlap-add.
//! Symphonia fixes its private generator to `FFmpeg`'s sequence, which differs from Core Audio's reconstruction.
//! No public setup option changes either generator. Post-decode correction cannot recover the coded bands.
//! Using the same system AAC backend as the C++ application reconciles those samples without discarding audio.
//! Selection is limited to mono/stereo AAC-LC in MP4. Other codecs and platforms retain Symphonia.
//! Core Audio owns edit-list trimming and seeking for this backend, so the generic MP4 trim is not applied twice.
//! Native read failures stop the pass with their original status, never silently switching reconstruction mid-stream.

#![expect(
    unsafe_code,
    reason = "Core Audio C handles, synchronous callbacks, and checked PCM buffers"
)]

use std::ffi::c_void;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::ptr::{self, NonNull};

use objc2_audio_toolbox::{
    AudioFileClose, AudioFileID, AudioFileOpenWithCallbacks, ExtAudioFileDispose, ExtAudioFileGetProperty,
    ExtAudioFileRead, ExtAudioFileRef, ExtAudioFileSeek, ExtAudioFileSetProperty, ExtAudioFileWrapAudioFileID,
    kExtAudioFileProperty_ClientDataFormat, kExtAudioFileProperty_FileDataFormat,
    kExtAudioFileProperty_FileLengthFrames,
};
use objc2_core_audio_types::{
    AudioBuffer, AudioBufferList, AudioStreamBasicDescription, kAudioFormatFlagsNativeFloatPacked,
    kAudioFormatLinearPCM, kAudioFormatMPEG4AAC,
};
use symphonia::core::codecs::audio::AudioCodecParameters;
use symphonia::core::codecs::audio::well_known::CODEC_ID_AAC;
use symphonia_common::mpeg::audio::{AudioObjectType, AudioSpecificConfig};

use super::DecodeError;

/// Bounded native read size, matching the reference reader's maximum read.
const READ_FRAMES: u32 = 8192;
/// Core Audio's generic file error, also used for invalid callback offsets and I/O failures.
const FILE_ERROR: i32 = i32::from_be_bytes(*b"wht?");
/// Core Audio's unsupported format status, used when native and probed geometry disagree.
const FORMAT_ERROR: i32 = i32::from_be_bytes(*b"fmt?");

/// Sole owner of one native decoder and its stable callback file.
/// Handles are disposed before the boxed file, including partial initialization failures.
pub(super) struct NativeAacDecoder {
    audio_file: AudioFileID,
    extended_file: ExtAudioFileRef,
    file: Box<File>,
    interleaved: Vec<f32>,
    channels: u32,
    frames: u64,
}

// SAFETY: The decoder has sole ownership of its handles and callback file.
// All native operations require exclusive access, callbacks are synchronous, and no thread-local state is retained.
unsafe impl Send for NativeAacDecoder {}

impl NativeAacDecoder {
    /// Opens read-only callbacks over the existing source handle and prepares native float PCM at the source rate.
    pub(super) fn open(file: File, sample_rate: u32, channels: u32) -> Result<Self, DecodeError> {
        if !(1..=2).contains(&channels) {
            return Err(DecodeError::NativeAac {
                position: 0,
                status: FORMAT_ERROR,
            });
        }
        let mut decoder = Self {
            audio_file: ptr::null_mut(),
            extended_file: ptr::null_mut(),
            file: Box::new(file),
            interleaved: vec![0.0; READ_FRAMES as usize * channels as usize],
            channels,
            frames: 0,
        };
        check(
            // SAFETY: The boxed file has a stable address until both handles are closed.
            // Read-only callbacks never mutate it or reopen its path, and the output handle is valid storage.
            unsafe {
                AudioFileOpenWithCallbacks(
                    NonNull::from(decoder.file.as_mut()).cast(),
                    Some(read_file),
                    None,
                    Some(file_size),
                    None,
                    0,
                    NonNull::from(&mut decoder.audio_file),
                )
            },
            0,
        )?;
        check(
            // SAFETY: The audio handle was opened successfully and remains owned by decoder.
            // The wrapper does not take ownership of that handle.
            unsafe {
                ExtAudioFileWrapAudioFileID(decoder.audio_file, false, NonNull::from(&mut decoder.extended_file))
            },
            0,
        )?;
        let mut source = AudioStreamBasicDescription {
            mSampleRate: 0.0,
            mFormatID: 0,
            mFormatFlags: 0,
            mBytesPerPacket: 0,
            mFramesPerPacket: 0,
            mBytesPerFrame: 0,
            mChannelsPerFrame: 0,
            mBitsPerChannel: 0,
            mReserved: 0,
        };
        decoder.property(kExtAudioFileProperty_FileDataFormat, &mut source)?;
        let mut frames = 0_i64;
        decoder.property(kExtAudioFileProperty_FileLengthFrames, &mut frames)?;
        if source.mFormatID != kAudioFormatMPEG4AAC
            || (source.mSampleRate - f64::from(sample_rate)).abs() > f64::EPSILON
            || source.mChannelsPerFrame != channels
            || !(1..=2).contains(&channels)
            || frames < 0
        {
            return Err(DecodeError::NativeAac {
                position: 0,
                status: FORMAT_ERROR,
            });
        }
        decoder.frames = frames as u64;
        let bytes_per_frame = size_of::<f32>() as u32 * channels;
        let mut output = AudioStreamBasicDescription {
            mSampleRate: f64::from(sample_rate),
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagsNativeFloatPacked,
            mBytesPerPacket: bytes_per_frame,
            mFramesPerPacket: 1,
            mBytesPerFrame: bytes_per_frame,
            mChannelsPerFrame: channels,
            mBitsPerChannel: size_of::<f32>() as u32 * u8::BITS,
            mReserved: 0,
        };
        check(
            // SAFETY: The wrapper is live and the property receives the complete PCM format description.
            unsafe {
                ExtAudioFileSetProperty(
                    decoder.extended_file,
                    kExtAudioFileProperty_ClientDataFormat,
                    size_of::<AudioStreamBasicDescription>() as u32,
                    NonNull::from(&mut output).cast(),
                )
            },
            0,
        )?;
        Ok(decoder)
    }

    /// Returns native playable frames, excluding MP4 priming and padding exactly once.
    pub(super) const fn frames(&self) -> u64 {
        self.frames
    }

    /// Reads bounded interleaved native PCM and presents the usual planar decoder contract.
    pub(super) fn read(&mut self, planar: &mut [Vec<f32>], position: u64) -> Result<u64, DecodeError> {
        if planar.len() != self.channels as usize {
            return Err(DecodeError::NativeAac {
                position,
                status: FORMAT_ERROR,
            });
        }
        let mut frames = self.frames.saturating_sub(position).min(u64::from(READ_FRAMES)) as u32;
        if frames == 0 {
            return Ok(0);
        }
        let mut buffers = AudioBufferList {
            mNumberBuffers: 1,
            mBuffers: [AudioBuffer {
                mNumberChannels: self.channels,
                mDataByteSize: frames * self.channels * size_of::<f32>() as u32,
                mData: self.interleaved.as_mut_ptr().cast(),
            }],
        };
        check(
            // SAFETY: Exactly one interleaved buffer is provided, with space for every requested frame and channel.
            // The native read is synchronous and neither pointer escapes it.
            unsafe {
                ExtAudioFileRead(
                    self.extended_file,
                    NonNull::from(&mut frames),
                    NonNull::from(&mut buffers),
                )
            },
            position,
        )?;
        if frames > READ_FRAMES {
            return Err(DecodeError::NativeAac {
                position,
                status: FILE_ERROR,
            });
        }
        for (channel, plane) in planar.iter_mut().enumerate() {
            plane.clear();
            plane.extend(
                self.interleaved
                    .chunks_exact(self.channels as usize)
                    .take(frames as usize)
                    .filter_map(|frame| frame.get(channel).copied()),
            );
        }
        Ok(u64::from(frames))
    }

    /// Seeks on the playable MP4 timeline, preserving native decoder pre-roll and overlap reconstruction.
    pub(super) fn seek(&mut self, position: u64) -> Result<(), DecodeError> {
        let position = position.min(self.frames);
        let offset = i64::try_from(position).map_err(|_| DecodeError::NativeAac {
            position,
            status: FILE_ERROR,
        })?;
        check(
            // SAFETY: The native wrapper is live and the offset is bounded by its nonnegative i64 frame length.
            unsafe { ExtAudioFileSeek(self.extended_file, offset) },
            position,
        )
    }

    /// Reads a fixed-size native property and rejects a mismatched output size.
    fn property<T>(&self, property: u32, value: &mut T) -> Result<(), DecodeError> {
        let mut size = size_of::<T>() as u32;
        // SAFETY: Callers supply initialized, aligned storage of the documented property type and exact byte size.
        let status = unsafe {
            ExtAudioFileGetProperty(
                self.extended_file,
                property,
                NonNull::from(&mut size),
                NonNull::from(value).cast(),
            )
        };
        check(status, 0)?;
        if size != size_of::<T>() as u32 {
            return Err(DecodeError::NativeAac {
                position: 0,
                status: FILE_ERROR,
            });
        }
        Ok(())
    }
}

impl Drop for NativeAacDecoder {
    fn drop(&mut self) {
        // SAFETY: Both handles are uniquely owned and checked for successful initialization.
        // Disposing the wrapper first leaves its callback file and underlying AudioFile alive until close completes.
        unsafe {
            if !self.extended_file.is_null() {
                ExtAudioFileDispose(self.extended_file);
            }
            if !self.audio_file.is_null() {
                AudioFileClose(self.audio_file);
            }
        }
    }
}

/// Selects only the existing AAC-LC mono/stereo MP4 contract, not ALAC, HE-AAC, or unrelated native codecs.
pub(super) fn supports(parameters: &AudioCodecParameters, container: &str) -> bool {
    parameters.codec == CODEC_ID_AAC
        && container == "isomp4"
        && parameters
            .channels
            .as_ref()
            .is_some_and(|channels| (1..=2).contains(&channels.count()))
        && parameters
            .extra_data
            .as_deref()
            .and_then(|data| AudioSpecificConfig::read(data).ok())
            .is_some_and(|config| {
                config.object_type == AudioObjectType::Lc && !config.sbr_present && !config.ps_present
            })
}

/// Converts an `OSStatus` into an owned decoder failure without hiding the native status or frame position.
const fn check(status: i32, position: u64) -> Result<(), DecodeError> {
    if status == 0 {
        Ok(())
    } else {
        Err(DecodeError::NativeAac { position, status })
    }
}

/// Supplies bytes from the original open file with positional reads, independent of Symphonia's shared cursor.
unsafe extern "C-unwind" fn read_file(
    file: NonNull<c_void>,
    position: i64,
    requested: u32,
    buffer: NonNull<c_void>,
    actual: NonNull<u32>,
) -> i32 {
    // SAFETY: AudioFile invokes callbacks synchronously with the stable boxed File supplied at open.
    // Its output buffer is valid for requested bytes and actual points to writable u32 storage.
    let (file, buffer, actual) = unsafe {
        (
            file.cast::<File>().as_ref(),
            std::slice::from_raw_parts_mut(buffer.as_ptr().cast::<u8>(), requested as usize),
            &mut *actual.as_ptr(),
        )
    };
    *actual = 0;
    let Ok(position) = u64::try_from(position) else {
        return FILE_ERROR;
    };
    while (*actual as usize) < buffer.len() {
        let Some(remaining) = buffer.get_mut(*actual as usize..) else {
            return FILE_ERROR;
        };
        let Some(offset) = position.checked_add(u64::from(*actual)) else {
            return FILE_ERROR;
        };
        match file.read_at(remaining, offset) {
            Ok(0) => break,
            Ok(count) => *actual += count as u32,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(_) => return FILE_ERROR,
        }
    }
    0
}

/// Returns the length of the original file without consulting a possibly replaced path.
unsafe extern "C-unwind" fn file_size(file: NonNull<c_void>) -> i64 {
    // SAFETY: The boxed File remains live until AudioFileClose has completed.
    let file = unsafe { file.cast::<File>().as_ref() };
    file.metadata()
        .ok()
        .and_then(|metadata| i64::try_from(metadata.len()).ok())
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::decode::AudioFileDecoder;

    /// Collects every planar chunk while checking bounded native reads and the playable frame count.
    fn decode(path: &std::path::Path) -> Vec<Vec<f32>> {
        let mut decoder = AudioFileDecoder::open(path).expect("opens AAC fixture");
        assert!(decoder.native_aac.is_some());
        let mut samples = vec![Vec::new(); decoder.info().channels as usize];
        while let Some(chunk) = decoder.next_chunk().expect("native AAC chunk") {
            for (plane, values) in samples.iter_mut().zip(chunk) {
                assert!(values.len() <= READ_FRAMES as usize);
                plane.extend_from_slice(values);
            }
        }
        assert_eq!(decoder.position(), 44_100);
        assert!(decoder.failures().notice().is_none());
        samples
    }

    /// Native selection is codec/configuration based, not a filename or a blanket system-decoder preference.
    #[test]
    fn selection_is_limited_to_lc_mono_stereo_mp4() {
        let mut parameters = AudioCodecParameters::new();
        parameters
            .for_codec(CODEC_ID_AAC)
            .with_extra_data(Box::new([0x12, 0x10]));
        let config = AudioSpecificConfig::read(&[0x12, 0x10]).expect("stereo LC config");
        parameters.channels = config.channels;
        assert!(supports(&parameters, "isomp4"));
        assert!(!supports(&parameters, "adts"));
        parameters.codec = symphonia::core::codecs::audio::well_known::CODEC_ID_ALAC;
        assert!(!supports(&parameters, "isomp4"));
        parameters.codec = CODEC_ID_AAC;
        parameters.extra_data = Some(Box::new([0x2b, 0x92, 0x08, 0x00]));
        assert!(!supports(&parameters, "isomp4"));
        parameters.extra_data = Some(Box::new([0x12, 0x30]));
        parameters.channels = AudioSpecificConfig::read(&[0x12, 0x30]).expect("5.1 config").channels;
        assert!(!supports(&parameters, "isomp4"));
        parameters.extra_data = Some(Box::new([0x12, 0x08]));
        parameters.channels = AudioSpecificConfig::read(&[0x12, 0x08]).expect("mono config").channels;
        assert!(supports(&parameters, "isomp4"));
    }

    /// Native decoding starts at the edit-list origin, not at the encoded priming packet or a discarded audio block.
    #[test]
    fn playable_timeline_keeps_startup_audio_and_both_channels() {
        let path = test_fixtures::fixture(test_fixtures::AAC_FIXTURE);
        let samples = decode(&path);
        assert_eq!(samples.len(), 2);
        for plane in samples {
            assert_eq!(plane.len(), 44_100);
            assert!(plane.iter().take(1024).any(|sample| sample.abs() > 0.25));
        }
    }

    /// A path replacement after open cannot redirect native callbacks onto a different source identity.
    #[test]
    fn callbacks_keep_the_original_open_file_after_path_replacement() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let path = test_fixtures::copy_fixture(&directory, test_fixtures::AAC_FIXTURE, None);
        let expected = decode(&path);
        let original = File::open(&path).expect("source handle");
        std::fs::rename(&path, directory.path().join("original.m4a")).expect("rename original");
        std::fs::write(&path, b"not the original source").expect("replace path");
        let mut decoder = NativeAacDecoder::open(original, 44_100, 2).expect("opens original handle");
        let mut actual = vec![Vec::new(); 2];
        let mut chunk = vec![Vec::new(); 2];
        let mut position = 0;
        loop {
            let frames = decoder.read(&mut chunk, position).expect("original bytes decode");
            if frames == 0 {
                break;
            }
            for (plane, values) in actual.iter_mut().zip(&chunk) {
                plane.extend_from_slice(values);
            }
            position += frames;
        }
        for (actual, expected) in actual.iter().flatten().zip(expected.iter().flatten()) {
            assert_eq!(actual.to_bits(), expected.to_bits());
        }
        assert_eq!(position, 44_100);
    }

    /// Seeking is expressed on the playable timeline and retains the source length and meaningful samples.
    #[test]
    fn native_seek_retains_edit_list_timeline_and_rejects_nonfinite_time() {
        let path = test_fixtures::fixture(test_fixtures::AAC_FIXTURE);
        let mut decoder = AudioFileDecoder::open(&path).expect("opens");
        assert!(decoder.seek(f64::NAN).is_err());
        assert!(decoder.seek(f64::INFINITY).is_err());
        assert_eq!(decoder.seek(0.5).expect("seeks"), 22_050);
        let chunk = decoder.next_chunk().expect("read").expect("samples");
        assert!(chunk.iter().flatten().any(|value| value.abs() > 0.25));
        while decoder.next_chunk().expect("decodes remainder").is_some() {}
        assert_eq!(decoder.position(), 44_100);
        assert_eq!(decoder.seek(10.0).expect("clamps seek"), 44_100);
        assert!(decoder.next_chunk().expect("end").is_none());
        assert_eq!(decoder.seek(0.0).expect("seeks to start"), 0);
        assert!(decoder.next_chunk().expect("read").is_some());
    }

    /// Native setup failures preserve their concrete status and release all partially initialized handles.
    #[test]
    fn invalid_geometry_and_non_audio_input_are_rejected() {
        let path = test_fixtures::fixture(test_fixtures::AAC_FIXTURE);
        assert!(NativeAacDecoder::open(File::open(&path).expect("file"), 44_100, 0).is_err());
        assert!(NativeAacDecoder::open(File::open(&path).expect("file"), 48_000, 2).is_err());
        assert!(NativeAacDecoder::open(File::open(&path).expect("file"), 44_100, 1).is_err());
        let broken = test_fixtures::fixture("broken.wav");
        assert!(NativeAacDecoder::open(File::open(&broken).expect("file"), 44_100, 2).is_err());
    }
}
