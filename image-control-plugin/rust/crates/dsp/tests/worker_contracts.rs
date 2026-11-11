//! Real processing-to-worker contracts using deterministic audio and a bounded completion deadline.

use std::time::{Duration, Instant};

use image_control_dsp::{AnalysisTap, AnalysisWorker, DisplayPreferences, FrequencyScale, Imager};

#[test]
fn worker_applies_projection_discontinuities_and_compatible_live_stereo_curve_edits() {
    let mut imager = Imager::new();
    assert!(imager.prepare(48_000.0));
    let shared = imager.shared();
    shared.set_analysis_tap(AnalysisTap::Pre);
    let worker = AnalysisWorker::new(std::sync::Arc::clone(&shared)).expect("worker");
    for (mel_scale, midpoint, edge, difference, expected_position) in [
        (false, 12.0, 32.0, 12.0, 0.25),
        (true, 6.0, 18.0, 6.0, 0.25),
        (true, 6.0, 18.0, 18.0, 0.005),
        (false, 24.0, 60.0, 24.0, 0.25),
    ] {
        let previous = shared.display_preferences();
        let previous_generation = shared.analysis_generation();
        let preferences = DisplayPreferences {
            mel_scale,
            pan_midpoint_db: midpoint,
            pan_edge_db: edge,
            ..DisplayPreferences::default()
        };
        assert!(shared.set_display_preferences(preferences));
        let generation = shared.analysis_generation();
        let compatible = previous.analysis_compatible(preferences);
        assert_eq!(generation == previous_generation, compatible);
        let mut discarded = Vec::new();
        assert_eq!(worker.drain_batch(&mut discarded).generation, generation);
        if !compatible {
            assert!(discarded.is_empty(), "old generation cannot survive projection changes");
        }
        let quiet = 10.0_f32.powf(-difference / 20.0);
        imager.begin_block();
        for index in 0..4096 {
            let sine = (std::f32::consts::TAU * 750.0 * index as f32 / 48_000.0).sin() * 0.5;
            let _ = imager.process_frame([sine, sine * quiet]);
        }
        let scale = if mel_scale {
            FrequencyScale::Mel
        } else {
            FrequencyScale::Logarithmic
        };
        let expected_frequency = scale.normalized(750.0) as f32;
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut particles = Vec::new();
        let mut observed = false;
        while !observed && Instant::now() < deadline {
            let update = worker.drain_batch(&mut particles);
            assert_eq!(update.generation, generation);
            observed = particles.iter().any(|particle| {
                (particle.position()[0] - expected_frequency).abs() < 0.0002
                    && (particle.position()[1] - expected_position).abs() < 0.0002
            });
            std::thread::yield_now();
        }
        assert!(
            observed,
            "worker projects the independent 750 Hz signal with the selected curve and axis"
        );
    }
}

#[test]
fn actual_audio_reaches_worker_projected_spatial_particles() {
    let mut imager = Imager::new();
    assert!(imager.prepare(48_000.0));
    let worker = AnalysisWorker::new(imager.shared()).expect("analysis worker");
    imager.begin_block();
    for index in 0..8192 {
        let sine = (std::f32::consts::TAU * 750.0 * index as f32 / 48_000.0).sin() * 0.5;
        let _ = imager.process_frame([sine, sine]);
    }
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut particles = Vec::new();
    while particles.is_empty() && Instant::now() < deadline {
        worker.drain(&mut particles);
        std::thread::yield_now();
    }
    assert!(!particles.is_empty(), "analysis did not complete before its deadline");
    assert!(particles.iter().all(|particle| {
        let [frequency, position] = particle.position();
        (0.0..=1.0).contains(&frequency)
            && (position - 0.5).abs() < 0.001
            && particle.color().iter().all(|value| value.is_finite() && *value >= 0.0)
    }));
}
