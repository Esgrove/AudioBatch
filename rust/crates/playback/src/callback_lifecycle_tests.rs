//! Hardware-free lifecycle contracts for stream epochs, capture retirement, and renderer disposal.

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
#[cfg(not(audiobatch_rtsan))]
use std::sync::mpsc::{Receiver, Sender};
use std::sync::{Arc, Weak};
use std::thread;

use crossbeam_channel::bounded;
use rtrb::{Producer, RingBuffer};

use crate::output::SharedState;
use crate::output::insert::DeviceFormat;
use crate::pipeline::Frame;
use crate::preview_insert::{InsertControl, insert_channel};
use crate::{
    CallbackCapture, OutputRenderer, PreviewInsert, RendererAccess, StreamCapture, StreamErrors, render_typed_callback,
    wait_for_stream_captures,
};

/// Records destruction of insert-owned storage on its construction thread.
struct DropInsert {
    dropped: Arc<AtomicBool>,
    owner: thread::ThreadId,
}

/// Checks that a flushed insert is destroyed once by its owner after renderer exclusion ends.
struct FlushInsert {
    drops: Arc<AtomicUsize>,
    owner: thread::ThreadId,
    access: Weak<RendererAccess>,
}

/// Holds a callback in insert processing so a control mutation can overlap it deterministically.
#[cfg(not(audiobatch_rtsan))]
struct BlockingInsert {
    entered: Sender<()>,
    release: Receiver<()>,
}

/// Prepared renderer and its production queues, without constructing a native device.
struct Harness {
    access: Arc<RendererAccess>,
    shared: Arc<SharedState>,
    producer: Producer<Frame>,
    insert: InsertControl,
}

impl PreviewInsert for DropInsert {
    fn max_block_frames(&self) -> usize {
        64
    }
    fn latency_frames(&self) -> usize {
        0
    }
    fn tail_frames(&self) -> usize {
        0
    }
    fn reset(&mut self, _position_frames: u64) {}
    fn process(&mut self, _left: &mut [f32], _right: &mut [f32]) -> bool {
        true
    }
}

impl Drop for DropInsert {
    fn drop(&mut self) {
        assert_eq!(thread::current().id(), self.owner, "insert must die on control owner");
        self.dropped.store(true, Ordering::Release);
    }
}

impl PreviewInsert for FlushInsert {
    fn max_block_frames(&self) -> usize {
        64
    }
    fn latency_frames(&self) -> usize {
        0
    }
    fn tail_frames(&self) -> usize {
        0
    }
    fn reset(&mut self, _position_frames: u64) {}
    fn process(&mut self, _left: &mut [f32], _right: &mut [f32]) -> bool {
        true
    }
}

impl Drop for FlushInsert {
    fn drop(&mut self) {
        assert_eq!(thread::current().id(), self.owner, "insert must die on control owner");
        let access = self.access.upgrade().expect("renderer is still owned");
        assert_eq!(
            access.renderer.try_with(|_| ()),
            Some(()),
            "drop must be outside exclusion"
        );
        assert_eq!(
            self.drops.fetch_add(1, Ordering::AcqRel),
            0,
            "insert must die exactly once"
        );
    }
}

#[cfg(not(audiobatch_rtsan))]
impl PreviewInsert for BlockingInsert {
    fn max_block_frames(&self) -> usize {
        64
    }
    fn latency_frames(&self) -> usize {
        0
    }
    fn tail_frames(&self) -> usize {
        0
    }
    fn reset(&mut self, _position_frames: u64) {}
    fn process(&mut self, _left: &mut [f32], _right: &mut [f32]) -> bool {
        self.entered.send(()).expect("callback reached insert");
        self.release.recv().expect("test releases callback");
        true
    }
}

impl Harness {
    /// Prepares real renderer state and one generation of production input.
    fn new() -> Self {
        let shared = Arc::new(SharedState::new());
        shared.generation.store(1, Ordering::Relaxed);
        shared.source_rate.store(48_000, Ordering::Relaxed);
        shared.playing.store(true, Ordering::Relaxed);
        let (mut producer, consumer) = RingBuffer::new(512);
        for position in 0..128 {
            producer
                .push(Frame {
                    generation: 1,
                    position,
                    left: 0.5,
                    right: -0.25,
                })
                .expect("input capacity");
        }
        let (insert, renderer) = insert_channel(Arc::clone(&shared));
        let renderer = OutputRenderer::new(
            Arc::clone(&shared),
            consumer,
            DeviceFormat {
                channels: 2,
                rate: 48_000,
            },
            renderer,
        );
        Self {
            access: Arc::new(RendererAccess::new(renderer)),
            shared,
            producer,
            insert,
        }
    }

    /// Queues an insert whose lifetime and disposal context remain observable after it is retired.
    fn queue_flush_insert(&mut self) -> Arc<AtomicUsize> {
        let drops = Arc::new(AtomicUsize::new(0));
        self.insert
            .set(Some(Box::new(FlushInsert {
                drops: Arc::clone(&drops),
                owner: thread::current().id(),
                access: Arc::downgrade(&self.access),
            })))
            .expect("insert capacity");
        drops
    }

    /// Fills both production queues, then flushes a final removal or replacement without device callbacks.
    fn check_saturated_flush(&mut self, replace: bool) {
        // Ten applied inserts leave nine retired entries and one live insert.
        let mut drops = Vec::new();
        for _ in 0..10 {
            drops.push(self.queue_flush_insert());
            self.access.renderer.with_control(OutputRenderer::apply_insert_changes);
        }
        assert!(self.insert.is_settled());
        // Eight pending commands fill the command queue while retirement prevents any further application.
        for _ in 0..7 {
            drops.push(self.queue_flush_insert());
        }
        let replacement = if replace {
            Some(self.queue_flush_insert())
        } else {
            self.insert.set(None).expect("final removal capacity");
            None
        };
        self.access.renderer.with_control(OutputRenderer::apply_insert_changes);
        assert!(!self.insert.is_settled());
        let retired = self.access.flush_insert_changes(&mut self.insert);
        assert!(
            self.insert.is_settled(),
            "one flush must settle the entire captured queue"
        );
        assert_eq!(
            retired.len(),
            drops.len(),
            "return both earlier and newly retired boxes"
        );
        assert!(drops.iter().all(|count| count.load(Ordering::Acquire) == 0));
        drop(retired);
        assert!(drops.iter().all(|count| count.load(Ordering::Acquire) == 1));
        assert!(self.insert.take_retired().is_empty());
        if let Some(replacement) = replacement {
            assert_eq!(
                replacement.load(Ordering::Acquire),
                0,
                "final replacement stays attached"
            );
            self.insert.set(None).expect("detach replacement");
            let retired = self.access.flush_insert_changes(&mut self.insert);
            assert_eq!(retired.len(), 1);
            drop(retired);
            assert_eq!(replacement.load(Ordering::Acquire), 1);
        }
        drop(self.access.renderer.retire());
        assert!(drops.iter().all(|count| count.load(Ordering::Acquire) == 1));
    }

    /// Creates a stream's pinned callback state and errors with the same production types.
    fn capture(&self, epoch: u64) -> Arc<StreamCapture> {
        let (sender, _receiver) = bounded(8);
        Arc::new(StreamCapture::new(
            Arc::clone(&self.access),
            epoch,
            StreamErrors {
                shared: Arc::clone(&self.shared),
                sender,
            },
        ))
    }
}

#[test]
#[cfg(not(audiobatch_rtsan))]
fn control_epoch_change_waits_for_actual_inflight_insert_processing() {
    let mut harness = Harness::new();
    let (entered, entry) = mpsc::channel();
    let (release, released) = mpsc::channel();
    harness
        .insert
        .set(Some(Box::new(BlockingInsert {
            entered,
            release: released,
        })))
        .expect("insert capacity");
    let epoch = harness.access.advance_epoch();
    let callback_access = Arc::clone(&harness.access);
    let control_access = Arc::clone(&harness.access);
    let changed = Arc::new(AtomicBool::new(false));
    thread::scope(|scope| {
        scope.spawn(|| {
            let mut output = [0.0_f32; 64];
            render_typed_callback(&callback_access, epoch, &mut output);
        });
        entry.recv().expect("callback processing holds admission");
        let completed = Arc::clone(&changed);
        scope.spawn(move || {
            control_access.advance_epoch();
            completed.store(true, Ordering::Release);
        });
        while !harness.access.renderer.control_requested() {
            thread::yield_now();
        }
        assert!(!changed.load(Ordering::Acquire));
        let mut excluded = [1.0_f32; 64];
        render_typed_callback(&harness.access, epoch, &mut excluded);
        assert!(excluded.iter().all(|sample| sample.abs() < f32::EPSILON));
        release.send(()).expect("complete original callback");
    });
    assert!(changed.load(Ordering::Acquire));
    drop(harness.access.renderer.retire());
}

#[test]
fn delayed_old_callback_cannot_consume_replacement_stream_audio() {
    let harness = Harness::new();
    let old = harness.access.advance_epoch();
    let (start, started) = mpsc::channel();
    let access = Arc::clone(&harness.access);
    thread::scope(|scope| {
        let delayed = scope.spawn(move || {
            started.recv().expect("replacement published");
            let mut output = [1.0_f32; 64];
            render_typed_callback(&access, old, &mut output);
            assert!(output.iter().all(|sample| sample.abs() < f32::EPSILON));
        });
        let current = harness.access.advance_epoch();
        start.send(()).expect("release delayed callback");
        delayed.join().expect("old callback exits");
        assert_eq!(harness.shared.position.load(Ordering::Relaxed), 0);
        assert_eq!(harness.producer.slots(), 384);
        let mut output = [0.0_f32; 64];
        render_typed_callback(&harness.access, current, &mut output);
        assert!(output.iter().any(|sample| sample.abs() > f32::EPSILON));
        assert_eq!(harness.shared.position.load(Ordering::Relaxed), 32);
    });
    drop(harness.access.renderer.retire());
}

#[test]
fn failed_start_invalidation_and_stale_error_cannot_stop_reopened_stream() {
    let harness = Harness::new();
    let failed_epoch = harness.access.advance_epoch();
    let capture = harness.capture(failed_epoch);
    capture.close();
    harness.access.advance_epoch();
    let current_epoch = harness.access.advance_epoch();
    let mut output = [1.0_f32; 32];
    render_typed_callback(&harness.access, failed_epoch, &mut output);
    assert!(output.iter().all(|sample| sample.abs() < f32::EPSILON));
    capture.report(cpal::Error::new(cpal::ErrorKind::DeviceNotAvailable));
    assert!(harness.shared.playing.load(Ordering::Relaxed));
    assert!(!harness.shared.stream_lost.load(Ordering::Acquire));
    render_typed_callback(&harness.access, current_epoch, &mut output);
    assert!(output.iter().any(|sample| sample.abs() > f32::EPSILON));
    drop(harness.access.renderer.retire());
}

#[test]
fn capture_pins_wait_for_both_backend_acknowledgements() {
    let harness = Harness::new();
    let capture = harness.capture(harness.access.advance_epoch());
    let render = CallbackCapture {
        capture: Arc::clone(&capture),
        is_error: false,
    };
    let errors = CallbackCapture {
        capture: Arc::clone(&capture),
        is_error: true,
    };
    assert!(!StreamCapture::released(&capture));
    drop(render);
    assert!(!StreamCapture::released(&capture));
    thread::spawn(move || drop(errors))
        .join()
        .expect("backend error capture disposal");
    assert!(StreamCapture::released(&capture));
    capture.close();
    drop(harness.access.renderer.retire());
}

#[test]
fn final_owner_retains_capture_storage_until_both_backend_closures_drop() {
    let harness = Harness::new();
    let capture = harness.capture(harness.access.advance_epoch());
    let weak = Arc::downgrade(&capture);
    let render = CallbackCapture {
        capture: Arc::clone(&capture),
        is_error: false,
    };
    let errors = CallbackCapture {
        capture: Arc::clone(&capture),
        is_error: true,
    };
    capture.close();
    drop(harness.access.renderer.retire());
    let mut captures = vec![capture];
    thread::scope(|scope| {
        scope.spawn(move || {
            drop(render);
            assert!(
                weak.upgrade().is_some(),
                "owner pin must outlive a single backend closure"
            );
            drop(errors);
        });
        wait_for_stream_captures(&mut captures);
    });
    assert!(captures.is_empty());
}

#[test]
fn teardown_retires_actual_insert_before_a_delayed_callback_capture() {
    let mut harness = Harness::new();
    let dropped = Arc::new(AtomicBool::new(false));
    harness
        .insert
        .set(Some(Box::new(DropInsert {
            dropped: Arc::clone(&dropped),
            owner: thread::current().id(),
        })))
        .expect("insert queue capacity");
    let epoch = harness.access.advance_epoch();
    let capture = harness.capture(epoch);
    let mut output = [0.0_f32; 64];
    render_typed_callback(&harness.access, epoch, &mut output);
    capture.close();
    harness.access.advance_epoch();
    drop(harness.access.renderer.retire());
    assert!(dropped.load(Ordering::Acquire));
    let render = CallbackCapture {
        capture: Arc::clone(&capture),
        is_error: false,
    };
    thread::spawn(move || {
        let mut output = [1.0_f32; 64];
        render.render(&mut output);
        assert!(output.iter().all(|sample| sample.abs() < f32::EPSILON));
    })
    .join()
    .expect("delayed retired callback");
}

#[test]
fn full_retirement_and_pending_queues_flush_final_removal_on_control() {
    Harness::new().check_saturated_flush(false);
}

#[test]
fn full_retirement_and_pending_queues_flush_final_replacement_on_control() {
    Harness::new().check_saturated_flush(true);
}

#[test]
fn idle_insert_detach_settles_synchronously_and_retires_on_control() {
    let mut harness = Harness::new();
    let dropped = Arc::new(AtomicBool::new(false));
    harness
        .insert
        .set(Some(Box::new(DropInsert {
            dropped: Arc::clone(&dropped),
            owner: thread::current().id(),
        })))
        .expect("attach");
    harness
        .access
        .renderer
        .with_control(OutputRenderer::apply_insert_changes);
    harness.insert.set(None).expect("detach");
    harness
        .access
        .renderer
        .with_control(OutputRenderer::apply_insert_changes);
    assert!(harness.insert.is_settled());
    assert!(!dropped.load(Ordering::Acquire));
    drop(harness.insert.take_retired());
    assert!(dropped.load(Ordering::Acquire));
    drop(harness.access.renderer.retire());
}
