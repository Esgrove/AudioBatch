//! Known-answer tests for the loudness measurement, independent of any reference implementation.
//!
//! The signals follow the definitions of the EBU Tech 3341 and 3342 conformance cases.
//! A 1 kHz sine with the same peak level on both channels reads the same in LUFS as its level in dBFS,
//! so the expected values are the published ones and are not derived from `ebur128` or from the C++ application.
//! The cases cover the absolute gate, the relative gate, and a loudness step.
//! Their segments are shorter than the originals, and the loudest part stays the longest so the gates select it.
//! The C++ application runs the same cases in `cpp/tests/MeasurementTests.cpp`, so keep both tables in step.
//! Readings must be within the 0.1 LU the EBU allows.
//! The true peak case allows the +0.2 and -0.4 dB range of the EBU true peak tests.

use std::f64::consts::{FRAC_PI_2, FRAC_PI_4, TAU};

use crate::golden::measure_signal;

/// Allowed deviation of a loudness reading, in LU.
const LOUDNESS_TOLERANCE: f64 = 0.1;
/// Allowed true peak reading above the expected level, in dB.
const TRUE_PEAK_UPPER_TOLERANCE: f64 = 0.2;
/// Allowed true peak reading below the expected level, in dB.
const TRUE_PEAK_LOWER_TOLERANCE: f64 = 0.4;
/// Frames per push, unrelated to the meter's block size.
const CHUNK_FRAMES: usize = 4096;
/// Frequency of the test tone in hertz.
const TONE_FREQUENCY: f64 = 1000.0;

/// A steady tone at the target level.
const TONE_AT_MINUS_23: Case = Case {
    label: "stereo tone at -23 LUFS",
    segments: &[(4.0, -23.0)],
    integrated_lufs: -23.0,
    max_short_term_lufs: Some(-23.0),
};

/// A steady tone at a lower level.
const TONE_AT_MINUS_33: Case = Case {
    label: "stereo tone at -33 LUFS",
    segments: &[(4.0, -33.0)],
    integrated_lufs: -33.0,
    max_short_term_lufs: Some(-33.0),
};

/// Quiet parts around the tone fall below the relative gate.
const QUIET_PARTS_AROUND_TONE: Case = Case {
    label: "quiet parts around a tone at -23 LUFS are below the relative gate",
    segments: &[(1.0, -36.0), (12.0, -23.0), (1.0, -36.0)],
    integrated_lufs: -23.0,
    max_short_term_lufs: None,
};

/// Parts below the absolute gate and the relative gate are both ignored.
const PARTS_BELOW_BOTH_GATES: Case = Case {
    label: "parts below the absolute gate and the relative gate are ignored",
    segments: &[(0.5, -72.0), (1.0, -36.0), (12.0, -23.0), (1.0, -36.0), (0.5, -72.0)],
    integrated_lufs: -23.0,
    max_short_term_lufs: None,
};

/// A louder part between quieter ones, all above the gates, averages by energy.
const LOUDER_PART_BETWEEN_QUIETER: Case = Case {
    label: "a louder part between quieter ones averages to -23 LUFS",
    segments: &[(3.0, -26.0), (3.0, -20.0), (3.0, -26.0)],
    integrated_lufs: -23.0,
    max_short_term_lufs: None,
};

/// One conformance signal and what it must measure.
struct Case {
    /// Description for failure messages.
    label: &'static str,
    /// Duration in seconds and peak level in dBFS of each consecutive segment.
    segments: &'static [(f64, f64)],
    /// Expected integrated loudness in LUFS.
    integrated_lufs: f64,
    /// Expected maximum short-term loudness in LUFS, when the signal defines one.
    max_short_term_lufs: Option<f64>,
}

/// Builds the stereo tone of a case, with the same samples on both channels and a continuous phase.
fn stereo_tone(sample_rate: u32, segments: &[(f64, f64)]) -> Vec<Vec<f32>> {
    let rate = f64::from(sample_rate);
    let mut samples = Vec::new();
    for &(seconds, level) in segments {
        let amplitude = 10.0_f64.powf(level / 20.0);
        let frames = (rate * seconds).round() as usize;
        let start = samples.len();
        samples.extend((start..start + frames).map(|frame| {
            let phase = TAU * TONE_FREQUENCY * frame as f64 / rate;
            (amplitude * phase.sin()) as f32
        }));
    }
    vec![samples.clone(), samples]
}

/// Measures a case and fails the test when a reading is outside the tolerance.
fn assert_case(case: &Case, sample_rate: u32) {
    let planes = stereo_tone(sample_rate, case.segments);
    let output = measure_signal(&planes, sample_rate, CHUNK_FRAMES);
    assert!(
        (output.integrated_lufs - case.integrated_lufs).abs() <= LOUDNESS_TOLERANCE,
        "{} at {sample_rate} Hz: integrated {} LUFS, expected {}",
        case.label,
        output.integrated_lufs,
        case.integrated_lufs
    );
    if let Some(expected) = case.max_short_term_lufs {
        assert!(
            (output.max_short_term_lufs - expected).abs() <= LOUDNESS_TOLERANCE,
            "{} at {sample_rate} Hz: maximum short-term {} LUFS, expected {expected}",
            case.label,
            output.max_short_term_lufs
        );
    }
}

#[cfg(test)]
mod test_known_answers {
    use super::*;

    #[test]
    fn steady_tones_read_their_level_at_both_common_rates() {
        for sample_rate in [44_100, 48_000] {
            assert_case(&TONE_AT_MINUS_23, sample_rate);
            assert_case(&TONE_AT_MINUS_33, sample_rate);
        }
    }

    #[test]
    fn quiet_parts_around_a_tone_are_gated_out() {
        assert_case(&QUIET_PARTS_AROUND_TONE, 48_000);
    }

    #[test]
    fn parts_below_both_gates_are_ignored() {
        assert_case(&PARTS_BELOW_BOTH_GATES, 48_000);
    }

    #[test]
    fn a_louder_part_averages_by_energy() {
        assert_case(&LOUDER_PART_BETWEEN_QUIETER, 48_000);
    }

    #[test]
    fn a_quarter_sample_rate_tone_has_its_true_peak_between_the_samples() {
        // Every sample of a quarter sample rate sine at 45 degrees is 0.7071 of its amplitude,
        // so the sample peak is 3 dB below the true peak, which is the amplitude.
        let sample_rate = 48_000;
        let level = -6.0;
        let amplitude = 10.0_f64.powf(level / 20.0);
        let samples: Vec<f32> = (0..2 * sample_rate)
            .map(|frame| (amplitude * FRAC_PI_2.mul_add(f64::from(frame), FRAC_PI_4).sin()) as f32)
            .collect();
        let output = measure_signal(&[samples.clone(), samples], sample_rate, CHUNK_FRAMES);
        for true_peak in output.true_peaks {
            let decibels = 20.0 * true_peak.log10();
            assert!(
                (level - TRUE_PEAK_LOWER_TOLERANCE..=level + TRUE_PEAK_UPPER_TOLERANCE).contains(&decibels),
                "true peak {decibels} dBTP, expected {level}"
            );
        }
    }
}
