//! Source overload regressions for fixed analysis blocks and decoded chunk boundaries.
//!
//! These tests pin strict source thresholds, channel separation, interpolation continuity,
//! and final partial coverage without changing the legacy measurement snapshots.

use crate::golden::measure_signal;
use crate::meter::BlockMeter;
use crate::overload::{SAMPLE_OVER, TRUE_PEAK_OVER};
use crate::signals::{SignalKind, generate};

#[test]
fn sample_thresholds_are_strict_on_both_signs_and_channels() {
    let below = f32::from_bits(1.0_f32.to_bits() - 1);
    let above = f32::from_bits(1.0_f32.to_bits() + 1);
    let planes = vec![vec![below, 1.0, -1.0, -below], vec![above, -above, 0.0, 0.0]];
    for rate in [48_000, 96_000, 192_000] {
        let output = measure_signal(&planes, rate, 1);
        assert_eq!(output.overloads.channels, 2);
        assert_eq!(output.overloads.flags[0] & SAMPLE_OVER, 0);
        assert_eq!(output.overloads.flags[1] & SAMPLE_OVER, SAMPLE_OVER);
        assert_eq!(output.overloads.flags[1] & TRUE_PEAK_OVER, TRUE_PEAK_OVER);
        if rate == 192_000 {
            assert_eq!(output.overloads.flags, vec![0, SAMPLE_OVER | TRUE_PEAK_OVER]);
        }
    }
}

#[test]
fn true_peak_only_overs_do_not_claim_sample_overs() {
    let planes = generate(SignalKind::Square, 48_000, 1, 4_800);
    let output = measure_signal(&planes, 48_000, 4096);
    assert_eq!(output.overloads.channels, 1);
    assert_eq!(output.overloads.flags, vec![TRUE_PEAK_OVER]);
}

#[test]
fn overload_does_not_stick_to_later_silent_blocks() {
    let block = 4_800;
    let mut plane = vec![0.0; block * 4 + 7];
    plane[100] = 2.0;
    plane[block * 3 + 100] = 1.25;
    let output = measure_signal(&[plane], 48_000, 4096);
    assert_eq!(
        output.overloads.flags,
        vec![SAMPLE_OVER | TRUE_PEAK_OVER, 0, 0, SAMPLE_OVER | TRUE_PEAK_OVER, 0]
    );
    assert_eq!(output.overloads.length_in_samples, (block * 4 + 7) as u64);
    assert!(output.overloads.is_valid());
}

#[test]
fn boundary_interpolation_uses_continuous_state_without_an_eof_flush() {
    let block = 4_800;
    let mut plane = vec![0.0; block + 24];
    plane[block - 2] = 0.9;
    plane[block - 1] = 0.9;
    plane[block] = -0.9;
    plane[block + 1] = -0.9;
    let whole = measure_signal(&[plane.clone()], 48_000, plane.len());
    for chunk in [1, 7, block - 1, block, block + 1] {
        let output = measure_signal(&[plane.clone()], 48_000, chunk);
        assert_eq!(output.overloads, whole.overloads);
        assert_eq!(output.true_peaks, whole.true_peaks);
    }
    assert_eq!(whole.overloads.flags.len(), 2);
    assert!(whole.overloads.flags.contains(&TRUE_PEAK_OVER));

    let short = measure_signal(&[vec![0.5, 0.5, -0.5, -0.5]], 48_000, 1);
    assert_eq!(short.overloads.flags, vec![0]);
    assert_eq!(short.true_peaks[0].to_bits(), 0.5_f64.to_bits());
}

#[test]
fn empty_and_nonfinite_streams_have_explicit_history() {
    let empty = BlockMeter::new(1, 48_000, None).expect("meter").finish(0);
    assert_eq!(empty.overloads.flags, [] as [u8; 0]);
    assert!(empty.overloads.is_valid());
    let planes = vec![vec![f32::NAN; 4], vec![f32::INFINITY, f32::NEG_INFINITY, -0.0, 0.0]];
    let output = measure_signal(&planes, 192_000, 1);
    assert_eq!(output.overloads.flags, vec![0, SAMPLE_OVER | TRUE_PEAK_OVER]);
}
