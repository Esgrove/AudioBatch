//! Real processing-to-worker contracts using deterministic audio and a bounded completion deadline.

use std::time::{Duration, Instant};

use image_control_dsp::{AnalysisWorker, Imager};

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
