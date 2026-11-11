//! Named fresh-thread CLAP scenarios with native-only nonblocking boundaries and bounded process completion.

use std::hint::black_box;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

use anyhow::{Context as _, Result, bail, ensure};
use clack_extensions::params::PluginParams;
use clack_extensions::render::RenderMode;
use clack_extensions::state::PluginState;
use clack_host::plugin::PluginInstanceError;
use clack_host::utils::ClapId;

use crate::host;

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
    let state = instance
        .plugin_handle()
        .get_extension::<PluginState>()
        .context("CLAP state")?;
    let mut saved = Vec::new();
    state.save(&instance.plugin_handle(), &mut saved)?;
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
                buffers.process(&mut processor, &events)?;
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
        (processor.stop_processing(), result)
    });
    let state_result = (|| -> Result<()> {
        if case == Case::StateInteraction {
            while progress.load(Ordering::Acquire) < 1 {
                ensure!(
                    !worker.is_finished(),
                    "callback thread stopped before state-interaction progress"
                );
                thread::yield_now();
            }
            for _ in 0..32 {
                state.load(&instance.plugin_handle(), &mut saved.as_slice())?;
                let mut snapshot = Vec::new();
                state.save(&instance.plugin_handle(), &mut snapshot)?;
                ensure!(!snapshot.is_empty(), "concurrent state save made no progress");
                host::service_callbacks(&mut instance);
            }
        }
        Ok(())
    })();
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
    let handle = instance.plugin_handle();
    let params = handle.get_extension::<PluginParams>().context("CLAP params")?;
    if case != Case::StateInteraction {
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
