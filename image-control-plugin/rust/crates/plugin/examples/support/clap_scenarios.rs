//! Named fresh-thread CLAP scenarios with native-only nonblocking boundaries and bounded process completion.

use std::hint::black_box;
use std::io::{self, Read};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, bail, ensure};
use clack_extensions::params::PluginParams;
use clack_extensions::render::RenderMode;
use clack_extensions::state::PluginState;
use clack_host::plugin::PluginInstanceError;
use clack_host::utils::ClapId;

use crate::host;

/// Each state-stream progress fence must finish well inside the host's 20-second process deadline.
const PROGRESS_TIMEOUT: Duration = Duration::from_secs(2);
/// Independent append-only plain-unit parameter count, not derived from the loaded plugin.
const PARAMETER_COUNT: usize = 31;
/// Alternating detached states prove successive recalls change the same running instance.
const RESTORE_COUNT: usize = 4;

/// Detached state and the complete plain-unit values accepted before any audio callback.
struct Recall {
    /// Native serialized payload, saved on the unmarked lifecycle thread.
    bytes: Vec<u8>,
    /// Exact accepted readback for all stable parameter ids, including snapped and skewed controls.
    plain: [f64; PARAMETER_COUNT],
}

/// Lifecycle-thread reader that fences callback progress inside the actual native state-load call stack.
struct ProgressReader<'a> {
    /// Unconsumed detached state bytes, never accessed by the callback worker.
    bytes: &'a [u8],
    /// Successful callbacks completed by this same plugin instance.
    callbacks: &'a AtomicUsize,
    /// Worker termination or lifecycle-thread retirement request.
    done: &'a AtomicBool,
    /// Successful in-stream progress observations, checked after native load returns.
    overlap_reads: usize,
}

impl Read for ProgressReader<'_> {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        if output.is_empty() || self.bytes.is_empty() {
            return Ok(0);
        }
        let before = self.callbacks.load(Ordering::Acquire);
        // Two completions exclude a callback already in flight when the state reader was entered.
        wait_for_callbacks(self.callbacks, self.done, before.saturating_add(2), PROGRESS_TIMEOUT)?;
        self.overlap_reads += 1;
        self.bytes.read(output)
    }
}

/// Requires observable completed callbacks without extending the timeout when progress is slow.
fn wait_for_callbacks(callbacks: &AtomicUsize, done: &AtomicBool, minimum: usize, timeout: Duration) -> io::Result<()> {
    let deadline = Instant::now() + timeout;
    while callbacks.load(Ordering::Acquire) < minimum {
        if done.load(Ordering::Acquire) {
            return Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "callback worker stopped during state load",
            ));
        }
        if Instant::now() >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "callbacks stalled during state load",
            ));
        }
        thread::yield_now();
    }
    Ok(())
}

/// Captures all accepted plain-unit controls through the same instance's native parameter extension.
fn plain_values(instance: &mut host::Instance) -> Result<[f64; PARAMETER_COUNT]> {
    let handle = instance.plugin_handle();
    let params = handle.get_extension::<PluginParams>().context("CLAP params")?;
    ensure!(
        usize::try_from(params.count(&handle))? == PARAMETER_COUNT,
        "exact 31-parameter state contract"
    );
    let mut values = [0.0; PARAMETER_COUNT];
    for (id, value) in values.iter_mut().enumerate() {
        *value = params
            .get_value(&handle, ClapId::new(u32::try_from(id)?))
            .context("plain state readback")?;
        ensure!(value.is_finite(), "non-finite plain readback for {id}");
    }
    Ok(values)
}

/// Seeds every control while inactive, then records its accepted representation and real native state.
fn capture_recall(instance: &mut host::Instance, sequence: u32) -> Result<Recall> {
    let mut edits = Vec::with_capacity(PARAMETER_COUNT);
    let offset = f64::from(sequence);
    for id in 0..u32::try_from(PARAMETER_COUNT)? {
        let value = match id {
            0 => offset.mul_add(6.0, -12.0),
            1 => offset.mul_add(60.0, 120.0),
            2 => offset.mul_add(600.0, 1200.0),
            3 => offset.mul_add(1200.0, 6000.0),
            4..=27 => match (id - 4) % 6 {
                0 => offset.mul_add(3.0, -12.0),
                1 => offset.mul_add(3.0, -9.0),
                2 => offset.mul_add(0.25, 0.5),
                3 => offset.mul_add(50.0, 50.0),
                4 => offset.mul_add(50.0, -25.0),
                _ => f64::from(sequence.min(1)),
            },
            _ => f64::from(sequence.min(1)),
        };
        edits.push((id, value));
    }
    host::flush(instance, &edits)?;
    let plain = plain_values(instance)?;
    let handle = instance.plugin_handle();
    let state = handle.get_extension::<PluginState>().context("CLAP state")?;
    let mut bytes = Vec::new();
    state.save(&handle, &mut bytes)?;
    ensure!(!bytes.is_empty(), "seeded state save made no progress");
    Ok(Recall { bytes, plain })
}

/// Checks the complete accepted plain representation without a conversion or tolerance masking a lost recall.
fn verify_recall(instance: &mut host::Instance, recall: &Recall) -> Result<()> {
    for (id, (actual, expected)) in plain_values(instance)?.iter().zip(&recall.plain).enumerate() {
        ensure!(
            actual.to_bits() == expected.to_bits(),
            "restored plain parameter {id} differs"
        );
    }
    let handle = instance.plugin_handle();
    let state = handle.get_extension::<PluginState>().context("CLAP state")?;
    let mut bytes = Vec::new();
    state.save(&handle, &mut bytes)?;
    ensure!(bytes == recall.bytes, "accepted restored state did not round trip");
    Ok(())
}

/// Leaves every live control different from the first recall, with two independently captured alternating targets.
fn prepare_recalls(instance: &mut host::Instance) -> Result<[Recall; 2]> {
    let first = capture_recall(instance, 0)?;
    let second = capture_recall(instance, 1)?;
    let mutated = capture_recall(instance, 2)?;
    for plain in [second.plain, mutated.plain] {
        ensure!(
            first
                .plain
                .iter()
                .zip(plain)
                .all(|(expected, actual)| expected.to_bits() != actual.to_bits()),
            "initial mutation and alternating recall must differ from every first-snapshot parameter"
        );
    }
    Ok([first, second])
}

/// Recalls live state on the lifecycle thread, proving in-load callback progress and exact accepted readback.
fn exercise_recalls(
    instance: &mut host::Instance,
    recalls: &[Recall],
    callbacks: &AtomicUsize,
    done: &AtomicBool,
) -> Result<()> {
    let state = instance
        .plugin_handle()
        .get_extension::<PluginState>()
        .context("CLAP state")?;
    wait_for_callbacks(callbacks, done, 1, PROGRESS_TIMEOUT)?;
    for recall in recalls.iter().cycle().take(RESTORE_COUNT) {
        let mut reader = ProgressReader {
            bytes: &recall.bytes,
            callbacks,
            done,
            overlap_reads: 0,
        };
        state.load(&instance.plugin_handle(), &mut reader)?;
        ensure!(
            reader.overlap_reads > 0,
            "no callback completed inside native CLAP state load"
        );
        let restored_at = callbacks.load(Ordering::Acquire);
        wait_for_callbacks(callbacks, done, restored_at.saturating_add(2), PROGRESS_TIMEOUT)?;
        verify_recall(instance, recall)?;
        host::service_callbacks(instance);
    }
    Ok(())
}

/// One explicitly named detector or native callback case.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Case {
    CleanControl,
    AllocationControl,
    MutexControl,
    ColdAutomation,
    Reset,
    StateInteraction,
}

impl Case {
    /// Selects exactly one supported workload, rejecting unknown names before module loading.
    fn parse(value: &str) -> Result<Self> {
        match value {
            "clean-control" => Ok(Self::CleanControl),
            "allocation-control" => Ok(Self::AllocationControl),
            "mutex-control" => Ok(Self::MutexControl),
            "clap-cold-automation" => Ok(Self::ColdAutomation),
            "clap-reset" => Ok(Self::Reset),
            "clap-state-interaction" => Ok(Self::StateInteraction),
            _ => bail!("unknown CLAP case: {value}"),
        }
    }

    /// Stable runner completion marker, not emitted until teardown has completed.
    const fn name(self) -> &'static str {
        match self {
            Self::CleanControl => "clean-control",
            Self::AllocationControl => "allocation-control",
            Self::MutexControl => "mutex-control",
            Self::ColdAutomation => "clap-cold-automation",
            Self::Reset => "clap-reset",
            Self::StateInteraction => "clap-state-interaction",
        }
    }
}

/// Reads help without touching artifacts, then runs one process-isolated case.
pub fn run() -> Result<()> {
    let arguments: Vec<_> = std::env::args().skip(1).collect();
    if arguments
        .iter()
        .any(|argument| argument == "-h" || argument == "--help")
    {
        println!(
            "Usage: clap_host --case <name> [-b|--bundle <absolute .clap path>]\n\nCases: clean-control, allocation-control, mutex-control, clap-cold-automation, clap-reset, clap-state-interaction\n\nNative cases require --bundle or IMAGE_CONTROL_CLAP_BUNDLE. No installed-plugin fallback.\nRTSan: compile host and package with the same realtime-instrumented configuration."
        );
        return Ok(());
    }
    let mut arguments = arguments.into_iter();
    let mut case = None;
    let mut bundle = None;
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--case" => case = Some(Case::parse(&arguments.next().context("--case requires a name")?)?),
            "-b" | "--bundle" => bundle = Some(PathBuf::from(arguments.next().context("--bundle requires a path")?)),
            _ => bail!("unknown argument: {argument}"),
        }
    }
    let case = case.context("--case is required")?;
    let _deadline = host::Deadline::start()?;
    match case {
        Case::CleanControl | Case::AllocationControl | Case::MutexControl => control(case)?,
        _ => {
            let bundle = bundle
                .or_else(|| std::env::var_os("IMAGE_CONTROL_CLAP_BUNDLE").map(PathBuf::from))
                .context("native cases require --bundle or IMAGE_CONTROL_CLAP_BUNDLE")?;
            native(case, &host::checked_bundle(&bundle)?)?;
        }
    }
    println!("CASE_OK {}", case.name());
    Ok(())
}

/// Executes detector controls on a new thread, with allocation and mutex storage prepared before marking.
fn control(case: Case) -> Result<()> {
    let mutex = Mutex::new(0);
    // Construct the platform mutex storage before the detector region, isolating the lock finding from lazy allocation.
    drop(mutex.lock().map_err(|_| anyhow::anyhow!("control mutex poisoned"))?);
    thread::spawn(move || -> Result<()> {
        match case {
            Case::CleanControl => ensure!(clean_region(41) == 42, "clean control result"),
            Case::AllocationControl => {
                let allocation = rust_allocation_region();
                ensure!(black_box(allocation.len()) == 4096, "observable allocation control");
            }
            Case::MutexControl => {
                mutex_region(&mutex);
                ensure!(
                    *mutex.lock().map_err(|_| anyhow::anyhow!("control mutex poisoned"))? == 1,
                    "mutex control result"
                );
            }
            _ => bail!("not a detector control"),
        }
        Ok(())
    })
    .join()
    .map_err(|_| anyhow::anyhow!("CLAP detector thread panicked"))?
}

/// Prepares a real dynamically loaded CLAP, then exercises its first callback on a fresh, unwarmed thread.
fn native(case: Case, bundle: &std::path::Path) -> Result<()> {
    let mut instance = host::load(bundle)?;
    host::configure(&mut instance, 2)?;
    host::set_render(&mut instance, RenderMode::Realtime)?;
    let recalls = if case == Case::StateInteraction {
        prepare_recalls(&mut instance)?.into_iter().collect()
    } else {
        Vec::new()
    };
    let stopped = host::activate(&mut instance)?;
    let buffers = host::Buffers::new(2);
    let progress = Arc::new(AtomicUsize::new(0));
    let done = Arc::new(AtomicBool::new(false));
    let worker_progress = Arc::clone(&progress);
    let worker_done = Arc::clone(&done);
    let worker = thread::spawn(move || {
        let mut processor = stopped
            .start_processing()
            .map_err(PluginInstanceError::from)
            .expect("CLAP start processing");
        let mut buffers = buffers;
        let result = (|| -> Result<()> {
            let mut block = 0;
            // State tests keep callbacks running until the lifecycle thread has completed its bounded recalls.
            while block < 96 || (case == Case::StateInteraction && !worker_done.load(Ordering::Acquire)) {
                ensure!(block < 100_000, "state workload exceeded callback bound");
                for (channel, samples) in buffers.input.iter_mut().enumerate() {
                    for (frame, sample) in samples.iter_mut().enumerate() {
                        let value = ((block * host::BLOCK_FRAMES + frame + channel * 19) % 257) as f32;
                        *sample = (value - 128.0) * (1.0 / 512.0);
                    }
                }
                let events = [
                    host::event(0, -6.0),
                    host::event(1, 180.0),
                    host::event(2, 1800.0),
                    host::event(3, 7200.0),
                    host::event(4, -3.0),
                    host::event(5, -9.0),
                    host::event(6, 0.4),
                    host::event(7, 200.0),
                    host::event(8, 25.0),
                    host::event(28, 0.0),
                ];
                if case == Case::Reset && block % 8 == 0 {
                    host::reset_region(&mut processor);
                }
                // State callbacks must remain authoritative, with no automation overwriting restored values.
                let events = if case == Case::StateInteraction {
                    &[][..]
                } else {
                    &events[..]
                };
                buffers.process(&mut processor, events)?;
                ensure!(
                    buffers.output.iter().flatten().all(|sample| sample.is_finite()),
                    "non-finite CLAP output"
                );
                worker_progress.fetch_add(1, Ordering::Release);
                block += 1;
                if case == Case::StateInteraction {
                    thread::yield_now();
                }
            }
            Ok(())
        })();
        worker_done.store(true, Ordering::Release);
        (processor.stop_processing(), result)
    });
    let state_result = if case == Case::StateInteraction {
        exercise_recalls(&mut instance, &recalls, &progress, &done)
    } else {
        Ok(())
    };
    done.store(true, Ordering::Release);
    let (stopped, result) = worker
        .join()
        .map_err(|_| anyhow::anyhow!("CLAP processing thread panicked"))?;
    instance.deactivate(stopped);
    state_result?;
    result?;
    ensure!(
        progress.load(Ordering::Acquire) >= 96,
        "insufficient completed callbacks"
    );
    if case == Case::StateInteraction {
        let last = recalls.iter().cycle().nth(RESTORE_COUNT - 1).context("final recall")?;
        verify_recall(&mut instance, last)?;
    } else {
        let handle = instance.plugin_handle();
        let params = handle.get_extension::<PluginParams>().context("CLAP params")?;
        ensure!(
            params.get_value(&handle, ClapId::new(7)) == Some(200.0),
            "cold automation was not consumed"
        );
    }
    Ok(())
}

/// Proves the marker admits a clean observable arithmetic region.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn clean_region(value: usize) -> usize {
    black_box(usize::wrapping_add as fn(usize, usize) -> usize)(value, 1)
}

/// Positive allocation detector control with storage escaping the marked region.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn rust_allocation_region() -> Box<[u8; 4096]> {
    black_box(Box::new([black_box(7); 4096]))
}

/// Positive mutex detector control with the native mutex already initialized outside the marker.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn mutex_region(mutex: &Mutex<usize>) {
    if let Ok(mut guard) = mutex.lock() {
        *guard += black_box(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The host stream adapter waits for new completions while preserving partial-read bytes and EOF semantics.
    #[test]
    fn progress_reader_fences_partial_reads() -> Result<()> {
        let callbacks = AtomicUsize::new(0);
        let done = AtomicBool::new(false);
        thread::scope(|scope| -> Result<()> {
            scope.spawn(|| {
                while !done.load(Ordering::Acquire) {
                    callbacks.fetch_add(1, Ordering::Release);
                    thread::yield_now();
                }
            });
            let result = (|| -> Result<()> {
                let mut reader = ProgressReader {
                    bytes: b"state",
                    callbacks: &callbacks,
                    done: &done,
                    overlap_reads: 0,
                };
                ensure!(
                    reader.read(&mut [])? == 0 && reader.overlap_reads == 0,
                    "empty output fenced"
                );
                let mut output = [0; 2];
                ensure!(
                    reader.read(&mut output)? == 2 && output == *b"st",
                    "partial read changed bytes"
                );
                ensure!(
                    reader.read(&mut output)? == 2 && output == *b"at",
                    "second partial read"
                );
                let mut last = [0];
                ensure!(reader.read(&mut last)? == 1 && last == *b"e", "final state byte");
                ensure!(reader.overlap_reads == 3, "missing in-stream progress fences");
                ensure!(
                    reader.read(&mut output)? == 0 && reader.overlap_reads == 3,
                    "EOF fenced"
                );
                Ok(())
            })();
            done.store(true, Ordering::Release);
            result
        })
    }

    /// A stalled worker fails at the local fence rather than relying on the process watchdog.
    #[test]
    fn callback_fence_times_out_without_progress() {
        let error = wait_for_callbacks(&AtomicUsize::new(0), &AtomicBool::new(false), 2, Duration::ZERO)
            .expect_err("stalled callbacks must time out");
        assert_eq!(error.kind(), io::ErrorKind::TimedOut);
    }

    /// A failed or retired worker rejects the load before consuming detached state bytes.
    #[test]
    fn progress_reader_rejects_stopped_worker() {
        let mut reader = ProgressReader {
            bytes: b"state",
            callbacks: &AtomicUsize::new(0),
            done: &AtomicBool::new(true),
            overlap_reads: 0,
        };
        let error = reader
            .read(&mut [0; 5])
            .expect_err("stopped worker must reject state read");
        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
        assert_eq!(reader.bytes, b"state");
        assert_eq!(reader.overlap_reads, 0);
    }
}
