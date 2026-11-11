//! Aligned Solo-only Delta reference contracts, independent impulse expectations, and coherent control/kernel fades.

use std::time::{Duration, Instant};

use image_control_dsp::{BandControls, MultibandImager};
use realfft::RealFftPlanner;
use realfft::num_complex::Complex64;

/// Shared production partition geometry, also used to locate the returned crossover fade.
const PARTITION: usize = 256;
/// Rate used for independent filter expectations and exactly 960 samples of control smoothing.
const SAMPLE_RATE: f32 = 48_000.0;
/// Warm history and end at a partition boundary before changing controls or kernels.
const WARMUP: usize = 12_032;
/// Distinct ordered crossover targets that measurably move the selected region.
const REPLACEMENT: [f32; 3] = [220.0, 1200.0, 8000.0];

/// Builds neutral source controls with an arbitrary set of selected bands.
fn selected_controls(selected: [bool; 4]) -> [BandControls; 4] {
    selected.map(|solo| BandControls::new(1.0, 1.0, 1.0, 0.0, 0.0, solo).expect("finite neutral controls"))
}

/// Prepares one neutral selected band and settles controls before the first pre-ring.
fn selected_imager(band: usize) -> MultibandImager {
    let mut imager = MultibandImager::new(SAMPLE_RATE).expect("supported rate");
    imager.set_controls(selected_controls(std::array::from_fn(|index| index == band)));
    imager.reset();
    imager
}

/// Produces deterministic bounded source audio with independent Mid/Side content and signed zeros.
fn signal(sample: usize) -> [f32; 2] {
    if sample.is_multiple_of(97) {
        [-0.0, 0.0]
    } else {
        let phase = sample as f32 * 0.037;
        [phase.sin() * 0.6, (phase * 1.731).cos() * 0.4]
    }
}

/// Compares the exact processed and selected reference bits, including every partition's final returned frame.
fn assert_matching_reference(imager: &mut MultibandImager, frame: [f32; 2]) -> [f32; 2] {
    let output = imager.process_frame(frame);
    let reference = imager.dry_reference();
    assert_eq!(output.map(f32::to_bits), reference.map(f32::to_bits));
    assert_eq!(reference.map(f32::to_bits), imager.dry_reference().map(f32::to_bits));
    reference
}

/// Designs an independent double-precision sampled lowpass and directly normalizes its windowed impulse.
/// It uses no production filter coefficients, convolution outputs, band mixer, or reference accessor.
fn expected_lowpass(cutoff: f64, half_length: usize) -> Vec<f64> {
    let size = half_length * 4;
    let inverse = RealFftPlanner::<f64>::new().plan_fft_inverse(size);
    let mut spectrum = inverse.make_input_vec();
    for (bin, value) in spectrum.iter_mut().enumerate() {
        let frequency = bin as f64 * f64::from(SAMPLE_RATE) / size as f64;
        let position = (frequency / cutoff).log2();
        let magnitude = if position <= -0.5 {
            1.0
        } else if position >= 0.5 {
            0.0
        } else {
            0.5 * (1.0 - (std::f64::consts::PI * position).sin())
        };
        *value = Complex64::new(magnitude, 0.0);
    }
    let mut impulse = inverse.make_output_vec();
    inverse
        .process(&mut spectrum, &mut impulse)
        .expect("independent inverse FFT");
    let mut taps: Vec<_> = (0..=half_length * 2)
        .map(|tap| {
            let circular = (tap + size - half_length) % size;
            let phase = std::f64::consts::PI * tap as f64 / half_length as f64;
            let window = (-0.488_29_f64).mul_add(phase.cos(), 0.358_75);
            let window = 0.141_28_f64.mul_add((2.0 * phase).cos(), window);
            let window = (-0.011_68_f64).mul_add((3.0 * phase).cos(), window);
            impulse.get(circular).copied().expect("circular impulse sample") * window
        })
        .collect();
    let sum: f64 = taps.iter().sum();
    for tap in &mut taps {
        *tap /= sum;
    }
    taps
}

/// Forms four complementary impulse bands independently of production sample mixing.
fn expected_bands(half_length: usize) -> Vec<[f64; 4]> {
    let low = expected_lowpass(100.0, half_length);
    let low_mid = expected_lowpass(500.0, half_length);
    let high_mid = expected_lowpass(5000.0, half_length);
    low.into_iter()
        .zip(low_mid)
        .zip(high_mid)
        .enumerate()
        .map(|(tap, ((low, low_mid), high_mid))| {
            let dry = f64::from(tap == half_length);
            [low, low_mid - low, high_mid - low_mid, dry - high_mid]
        })
        .collect()
}

/// Verifies all selected impulse responses independently, not merely processed/reference agreement.
#[test]
fn selected_neutral_reference_matches_independent_ordinary_band_impulses() {
    let mut imagers: Vec<_> = (0..4).map(selected_imager).collect();
    let half_length = imagers.first().expect("four bands").tail_samples();
    let expected = expected_bands(half_length);
    let mut power = [0.0_f64; 4];
    for sample in 0..PARTITION + expected.len() + PARTITION {
        let frame = if sample == 0 { [0.75, -0.25] } else { [0.0; 2] };
        for (band, (imager, power)) in imagers.iter_mut().zip(&mut power).enumerate() {
            let reference = assert_matching_reference(imager, frame);
            let coefficient = sample
                .checked_sub(PARTITION)
                .and_then(|tap| expected.get(tap))
                .and_then(|bands| bands.get(band))
                .copied()
                .unwrap_or_default();
            for (actual, source) in reference.into_iter().zip([0.75, -0.25]) {
                assert!(
                    coefficient.mul_add(-source, f64::from(actual)).abs() < 2.0e-7,
                    "band {band}, sample {sample}"
                );
                *power += f64::from(actual).powi(2);
            }
        }
    }
    assert!(
        power.into_iter().all(|value| value > 0.0001),
        "every selected band must carry audio"
    );
}

/// A selected source trim changes only its processed contribution, never the ordinary dry reference.
#[test]
fn selected_trim_delta_matches_independent_band_impulse() {
    let mut imager = selected_imager(2);
    let mut controls = selected_controls([false, false, true, false]);
    *controls.get_mut(2).expect("selected band") =
        BandControls::new(1.5, 0.25, 1.0, 0.0, 0.0, true).expect("finite trims");
    imager.set_controls(controls);
    imager.reset();
    let expected = expected_bands(imager.tail_samples());
    let mut delta_power = 0.0_f64;
    for sample in 0..PARTITION + expected.len() + PARTITION {
        let output = imager.process_frame(if sample == 0 { [0.75, -0.25] } else { [0.0; 2] });
        let reference = imager.dry_reference();
        let coefficient = sample
            .checked_sub(PARTITION)
            .and_then(|tap| expected.get(tap))
            .map_or(0.0, |&[_, _, band, _]| band);
        for ((actual, dry), (processed_gain, dry_gain)) in
            output.into_iter().zip(reference).zip([(0.5, 0.75), (0.25, -0.25)])
        {
            assert!(coefficient.mul_add(-dry_gain, f64::from(dry)).abs() < 2.0e-7);
            assert!(coefficient.mul_add(-processed_gain, f64::from(actual)).abs() < 2.0e-7);
            let delta = f64::from(actual) - f64::from(dry);
            assert!(coefficient.mul_add(dry_gain - processed_gain, delta).abs() < 2.0e-7);
            delta_power += delta.powi(2);
        }
    }
    assert!(delta_power > 0.001, "selected trim must produce a nonzero difference");
}

/// Full-band dry uses original delayed bits even when processing is nonneutral or all Solo buttons are on.
#[test]
fn fully_audible_reference_preserves_original_bits_for_non_neutral_output() {
    for controls in [
        BandControls::default(),
        BandControls::new(0.3, 0.7, 0.2, 0.8, 0.6, false).expect("controls"),
        BandControls::new(0.3, 0.7, 0.2, 0.8, 0.6, true).expect("all Solo controls"),
    ] {
        let mut imager = MultibandImager::new(SAMPLE_RATE).expect("rate");
        assert_eq!(imager.dry_reference().map(f32::to_bits), [0.0_f32; 2].map(f32::to_bits));
        imager.set_controls([controls; 4]);
        imager.reset();
        for sample in 0..imager.latency_samples() + 700 {
            let _ = imager.process_frame(signal(sample));
            let expected = sample.checked_sub(imager.latency_samples()).map_or([0.0; 2], signal);
            assert_eq!(imager.dry_reference().map(f32::to_bits), expected.map(f32::to_bits));
        }
        imager.reset();
        assert_eq!(imager.dry_reference().map(f32::to_bits), [0.0_f32; 2].map(f32::to_bits));
        for _ in 0..PARTITION * 2 {
            let _ = imager.process_frame([0.0; 2]);
            assert_eq!(imager.dry_reference().map(f32::to_bits), [0.0_f32; 2].map(f32::to_bits));
        }
    }
}

/// Mono references duplicate the selected ordinary Mid and ignore the second input and stereo-only controls.
#[test]
fn mono_reference_contains_only_selected_mid_in_both_channels() {
    let mut imager = selected_imager(1);
    assert!(imager.set_channels(1));
    imager.set_controls(std::array::from_fn(|band| {
        BandControls::new(1.0, 0.2, 0.0, 1.0, 1.0, band == 1).expect("mono controls")
    }));
    imager.reset();
    let mut power = 0.0_f64;
    for sample in 0..8000 {
        let [left, _] = signal(sample);
        let output = imager.process_mono(left);
        let [reference, duplicate] = imager.dry_reference();
        assert_eq!(output.to_bits(), reference.to_bits());
        assert_eq!(reference.to_bits(), duplicate.to_bits());
        power += f64::from(reference).powi(2);
    }
    assert!(power > 0.01);
}

/// Source trims, Width, Recover, and Pan are absent from the raw reference even during their glides.
#[test]
fn reference_ignores_processing_controls_and_repeated_reads_do_not_advance_solo() {
    let mut neutral = selected_imager(1);
    let mut processed = selected_imager(1);
    let mut difference_power = 0.0_f64;
    for sample in 0..WARMUP + 2000 {
        if sample == WARMUP {
            neutral.set_controls(selected_controls([false, false, true, true]));
            processed.set_controls(std::array::from_fn(|band| {
                BandControls::new(0.4, 0.7, 0.25, 0.8, -0.6, band >= 2).expect("processing controls")
            }));
        }
        let ordinary = assert_matching_reference(&mut neutral, signal(sample));
        let output = processed.process_frame(signal(sample));
        let reference = processed.dry_reference();
        for _ in 0..3 {
            assert_eq!(processed.dry_reference().map(f32::to_bits), reference.map(f32::to_bits));
        }
        assert_eq!(ordinary.map(f32::to_bits), reference.map(f32::to_bits));
        for (actual, dry) in output.into_iter().zip(reference) {
            difference_power += f64::from(actual - dry).powi(2);
        }
    }
    assert!(
        difference_power > 1.0,
        "processing controls must actually change the output"
    );
}

/// The audible 20 ms ramp is shared by processed/reference outputs and lands on exact original bits.
#[test]
fn solo_smoothing_matches_independent_audible_mix_and_unsolo_endpoint() {
    let mut old = selected_imager(0);
    let mut new = selected_imager(2);
    let mut changing = selected_imager(0);
    for sample in 0..WARMUP + 3000 {
        if sample == WARMUP {
            changing.set_controls(selected_controls([false, false, true, false]));
        } else if sample == WARMUP + 1536 {
            changing.set_controls(selected_controls([false; 4]));
        }
        if sample >= WARMUP && sample % 17 == 0 {
            let solo = if sample < WARMUP + 1536 {
                [false, false, true, false]
            } else {
                [false; 4]
            };
            changing.set_controls(selected_controls(solo));
        }
        let old = old.process_frame(signal(sample));
        let new = new.process_frame(signal(sample));
        let actual = assert_matching_reference(&mut changing, signal(sample));
        if (WARMUP..WARMUP + 1536).contains(&sample) {
            let advanced = sample.saturating_sub(WARMUP + PARTITION - 1).min(960);
            let fade = advanced as f32 / 960.0;
            for ((old, new), actual) in old.into_iter().zip(new).zip(actual) {
                let expected = (1.0 - fade).mul_add(old, fade * new);
                assert!((actual - expected).abs() < 5.0e-6, "audible ramp sample {sample}");
            }
        }
        if sample >= WARMUP + 1536 + PARTITION + 960 {
            let expected = signal(sample - changing.latency_samples());
            assert_eq!(actual.map(f32::to_bits), expected.map(f32::to_bits));
        }
    }
}

/// Checks one returned dry/processed sample against the interpolation of independently warmed kernel generations.
fn assert_kernel_fade(old: [f32; 2], new: [f32; 2], actual: [f32; 2], sample: usize, fade_start: Option<usize>) {
    let fade = fade_start.map_or(0.0, |start| {
        if sample < start {
            0.0
        } else {
            ((sample - start + 1).min(PARTITION)) as f32 / PARTITION as f32
        }
    });
    for ((old, new), actual) in old.into_iter().zip(new).zip(actual) {
        assert!(
            (actual - (new - old).mul_add(fade, old)).abs() < 1.0e-6,
            "kernel fade sample {sample}"
        );
    }
}

/// Offline crossover replacement shares the exact histories and one-partition fade with the selected reference.
#[test]
fn offline_selected_reference_tracks_coherent_kernel_handoff() {
    let mut old = selected_imager(1);
    let mut new = selected_imager(1);
    let mut changing = selected_imager(1);
    for imager in [&mut old, &mut new, &mut changing] {
        imager.set_offline(true).expect("offline");
    }
    assert!(new.set_crossovers(REPLACEMENT));
    let mut changed_power = 0.0_f64;
    for sample in 0..WARMUP + 1024 {
        if sample == WARMUP {
            assert!(changing.set_crossovers(REPLACEMENT));
        }
        let old = old.process_frame(signal(sample));
        let new = new.process_frame(signal(sample));
        let actual = assert_matching_reference(&mut changing, signal(sample));
        assert_kernel_fade(old, new, actual, sample, Some(WARMUP + PARTITION));
        if sample >= WARMUP {
            changed_power += old
                .into_iter()
                .zip(new)
                .map(|(old, new)| f64::from(new - old).powi(2))
                .sum::<f64>();
        }
    }
    assert_eq!(
        changing.active_crossovers().map(f32::to_bits),
        REPLACEMENT.map(f32::to_bits)
    );
    assert!(changed_power > 0.01, "handoff must move audible band content");
}

/// A real-time worker handoff leaves the final old frame aligned before returning the first coherent fade frame.
#[test]
fn realtime_selected_reference_tracks_coherent_kernel_handoff() {
    let mut old = selected_imager(1);
    let mut new = selected_imager(1);
    let mut changing = selected_imager(1);
    new.set_offline(true).expect("deterministic reference generation");
    assert!(new.set_crossovers(REPLACEMENT));
    for sample in 0..WARMUP {
        let _ = old.process_frame(signal(sample));
        let _ = new.process_frame(signal(sample));
        let _ = assert_matching_reference(&mut changing, signal(sample));
    }
    assert!(changing.set_crossovers(REPLACEMENT));
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut fade_start = None;
    let mut changed_power = 0.0_f64;
    for sample in WARMUP.. {
        assert!(Instant::now() < deadline, "bounded worker publication and fade");
        let old = old.process_frame(signal(sample));
        let new = new.process_frame(signal(sample));
        let actual = assert_matching_reference(&mut changing, signal(sample));
        if fade_start.is_none() && changing.active_crossovers().map(f32::to_bits) == REPLACEMENT.map(f32::to_bits) {
            assert_eq!((sample + 1) % PARTITION, 0, "kernel activation must finish a partition");
            fade_start = Some(sample + 1);
        }
        assert_kernel_fade(old, new, actual, sample, fade_start);
        if let Some(start) = fade_start {
            changed_power += old
                .into_iter()
                .zip(new)
                .map(|(old, new)| f64::from(new - old).powi(2))
                .sum::<f64>();
            if sample >= start + PARTITION * 2 {
                break;
            }
        }
        if sample.is_multiple_of(PARTITION) {
            std::thread::yield_now();
        }
    }
    assert!(!changing.kernel_worker_failed());
    assert!(changed_power > 0.01, "worker replacement must change selected content");
}
