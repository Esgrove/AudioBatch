//! Exact-input regression for a checked arithmetic panic in Symphonia 0.6.1.
//!
//! Only the real production analysis boundary may unwind through the libFuzzer panic hook,
//! and only for the committed input's verified dependency location and overflow payload.
//! Mutated inputs, other decoder panics, and harness assertions retain libFuzzer's aborting hook.

use std::cell::Cell;
use std::path::Path;

use audiobatch_core::analysis::analyze_file_catching_panics;
use audiobatch_core::record::{AnalysisError, AnalysisStatus};

/// The complete WAV fuzz input, including its extension selector.
const WAV_REGRESSION: &[u8] = include_bytes!("../corpus/decode_audio/wav-block-align-overflow");

thread_local! {
    /// The exact input being checked inside the production catch-unwind entry point.
    static IN_PRODUCTION_BOUNDARY: Cell<Option<DecoderRegression>> = const { Cell::new(None) };
    /// Counts the verified panic, so the failed record must correspond to exactly one panic.
    static EXPECTED_PANICS: Cell<usize> = const { Cell::new(0) };
}

/// A committed input whose checked arithmetic panic has a production-boundary regression.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecoderRegression {
    /// PCM channel count times sample width overflows u16 during WAV probing.
    WavBlockAlignment,
}

impl DecoderRegression {
    /// Identifies only a byte-exact committed regression, never a broader malformed-header class.
    #[must_use]
    pub fn from_input(input: &[u8]) -> Option<Self> {
        (input == WAV_REGRESSION).then_some(Self::WavBlockAlignment)
    }

    /// Matches the versioned dependency location verified by the checked subprocess tests.
    fn matches_location(self, location: &std::panic::Location<'_>) -> bool {
        let (suffix, line) = match self {
            Self::WavBlockAlignment => ("symphonia-format-riff-0.6.1/src/wave/chunks.rs", 100),
        };
        location.file().ends_with(suffix) && location.line() == line
    }
}

/// Lets the verified panic unwind only while checking its exact input through production analysis.
/// Install after libFuzzer installs its aborting hook. Every other panic retains the original hook.
pub fn install_decoder_regression_hook() {
    let aborting_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |information| {
        let expected_location = IN_PRODUCTION_BOUNDARY.get().is_some_and(|regression| {
            information
                .location()
                .is_some_and(|location| regression.matches_location(location))
        });
        let expected_payload =
            information.payload().downcast_ref::<&str>() == Some(&"attempt to multiply with overflow");
        if expected_location && expected_payload {
            EXPECTED_PANICS.set(EXPECTED_PANICS.get() + 1);
        } else {
            aborting_hook(information);
        }
    }));
}

/// Requires the exact regression to produce a failed record through the real production entry point.
///
/// The panic happens during probing, before meter construction or the first cancellation check.
/// Cancellation prevents an unexpectedly accepted input from starting an unbounded analysis pass.
///
/// # Panics
///
/// If the panic changes location or payload, stops reproducing, or fails to produce the expected record.
pub fn check_decoder_production_boundary(path: &Path, regression: DecoderRegression) {
    EXPECTED_PANICS.set(0);
    IN_PRODUCTION_BOUNDARY.set(Some(regression));
    let record = analyze_file_catching_panics(path, &|| true, &mut |_| {});
    IN_PRODUCTION_BOUNDARY.set(None);
    assert_eq!(
        EXPECTED_PANICS.get(),
        1,
        "the exact checked regression must still reproduce"
    );
    assert_eq!(record.status, AnalysisStatus::Failed);
    assert_eq!(record.error, Some(AnalysisError::Panicked));
    assert_eq!(record.error_message, "Analysis failed unexpectedly");
}
