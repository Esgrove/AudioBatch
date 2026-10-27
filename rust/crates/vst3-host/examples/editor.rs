//! Opens the editor of a plugin in a window, for trying plugin editors by hand.
//!
//! Run with `cargo run -p vst3-host --features editor --example editor -- <bundle> [name]`.
//! It opens the first audio effect in the bundle, or the first one whose name contains `name`,
//! and prints the size of the captured state after the window is closed.

#![cfg_attr(target_os = "macos", expect(unsafe_code, reason = "runs the AppKit event loop"))]

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context as _, Result};
#[cfg(target_os = "macos")]
use objc2::MainThreadMarker;
#[cfg(target_os = "macos")]
use objc2_app_kit::{NSApplication, NSApplicationActivationPolicy, NSEventMask};
#[cfg(target_os = "macos")]
use objc2_foundation::{NSDate, NSDefaultRunLoopMode};
use vst3_host::{EditorWindow, Module, PluginInstance, ProcessMode, UiThread, ui_channel};

/// Category of the classes that are audio effects or instruments.
const AUDIO_MODULE_CATEGORY: &str = "Audio Module Class";

/// How long one pass of the event loop waits for an event.
const EVENT_WAIT: Duration = Duration::from_millis(50);

fn main() -> Result<()> {
    let ui = UiThread::register_current()?;
    let mut arguments = std::env::args_os().skip(1);
    let bundle = PathBuf::from(arguments.next().context("Give the path of a VST3 bundle")?);
    let name = arguments.next().map(|name| name.to_string_lossy().into_owned());
    start_application()?;

    let module = Module::load(&ui, &bundle)?;
    let class = module
        .classes()
        .iter()
        .filter(|class| class.category == AUDIO_MODULE_CATEGORY)
        .find(|class| name.as_ref().is_none_or(|name| class.name.contains(name.as_str())))
        .context("The bundle has no matching audio effect")?;
    println!("Opening the editor of {} by {}", class.name, class.vendor);

    let (dispatcher, _jobs) = ui_channel();
    let mut instance = PluginInstance::create(&ui, &module, class.id, Some(dispatcher))?;
    instance.configure_main_buses(&ui, 2)?;
    instance.setup(&ui, ProcessMode::Realtime, 48_000.0, 1024)?;
    instance.activate(&ui)?;

    let editor = EditorWindow::open(&ui, &instance, &class.name, None)?;
    while !editor.is_closed() {
        handle_events(EVENT_WAIT);
    }
    drop(editor);
    instance.flush_parameter_edits(&ui);
    let state = instance.state(&ui)?;
    println!(
        "Captured {} bytes of component state and {} bytes of controller state",
        state.component.len(),
        state.controller.len()
    );
    Ok(())
}

/// Starts the shared application as a regular app in front, so its windows show and get events.
#[cfg(target_os = "macos")]
fn start_application() -> Result<()> {
    let marker = MainThreadMarker::new().context("The example must run on the main thread")?;
    let application = NSApplication::sharedApplication(marker);
    application.setActivationPolicy(NSApplicationActivationPolicy::Regular);
    application.finishLaunching();
    application.activate();
    Ok(())
}

/// Does nothing, because Win32 windows need no application object.
#[cfg(not(target_os = "macos"))]
#[expect(clippy::unnecessary_wraps, reason = "matches the macOS version, which can fail")]
const fn start_application() -> Result<()> {
    Ok(())
}

/// Handles the events that arrive within `timeout`.
#[cfg(target_os = "macos")]
fn handle_events(timeout: Duration) {
    let Some(marker) = MainThreadMarker::new() else {
        return;
    };
    let application = NSApplication::sharedApplication(marker);
    let mut until = NSDate::dateWithTimeIntervalSinceNow(timeout.as_secs_f64());
    // SAFETY: the default run loop mode is a constant that Foundation initializes before any code runs,
    // and events are taken and sent on the main thread.
    while let Some(event) = unsafe {
        application.nextEventMatchingMask_untilDate_inMode_dequeue(
            NSEventMask::Any,
            Some(&until),
            NSDefaultRunLoopMode,
            true,
        )
    } {
        application.sendEvent(&event);
        until = NSDate::distantPast();
    }
}

/// Handles the window messages that arrive within `timeout`.
#[cfg(not(target_os = "macos"))]
fn handle_events(timeout: Duration) {
    if let Some(ui) = UiThread::current() {
        vst3_host::pump_platform_events(&ui, timeout);
    }
}
