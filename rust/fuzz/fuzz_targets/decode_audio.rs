//! libFuzzer target for probing and decoding audio files with `AudioFileDecoder`.
//!
//! The first input byte selects the file extension, which guides the container probe,
//! and the remaining bytes are the file.
//! Input size, accepted stream dimensions, chunk calls, and decoded frames have separate bounds.
//! Probe and codec allocations happen before chunk checks, so fuzz.sh also enforces libFuzzer allocation,
//! RSS, and per-input time limits. Exceeding those limits is a finding, not an ordinary decoder error.
//! Only the exact committed WAV regression checks the production panic boundary.
//! All other panics remain findings, including mutations that hit the same upstream defect.

#![no_main]

use std::io::Write as _;
use std::sync::OnceLock;

use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_fuzz::{DecoderRegression, check_decoder_production_boundary, install_decoder_regression_hook};
use libfuzzer_sys::fuzz_target;
use tempfile::TempDir;

/// Largest file that is decoded, which keeps each input fast while fitting short files of every format.
const MAXIMUM_INPUT_BYTES: usize = 256 * 1024;

/// Most frames requested per pass, with at most one packet of overshoot.
const MAXIMUM_DECODED_FRAMES: u64 = 1 << 16;

/// Most chunks requested per pass, independent of whether the decoder makes frame progress.
const MAXIMUM_CHUNKS: usize = 256;

/// Stream dimensions admitted after probing. Earlier allocations remain subject to libFuzzer's limits.
const MAXIMUM_CHANNELS: u32 = 8;

/// Largest sample rate admitted, which also bounds silence inserted for a failed packet.
const MAXIMUM_SAMPLE_RATE: u32 = 192_000;

/// File extensions the selector byte chooses from, one for each container the analysis supports.
const EXTENSIONS: [&str; 6] = ["wav", "aiff", "flac", "mp3", "m4a", "aac"];

/// The folder that holds the file of the current input, removed when the process exits normally.
static FOLDER: OnceLock<TempDir> = OnceLock::new();

fuzz_target!(init: install_decoder_regression_hook(), |input: &[u8]| {
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

    if let Some(regression) = DecoderRegression::from_input(input) {
        check_decoder_production_boundary(&path, regression);
        return;
    }
    let Ok(mut decoder) = AudioFileDecoder::open(&path) else {
        return;
    };
    if decoder.info().channels > MAXIMUM_CHANNELS || decoder.info().sample_rate > MAXIMUM_SAMPLE_RATE {
        return;
    }
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
    for _ in 0..MAXIMUM_CHUNKS {
        if decoded_frames >= MAXIMUM_DECODED_FRAMES {
            return;
        }
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
