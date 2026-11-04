//! libFuzzer target for the command line argument parser of `audiobatch-rs`.
//!
//! The input layout is the same as the C++ `CliArgumentsFuzzer`, so the two replay the same corpus.
//! The input is split into lines, which are parsed as the arguments that follow the executable name.
//! The parser and the checks after it must return arguments or an error for any input, without panicking.

#![no_main]

use audiobatch_cli::args::CliArgs;
use clap::Parser as _;
use libfuzzer_sys::fuzz_target;

/// Longest input that is parsed, since longer command lines exercise no new code paths.
const MAXIMUM_INPUT_BYTES: usize = 4096;

fuzz_target!(|input: &[u8]| {
    if input.len() > MAXIMUM_INPUT_BYTES {
        return;
    }
    let text = String::from_utf8_lossy(input);
    let arguments = std::iter::once("audiobatch-rs").chain(text.lines());
    if let Ok(args) = CliArgs::try_parse_from(arguments) {
        let _ = args.normalization();
        let _ = args.worker_count();
    }
});
