//! Source boundaries and monitoring preferences exercised through the actual audio producer and worker.

use std::sync::Arc;
use std::time::{Duration, Instant};

use image_control_dsp::{AnalysisStatus, AnalysisTap, AnalysisWorker, DisplayPreferences, Imager};

/// Waits for a bounded worker status without consuming its audio queue from another thread.
fn await_status(shared: &image_control_dsp::SharedState, expected: AnalysisStatus) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while shared.analysis_status() != expected && Instant::now() < deadline {
        std::thread::yield_now();
    }
    assert_eq!(shared.analysis_status(), expected);
}

#[test]
fn pre_is_untouched_input_and_post_is_final_mono_delta_output() {
    let mut imager = Imager::new();
    let shared = imager.shared();
    shared.set_analysis_tap(AnalysisTap::Pre);
    shared.set_analysis_enabled(true);
    shared.set_mono(true);
    imager.reset();
    for _ in 0..16 {
        let _ = imager.process_frame([0.75, -0.25]);
    }
    let mut frames = Vec::new();
    shared.drain_audio(&mut frames);
    assert_eq!(frames, vec![[0.75, -0.25]; 16]);
    shared.set_analysis_tap(AnalysisTap::Post);
    for _ in 0..16 {
        assert_eq!(imager.process_frame([0.75, -0.25]), [0.25; 2]);
    }
    shared.drain_audio(&mut frames);
    assert_eq!(frames, vec![[0.25; 2]; 16]);
    shared.set_delta(true);
    imager.reset();
    let _ = imager.process_frame([0.75, -0.25]);
    shared.drain_audio(&mut frames);
    assert_eq!(frames, vec![[0.0; 2]]);
}

#[test]
fn reset_tap_and_enable_changes_discard_queued_old_samples() {
    let mut imager = Imager::new();
    let shared = imager.shared();
    shared.set_analysis_enabled(true);
    for transition in 0..3 {
        let _ = imager.process_frame([1.0; 2]);
        let generation = shared.analysis_generation();
        match transition {
            0 => imager.reset(),
            1 => shared.set_analysis_tap(AnalysisTap::Pre),
            _ => {
                shared.set_analysis_enabled(false);
                shared.set_analysis_enabled(true);
            }
        }
        assert_ne!(generation, shared.analysis_generation());
        let _ = imager.process_frame([0.25; 2]);
        let mut frames = Vec::new();
        shared.drain_audio(&mut frames);
        assert_eq!(frames, [[0.25; 2]]);
    }
}

#[test]
fn same_rate_prepare_also_invalidates_queued_history() {
    let mut imager = Imager::new();
    assert!(imager.prepare(48_000.0));
    let shared = imager.shared();
    shared.set_analysis_tap(AnalysisTap::Pre);
    shared.set_analysis_enabled(true);
    let _ = imager.process_frame([1.0; 2]);
    let generation = shared.analysis_generation();
    assert!(imager.prepare(48_000.0));
    assert_ne!(generation, shared.analysis_generation());
    let _ = imager.process_frame([0.5; 2]);
    let mut frames = Vec::new();
    shared.drain_audio(&mut frames);
    assert_eq!(frames, [[0.5; 2]]);
}

#[test]
fn unsupported_analysis_rate_is_explicit_and_recovers_without_reopening_worker() {
    let mut imager = Imager::new();
    assert!(imager.prepare(4000.0));
    let shared = imager.shared();
    let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("worker starts independently of rate");
    await_status(&shared, AnalysisStatus::UnsupportedRate);
    assert!(imager.prepare(48_000.0));
    await_status(&shared, AnalysisStatus::Waiting);
    shared.set_analysis_tap(AnalysisTap::Pre);
    for index in 0..4096 {
        let sine = (std::f32::consts::TAU * index as f32 / 64.0).sin();
        let _ = imager.process_frame([sine; 2]);
    }
    await_status(&shared, AnalysisStatus::Running);
    let mut particles = Vec::new();
    let update = worker.drain_batch(&mut particles);
    assert_eq!(update.generation, shared.analysis_generation());
    assert_ne!(particles.len(), 0);
    shared.set_analysis_enabled(false);
    assert_eq!(shared.analysis_status(), AnalysisStatus::Disabled);
    shared.set_analysis_enabled(true);
    assert_ne!(shared.analysis_generation(), update.generation);
}

#[test]
fn silent_generation_clear_reaches_completed_batches() {
    let mut imager = Imager::new();
    let shared = imager.shared();
    let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("worker");
    imager.reset();
    let mut particles = Vec::new();
    let update = worker.drain_batch(&mut particles);
    assert_eq!(update.generation, shared.analysis_generation());
    assert_eq!(particles.len(), 0);
    shared.set_analysis_enabled(false);
    assert!(AnalysisWorker::new(Arc::clone(&shared)).is_err());
    drop(worker);
}

#[test]
fn monitoring_does_not_change_pcm_with_global_mono_delta_and_automation() {
    let mut measured = Imager::new();
    let mut plain = Imager::new();
    for imager in [&mut measured, &mut plain] {
        assert!(imager.prepare(48_000.0));
    }
    measured.shared().set_correlation_enabled(true);
    measured.shared().set_analysis_enabled(true);
    for index in 0..24_000 {
        if index % 127 == 0 {
            for imager in [&mut measured, &mut plain] {
                let shared = imager.shared();
                shared.set_mono(index > 4000 && index < 16_000);
                shared.set_delta(index > 12_000);
                shared.set_band_parameter(1, 2, if index > 8000 { 0.2 } else { 0.5 });
                shared.set_band_parameter(1, 3, 0.8);
                shared.set_band_parameter(2, 4, if index > 16_000 { 0.8 } else { 0.5 });
                imager.begin_block();
            }
        }
        if index % 503 == 0 {
            measured.shared().set_analysis_tap(if index % 2 == 0 {
                AnalysisTap::Pre
            } else {
                AnalysisTap::Post
            });
        }
        let frame = [(index as f32 * 0.13).sin() * 0.5, (index as f32 * 0.17).cos() * 0.3];
        assert_eq!(
            measured.process_frame(frame).map(f32::to_bits),
            plain.process_frame(frame).map(f32::to_bits)
        );
    }
    let snapshot = measured.shared().correlation_snapshot();
    assert!(snapshot.revision > 0);
    measured.shared().set_processing_active(false);
    assert!(
        measured
            .shared()
            .correlation_snapshot()
            .readings
            .iter()
            .all(|value| value.is_nan())
    );
}

#[test]
fn shutdown_is_bounded_under_continuously_refilling_producer() {
    let mut imager = Imager::new();
    let worker = AnalysisWorker::new(imager.shared()).expect("worker");
    let stop = std::sync::atomic::AtomicBool::new(false);
    std::thread::scope(|scope| {
        let stop_ref = &stop;
        scope.spawn(move || {
            while !stop_ref.load(std::sync::atomic::Ordering::Acquire) {
                let _ = imager.process_frame([0.1, -0.2]);
            }
        });
        let start = Instant::now();
        worker.shutdown_handle().shutdown();
        stop.store(true, std::sync::atomic::Ordering::Release);
        assert!(start.elapsed() < Duration::from_secs(5));
    });
}

#[test]
fn complete_preferences_round_trip_and_no_op_publication_preserves_generation() {
    let shared = Imager::new().shared();
    let preferences = DisplayPreferences {
        persistence_seconds: 1.25,
        contrast: 0.875,
        color_mode: image_control_dsp::ColourMode::Phase,
        mel_scale: true,
        solid_rendering: true,
        pan_midpoint_db: 6.0,
        pan_edge_db: 18.0,
        smooth_correlation: false,
        show_fps: true,
    };
    let initial_generation = shared.analysis_generation();
    assert!(shared.set_display_preferences(preferences));
    assert_eq!(shared.display_preferences(), preferences);
    let generation = shared.analysis_generation();
    assert_ne!(generation, initial_generation);
    assert!(shared.set_display_preferences(preferences));
    assert_eq!(shared.analysis_generation(), generation);
}

#[test]
fn malformed_preferences_are_transactionally_rejected() {
    let shared = Imager::new().shared();
    let negative_zero = DisplayPreferences {
        contrast: -0.0,
        ..DisplayPreferences::default()
    };
    assert!(shared.set_display_preferences(negative_zero));
    assert_eq!(
        shared.display_preferences().color_mode,
        image_control_dsp::ColourMode::Frequency
    );
    assert!(shared.set_display_preferences(DisplayPreferences::default()));
    let initial = shared.display_preferences();
    let generation = shared.analysis_generation();
    for invalid in [
        DisplayPreferences {
            persistence_seconds: f32::NAN,
            ..initial
        },
        DisplayPreferences {
            persistence_seconds: 0.0,
            ..initial
        },
        DisplayPreferences {
            contrast: 1.1,
            ..initial
        },
        DisplayPreferences {
            pan_midpoint_db: f32::NAN,
            ..initial
        },
        DisplayPreferences {
            pan_edge_db: f32::INFINITY,
            ..initial
        },
        DisplayPreferences {
            pan_midpoint_db: 2.99,
            ..initial
        },
        DisplayPreferences {
            pan_midpoint_db: 24.01,
            ..initial
        },
        DisplayPreferences {
            pan_edge_db: 11.99,
            ..initial
        },
        DisplayPreferences {
            pan_edge_db: 60.01,
            ..initial
        },
        DisplayPreferences {
            pan_midpoint_db: 24.0,
            pan_edge_db: 26.99,
            ..initial
        },
    ] {
        assert!(!shared.set_display_preferences(invalid));
        assert_eq!(shared.display_preferences(), initial);
        assert_eq!(shared.analysis_generation(), generation);
    }
}
