//! Bounded, hardware-free side-by-side native editors for manual C++/Rust layout and spectrum inspection.
//!
//! Loads explicit build-tree bundles only. No screenshots or pixels determine test success.

#![cfg_attr(
    target_os = "macos",
    expect(unsafe_code, reason = "dispatches AppKit events in this private native host")
)]

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, ensure};
#[cfg(target_os = "macos")]
use objc2::MainThreadMarker;
#[cfg(target_os = "macos")]
use objc2_app_kit::{NSApplication, NSApplicationActivationPolicy, NSEventMask};
#[cfg(target_os = "macos")]
use objc2_foundation::{NSDate, NSDefaultRunLoopMode};
use vst3_host::{EditorWindow, Module, PluginInstance, ProcessMode, UiThread};

/// Both plugins receive the same deterministic stereo comb without accessing any audio device.
const SAMPLE_RATE: f64 = 48_000.0;
/// Work and event loops terminate even if no user closes a window.
const DEFAULT_SECONDS: u64 = 15;
/// Host callback size is unrelated to the observation FFT geometry.
const BLOCK_FRAMES: usize = 512;

/// Opens the reference and candidate with identical geometry and a running stereo comb.
fn main() -> Result<()> {
    let mut arguments = std::env::args_os().skip(1);
    let first = PathBuf::from(arguments.next().context("first build-tree VST3 bundle required")?);
    let second = PathBuf::from(arguments.next().context("second build-tree VST3 bundle required")?);
    let seconds = arguments
        .next()
        .map_or(Ok(DEFAULT_SECONDS), |value| value.to_string_lossy().parse::<u64>())?;
    ensure!((1..=120).contains(&seconds), "duration must be 1 through 120 seconds");
    let ui = UiThread::register_current()?;
    start_application()?;
    let mut instances = Vec::new();
    let mut processors = Vec::new();
    for bundle in [&first, &second] {
        let module = Module::load(&ui, bundle)?;
        let class = module
            .classes()
            .iter()
            .find(|class| class.is_audio_effect())
            .context("audio effect")?;
        let mut instance = PluginInstance::create(&ui, &module, class.id, None)?;
        instance.configure_main_buses(&ui, 2)?;
        instance.setup(&ui, ProcessMode::Realtime, SAMPLE_RATE, BLOCK_FRAMES)?;
        instance.activate(&ui)?;
        processors.push(instance.processor(&ui)?);
        instances.push(instance);
    }
    let stopped = Arc::new(AtomicBool::new(false));
    let worker_stop = Arc::clone(&stopped);
    let worker = std::thread::spawn(move || -> Result<_> {
        let mut position = 0_usize;
        while !worker_stop.load(Ordering::Acquire) {
            for processor in &mut processors {
                for channel in 0..2 {
                    let plane = processor.input_channel_mut(channel).context("stereo input")?;
                    for (offset, sample) in plane.iter_mut().enumerate() {
                        let time = (position + offset) as f64 / SAMPLE_RATE;
                        let mut value = 0.0;
                        for (tone, frequency) in [40.0, 80.0, 150.0, 300.0, 600.0, 1200.0, 2400.0, 4800.0, 9600.0]
                            .into_iter()
                            .enumerate()
                        {
                            let right_gain = [0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 1.0, 0.5]
                                .get(tone)
                                .copied()
                                .context("tone level")?;
                            let gain = if channel == 0 { 1.0 } else { right_gain };
                            value = ((std::f64::consts::TAU * frequency * time).sin() * gain).mul_add(0.015, value);
                        }
                        *sample = value as f32;
                    }
                }
                processor.process(BLOCK_FRAMES)?;
            }
            position = position.wrapping_add(BLOCK_FRAMES);
            std::thread::park_timeout(Duration::from_secs_f64(BLOCK_FRAMES as f64 / SAMPLE_RATE));
        }
        Ok(processors)
    });
    // Keep callback retirement unconditional if either editor refuses attachment.
    let editors_result = (|| -> Result<()> {
        let mut editors = Vec::new();
        for (index, instance) in instances.iter().enumerate() {
            editors.push(EditorWindow::open(
                &ui,
                instance,
                if index == 0 {
                    "ImageControl C++ reference"
                } else {
                    "ImageControl Rust candidate"
                },
                None,
                None,
            )?);
        }
        #[cfg(target_os = "macos")]
        print_native_window_ids();
        let deadline = Instant::now() + Duration::from_secs(seconds);
        while Instant::now() < deadline && editors.iter().any(|editor| !editor.is_closed()) {
            handle_events(&ui);
        }
        Ok(())
    })();
    stopped.store(true, Ordering::Release);
    let processors = worker
        .join()
        .map_err(|_| anyhow::anyhow!("comparison callback panicked"))??;
    drop(processors);
    for instance in &mut instances {
        instance.deactivate(&ui)?;
    }
    editors_result
}

/// Starts a native host without modifying another process's application delegate.
#[cfg(target_os = "macos")]
fn start_application() -> Result<()> {
    let marker = MainThreadMarker::new().context("comparison requires main thread")?;
    let application = NSApplication::sharedApplication(marker);
    application.setActivationPolicy(NSApplicationActivationPolicy::Regular);
    application.finishLaunching();
    application.activate();
    Ok(())
}

/// Win32 initializes its own windowing without an application object.
#[cfg(target_os = "windows")]
#[expect(
    clippy::unnecessary_wraps,
    reason = "matches fallible macOS application initialization"
)]
const fn start_application() -> Result<()> {
    Ok(())
}

/// Reports only this host's own window identities for optional external manual capture.
#[cfg(target_os = "macos")]
fn print_native_window_ids() {
    let Some(marker) = MainThreadMarker::new() else { return };
    for window in NSApplication::sharedApplication(marker).windows() {
        println!("window={} title={}", window.windowNumber(), window.title());
    }
}

/// Dispatches the host's real native events, without desktop input automation.
#[cfg(target_os = "macos")]
fn handle_events(_ui: &UiThread) {
    let Some(marker) = MainThreadMarker::new() else { return };
    let application = NSApplication::sharedApplication(marker);
    let until = NSDate::dateWithTimeIntervalSinceNow(0.01);
    // SAFETY: only this executable's main thread dispatches live AppKit events in the default run loop.
    if let Some(event) = unsafe {
        application.nextEventMatchingMask_untilDate_inMode_dequeue(
            NSEventMask::Any,
            Some(&until),
            NSDefaultRunLoopMode,
            true,
        )
    } {
        application.sendEvent(&event);
    }
    application.updateWindows();
}

/// Pumps the current Win32 host event queue while all editor lifetimes stay on its owner thread.
#[cfg(target_os = "windows")]
fn handle_events(ui: &UiThread) {
    vst3_host::pump_platform_events(ui, Duration::from_millis(10));
}
