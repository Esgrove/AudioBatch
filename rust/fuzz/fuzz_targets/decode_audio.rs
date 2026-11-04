//! libFuzzer target for probing and decoding audio files with `AudioFileDecoder`.
//!
//! The first input byte selects the file extension, which guides the container probe,
//! and the remaining bytes are the file.
//! Inputs and decoded lengths are bounded so that one input stays fast,
//! because a valid header can state hours of audio.
//! The decoder must return samples or an error for any file, and keep every channel the same length.

#![no_main]

use std::io::Write as _;
use std::sync::OnceLock;

use audiobatch_core::decode::AudioFileDecoder;
use libfuzzer_sys::fuzz_target;
use tempfile::TempDir;

/// Largest file that is decoded, which keeps each input fast while fitting short files of every format.
const MAXIMUM_INPUT_BYTES: usize = 256 * 1024;

/// Most frames decoded from one input, about 20 seconds at 48 kHz.
const MAXIMUM_DECODED_FRAMES: u64 = 1 << 20;

/// File extensions the selector byte chooses from, one for each container the analysis supports.
const EXTENSIONS: [&str; 8] = ["wav", "aiff", "flac", "mp3", "m4a", "ogg", "caf", "aac"];

/// The folder that holds the file of the current input, removed when the process exits normally.
static FOLDER: OnceLock<TempDir> = OnceLock::new();

fuzz_target!(|input: &[u8]| {
    let Some((&selector, file)) = input.split_first() else {
        return;
    };
    if file.len() > MAXIMUM_INPUT_BYTES {
        return;
    }
    let extension = EXTENSIONS
        .get(usize::from(selector) % EXTENSIONS.len())
        .copied()
        .unwrap_or("wav");
    let folder = FOLDER.get_or_init(|| tempfile::tempdir().expect("temporary folder for the fuzz input"));
    let path = folder.path().join(format!("input.{extension}"));
    let mut output = std::fs::File::create(&path).expect("creates the fuzz input file");
    output.write_all(file).expect("writes the fuzz input file");
    drop(output);

    let Ok(mut decoder) = AudioFileDecoder::open(&path) else {
        return;
    };
    let channels = decoder.info().channels as usize;
    decode_bounded(&mut decoder, channels);
    if decoder.seek(0.0).is_ok() {
        decode_bounded(&mut decoder, channels);
    }
    let _ = decoder.failures().notice();
});

/// Decodes until the end of the stream, an error, or the frame bound,
/// and requires every chunk to hold the same number of frames in each of the stream's channels.
fn decode_bounded(decoder: &mut AudioFileDecoder, channels: usize) {
    let mut decoded_frames = 0_u64;
    while decoded_frames < MAXIMUM_DECODED_FRAMES {
        let Ok(Some(chunk)) = decoder.next_chunk() else {
            return;
        };
        assert_eq!(
            chunk.len(),
            channels,
            "a chunk has another channel count than the stream"
        );
        let frames = chunk.first().map_or(0, Vec::len);
        assert!(
            chunk.iter().all(|channel| channel.len() == frames),
            "the channels of a chunk have different lengths"
        );
        decoded_frames += frames as u64;
    }
}
