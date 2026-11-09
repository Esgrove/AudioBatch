//! Repeated stream and transport transitions with production typed callback admission.

use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread;

use rtrb::RingBuffer;

use crate::output::insert::DeviceFormat;
use crate::output::{OutputRenderer, SharedState};
use crate::pipeline::Frame;
use crate::preview_insert::insert_channel;
use crate::{RendererAccess, render_typed_callback};

#[test]
fn repeated_reopen_and_generation_changes_preserve_source_audio() {
    let shared = Arc::new(SharedState::new());
    shared.source_rate.store(48_000, Ordering::Relaxed);
    shared.playing.store(true, Ordering::Relaxed);
    let (mut producer, consumer) = RingBuffer::new(128);
    let (_control, insert) = insert_channel(Arc::clone(&shared));
    let access = RendererAccess::new(OutputRenderer::new(
        Arc::clone(&shared),
        consumer,
        DeviceFormat {
            channels: 2,
            rate: 48_000,
        },
        insert,
    ));
    let mut previous_epoch = access.advance_epoch();
    for generation in 1..33 {
        shared.generation.store(generation, Ordering::Release);
        for position in 0..63 {
            producer
                .push(Frame {
                    generation,
                    position,
                    left: 0.5,
                    right: -0.25,
                })
                .expect("prepared generation");
        }
        let current_epoch = access.advance_epoch();
        assert!(current_epoch > previous_epoch);
        let mut output = [1.0_f32; 126];
        thread::scope(|scope| {
            scope
                .spawn(|| render_typed_callback(&access, previous_epoch, &mut output))
                .join()
                .expect("stale callback");
        });
        assert!(output.iter().all(|sample| sample.abs() < f32::EPSILON));
        assert_eq!(producer.slots(), 65);
        render_typed_callback(&access, current_epoch, &mut output);
        assert!(
            output
                .as_chunks::<2>()
                .0
                .iter()
                .all(|&[left, right]| (left - 0.5).abs() < f32::EPSILON && (right + 0.25).abs() < f32::EPSILON)
        );
        assert_eq!(shared.position.load(Ordering::Relaxed), 63);
        assert_eq!(shared.underruns.load(Ordering::Relaxed), 0);
        previous_epoch = current_epoch;
    }
    drop(access.renderer.retire());
}
