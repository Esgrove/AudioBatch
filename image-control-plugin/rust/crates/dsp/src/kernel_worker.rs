//! Background FIR design with coherent atomic requests and bounded ready/retirement ownership transfer.
//!
//! The audio thread moves boxes through SPSC queues without dropping their contents.
//! Only explicit offline mode, stopped destruction, and the worker may allocate or destroy kernels.

use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use rtrb::{Consumer, Producer, RingBuffer};

use crate::convolver::KernelSpectra;
use crate::crossover::{MultibandError, design_kernels, same_crossovers};

/// Bounded publications and retirements, independent of host block size and automation rate.
const QUEUE_CAPACITY: usize = 2;
/// Worker polling keeps callback publication free of condition-variable notifications.
const POLL_INTERVAL: Duration = Duration::from_millis(20);

/// Shared atomic request, with one processing-thread writer and one design-thread reader.
struct Request {
    values: [AtomicU32; 3],
    sequence: AtomicU32,
    mode_generation: AtomicU32,
    offline: AtomicBool,
    stopping: AtomicBool,
    failed: AtomicBool,
    design_barrier: Mutex<()>,
}

/// Processing-thread queue endpoints and a designer joined only during stopped destruction.
pub struct KernelWorker {
    request: Arc<Request>,
    ready: Consumer<Box<KernelSpectra>>,
    retired: Producer<Box<KernelSpectra>>,
    deferred_retirement: Option<Box<KernelSpectra>>,
    thread: Option<JoinHandle<()>>,
    sample_rate: f32,
    half_length: usize,
}

impl Request {
    /// Reads a coherent request in one bounded attempt, retrying only on a later worker poll.
    fn read(&self) -> Option<[f32; 3]> {
        let before = self.sequence.load(Ordering::SeqCst);
        if before & 1 != 0 {
            return None;
        }
        let values = self
            .values
            .each_ref()
            .map(|value| f32::from_bits(value.load(Ordering::SeqCst)));
        (self.sequence.load(Ordering::SeqCst) == before).then_some(values)
    }
}

impl KernelWorker {
    /// Starts the designer after all initial kernels exist, outside callback admission.
    pub fn new(crossovers: [f32; 3], sample_rate: f32, half_length: usize) -> Result<Self, MultibandError> {
        let request = Arc::new(Request {
            values: crossovers.map(|value| AtomicU32::new(value.to_bits())),
            sequence: AtomicU32::new(0),
            mode_generation: AtomicU32::new(0),
            offline: AtomicBool::new(false),
            stopping: AtomicBool::new(false),
            failed: AtomicBool::new(false),
            design_barrier: Mutex::new(()),
        });
        let (ready_producer, ready) = RingBuffer::new(QUEUE_CAPACITY);
        let (retired, retired_consumer) = RingBuffer::new(QUEUE_CAPACITY);
        let worker_request = Arc::clone(&request);
        let thread = thread::Builder::new()
            .name("ImageControl kernel design".into())
            .spawn(move || {
                run_worker(
                    &worker_request,
                    ready_producer,
                    retired_consumer,
                    crossovers,
                    sample_rate,
                    half_length,
                );
            })?;
        Ok(Self {
            request,
            ready,
            retired,
            deferred_retirement: None,
            thread: Some(thread),
            sample_rate,
            half_length,
        })
    }

    /// Publishes one complete request using atomics only, with no callback-side retry or wakeup.
    pub fn request(&self, crossovers: [f32; 3]) {
        self.request.sequence.fetch_add(1, Ordering::SeqCst);
        for (destination, value) in self.request.values.iter().zip(crossovers) {
            destination.store(value.to_bits(), Ordering::SeqCst);
        }
        self.request.sequence.fetch_add(1, Ordering::SeqCst);
    }

    /// Enables explicit synchronous rendering, joining in-flight publication before discarding queued work.
    /// Entering offline mode may lock and destroy kernels and must never run in a real-time callback.
    pub fn set_offline(&mut self, offline: bool) {
        self.request.offline.store(offline, Ordering::Release);
        self.request.mode_generation.fetch_add(1, Ordering::Release);
        if offline {
            let guard = self
                .request
                .design_barrier
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            while let Ok(kernel) = self.ready.pop() {
                drop(kernel);
            }
            drop(self.deferred_retirement.take());
            drop(guard);
        }
    }

    /// Returns one ready publication only when the old active allocation has guaranteed retirement capacity.
    pub fn take_ready(&mut self) -> Option<Box<KernelSpectra>> {
        if let Some(kernel) = self.deferred_retirement.take()
            && let Err(rtrb::PushError::Full(kernel)) = self.retired.push(kernel)
        {
            self.deferred_retirement = Some(kernel);
            return None;
        }
        if self.retired.slots() == 0 {
            return None;
        }
        self.ready.pop().ok()
    }

    /// Transfers the old allocation to its sole destruction thread, retaining it if backpressure intervenes.
    pub fn retire(&mut self, kernel: Box<KernelSpectra>) {
        if let Err(rtrb::PushError::Full(kernel)) = self.retired.push(kernel) {
            // take_ready guarantees there is no outstanding deferred allocation when accepting a replacement.
            self.deferred_retirement = Some(kernel);
        }
    }

    /// Designs deterministically on the calling offline thread, independently of background scheduling.
    pub fn design_offline(&self, crossovers: [f32; 3]) -> Result<Box<KernelSpectra>, MultibandError> {
        design(crossovers, self.sample_rate, self.half_length)
    }

    /// Reports an internal background transform failure without formatting or logging in processing.
    pub fn failed(&self) -> bool {
        self.request.failed.load(Ordering::Relaxed)
    }
}

impl Drop for KernelWorker {
    fn drop(&mut self) {
        self.request.stopping.store(true, Ordering::Release);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// Designs complete coherent ordinary/quadrature spectra off the real-time thread.
pub fn design(
    crossovers: [f32; 3],
    sample_rate: f32,
    half_length: usize,
) -> Result<Box<KernelSpectra>, MultibandError> {
    let kernels = design_kernels(crossovers, sample_rate, half_length)?;
    Ok(Box::new(KernelSpectra::new(&kernels, crossovers)?))
}

/// Polls requests and destroys retired kernels exclusively on the worker thread.
fn run_worker(
    request: &Request,
    mut ready: Producer<Box<KernelSpectra>>,
    mut retired: Consumer<Box<KernelSpectra>>,
    initial_crossovers: [f32; 3],
    sample_rate: f32,
    half_length: usize,
) {
    let mut designed = initial_crossovers;
    let mut mode_generation = 0;
    let mut pending = None;
    while !request.stopping.load(Ordering::Acquire) {
        while let Ok(kernel) = retired.pop() {
            drop(kernel);
        }
        let guard = request
            .design_barrier
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let generation = request.mode_generation.load(Ordering::Acquire);
        if generation != mode_generation {
            mode_generation = generation;
            designed = [0.0; 3];
            pending = None;
        }
        if request.offline.load(Ordering::Acquire) {
            pending = None;
            // A later real-time transition must publish the latest request even if it was designed offline.
            designed = [0.0; 3];
        } else {
            if let Some(crossovers) = request.read()
                && !same_crossovers(crossovers, designed)
            {
                match design(crossovers, sample_rate, half_length) {
                    Ok(kernel) => {
                        pending = Some(kernel);
                        designed = crossovers;
                    }
                    Err(_) => request.failed.store(true, Ordering::Relaxed),
                }
            }
            if let Some(kernel) = pending.take()
                && let Err(rtrb::PushError::Full(kernel)) = ready.push(kernel)
            {
                pending = Some(kernel);
            }
        }
        drop(guard);
        thread::sleep(POLL_INTERVAL);
    }
}
