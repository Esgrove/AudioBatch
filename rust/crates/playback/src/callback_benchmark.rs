//! Hardware-free measurements of the complete production typed callback, available only in libtest.
//!
//! Run `callback_benchmark::production_callback` with `--ignored --exact --nocapture --test-threads=1`.
//! Environment variables use the `AUDIOBATCH_CALLBACK_` prefix:
//! `WORKLOAD=idle|dry|wet|native-wet|contention|transitions`, `FORMAT=f32|f64|i16|i24|i32|u16`,
//! `RATE=44100|48000|96000`, `BLOCKS=32,63,127`, `CALLBACKS=4096`, `WARMUP=256`,
//! `MODE=latency|paced|idle_cpu`, `TRANSITION_INTERVAL=32`, `COLD_ONLY=0|1`, and `LABEL=<build identity>`.
//! `COLD_ONLY=1` runs only the fresh thread's first callback, repeat the executable for cold distributions.
//! `idle_cpu` measures a sleeping callback thread without invoking the callback.
//! `paced` omits callback duration clocks and records actual callback-start lateness against cumulative start deadlines.
//! Each batch restarts its schedule after the untimed refill and handshake, and retains a final audio-period sleep.
//! The first callback on the persistent thread is retained separately from warmup and steady-state samples.
//! No audio hardware, decoder, or GUI framework is involved.
//! Only the opt-in `native-wet` companion loads the repository's built `AudioBatch Test MS` VST3.
//! It has no frozen pre-fix production callback baseline and must not be presented as a paired gate result.
//!
//! The control thread fills the bounded production-sized queue before each batch and waits for completion.
//! There is no concurrent producer to starve the callback, and queue writes and control setup are not timed.
//! CPU counters enclose batches, including pacing and loop overhead but excluding queue refill and handshakes.
//! Output verification and oracle replay run after all callback CPU intervals and worker join.
//! The controller snapshots production atomics after each completion handshake and before dispatching another batch.

use std::env;
use std::hint::black_box;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread;
use std::time::{Duration, Instant};

use cpal::{FromSample, Sample, SizedSample};
use crossbeam_channel::{Receiver, Sender, bounded};
use rtrb::{Producer, RingBuffer};
use serde_json::{Value, json};

use crate::callback_benchmark_cpu::{CpuClock, CpuTimes};
use crate::callback_benchmark_native::NativeFixture;
use crate::output::insert::{DeviceFormat, TRANSITION_FRAMES};
use crate::output::{OutputRenderer, SharedState};
use crate::pipeline::Frame;
use crate::preview_insert::{InsertControl, PreviewInsert, insert_channel};
use crate::{BUFFER_SECONDS, RendererAccess, render_typed_callback};

/// Keeps output and timing storage bounded even for malformed environment input.
const MAX_CALLBACKS: usize = 100_000;
/// Avoids a large preallocated output when a large block is paired with many callbacks.
const MAX_OUTPUT_SAMPLES: usize = 32_000_000;
/// Number of control-free callbacks between queue refills, also bounded by queue capacity.
const MAX_BATCH_CALLBACKS: usize = 128;
/// Deterministic exclusion frequency for the contention workload.
const CONTENTION_INTERVAL: usize = 8;
/// Most frames accepted by the fake insert, so uneven callbacks exercise internal splitting.
const INSERT_BLOCK_FRAMES: usize = 64;
/// Stable hash basis for converted output checksums.
const HASH_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
/// Stable hash multiplier for converted output checksums.
const HASH_MULTIPLIER: u64 = 0x100_0000_01b3;

/// Which production rendering path or control interaction is exercised.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Workload {
    Idle,
    Dry,
    Wet,
    NativeWet,
    Contention,
    Transitions,
}

/// Individual callback latency, separately paced CPU, or no-callback sleeping CPU.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Latency,
    Paced,
    IdleCpu,
}

/// One environment-configured invocation, serialized with the result for reproducibility.
struct Options {
    workload: Workload,
    mode: Mode,
    format: String,
    rate: u32,
    blocks: Vec<usize>,
    callbacks: usize,
    warmup: usize,
    transition_interval: usize,
    cold_only: bool,
    label: String,
}

/// Allocation-free stereo matrix with zero latency and no tail.
struct FakeInsert;

/// One preallocated callback record, filled without allocation on the persistent thread.
#[derive(Default)]
struct CallbackSample {
    frames: usize,
    duration_ns: u64,
    start_lateness_ns: u64,
    non_silent_frames: usize,
    expected_active_frames: usize,
    expected_resets: usize,
    excluded: bool,
    checksum: u64,
}

/// Prepared batch descriptor, sent before the callback CPU interval begins.
struct Batch {
    start: usize,
    end: usize,
    excluded: bool,
}

/// Batch completion, published immediately after the CPU interval and before output verification.
struct Completion {
    finished: Instant,
    thread_ns: u64,
    process_ns: u64,
    wall_ns: u64,
}

/// Controller-owned raw work and CPU accounting for one bounded batch.
struct BatchSample {
    start: usize,
    end: usize,
    consumed: usize,
    stale_discarded: usize,
    prepared: usize,
    snapshot: StateSnapshot,
    expected_active_frames: usize,
    expected_resets: usize,
    excluded: bool,
    transition: Option<&'static str>,
    transition_ns: Option<u64>,
    completion: Completion,
}

/// Quiescent production atomics observed by the controller after the batch-completion handshake.
#[derive(Debug, PartialEq, Eq)]
struct StateSnapshot {
    position: u64,
    generation: u32,
    playing: bool,
    insert_changes: u64,
    insert_settled: bool,
    insert_failed: bool,
    ended: bool,
    stream_lost: bool,
    underruns: u64,
    callback_frames: u32,
}

/// Independent zero-latency fixture model, never called within wall-clock or thread-CPU timing.
struct Oracle {
    source_position: u64,
    prepared_position: u64,
    position: u64,
    generation: u32,
    playing: bool,
    wet: bool,
    insert_changes: u64,
    pending_changes: u64,
    active: bool,
    paused: bool,
    needs_reset: bool,
    insert_generation: u32,
    faded_in: usize,
    fade_remaining: Option<usize>,
}

/// Expected source consumption and reset behavior for one callback.
struct ExpectedCallback {
    audio: Vec<(f32, f32)>,
    active_frames: usize,
    resets: usize,
}

/// State prepared on the control thread and shared with the production callback.
struct Harness {
    shared: Arc<SharedState>,
    renderer: Arc<RendererAccess>,
    producer: Producer<Frame>,
    control: InsertControl,
    capacity: usize,
    next_position: u64,
    stale_frames: usize,
    native: Option<NativeFixture>,
}

impl Options {
    /// Parses and bounds the standalone libtest command without changing the process environment.
    fn from_environment() -> Self {
        let workload = match setting("WORKLOAD", "dry").as_str() {
            "idle" => Workload::Idle,
            "dry" => Workload::Dry,
            "wet" => Workload::Wet,
            "native-wet" => Workload::NativeWet,
            "contention" => Workload::Contention,
            value => {
                assert_eq!(value, "transitions", "unknown WORKLOAD");
                Workload::Transitions
            }
        };
        let mode = match setting("MODE", "latency").as_str() {
            "latency" => Mode::Latency,
            "paced" => Mode::Paced,
            value => {
                assert_eq!(value, "idle_cpu", "unknown MODE");
                Mode::IdleCpu
            }
        };
        let format = setting("FORMAT", "f32");
        assert!(["f32", "f64", "i16", "i24", "i32", "u16"].contains(&format.as_str()));
        let rate = setting("RATE", "48000").parse().expect("integer RATE");
        assert!([44_100, 48_000, 96_000].contains(&rate));
        let blocks: Vec<usize> = setting("BLOCKS", "32,63,127")
            .split(',')
            .map(|block| block.parse().expect("integer BLOCKS"))
            .collect();
        assert!(!blocks.is_empty() && blocks.iter().all(|&block| (1..=4096).contains(&block)));
        let callbacks = integer_setting("CALLBACKS", "4096", 1, MAX_CALLBACKS);
        let warmup = integer_setting("WARMUP", "256", 0, MAX_CALLBACKS);
        let transition_interval = integer_setting("TRANSITION_INTERVAL", "32", 1, MAX_CALLBACKS);
        let options = Self {
            workload,
            mode,
            format,
            rate,
            blocks,
            callbacks,
            warmup,
            transition_interval,
            cold_only: integer_setting("COLD_ONLY", "0", 0, 1) == 1,
            label: setting("LABEL", "unlabelled"),
        };
        assert!(
            options.total_samples() <= MAX_OUTPUT_SAMPLES,
            "output storage exceeds bounded limit"
        );
        options
    }

    /// Includes the first cold call and untimed warmup in bounded preallocation.
    const fn total_callbacks(&self) -> usize {
        if self.cold_only {
            1
        } else {
            1 + self.warmup + self.callbacks
        }
    }

    /// Selects a repeating block sequence without introducing random workload variation.
    fn frames(&self, callback: usize) -> usize {
        *self.blocks.get(callback % self.blocks.len()).expect("nonempty blocks")
    }

    /// Counts every stereo output sample before constructing the callback thread.
    fn total_samples(&self) -> usize {
        (0..self.total_callbacks()).map(|index| self.frames(index) * 2).sum()
    }

    /// Distinguishes cold initialization, warmup, and measured samples in the durable output.
    const fn phase(&self, callback: usize) -> &'static str {
        if callback == 0 {
            "cold"
        } else if callback <= self.warmup {
            "warmup"
        } else {
            "steady"
        }
    }
}

impl PreviewInsert for FakeInsert {
    fn max_block_frames(&self) -> usize {
        INSERT_BLOCK_FRAMES
    }
    fn latency_frames(&self) -> usize {
        0
    }
    fn tail_frames(&self) -> usize {
        0
    }
    fn reset(&mut self, _position_frames: u64) {}
    fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool {
        for (left, right) in left.iter_mut().zip(right.iter_mut()) {
            let mid = f32::midpoint(*left, *right);
            let side = (*left - *right) * 0.25;
            *left = f32::midpoint(mid, side);
            *right = (mid - side) * 0.5;
        }
        true
    }
}

impl Oracle {
    /// Starts from the benchmark's published transport intent, not observed callback output.
    fn new(options: &Options) -> Self {
        let wet = matches!(options.workload, Workload::Wet | Workload::NativeWet);
        Self {
            source_position: 0,
            prepared_position: 0,
            position: 0,
            generation: 1,
            playing: options.workload != Workload::Idle,
            wet,
            insert_changes: 0,
            pending_changes: u64::from(wet),
            active: false,
            paused: false,
            needs_reset: wet,
            insert_generation: 0,
            faded_in: 0,
            fade_remaining: None,
        }
    }

    /// Applies the independently specified six-step transport sequence before modeling its callbacks.
    fn transition(&mut self, number: usize) -> usize {
        match number % 6 {
            0 | 4 => {
                self.wet = number.is_multiple_of(6);
                self.pending_changes += 1;
                self.active = false;
                self.paused = false;
                self.needs_reset = true;
            }
            1 => self.playing = false,
            2 => self.playing = true,
            5 => {
                self.insert_changes += self.pending_changes;
                self.pending_changes = 0;
                self.playing = true;
            }
            3 => {
                let stale = usize::try_from(self.prepared_position - self.source_position).expect("bounded queue");
                self.source_position = self.prepared_position;
                self.position = self.source_position;
                self.generation += 1;
                return stale;
            }
            _ => unreachable!("six-step sequence"),
        }
        0
    }

    /// Evaluates active windows, both fades, reset boundaries, and the independently expanded stereo matrix.
    fn callback(&mut self, frames: usize, excluded: bool, options: &Options) -> ExpectedCallback {
        let mut expected = ExpectedCallback {
            audio: Vec::with_capacity(frames),
            active_frames: 0,
            resets: 0,
        };
        if options.mode == Mode::IdleCpu || excluded {
            expected.audio.resize(frames, (0.0, 0.0));
            return expected;
        }
        self.insert_changes += self.pending_changes;
        self.pending_changes = 0;
        if self.wet {
            if self.playing {
                if self.needs_reset || self.insert_generation != self.generation || (!self.active && !self.paused) {
                    self.active = true;
                    self.paused = false;
                    self.needs_reset = false;
                    self.insert_generation = self.generation;
                    self.faded_in = 0;
                    self.fade_remaining = None;
                    expected.resets = 1;
                } else if !self.active || self.fade_remaining.is_some() {
                    self.active = true;
                    self.paused = false;
                    self.fade_remaining = None;
                    self.faded_in = 0;
                }
            } else if self.active && self.fade_remaining.is_none() {
                self.fade_remaining = Some(TRANSITION_FRAMES);
            }
        }
        for _ in 0..frames {
            if !(if self.wet { self.active } else { self.playing }) {
                expected.audio.push((0.0, 0.0));
                continue;
            }
            let source = source_frame(self.generation, self.source_position);
            let (mut left, mut right) = if options.workload == Workload::NativeWet {
                // The native operations are (L+R)/2 and (L-R)/2 at width 1, then gain 0.5.
                // This power-of-two fixture makes reconstruction exactly L*0.5 and R*0.5 in binary32.
                (source.left * 0.5, source.right * 0.5)
            } else if self.wet {
                (
                    source.right.mul_add(0.125, source.left * 0.375),
                    source.right.mul_add(0.375, source.left * 0.125),
                )
            } else {
                (source.left, source.right)
            };
            if self.wet {
                self.faded_in = (self.faded_in + 1).min(TRANSITION_FRAMES);
                let mut gain = self.faded_in as f32 / TRANSITION_FRAMES as f32;
                if let Some(remaining) = &mut self.fade_remaining {
                    gain *= *remaining as f32 / TRANSITION_FRAMES as f32;
                    *remaining -= 1;
                    if *remaining == 0 {
                        self.active = false;
                        self.paused = true;
                    }
                }
                left *= gain;
                right *= gain;
            }
            expected.audio.push((left, right));
            self.source_position += 1;
            self.position = self.source_position;
            expected.active_frames += 1;
        }
        expected
    }

    /// Checks quiescent production state without permitting a failed or stopped callback to redefine expectations.
    fn validate_snapshot(&self, snapshot: &StateSnapshot, callback_frames: u32) {
        assert_eq!(snapshot.position, self.position, "source playhead acknowledgement");
        assert_eq!(snapshot.generation, self.generation, "generation acknowledgement");
        assert_eq!(snapshot.playing, self.playing, "transport intent must remain intact");
        assert_eq!(
            snapshot.insert_changes, self.insert_changes,
            "insert change acknowledgement"
        );
        assert_eq!(
            snapshot.insert_settled,
            self.pending_changes == 0,
            "insert queue acknowledgement"
        );
        assert!(!snapshot.insert_failed, "insert failure cannot pass as finite silence");
        assert!(!snapshot.ended, "prepared source cannot reach EOF");
        assert!(!snapshot.stream_lost, "benchmark stream cannot fail");
        assert_eq!(snapshot.underruns, 0, "prepared source cannot underrun");
        assert_eq!(
            snapshot.callback_frames, callback_frames,
            "callback admission acknowledgement"
        );
    }
}

impl Harness {
    /// Builds exactly the production renderer and publishes its first stream epoch on the control thread.
    fn new(options: &Options) -> Self {
        let shared = Arc::new(SharedState::new());
        shared.generation.store(1, Ordering::Relaxed);
        shared.source_rate.store(options.rate, Ordering::Relaxed);
        shared
            .playing
            .store(options.workload != Workload::Idle, Ordering::Relaxed);
        let capacity = ((f64::from(options.rate) * BUFFER_SECONDS) as usize).max(1024);
        let (producer, consumer) = RingBuffer::new(capacity);
        let (mut control, insert) = insert_channel(Arc::clone(&shared));
        if options.workload == Workload::Wet {
            control
                .set(Some(Box::new(FakeInsert)))
                .expect("initial insert capacity");
        }
        let native = if options.workload == Workload::NativeWet {
            let block = options.blocks.iter().copied().max().expect("nonempty blocks");
            let crate::callback_benchmark_native::PreparedNativeFixture { fixture, insert } =
                NativeFixture::prepare(options.rate, block).expect("native callback fixture setup");
            control.set(Some(insert)).expect("initial native insert capacity");
            Some(fixture)
        } else {
            None
        };
        let renderer = Arc::new(RendererAccess::new(OutputRenderer::new(
            Arc::clone(&shared),
            consumer,
            DeviceFormat {
                channels: 2,
                rate: options.rate,
            },
            insert,
        )));
        renderer.advance_epoch();
        Self {
            shared,
            renderer,
            producer,
            control,
            capacity,
            next_position: 0,
            stale_frames: 0,
            native,
        }
    }

    /// Captures atomics only after completion, while the callback worker awaits its next batch.
    fn snapshot(&self) -> StateSnapshot {
        StateSnapshot {
            position: self.shared.position.load(Ordering::Relaxed),
            generation: self.shared.generation.load(Ordering::Acquire),
            playing: self.shared.playing.load(Ordering::Relaxed),
            insert_changes: self.shared.insert_changes_applied.load(Ordering::Acquire),
            insert_settled: self.control.is_settled(),
            insert_failed: self.shared.insert_failed.load(Ordering::Acquire),
            ended: self.shared.ended.load(Ordering::Acquire),
            stream_lost: self.shared.stream_lost.load(Ordering::Acquire),
            underruns: self.shared.underruns.load(Ordering::Relaxed),
            callback_frames: self.shared.callback_frames.load(Ordering::Relaxed),
        }
    }

    /// Disposes returned processor handles on the required owning thread, never in callback timing.
    fn collect_retired(&mut self) {
        let inserts = self.control.take_retired();
        if let Some(native) = &self.native {
            native.dispose_inserts(inserts).expect("native insert retirement");
        } else {
            drop(inserts);
        }
    }

    /// Settles insert removal and destroys native processor ownership before final instance teardown.
    fn finish_native(&mut self) {
        if self.native.is_some() {
            self.control.set(None).expect("final native detach capacity");
            self.renderer
                .renderer
                .with_control(OutputRenderer::apply_insert_changes);
            self.collect_retired();
            let native = self.native.take().expect("native fixture attached");
            let validation = native.validate();
            let teardown = native.finish();
            validation.expect("native callback output validated");
            teardown.expect("native callback fixture teardown");
        }
    }

    /// Fills only the upcoming batch, guaranteeing adequate decoded input before admission.
    fn prepare_frames(&mut self, frames: usize, options: &Options) {
        if options.workload == Workload::Idle || options.mode == Mode::IdleCpu {
            return;
        }
        let generation = self.shared.generation.load(Ordering::Acquire);
        for _ in 0..frames {
            self.producer
                .push(source_frame(generation, self.next_position))
                .expect("bounded prepared queue");
            self.next_position += 1;
        }
    }

    /// Applies bounded transport and insert transitions, before queue refill and callback timing.
    fn transition(&mut self, number: usize) -> &'static str {
        match number % 6 {
            0 => {
                self.control
                    .set(Some(Box::new(FakeInsert)))
                    .expect("drained insert queue");
                "attach"
            }
            1 => {
                self.shared.playing.store(false, Ordering::Relaxed);
                "pause"
            }
            2 => {
                self.shared.playing.store(true, Ordering::Relaxed);
                "resume"
            }
            3 => {
                self.stale_frames = self.capacity - self.producer.slots();
                self.shared.advance_generation();
                self.shared.position.store(self.next_position, Ordering::Relaxed);
                "seek_generation"
            }
            4 => {
                self.control.set(None).expect("drained insert queue");
                "detach"
            }
            _ => {
                // Simulate a stopped stream's control-side renderer access and subsequent reopening.
                self.renderer
                    .renderer
                    .with_control(OutputRenderer::apply_insert_changes);
                self.shared.playing.store(true, Ordering::Relaxed);
                "stream_reentry"
            }
        }
    }
}

impl Drop for Harness {
    fn drop(&mut self) {
        // Also run synchronous native disposal when callback validation unwinds before report serialization.
        self.finish_native();
        drop(self.renderer.renderer.retire());
    }
}

/// Reads one explicitly named setting, preserving a simple stable command schema.
fn setting(name: &str, default: &str) -> String {
    env::var(format!("AUDIOBATCH_CALLBACK_{name}")).unwrap_or_else(|_| default.to_owned())
}

/// Parses a bounded integer before creating buffers or threads.
fn integer_setting(name: &str, default: &str, minimum: usize, maximum: usize) -> usize {
    let value = setting(name, default).parse().expect("integer benchmark parameter");
    assert!((minimum..=maximum).contains(&value), "benchmark parameter out of range");
    value
}

/// Generates strictly nonzero binary32 input, with a stable integer-defined fixture identity.
fn source_frame(generation: u32, position: u64) -> Frame {
    let step = (position % 251) as f32 / 4096.0;
    Frame {
        generation,
        position,
        left: 0.25 + step,
        right: -0.125 - step,
    }
}

/// Checks each converted channel exactly, since this binary fixture, matrix, and fade use exact power-of-two steps.
fn verify_expected<T>(actual: &[T], expected: &ExpectedCallback, sample: &mut CallbackSample)
where
    T: Sample + FromSample<f32>,
    f32: FromSample<T>,
{
    assert_eq!(
        actual.len(),
        expected.audio.len() * 2,
        "complete callback reference coverage"
    );
    sample.expected_active_frames = expected.active_frames;
    sample.expected_resets = expected.resets;
    sample.checksum = HASH_BASIS;
    for (&[left, right], &(expected_left, expected_right)) in actual.as_chunks::<2>().0.iter().zip(&expected.audio) {
        let left = f32::from_sample(left);
        let right = f32::from_sample(right);
        let expected_left = f32::from_sample(T::from_sample(expected_left));
        let expected_right = f32::from_sample(T::from_sample(expected_right));
        assert!(left.is_finite() && right.is_finite(), "nonfinite callback audio");
        assert_eq!(
            left.to_bits(),
            expected_left.to_bits(),
            "left callback reference mismatch"
        );
        assert_eq!(
            right.to_bits(),
            expected_right.to_bits(),
            "right callback reference mismatch"
        );
        sample.non_silent_frames += usize::from(left.abs() + right.abs() > 0.0);
        sample.checksum = (sample.checksum ^ u64::from(left.to_bits())).wrapping_mul(HASH_MULTIPLIER);
        sample.checksum = (sample.checksum ^ u64::from(right.to_bits())).wrapping_mul(HASH_MULTIPLIER);
    }
    assert_eq!(
        sample.non_silent_frames, expected.active_frames,
        "active-window reference count"
    );
}

/// Replays every prepared batch after workers join, rejecting fast silence and unintended state transitions.
fn validate_run<T>(options: &Options, output: &[T], samples: &mut [CallbackSample], batches: &mut [BatchSample])
where
    T: Sample + FromSample<f32>,
    f32: FromSample<T>,
{
    let mut oracle = Oracle::new(options);
    let mut offset = 0;
    for batch in batches {
        let stale = if batch.transition.is_some() {
            oracle.transition(batch.start / options.transition_interval)
        } else {
            0
        };
        let available = usize::try_from(oracle.prepared_position - oracle.source_position).expect("bounded queue");
        let requested: usize = (batch.start..batch.end).map(|index| options.frames(index)).sum();
        let expected_prepared = if options.mode == Mode::IdleCpu || options.workload == Workload::Idle {
            0
        } else {
            requested.saturating_sub(available)
        };
        assert_eq!(batch.prepared, expected_prepared, "independent prepared-source count");
        oracle.prepared_position += batch.prepared as u64;
        for sample in samples.iter_mut().take(batch.end).skip(batch.start) {
            let expected = oracle.callback(sample.frames, batch.excluded, options);
            batch.expected_active_frames += expected.active_frames;
            batch.expected_resets += expected.resets;
            let length = sample.frames * 2;
            verify_expected(
                output.get(offset..offset + length).expect("retained callback"),
                &expected,
                sample,
            );
            offset += length;
        }
        assert_eq!(
            batch.consumed, batch.expected_active_frames,
            "batch active source consumption"
        );
        assert_eq!(batch.stale_discarded, stale, "seek stale-prefix consumption");
        let callback_frames = if options.mode == Mode::IdleCpu || batch.excluded {
            batches_last_callback_frames(options, batch.start)
        } else {
            u32::try_from(options.frames(batch.end - 1)).expect("bounded callback size")
        };
        oracle.validate_snapshot(&batch.snapshot, callback_frames);
    }
    assert_eq!(offset, output.len(), "all callback output verified");
}

/// Finds the last admitted callback's frame count for idle intervals or intentionally excluded callbacks.
fn batches_last_callback_frames(options: &Options, start: usize) -> u32 {
    if options.mode == Mode::IdleCpu || start == 0 {
        0
    } else {
        u32::try_from(options.frames(start - 1)).expect("bounded callback size")
    }
}

/// Converts bounded elapsed times to the durable nanosecond unit.
fn nanoseconds(duration: Duration) -> u64 {
    u64::try_from(duration.as_nanos()).expect("benchmark duration fits u64")
}

/// Determines a batch that never straddles a phase, a control change, or the prepared queue capacity.
fn batch_end(options: &Options, start: usize, capacity: usize) -> usize {
    let mut end = start;
    let mut frames = 0;
    while end < options.total_callbacks() && end - start < MAX_BATCH_CALLBACKS {
        if end > start
            && (options.phase(end) != options.phase(start)
                || options.workload == Workload::Contention
                || (options.workload == Workload::Transitions && end.is_multiple_of(options.transition_interval)))
        {
            break;
        }
        let next = options.frames(end);
        if frames + next > capacity {
            break;
        }
        frames += next;
        end += 1;
    }
    assert!(end > start, "queue must fit one callback");
    end
}

/// Measures callback pairs of clock reads outside the native callback to retain instrumentation cost.
fn clock_overhead() -> Vec<u64> {
    (0..128)
        .map(|_| {
            let start = Instant::now();
            nanoseconds(start.elapsed())
        })
        .collect()
}

/// Sleeps to a cumulative callback-start deadline without replacing actual wakeup lateness with zero.
fn pace(start: Instant, preceding_frames: usize, rate: u32) -> Instant {
    let deadline = start + Duration::from_secs_f64(preceding_frames as f64 / f64::from(rate));
    if let Some(wait) = deadline.checked_duration_since(Instant::now()) {
        thread::sleep(wait);
    }
    deadline
}

/// Computes nonnegative actual start lateness without inventing on-time wakeups after sleeping.
fn lateness(deadline: Instant, actual_start: Instant) -> u64 {
    nanoseconds(actual_start.saturating_duration_since(deadline))
}

/// Executes all batches on one persistent thread, retaining output until after its CPU sample.
fn callback_worker<T>(
    options: &Options,
    renderer: &RendererAccess,
    commands: &Receiver<Batch>,
    completions: &Sender<Completion>,
    output: &mut [T],
    samples: &mut [CallbackSample],
) where
    T: SizedSample + FromSample<f32> + Send,
    f32: FromSample<T>,
{
    let cpu = CpuClock::new();
    let mut offset = 0;
    for batch in commands {
        let before = cpu.read();
        let started = Instant::now();
        let mut paced_frames = 0;
        for (index, sample) in samples.iter_mut().enumerate().take(batch.end).skip(batch.start) {
            let length = options.frames(index) * 2;
            let target = output.get_mut(offset..offset + length).expect("preallocated output");
            sample.frames = length / 2;
            sample.excluded = batch.excluded;
            if options.mode == Mode::Latency {
                let start = Instant::now();
                render_typed_callback(black_box(renderer), 1, black_box(target));
                sample.duration_ns = nanoseconds(start.elapsed());
            } else {
                let deadline = pace(started, paced_frames, options.rate);
                if options.mode == Mode::Paced {
                    let actual_start = Instant::now();
                    render_typed_callback(black_box(renderer), 1, black_box(target));
                    sample.start_lateness_ns = lateness(deadline, actual_start);
                } else {
                    sample.start_lateness_ns = lateness(deadline, Instant::now());
                }
            }
            paced_frames += sample.frames;
            offset += length;
        }
        if options.mode != Mode::Latency {
            // Retain the final audio period in CPU accounting without mislabeling its end as a callback start.
            pace(started, paced_frames, options.rate);
        }
        let finished = Instant::now();
        let after = cpu.read();
        completions
            .send(Completion {
                finished,
                thread_ns: after.thread_ns.saturating_sub(before.thread_ns),
                process_ns: after.process_ns.saturating_sub(before.process_ns),
                wall_ns: nanoseconds(finished.duration_since(started)),
            })
            .expect("controller remains alive");
    }
}

/// Runs the production benchmark with no injected faults or control-side test modifications.
fn run<T>(options: &Options) -> Value
where
    T: SizedSample + FromSample<f32> + Send,
    f32: FromSample<T>,
{
    run_with_hook::<T>(options, |_, _| {})
}

/// Runs the control side, optionally injecting test faults outside timing to exercise the benchmark validator.
#[expect(
    clippy::too_many_lines,
    reason = "scoped ownership keeps controller and persistent callback lifetimes explicit"
)]
fn run_with_hook<T>(options: &Options, mut before_batch: impl FnMut(&mut Harness, usize)) -> Value
where
    T: SizedSample + FromSample<f32> + Send,
    f32: FromSample<T>,
{
    let mut harness = Harness::new(options);
    // Nonzero initialization commits writable output pages before the callback can fault them in.
    let mut output = vec![T::from_sample(0.125); options.total_samples()];
    if options.mode == Mode::IdleCpu {
        output.fill(T::from_sample(0.0));
    }
    let mut samples: Vec<CallbackSample> = (0..options.total_callbacks())
        .map(|_| CallbackSample::default())
        .collect();
    for sample in &mut samples {
        sample.checksum = black_box(HASH_BASIS);
    }
    let mut batches = Vec::with_capacity(options.total_callbacks());
    let overhead = clock_overhead();
    let (commands, receiver) = bounded(1);
    let (sender, completions) = bounded(1);
    let callback_renderer = Arc::clone(&harness.renderer);
    thread::scope(|scope| {
        scope.spawn(|| {
            callback_worker(
                options,
                &callback_renderer,
                &receiver,
                &sender,
                &mut output,
                &mut samples,
            );
        });
        let mut start = 0;
        while start < options.total_callbacks() {
            harness.collect_retired();
            // Every previous batch can have left unconsumed input while paused or excluded.
            // Fill only available room and account consumption through the producer's exact queue occupancy.
            // A seek can leave one paused batch stale, reserve room for an equally large fresh prefix.
            let batch_capacity = if options.workload == Workload::Transitions {
                harness.capacity / 2
            } else {
                harness.capacity
            };
            let end = batch_end(options, start, batch_capacity);
            let transition_start = Instant::now();
            let transition = (options.workload == Workload::Transitions
                && start.is_multiple_of(options.transition_interval))
            .then(|| harness.transition(start / options.transition_interval));
            before_batch(&mut harness, start);
            let frames: usize = (start..end).map(|index| options.frames(index)).sum();
            let occupied = harness.capacity - harness.producer.slots();
            let current = occupied.saturating_sub(harness.stale_frames);
            let needed = frames.saturating_sub(current);
            assert!(
                needed <= harness.producer.slots(),
                "prepared queue must fit fresh generation and stale prefix"
            );
            harness.prepare_frames(needed, options);
            let occupied_before = harness.capacity - harness.producer.slots();
            let excluded =
                options.workload == Workload::Contention && start > 0 && start.is_multiple_of(CONTENTION_INTERVAL);
            let dispatch = || {
                commands
                    .send(Batch { start, end, excluded })
                    .expect("callback thread remains alive");
                completions
                    .recv_timeout(Duration::from_secs(10))
                    .expect("callback completes bounded batch")
            };
            let completion = if excluded {
                harness
                    .renderer
                    .renderer
                    .with_control(|_| dispatch())
                    .expect("live benchmark renderer")
            } else {
                dispatch()
            };
            let consumed = occupied_before - (harness.capacity - harness.producer.slots());
            let stale_discarded = harness.stale_frames.min(consumed);
            harness.stale_frames -= stale_discarded;
            let snapshot = harness.snapshot();
            batches.push(BatchSample {
                start,
                end,
                consumed: consumed - stale_discarded,
                stale_discarded,
                prepared: if options.workload == Workload::Idle || options.mode == Mode::IdleCpu {
                    0
                } else {
                    needed
                },
                snapshot,
                expected_active_frames: 0,
                expected_resets: 0,
                excluded,
                transition,
                transition_ns: transition.map(|_| nanoseconds(completion.finished.duration_since(transition_start))),
                completion,
            });
            start = end;
        }
        drop(commands);
    });
    // Replay only after joining, so no oracle work leaks into this or the following thread-CPU intervals.
    validate_run(options, &output, &mut samples, &mut batches);
    let underruns = harness.shared.underruns.load(Ordering::Relaxed);
    assert_eq!(underruns, 0, "prepared benchmark input must not underrun");
    let mut report = report(options, &samples, &batches, &overhead, harness.capacity, underruns);
    if let Some(native) = &harness.native {
        let object = report.as_object_mut().expect("benchmark report object");
        object.insert("native_bundle".to_owned(), json!(native.bundle()));
        object.insert("native_binary_checksum".to_owned(), json!(native.binary_checksum()));
        object.insert(
            "native_initialization_policy".to_owned(),
            json!("single UI-thread preparation and activation at zero default latency, queued first gain/width/latency automation, no flush/process/reactivation prewarm"),
        );
        object.insert(
            "native_cold_scope".to_owned(),
            json!("fresh callback thread first typed callback, first callback-thread native reset/setProcessing and first native process with queued automation delivery, excludes module loading/UI activation/instance setup/buffer allocation"),
        );
        object.insert(
            "native_reference".to_owned(),
            json!("mid=(L+R)*0.5, side=(L-R)*0.5*width1, (mid+/-side)*gain0.5, exact source*0.5 for integer_step_251_v1, followed by production reset fade and conversion"),
        );
        object.insert(
            "native_parameters".to_owned(),
            json!({ "gain": 0.5, "width": 1.0, "latency": 0 }),
        );
    }
    harness.finish_native();
    drop(harness.renderer.renderer.retire());
    report
}

/// Summarizes a sorted distribution without discarding the retained raw samples.
fn distribution(values: &mut [u64]) -> Value {
    values.sort_unstable();
    if values.is_empty() {
        return Value::Null;
    }
    let percentile = |fraction: usize| values.get(((values.len() - 1) * fraction).div_ceil(100)).copied();
    json!({ "p50": percentile(50), "p95": percentile(95), "p99": percentile(99), "max": values.last() })
}

/// Sums only steady-state CPU batches so cold calls and warmup cannot dilute paced measurements.
fn steady_cpu(options: &Options, batches: &[BatchSample]) -> CpuTimes {
    batches
        .iter()
        .filter(|batch| options.phase(batch.start) == "steady")
        .fold(
            CpuTimes {
                thread_ns: 0,
                process_ns: 0,
            },
            |sum, batch| CpuTimes {
                thread_ns: sum.thread_ns + batch.completion.thread_ns,
                process_ns: sum.process_ns + batch.completion.process_ns,
            },
        )
}

/// Serializes the stable report after the callback thread and all callback timing have finished.
#[expect(
    clippy::too_many_lines,
    reason = "raw samples and explicit benchmark provenance share one report schema"
)]
fn report(
    options: &Options,
    samples: &[CallbackSample],
    batches: &[BatchSample],
    overhead: &[u64],
    capacity: usize,
    underruns: u64,
) -> Value {
    let mut durations: Vec<u64> = samples
        .iter()
        .enumerate()
        .filter(|(index, _)| options.phase(*index) == "steady" && options.mode == Mode::Latency)
        .map(|(_, sample)| sample.duration_ns)
        .collect();
    let summary = distribution(&mut durations);
    let steady_frames: usize = samples
        .iter()
        .enumerate()
        .filter(|(index, _)| options.phase(*index) == "steady")
        .map(|(_, sample)| sample.frames)
        .sum();
    let duration_sum: u64 = durations.iter().sum();
    let total_cpu = steady_cpu(options, batches);
    let wall_ns: u64 = batches
        .iter()
        .filter(|batch| options.phase(batch.start) == "steady")
        .map(|batch| batch.completion.wall_ns)
        .sum();
    let raw: Vec<Value> = samples
        .iter()
        .enumerate()
        .map(|(index, sample)| {
            let deadline_ns = sample.frames as f64 / f64::from(options.rate) * 1e9;
            json!({ "index": index, "phase": options.phase(index), "frames_requested": sample.frames,
            "duration_ns": (options.mode == Mode::Latency).then_some(sample.duration_ns),
            "callback_invoked": options.mode != Mode::IdleCpu, "intentional_exclusion": sample.excluded,
            "deadline_ratio": (options.mode == Mode::Latency).then_some(sample.duration_ns as f64 / deadline_ns),
            "callback_start_lateness_ns": (options.mode == Mode::Paced).then_some(sample.start_lateness_ns),
            "idle_interval_start_lateness_ns": (options.mode == Mode::IdleCpu).then_some(sample.start_lateness_ns),
            "expected_active_frames": sample.expected_active_frames, "expected_reset_count": sample.expected_resets,
            "non_silent_frames": sample.non_silent_frames,
            "checksum": format!("{:016x}", sample.checksum) })
        })
        .collect();
    let raw_batches: Vec<Value> = batches
        .iter()
        .map(|batch| {
            json!({
                "start": batch.start, "end": batch.end, "phase": options.phase(batch.start),
                "source_frames_consumed": batch.consumed, "stale_frames_discarded": batch.stale_discarded,
                "intentional_exclusion": batch.excluded,
                "transition": batch.transition, "transition_ack_ns": batch.transition_ns,
                "source_frames_prepared": batch.prepared, "expected_active_frames": batch.expected_active_frames,
                "expected_reset_count": batch.expected_resets, "source_position": batch.snapshot.position,
                "generation": batch.snapshot.generation, "playing": batch.snapshot.playing,
                "insert_changes_applied": batch.snapshot.insert_changes, "insert_settled": batch.snapshot.insert_settled,
                "insert_failed": batch.snapshot.insert_failed, "ended": batch.snapshot.ended,
                "stream_lost": batch.snapshot.stream_lost,
                "callback_thread_cpu_ns": batch.completion.thread_ns, "process_cpu_ns": batch.completion.process_ns,
                "wall_ns": batch.completion.wall_ns,
            })
        })
        .collect();
    let mut report = json!({
        "schema_version": 1, "implementation": "rust", "boundary": "render_typed_callback",
        "label": options.label, "workload": if options.workload == Workload::NativeWet { "native-wet".to_owned() } else { format!("{:?}", options.workload).to_lowercase() },
        "mode": match options.mode { Mode::Latency => "latency", Mode::Paced => "paced", Mode::IdleCpu => "idle_cpu" }, "sample_format": options.format,
        "sample_rate": options.rate, "channels": 2, "blocks": options.blocks,
        "fixture": "integer_step_251_v1", "queue_capacity_frames": capacity,
        "fake_insert_block_frames": INSERT_BLOCK_FRAMES, "transition_frames": TRANSITION_FRAMES,
        "transition_interval": options.transition_interval, "contention_interval": CONTENTION_INTERVAL,
    });
    let measurements = json!({
        "callback_count": if options.mode == Mode::IdleCpu { 0 } else { options.total_callbacks() },
        "measured_callbacks": if options.mode == Mode::IdleCpu || options.cold_only { 0 } else { options.callbacks },
        "warmup_callbacks": if options.mode == Mode::IdleCpu || options.cold_only { 0 } else { options.warmup },
        "cold_only": options.cold_only,
        "cold_callbacks": usize::from(options.mode != Mode::IdleCpu),
        "idle_intervals": if options.mode == Mode::IdleCpu { options.total_callbacks() } else { 0 },
        "frames_requested": if options.mode == Mode::IdleCpu { 0 } else { samples.iter().map(|sample| sample.frames).sum::<usize>() },
        "steady_frames_requested": if options.mode == Mode::IdleCpu { 0 } else { steady_frames },
        "valid_frames_emitted": samples.iter().map(|sample| sample.non_silent_frames).sum::<usize>(),
        "silent_frames": if options.mode == Mode::IdleCpu { 0 } else { samples.iter().map(|sample| sample.frames - sample.non_silent_frames).sum::<usize>() },
        "excluded_callbacks": samples.iter().filter(|sample| sample.excluded).count(),
        "source_frames_consumed": batches.iter().map(|batch| batch.consumed).sum::<usize>(),
        "stale_frames_discarded": batches.iter().map(|batch| batch.stale_discarded).sum::<usize>(),
        "non_silent_frames_emitted": samples.iter().map(|sample| sample.non_silent_frames).sum::<usize>(),
        "expected_active_frames": samples.iter().map(|sample| sample.expected_active_frames).sum::<usize>(),
        "expected_steady_active_frames": samples.iter().enumerate().filter(|(index, _)| options.phase(*index) == "steady").map(|(_, sample)| sample.expected_active_frames).sum::<usize>(),
        "expected_reset_count": samples.iter().map(|sample| sample.expected_resets).sum::<usize>(),
        "source_position": batches.last().map(|batch| batch.snapshot.position),
        "insert_failed": batches.iter().any(|batch| batch.snapshot.insert_failed),
        "underruns": underruns, "duration_ns": summary,
    });
    let timing = json!({
        "seconds_per_audio_second": (options.mode == Mode::Latency).then_some(duration_sum as f64 / 1e9 / (steady_frames as f64 / f64::from(options.rate))),
        "deadline_misses": samples.iter().filter(|sample| sample.duration_ns as f64 > sample.frames as f64 / f64::from(options.rate) * 1e9).count(),
        "cpu": { "callback_thread_ns": total_cpu.thread_ns, "process_ns": total_cpu.process_ns,
            "wall_ns": wall_ns, "callback_thread_fraction": total_cpu.thread_ns as f64 / wall_ns as f64,
            "process_fraction": total_cpu.process_ns as f64 / wall_ns as f64,
            "scope": "batch callback loop and pacing, excludes refill, handshakes, verification" },
        "clock_pair_overhead_ns": overhead, "callbacks": raw, "batches": raw_batches,
        "transition_latency_scope": "control publication through callback completion including refill and dispatch",
        "stream_transition_scope": "hardware-free renderer reentry, not native stream construction",
        "valid_frames_scope": "strictly nonzero stereo fixture, counted after output conversion",
        "validation": "exact converted per-frame reference, active-window source counts, pause/resume and reset fades, quiescent transport/insert acknowledgements, failure flags",
        "reset_validation_scope": "independent expected reset count and exact fade output, no callback-side reset probe",
        "pacing_scope": "actual callback-start lateness after wakeup, cumulative frame deadlines within each batch, schedule restarts after untimed handshake",
        "benchmark_contract": "transition_oracle_and_callback_start_pacing_v2",
        "prior_timing_compatibility": "earlier transition validation, inter-batch verification, native prewarming and completion-boundary pacing results are not equivalent",
        "valid": true,
        "reference_comparison": "exact converted binary32 bits for this power-of-two fixture, zero tolerance",
        "cpu_resolution_ns": if cfg!(windows) { 100 } else { 1000 },
        "architecture": std::env::consts::ARCH, "os": std::env::consts::OS,
    });
    let object = report.as_object_mut().expect("report object");
    object.extend(measurements.as_object().expect("measurement object").clone());
    object.extend(timing.as_object().expect("timing object").clone());
    report
}

#[test]
#[ignore = "controlled Release measurement, run only on an idle machine"]
fn production_callback() {
    assert!(
        !black_box(cfg!(debug_assertions)),
        "callback measurements require --release"
    );
    assert!(
        !black_box(cfg!(audiobatch_rtsan)),
        "callback measurements must not use sanitizer instrumentation"
    );
    let options = Options::from_environment();
    let report = match options.format.as_str() {
        "f32" => run::<f32>(&options),
        "f64" => run::<f64>(&options),
        "i16" => run::<i16>(&options),
        "i24" => run::<cpal::I24>(&options),
        "i32" => run::<i32>(&options),
        _ => run::<u16>(&options),
    };
    println!("CALLBACK_BENCHMARK_JSON={report}");
}

#[cfg(test)]
mod test_benchmark_validation {
    use std::panic::{AssertUnwindSafe, catch_unwind};

    use super::*;

    /// Small deterministic transition run, long enough to include repeated pause/resume, seek, and detach windows.
    fn options() -> Options {
        Options {
            workload: Workload::Transitions,
            mode: Mode::Latency,
            format: "f32".to_owned(),
            rate: 48_000,
            blocks: vec![32, 63, 127],
            callbacks: 64,
            warmup: 0,
            transition_interval: 4,
            cold_only: false,
            label: "validator-unit-test".to_owned(),
        }
    }

    /// Deliberately fails native-independent processing so finite silence cannot be accepted after a good cold call.
    struct FailedInsert;

    impl PreviewInsert for FailedInsert {
        fn max_block_frames(&self) -> usize {
            INSERT_BLOCK_FRAMES
        }
        fn latency_frames(&self) -> usize {
            0
        }
        fn tail_frames(&self) -> usize {
            0
        }
        fn reset(&mut self, _position_frames: u64) {}
        fn process(&mut self, _left: &mut [f32], _right: &mut [f32]) -> bool {
            false
        }
    }

    #[test]
    fn transition_reference_covers_all_converted_formats() {
        let options = options();
        for report in [
            run::<f32>(&options),
            run::<f64>(&options),
            run::<i16>(&options),
            run::<cpal::I24>(&options),
            run::<i32>(&options),
            run::<u16>(&options),
        ] {
            assert_eq!(report["valid"], true);
            assert!(report["expected_steady_active_frames"].as_u64().expect("active count") > 0);
            assert!(report["expected_reset_count"].as_u64().expect("reset count") > 1);
            assert_eq!(report["source_frames_consumed"], report["expected_active_frames"]);
            assert!(report["silent_frames"].as_u64().expect("pause silence") > 0);
        }
    }

    #[test]
    fn rejects_unintentionally_stopped_after_good_cold_callback() {
        assert!(
            catch_unwind(AssertUnwindSafe(|| run_with_hook::<f32>(
                &options(),
                |harness, start| {
                    if start == 1 {
                        harness.shared.playing.store(false, Ordering::Relaxed);
                    }
                }
            )))
            .is_err()
        );
    }

    #[test]
    fn rejects_failed_insert_after_good_cold_callback() {
        assert!(
            catch_unwind(AssertUnwindSafe(|| run_with_hook::<f32>(
                &options(),
                |harness, start| {
                    if start == 1 {
                        harness.control.set(Some(Box::new(FailedInsert))).expect("fault insert");
                    }
                }
            )))
            .is_err()
        );
    }

    #[test]
    fn rejects_silent_insert_after_good_cold_callback() {
        assert!(
            catch_unwind(AssertUnwindSafe(|| run_with_hook::<f32>(
                &options(),
                |harness, start| {
                    if start == 1 {
                        harness
                            .shared
                            .insert_gain_bits
                            .store(0.0_f32.to_bits(), Ordering::Relaxed);
                    }
                }
            )))
            .is_err()
        );
    }

    #[test]
    fn rejects_failure_flag_even_with_correct_audio() {
        assert!(
            catch_unwind(AssertUnwindSafe(|| run_with_hook::<f32>(
                &options(),
                |harness, start| {
                    if start == 1 {
                        harness.shared.insert_failed.store(true, Ordering::Release);
                    }
                }
            )))
            .is_err()
        );
    }

    #[test]
    fn rejects_missing_insert_acknowledgement_after_good_cold_callback() {
        assert!(
            catch_unwind(AssertUnwindSafe(|| run_with_hook::<f32>(
                &options(),
                |harness, start| {
                    if start == 1 {
                        harness.shared.insert_changes_applied.store(0, Ordering::Release);
                    }
                }
            )))
            .is_err()
        );
    }

    #[test]
    fn rejects_replayed_cold_fade_after_good_cold_callback() {
        assert!(
            catch_unwind(AssertUnwindSafe(|| run_with_hook::<f32>(
                &options(),
                |harness, start| {
                    if start == 1 {
                        harness
                            .control
                            .set(Some(Box::new(FakeInsert)))
                            .expect("unintended reset");
                    }
                }
            )))
            .is_err()
        );
    }

    #[test]
    fn short_pause_and_resume_preserve_partial_fades_without_reset() {
        let mut options = options();
        options.blocks = vec![1, 7, 13];
        options.transition_interval = 1;
        let report = run::<f32>(&options);
        assert_eq!(report["valid"], true);
        assert_eq!(report["callbacks"][2]["expected_reset_count"], 0);
        assert_eq!(report["callbacks"][3]["expected_reset_count"], 1);
    }

    #[test]
    fn active_idle_contention_and_cpu_modes_use_reference() {
        for workload in [Workload::Idle, Workload::Dry, Workload::Wet, Workload::Contention] {
            let mut options = options();
            options.workload = workload;
            assert_eq!(run::<f32>(&options)["valid"], true);
        }
        for workload in [Workload::Idle, Workload::Wet, Workload::Transitions] {
            let mut options = options();
            options.mode = Mode::IdleCpu;
            options.workload = workload;
            options.blocks = vec![1];
            options.callbacks = 24;
            assert_eq!(run::<f32>(&options)["valid"], true);
        }
    }

    #[test]
    fn paced_reports_actual_start_lateness_without_callback_duration() {
        let mut options = options();
        options.mode = Mode::Paced;
        options.callbacks = 2;
        let report = run::<f32>(&options);
        assert!(report["callbacks"][1]["callback_start_lateness_ns"].is_u64());
        assert!(report["callbacks"][1]["duration_ns"].is_null());
        assert!(report["callbacks"][1].get("pacing_lateness_ns").is_none());
    }

    #[test]
    fn actual_start_lateness_includes_oversleep() {
        let deadline = Instant::now();
        assert_eq!(lateness(deadline, deadline + Duration::from_nanos(1234)), 1234);
        assert_eq!(
            lateness(
                deadline,
                deadline
                    .checked_sub(Duration::from_nanos(1234))
                    .expect("earlier instant")
            ),
            0
        );
    }
}
