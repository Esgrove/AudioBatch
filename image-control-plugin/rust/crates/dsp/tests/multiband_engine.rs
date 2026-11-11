//! Low-level production FIR and convolution contracts with independent direct-convolution expectations.

#[path = "../src/convolver.rs"]
mod convolver;
#[path = "../src/correlation.rs"]
mod correlation;
#[path = "../src/crossover.rs"]
mod crossover;
#[path = "../src/kernel_worker.rs"]
mod kernel_worker;
#[path = "../src/multiband.rs"]
mod multiband;

use convolver::{KernelSpectra, PartitionedConvolver};
use crossover::{DEFAULT_CROSSOVERS, PARTITION_SIZE, design_kernels, half_length, order_crossovers};
use multiband::{BandControls, MultibandImager};

#[test]
fn low_level_monitoring_api_publishes_unavailable_until_power_exists() {
    let mut imager = MultibandImager::new(48_000.0).expect("imager");
    imager.set_correlation_enabled(true);
    for _ in 0..256 {
        let _ = imager.process_frame([0.0; 2]);
    }
    assert!(imager.correlation_readings().iter().all(|value| value.is_nan()));
    assert!(imager.correlation_revision() > 0);
}

#[test]
fn partitioned_overlap_save_matches_independent_direct_convolution() {
    let taps: Vec<_> = (0..1027)
        .map(|sample| (sample as f32 * 0.071).cos() * (-0.006 * sample as f32).exp() * 0.01)
        .collect();
    let input: Vec<_> = (0..768).map(|sample| (sample as f32 * 0.037).sin()).collect();
    let kernels = KernelSpectra::new(std::slice::from_ref(&taps), DEFAULT_CROSSOVERS).expect("kernels");
    let mut convolver = PartitionedConvolver::new(kernels.partitions());
    let mut output = [0.0; PARTITION_SIZE];
    for block in 0..8 {
        let partition =
            std::array::from_fn(|sample| input.get(block * PARTITION_SIZE + sample).copied().unwrap_or_default());
        convolver.push_input(&partition);
        convolver.convolve(&kernels, 0, &mut output);
        for (sample, actual) in output.iter().enumerate() {
            let position = block * PARTITION_SIZE + sample;
            let expected: f64 = taps
                .iter()
                .enumerate()
                .filter_map(|(tap, &coefficient)| {
                    position
                        .checked_sub(tap)
                        .and_then(|source| input.get(source))
                        .map(|&value| f64::from(value) * f64::from(coefficient))
                })
                .sum();
            assert!(
                (f64::from(*actual) - expected).abs() < 2.0e-7,
                "sample {position}: {actual} vs {expected}"
            );
        }
    }
}

#[test]
fn frequency_sampled_lowpasses_are_symmetric_unity_dc_and_complementary() {
    let half_length = half_length(48_000.0).expect("rate");
    let kernels = design_kernels(DEFAULT_CROSSOVERS, 48_000.0, half_length).expect("design");
    for kernel in kernels.iter().take(3) {
        let sum: f64 = kernel.iter().map(|&value| f64::from(value)).sum();
        assert!((sum - 1.0).abs() < 1.0e-7);
        for (&positive, &negative) in kernel.iter().zip(kernel.iter().rev()) {
            assert!((positive - negative).abs() < 2.0e-8);
        }
    }
    for kernel in kernels.iter().skip(3) {
        for (&positive, &negative) in kernel.iter().zip(kernel.iter().rev()) {
            assert!((positive + negative).abs() < f32::EPSILON);
        }
    }
    for tap in 0..=half_length * 2 {
        let low = kernels
            .first()
            .and_then(|kernel| kernel.get(tap))
            .copied()
            .expect("tap");
        let low_mid = kernels.get(1).and_then(|kernel| kernel.get(tap)).copied().expect("tap");
        let high_mid = kernels.get(2).and_then(|kernel| kernel.get(tap)).copied().expect("tap");
        let delayed = f32::from(tap == half_length);
        let sum = low + (low_mid - low) + (high_mid - low_mid) + (delayed - high_mid);
        assert!((sum - delayed).abs() < 2.0e-8);
    }
}

#[test]
fn crossover_ordering_retains_minimum_third_octave_spacing() {
    let ordered = order_crossovers([20_000.0, -100.0, 1.0]).expect("finite");
    assert!(ordered.into_iter().all(|value| (20.0..=20_000.0).contains(&value)));
    for pair in ordered.windows(2) {
        let [lower, upper] = pair else { continue };
        assert!(*upper >= *lower * 1.259_921_1);
    }
}

#[test]
fn private_module_public_api_supports_warm_processing_and_offline_replacement() {
    let mut imager = MultibandImager::new(48_000.0).expect("rate");
    let controls = BandControls::new(0.7, 0.8, 0.4, 0.9, 0.2, false).expect("controls");
    assert!((controls.mid_gain() - 0.7).abs() < f32::EPSILON);
    assert!((controls.side_gain() - 0.8).abs() < f32::EPSILON);
    assert!((controls.width() - 0.4).abs() < f32::EPSILON);
    assert!((controls.recover() - 0.9).abs() < f32::EPSILON);
    assert!((controls.pan() - 0.2).abs() < f32::EPSILON);
    assert!(!controls.solo());
    imager.set_controls([controls; 4]);
    assert!(imager.set_output_controls(0.8, 1.0, true));
    imager.reset();
    imager.set_offline(true).expect("offline");
    assert!(imager.set_crossovers([80.0, 800.0, 8000.0]));
    for _ in 0..imager.latency_samples() + imager.tail_samples() {
        assert!(imager.process_frame([0.5, -0.25]).into_iter().all(f32::is_finite));
        assert!(imager.dry_reference().into_iter().all(f32::is_finite));
    }
    assert_eq!(imager.active_crossovers(), [80.0, 800.0, 8000.0]);
    assert!(!imager.kernel_worker_failed());
    assert!(imager.set_channels(1));
    assert!(imager.process_mono(0.5).is_finite());
}
