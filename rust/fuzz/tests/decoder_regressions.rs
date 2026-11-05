//! Checked arithmetic regressions and subprocess checks for exact-input production unwind allowances.

use std::process::{Command, Output};

use audiobatch_core::analysis::analyze_file_catching_panics;
use audiobatch_core::decode::AudioFileDecoder;
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
fn unrelated_panics_still_abort_even_inside_a_catch_unwind() {
    aborts("unrelated", "unrelated harness failure");
}

#[test]
fn normal_decoder_errors_need_no_panic_allowance() {
    succeeds("error");
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
        "wav-raw" | "caf-raw" => {
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
