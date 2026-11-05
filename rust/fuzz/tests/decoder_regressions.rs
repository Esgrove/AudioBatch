//! Checked arithmetic regressions and subprocess checks for exact-input production unwind allowances.

use std::path::Path;
use std::process::{Command, Output};

use audiobatch_core::analysis::analyze_file_catching_panics;
use audiobatch_core::decode::{AudioFileDecoder, DecodeError};
use audiobatch_core::record::AnalysisError;
use audiobatch_fuzz::{DecoderRegression, check_decoder_production_boundary, install_decoder_regression_hook};

/// Runs an isolated test process so panic hooks never change another test's behavior.
fn child(mode: &str) -> Output {
    Command::new(std::env::current_exe().expect("test executable"))
        .args(["--exact", "regression_child", "--nocapture"])
        .env("AUDIOBATCH_FUZZ_REGRESSION", mode)
        .output()
        .expect("runs regression child")
}

/// Requires the scenario to complete, including any assertions on the production failure record.
fn succeeds(mode: &str) {
    let output = child(mode);
    assert!(output.status.success(), "{}", String::from_utf8_lossy(&output.stderr));
}

/// Requires an unallowed panic to terminate the child, not silently become an ordinary decoder error.
fn aborts(mode: &str, message: &str) {
    let output = child(mode);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains(message));
}

#[test]
fn production_boundary_catches_the_exact_wav_overflow() {
    succeeds("wav-production");
}

#[test]
fn production_boundary_catches_the_exact_caf_overflow() {
    succeeds("caf-production");
}

#[test]
fn raw_wav_overflow_still_aborts() {
    aborts("wav-raw", "attempt to multiply with overflow");
}

#[test]
fn raw_caf_overflow_still_aborts() {
    aborts("caf-raw", "attempt to multiply with overflow");
}

#[test]
fn a_mutated_regression_is_not_allowlisted() {
    aborts("wav-mutated", "attempt to multiply with overflow");
}

#[test]
fn the_wrong_dependency_location_aborts_inside_production() {
    aborts("caf-wrong-location", "attempt to multiply with overflow");
}

#[test]
fn the_fuzz_discovered_wav_variant_remains_a_visible_failure() {
    aborts("wav-unallowlisted", "attempt to multiply with overflow");
}

#[test]
fn prefixed_wav_overflow_still_aborts() {
    aborts("wav-prefixed", "attempt to multiply with overflow");
}

#[test]
fn prefixed_caf_overflow_still_aborts() {
    aborts("caf-prefixed", "attempt to multiply with overflow");
}

#[test]
fn oversized_extensible_channel_count_still_aborts() {
    aborts("wav-extensible", "attempt to shift left with overflow");
}

#[test]
fn wav_declared_chunk_end_can_differ_from_the_next_parsed_header() {
    aborts("wav-chunk-cursor", "attempt to multiply with overflow");
}

#[test]
fn caf_packet_offsets_can_overflow_after_a_safe_description() {
    aborts("caf-packet-offset", "attempt to add with overflow");
}

#[test]
fn wav_block_alignment_limit_is_checked_before_channel_rejection() {
    succeeds("wav-alignment-limit");
    aborts("wav-alignment-overflow", "attempt to multiply with overflow");
}

#[test]
fn caf_coded_width_limit_is_checked_before_pcm_rejection() {
    succeeds("caf-width-limit");
    aborts("caf-width-overflow", "attempt to multiply with overflow");
}

#[test]
fn nearby_valid_streams_need_no_allowance() {
    succeeds("wav-valid");
    succeeds("caf-valid");
}

#[test]
fn nearby_invalid_parameters_return_structured_errors() {
    succeeds("wav-invalid-channels");
    succeeds("caf-invalid-width");
}

#[test]
fn unrelated_panics_still_abort_even_inside_a_catch_unwind() {
    aborts("unrelated", "unrelated harness failure");
}

#[test]
fn normal_decoder_errors_need_no_panic_allowance() {
    succeeds("error");
}

/// Builds a minimal WAVE container from format fields without depending on either decoder.
fn wave(format: u16, channels: u16, bits: u16, extension: &[u8]) -> Vec<u8> {
    let mut body = Vec::from(b"WAVEfmt ");
    body.extend_from_slice(&(16 + u32::try_from(extension.len()).expect("small extension")).to_le_bytes());
    body.extend_from_slice(&format.to_le_bytes());
    body.extend_from_slice(&channels.to_le_bytes());
    body.extend_from_slice(&8000_u32.to_le_bytes());
    body.extend_from_slice(&16000_u32.to_le_bytes());
    body.extend_from_slice(&2_u16.to_le_bytes());
    body.extend_from_slice(&bits.to_le_bytes());
    body.extend_from_slice(extension);
    body.extend_from_slice(b"data");
    body.extend_from_slice(&2_u32.to_le_bytes());
    body.extend_from_slice(&0_i16.to_le_bytes());
    let mut file = Vec::from(b"RIFF");
    file.extend_from_slice(&u32::try_from(body.len()).expect("small RIFF body").to_le_bytes());
    file.extend_from_slice(&body);
    file
}

/// Appends one CAF chunk, whose size excludes the twelve-byte header and has no RIFF-style padding.
fn caf_chunk(file: &mut Vec<u8>, kind: [u8; 4], body: &[u8]) {
    file.extend_from_slice(&kind);
    file.extend_from_slice(&i64::try_from(body.len()).expect("small CAF body").to_be_bytes());
    file.extend_from_slice(body);
}

/// Builds a CAF description followed by optional packet-table bytes and a silent sample.
fn caf(bytes_per_packet: u32, frames_per_packet: u32, packet_table: Option<&[u8]>) -> Vec<u8> {
    let mut file = Vec::from(b"caff\0\x01\0\0");
    let mut description = Vec::from(8000_f64.to_be_bytes());
    description.extend_from_slice(b"lpcm");
    description.extend_from_slice(&0_u32.to_be_bytes());
    description.extend_from_slice(&bytes_per_packet.to_be_bytes());
    description.extend_from_slice(&frames_per_packet.to_be_bytes());
    description.extend_from_slice(&1_u32.to_be_bytes());
    description.extend_from_slice(&16_u32.to_be_bytes());
    caf_chunk(&mut file, *b"desc", &description);
    if let Some(table) = packet_table {
        caf_chunk(&mut file, *b"pakt", table);
    }
    caf_chunk(&mut file, *b"data", &[0; 6]);
    file
}

/// Builds three individually representable packet sizes whose aggregate byte offset overflows u64.
fn overflowing_packet_table() -> Vec<u8> {
    let mut table = Vec::from(3_i64.to_be_bytes());
    table.extend_from_slice(&3_i64.to_be_bytes());
    table.extend_from_slice(&0_i32.to_be_bytes());
    table.extend_from_slice(&0_i32.to_be_bytes());
    for _ in 0..3 {
        // Nine base-128 groups encode 2^63 - 1, which the dependency accepts as one packet size.
        table.extend_from_slice(&[0xff; 8]);
        table.push(0x7f);
    }
    table
}

/// Builds an extended fmt chunk whose ADPCM extra-data count leaves the parser inside its declared body.
fn wave_with_divergent_cursor() -> Vec<u8> {
    // IMA ADPCM accepts cbSize = 2, consumes the sample count, and leaves the remaining fmt bytes unread.
    let mut extension = Vec::from([2, 0, 1, 0]);
    extension.extend_from_slice(b"fmt ");
    extension.extend_from_slice(&16_u32.to_le_bytes());
    let overflowing = wave(1, 32768, 16, &[]);
    extension.extend_from_slice(overflowing.get(20..36).expect("PCM format fields"));
    extension.extend_from_slice(&[0; 32]);
    let mut file = wave(0x11, 1, 4, &extension);
    // Streaming RIFF has no parent bound, so the dependency continues at its actual parse cursor.
    file.get_mut(4..8)
        .expect("RIFF size")
        .copy_from_slice(&u32::MAX.to_le_bytes());
    file.get_mut(32..34)
        .expect("ADPCM block size")
        .copy_from_slice(&4_u16.to_le_bytes());
    file
}

/// Builds valid extensible PCM fields with a channel count that overflows channel-mask repair.
fn wave_with_oversized_channel_mask() -> Vec<u8> {
    let mut extension = Vec::from(22_u16.to_le_bytes());
    extension.extend_from_slice(&16_u16.to_le_bytes());
    extension.extend_from_slice(&0_u32.to_le_bytes());
    extension.extend_from_slice(&[1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71]);
    wave(0xfffe, 33, 16, &extension)
}

/// Checks the nearest representable and overflowing products without admitting any panic allowance.
fn arithmetic_boundary(path: &Path, mode: &str) {
    let at_limit = mode.ends_with("-limit");
    let input = if mode.starts_with("wav-") {
        // A 16-bit sample occupies two bytes, so this tests the u16 frame-width product.
        wave(1, u16::MAX / 2 + u16::from(!at_limit), 16, &[])
    } else {
        caf(u32::MAX / 8 + u32::from(!at_limit), 1, None)
    };
    std::fs::write(path, input).expect("writes arithmetic boundary input");
    let error = AudioFileDecoder::open(path).err().expect("unsupported boundary input");
    assert!(at_limit, "above-limit input must retain its checked panic");
    assert!(matches!(error, DecodeError::Probe(_) | DecodeError::Codec(_)));
}

/// Executes one scenario with a libFuzzer-like aborting panic hook in its own process.
#[test]
fn regression_child() {
    let Ok(mode) = std::env::var("AUDIOBATCH_FUZZ_REGRESSION") else {
        return;
    };
    std::panic::set_hook(Box::new(|information| {
        eprintln!("{information}");
        std::process::abort();
    }));
    install_decoder_regression_hook();
    let seed: &[u8] = if mode.starts_with("caf-") {
        include_bytes!("../corpus/decode_audio/caf-bits-per-coded-sample-overflow")
    } else {
        include_bytes!("../corpus/decode_audio/wav-block-align-overflow")
    };
    let regression = DecoderRegression::from_input(seed).expect("exact committed regression");
    assert!(DecoderRegression::from_input(b"not a WAV").is_none());
    let (_, file) = seed.split_first().expect("selector");
    let directory = tempfile::tempdir().expect("temporary folder");
    let path = directory.path().join(if mode.starts_with("caf-") {
        "input.caf"
    } else {
        "input.wav"
    });
    std::fs::write(&path, file).expect("writes regression input");
    match mode.as_str() {
        "wav-production" | "caf-production" => check_decoder_production_boundary(&path, regression),
        "wav-prefixed" | "caf-prefixed" => {
            let mut prefixed = Vec::from(b"junk before the probed container");
            prefixed.extend_from_slice(file);
            std::fs::write(&path, prefixed).expect("writes prefixed input");
            let _ = AudioFileDecoder::open(&path);
        }
        "wav-chunk-cursor" => {
            std::fs::write(&path, wave_with_divergent_cursor()).expect("writes mismatched chunk body");
            let _ = AudioFileDecoder::open(&path);
        }
        "wav-extensible" => {
            std::fs::write(&path, wave_with_oversized_channel_mask()).expect("writes extensible WAV");
            let _ = AudioFileDecoder::open(&path);
        }
        "caf-packet-offset" => {
            std::fs::write(&path, caf(0, 1, Some(&overflowing_packet_table()))).expect("writes variable packet CAF");
            let _ = AudioFileDecoder::open(&path);
        }
        "wav-alignment-limit" | "wav-alignment-overflow" | "caf-width-limit" | "caf-width-overflow" => {
            arithmetic_boundary(&path, &mode);
        }
        "wav-valid" | "caf-valid" => {
            let valid = if mode == "wav-valid" {
                wave(1, 1, 16, &[])
            } else {
                caf(2, 1, None)
            };
            std::fs::write(&path, valid).expect("writes valid boundary input");
            let mut decoder = AudioFileDecoder::open(&path).expect("valid input opens");
            assert_eq!(decoder.info().channels, 1);
            assert_eq!(decoder.info().sample_rate, 8000);
            assert!(decoder.next_chunk().expect("decodes silent sample").is_some());
        }
        "wav-invalid-channels" | "caf-invalid-width" => {
            let invalid = if mode == "wav-invalid-channels" {
                wave(1, 0, 16, &[])
            } else {
                // Three bytes fit u32 multiplication, but cannot encode mono 16-bit PCM.
                caf(3, 1, None)
            };
            std::fs::write(&path, invalid).expect("writes malformed boundary input");
            let error = AudioFileDecoder::open(&path).err().expect("structured decoding error");
            assert!(matches!(error, DecodeError::Probe(_) | DecodeError::Codec(_)));
            assert_eq!(error.analysis_kind(), AnalysisError::Unsupported);
        }
        "wav-raw" | "caf-raw" => {
            let _ = AudioFileDecoder::open(&path);
        }
        "wav-unallowlisted" => {
            let variant = include_bytes!("wav-overflow-unallowlisted");
            assert!(DecoderRegression::from_input(variant).is_none());
            std::fs::write(&path, variant.get(1..).expect("payload")).expect("writes captured fuzz finding");
            let _ = AudioFileDecoder::open(&path);
        }
        "wav-mutated" => {
            let mut mutated = seed.to_vec();
            mutated.push(0);
            assert!(DecoderRegression::from_input(&mutated).is_none());
            std::fs::write(&path, mutated.get(1..).expect("payload")).expect("writes mutated regression");
            let _ = AudioFileDecoder::open(&path);
        }
        "caf-wrong-location" => check_decoder_production_boundary(&path, DecoderRegression::WavBlockAlignment),
        "unrelated" => {
            let _ = std::panic::catch_unwind(|| panic!("unrelated harness failure"));
        }
        "error" => {
            std::fs::write(&path, b"not a WAV").expect("writes malformed input");
            assert!(AudioFileDecoder::open(&path).is_err());
            let record = analyze_file_catching_panics(&path, &|| true, &mut |_| {});
            assert_eq!(record.error, Some(AnalysisError::Unsupported));
        }
        _ => unreachable!("unknown subprocess mode"),
    }
}
