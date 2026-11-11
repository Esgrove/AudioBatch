//! Atomic source and meter invalidation contracts under concurrent control-thread publication.

use std::sync::atomic::{AtomicBool, Ordering};

use image_control_dsp::{AnalysisTap, ColourMode, DisplayPreferences, Imager};

#[test]
fn tap_and_preference_changes_never_publish_stale_audio_after_generation_change() {
    let mut imager = Imager::new();
    let shared = imager.shared();
    shared.set_analysis_enabled(true);
    let done = AtomicBool::new(false);
    std::thread::scope(|scope| {
        let shared_ref = &shared;
        let done_ref = &done;
        scope.spawn(move || {
            for index in 0..2000 {
                shared_ref.set_analysis_tap(if index % 2 == 0 {
                    AnalysisTap::Pre
                } else {
                    AnalysisTap::Post
                });
                assert!(shared_ref.set_display_preferences(DisplayPreferences {
                    color_mode: if index % 2 == 0 {
                        ColourMode::Phase
                    } else {
                        ColourMode::Frequency
                    },
                    contrast: index as f32 / 2000.0,
                    ..DisplayPreferences::default()
                }));
            }
            done_ref.store(true, Ordering::Release);
        });
        while !done.load(Ordering::Acquire) {
            let _ = imager.process_frame([0.25, -0.5]);
        }
    });
    shared.invalidate_monitoring();
    let _ = imager.process_frame([0.75, -0.125]);
    let mut frames = Vec::new();
    shared.drain_audio(&mut frames);
    // A full old queue may drop the new frame. It must never return any old generation.
    assert!(
        frames
            .iter()
            .all(|frame| frame.map(f32::to_bits) == [0.75_f32, -0.125].map(f32::to_bits))
    );
}

#[test]
fn display_records_remain_coherent_under_multiple_non_audio_writers() {
    let shared = Imager::new().shared();
    let other = Imager::new().shared();
    let first = DisplayPreferences::default();
    let second = DisplayPreferences {
        persistence_seconds: 2.75,
        contrast: 0.125,
        color_mode: ColourMode::Phase,
        mel_scale: true,
        solid_rendering: true,
        pan_midpoint_db: 24.0,
        pan_edge_db: 60.0,
        smooth_correlation: false,
        show_fps: true,
    };
    std::thread::scope(|scope| {
        for preferences in [first, second] {
            let shared = &shared;
            scope.spawn(move || {
                for _ in 0..2000 {
                    assert!(shared.set_display_preferences(preferences));
                }
            });
        }
        for _ in 0..4000 {
            let observed = shared.display_preferences();
            assert!(observed == first || observed == second, "preference fields cannot tear");
            assert_eq!(other.display_preferences(), first, "instances remain independent");
        }
    });
    let mut detached = shared.display_preferences();
    detached.show_fps = !detached.show_fps;
    assert_ne!(
        shared.display_preferences(),
        detached,
        "detached copies cannot modify stored preferences"
    );
}

#[test]
fn correlation_invalidation_does_not_require_access_to_processing_history() {
    let mut imager = Imager::new();
    assert!(imager.prepare(48_000.0));
    let shared = imager.shared();
    shared.set_correlation_enabled(true);
    imager.begin_block();
    for index in 0..16_384 {
        let sample = (index as f32 * 0.1).sin();
        let _ = imager.process_frame([sample; 2]);
    }
    assert!(
        shared
            .correlation_snapshot()
            .readings
            .iter()
            .any(|value| value.is_finite())
    );
    let initial = shared.correlation_snapshot().generation;
    shared.set_correlation_enabled(false);
    shared.set_correlation_enabled(true);
    let snapshot = shared.correlation_snapshot();
    assert_ne!(initial, snapshot.generation);
    assert!(snapshot.readings.iter().all(|value| value.is_nan()));
    // A partial old partition completing before the next block must not publish its old identity.
    for _ in 0..256 {
        let _ = imager.process_frame([0.25; 2]);
    }
    assert!(
        shared
            .correlation_snapshot()
            .readings
            .iter()
            .all(|value| value.is_nan())
    );
}
