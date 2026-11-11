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
