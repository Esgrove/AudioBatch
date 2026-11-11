//! Nonvisual integration workflows using the production view, table, controls, workers, and SQLite cache.
//! All writable paths are temporary, playback is disabled, and converted sources use a deleting disposer.

#![expect(
    clippy::future_not_send,
    reason = "GPUI test contexts remain on their foreground thread"
)]

use std::cell::{Cell, RefCell};
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;
use std::time::Duration;

use audiobatch_core::record::same_custom_gain;
use audiobatch_core::storage::StoragePaths;
use audiobatch_core::{AnalysisCache, AnalysisStatus};
use gpui_kit::component::slider::{SliderEvent, SliderState};
use gpui_kit::test::{TestAppContextExt as _, TestEventExt as _, TestQueryExt as _, TestWindowExt as _};
use gpui_kit::{
    Action, App, AppContext as _, Bounds, Entity, Focusable as _, InputEvent as _, Modifiers, MouseButton,
    OwnedMenuItem, Pixels, Point, ScrollDelta, Subscription, TestAppContext, Window, WindowBounds, WindowOptions,
    point, px, size,
};
use tempfile::TempDir;

use super::cue_start::CueStartIntent;
use super::{AppOptions, AppView, LaunchOptions};
use crate::control_ids;
use crate::menus::{
    AddFiles, CancelRun, ClearList, OpenAudioSettings, ReanalyzeSelected, RemoveSelected, SelectAll,
    SetPreviewViewMode, ToggleColumn, ToggleContinuePlayback, ToggleGainAdjustedMetrics, ToggleLoudnessGraph,
    ToggleOverloadLines, TogglePlayFromFirstCuePoint, TogglePreviewSection, ToggleSampleOverloads, ToggleSeratoColours,
    ToggleTruePeakOverloads,
};
use crate::settings::{GuiSettings, NormalizeMode, PreviewViewMode};
use crate::test_helpers::plugin_dispatcher;
use crate::waveform::WaveformEvent;

/// Source-time tolerance for pointer fractions resolved through binary32 window coordinates.
const SEEK_TOLERANCE_SECONDS: f64 = 0.000_01;

/// Records semantic events emitted by the retained production renderer, without replacing its root subscriber.
struct WaveformEvents {
    events: Rc<RefCell<Vec<WaveformEvent>>>,
    _subscription: Subscription,
}

/// Owns isolated application storage and source copies for a workflow.
struct Workflow {
    directory: Arc<TempDir>,
    paths: StoragePaths,
    window: gpui_kit::AnyWindowHandle,
    view: Entity<AppView>,
}

impl WaveformEvents {
    /// Subscribes beside the production root listener so native gestures must reach the real renderer first.
    fn subscribe(workflow: &Workflow, cx: &mut TestAppContext) -> Self {
        let events = Rc::new(RefCell::new(Vec::new()));
        let recorded = Rc::clone(&events);
        let subscription = cx
            .update_window(workflow.window, |_, window, cx| {
                workflow.view.update(cx, |view, cx| {
                    cx.subscribe_in(&view.waveform, window, move |_, _, event: &WaveformEvent, _, _| {
                        recorded.borrow_mut().push(event.clone());
                    })
                })
            })
            .expect("subscribe to production waveform");
        Self {
            events,
            _subscription: subscription,
        }
    }

    /// Asserts exactly one release event and its source-time position, excluding restart, gain, or zoom events.
    fn assert_seek(&self, seconds: f64) {
        let events = self.events.borrow();
        assert_eq!(events.len(), 1, "one event per release: {events:?}");
        let actual = events
            .iter()
            .find_map(|event| match event {
                WaveformEvent::SeekAndPlayRequested(seconds) => Some(*seconds),
                _ => None,
            })
            .expect("release requests seek and play, not pause or restart");
        assert!(
            (actual - seconds).abs() < SEEK_TOLERANCE_SECONDS,
            "{actual} != {seconds}"
        );
    }
}

impl Workflow {
    /// Opens the real application in a GPUI test-platform window, without physical audio or native editors.
    fn new(cx: &mut TestAppContext) -> Self {
        cx.update(|cx| {
            gpui_kit::init(cx);
            crate::theme::install(cx);
            crate::menus::install(cx);
        });
        let (window, view) = Self::open(None, cx);
        let directory = cx.read(|cx| Arc::clone(&view.read(cx).test_directory));
        let paths = StoragePaths::in_root(directory.path()).expect("view-owned test storage");
        Self {
            directory,
            paths,
            window,
            view,
        }
    }

    /// Reuses the persisted storage while replacing all view-owned entities and subscriptions.
    fn open(services: Option<AppOptions>, cx: &mut TestAppContext) -> (gpui_kit::AnyWindowHandle, Entity<AppView>) {
        let opened = cx.update(|cx| {
            gpui_kit::open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                        Point::default(),
                        size(px(1600.0), px(1000.0)),
                    ))),
                    ..WindowOptions::default()
                },
                cx,
                |window, cx| {
                    cx.new(|cx| match services {
                        Some(services) => {
                            AppView::with_options(&LaunchOptions::default(), plugin_dispatcher(), services, window, cx)
                        }
                        None => AppView::new(&LaunchOptions::default(), plugin_dispatcher(), window, cx),
                    })
                },
            )
            .expect("headless production window")
        });
        cx.update_window(opened.0, |_, window, cx| window.render_frame(cx))
            .expect("render initial frame");
        opened
    }

    /// Copies a shared fixture so a real file operation can never alter committed audio.
    fn copy(&self, fixture: &str, name: &str) -> PathBuf {
        let path = self.directory.path().join(name);
        fs::copy(test_fixtures::fixture(fixture), &path).expect("copy shared fixture");
        path.canonicalize().expect("canonical source")
    }

    /// Dispatches the production Open action and completes its test-platform file picker.
    fn add(&self, paths: Vec<PathBuf>, cx: &mut TestAppContext) {
        self.focus(cx);
        cx.dispatch_action(self.window, AddFiles);
        assert!(cx.did_prompt_for_paths());
        cx.simulate_path_prompt_response(|options| {
            assert!(options.files && options.multiple && !options.directories);
            Some(paths)
        });
        cx.run_until_parked();
    }

    /// Focuses the action owner before dispatching menu commands, including after table interactions.
    fn focus(&self, cx: &mut TestAppContext) {
        cx.update_window(self.window, |_, window, cx| {
            let focus = self.view.read(cx).focus_handle.clone();
            window.focus(&focus, cx);
            window.render_frame(cx);
        })
        .expect("focus root view");
    }

    /// Dispatches a command from the retained root focus path and drains deferred action delivery.
    fn dispatch(&self, action: impl Action, cx: &mut TestAppContext) {
        self.focus(cx);
        cx.update_window(self.window, |_, window, cx| {
            window.dispatch_action(Box::new(action), cx);
        })
        .expect("dispatch production root action");
        cx.run_until_parked();
    }

    /// Renders the actual preview before reading its pane-local gesture geometry.
    fn plots(&self, cx: &mut TestAppContext) -> [Bounds<Pixels>; 2] {
        cx.update_window(self.window, |_, window, cx| {
            window.render_frame(cx);
            self.view.read(cx).waveform.read(cx).preview_plots()
        })
        .expect("resolve production preview plots")
    }

    /// Sends a native wheel event with keyboard modifiers over a real pane,
    /// using layout only to target the existing renderer.
    fn scroll_preview(&self, delta: Point<Pixels>, modifiers: Modifiers, cx: &mut TestAppContext) {
        let plot = self
            .plots(cx)
            .into_iter()
            .find(|plot| plot.size.width > px(0.0))
            .expect("visible source plot");
        cx.update_window(self.window, |_, window, cx| {
            window.dispatch_event(
                gpui_kit::ScrollEvent {
                    position: plot.center(),
                    delta: ScrollDelta::Pixels(delta),
                    modifiers,
                    touch_phase: gpui_kit::TouchPhase::Moved,
                    source: gpui_kit::ScrollSource::default(),
                }
                .to_platform_input(),
                cx,
            );
        })
        .expect("wheel over production preview");
        cx.run_until_parked();
    }

    /// Waits for a selected immutable spectral artifact to reach the real renderer.
    async fn spectrogram(&self, cx: &mut TestAppContext) {
        let view = self.view.clone();
        cx.wait_for(self.window, Duration::from_secs(30), move |_, cx| {
            std::thread::yield_now();
            view.read(cx).waveform.read(cx).has_spectrogram_raster()
        })
        .await;
    }

    /// Opens the real preview context menu over a current source pane, retaining the parent's native identity scope.
    fn open_preview_menu(&self, compact: bool, cx: &mut TestAppContext) {
        let plots = self.plots(cx);
        let plot = plots
            .into_iter()
            .find(|plot| plot.size.width > px(0.0) && plot.size.height > px(0.0))
            .expect("visible source pane");
        cx.update_window(self.window, |_, window, cx| {
            let _scope = window.within(if compact {
                "compact-preview-plot"
            } else {
                "preview-plot"
            });
            window.pointer_down(plot.center(), MouseButton::Right, cx);
        })
        .expect("open production preview menu");
        cx.run_until_parked();
        cx.update_window(self.window, |_, window, cx| {
            window.pointer_up(plot.center(), MouseButton::Right, cx);
        })
        .expect("release context gesture");
        cx.run_until_parked();
    }

    /// Sends separate native key transitions so context-menu dismissal drains before a later frame can refocus it.
    fn menu_key(&self, key: &str, cx: &mut TestAppContext) {
        cx.update_window(self.window, |_, window, cx| window.key_down(key, false, cx))
            .expect("press menu key");
        cx.run_until_parked();
        cx.update_window(self.window, |_, window, cx| window.key_up(key, cx))
            .expect("release menu key");
        cx.run_until_parked();
    }

    /// Waits for real worker drains, rather than merely observing that the progress display has stopped.
    async fn idle(&self, cx: &mut TestAppContext) {
        let view = self.view.clone();
        cx.wait_for(self.window, Duration::from_secs(30), move |_, cx| {
            std::thread::yield_now();
            let view = view.read(cx);
            !view.is_running() && view.analysis_workers == 0
        })
        .await;
    }

    /// Selects a real table row through its stable path identity and native pointer handler.
    fn select(&self, path: &Path, cx: &mut TestAppContext) {
        cx.update_window(self.window, |_, window, cx| {
            window.within(Arc::<Path>::from(path)).click(("cell", 0_usize), cx);
        })
        .expect("click result row");
        cx.run_until_parked();
    }

    /// Waits until the selected file's own metadata and waveform have reached their production owners.
    async fn preview(&self, path: &Path, cx: &mut TestAppContext) {
        let path = path.to_path_buf();
        let view = self.view.clone();
        cx.wait_for(self.window, Duration::from_secs(30), move |_, cx| {
            std::thread::yield_now();
            let view = view.read(cx);
            view.selected_tags
                .as_ref()
                .is_some_and(|(selected, _)| selected == &path)
                && view.waveform.read(cx).waveform_source() == Some(path.as_path())
        })
        .await;
    }
}

/// Sends the exact Change-only state operation used by native accessibility Increment and Decrement.
/// GPUI Kit's test platform exposes accessibility snapshots but no native accessibility-action dispatcher.
/// Calling the retained production slider's `step_thumb` therefore proves its real owner subscription and persistence,
/// not OS accessibility routing, disabled-action suppression, or a keyboard binding.
fn step_slider(workflow: &Workflow, slider: &Entity<SliderState>, delta: f32, cx: &mut TestAppContext) {
    cx.update_window(workflow.window, |_, window, cx| {
        slider.update(cx, |slider, cx| slider.step_thumb(false, delta, window, cx));
    })
    .expect("step the retained production slider");
    cx.run_until_parked();
}

/// Advances only GPUI test time, then delivers ready timers and foreground subscription effects.
fn advance_slider_clock(duration: Duration, cx: &TestAppContext) {
    cx.executor().advance_clock(duration);
    cx.run_until_parked();
}

/// Shares the production cache so checks exercise the ordered SQLite writer, not just table feedback.
fn workflow_cache(workflow: &Workflow, cx: &TestAppContext) -> Arc<AnalysisCache> {
    cx.read(|cx| {
        Arc::clone(
            workflow
                .view
                .read(cx)
                .coordinator
                .as_ref()
                .expect("coordinator")
                .cache(),
        )
    })
}

/// Waits for the real writer with a bounded executor-aware poll, yielding to its operating-system thread.
async fn wait_for_gain(workflow: &Workflow, path: &Path, expected: Option<f32>, cx: &mut TestAppContext) {
    let cache = workflow_cache(workflow, cx);
    cx.wait_for(workflow.window, Duration::from_secs(30), |_, _| {
        std::thread::yield_now();
        cache
            .get_analysis(path)
            .is_some_and(|record| same_custom_gain(record.custom_gain_db, expected))
    })
    .await;
}

/// Enables and chooses a target through the production controls, leaving event delivery to their owner.
fn choose_target(workflow: &Workflow, mode: NormalizeMode, cx: &mut TestAppContext) -> Entity<SliderState> {
    cx.simulate_window_resize(workflow.window, size(px(1920.0), px(1080.0)));
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        if window.find(control_ids::PROCESS_NORMALIZE_FIRST).checked() != Some(true) {
            window.click(control_ids::PROCESS_NORMALIZE_FIRST, cx);
        }
        window.click(
            match mode {
                NormalizeMode::Peak => control_ids::NORMALIZE_PEAK,
                NormalizeMode::Loudness => control_ids::NORMALIZE_LOUDNESS,
            },
            cx,
        );
    })
    .expect("choose the production normalization target");
    cx.run_until_parked();
    cx.read(|cx| workflow.view.read(cx).normalize_target_slider(mode).clone())
}

/// Reads the independently persisted value for one mode without changing modes or triggering quit-time saving.
fn saved_target(workflow: &Workflow, mode: NormalizeMode) -> f64 {
    let settings = GuiSettings::load_from(&workflow.paths.data().join(crate::settings::SETTINGS_FILE_NAME));
    match mode {
        NormalizeMode::Peak => settings.normalize_target_peak_dbfs,
        NormalizeMode::Loudness => settings.normalize_target_lufs,
    }
}

/// Resolves a slider's observed semantic child inside its shared control scope.
/// The target's production wrapper deliberately has no independent observation registration.
fn slider_snapshot(window: &mut Window, id: &'static str) -> gpui_kit::test::ElementSnapshot {
    if let Some(snapshot) = window.try_find(id) {
        return snapshot;
    }
    window.within(id).find_by_label(control_ids::NORMALIZE_TARGET_LABEL)
}

/// Sends each double-click transition separately so queued slider subscriptions drain before the reset handler.
/// A single borrowed window update would artificially defer both presses' Change events until after the reset.
fn double_click_slider(workflow: &Workflow, id: &'static str, cx: &mut TestAppContext) {
    let position = cx
        .update_window(workflow.window, |_, window, cx| {
            window.render_frame(cx);
            let snapshot = slider_snapshot(window, id);
            let native_id = snapshot.path().last().expect("observed slider identity").clone();
            let position = snapshot.bounds().center();
            window.click(native_id, cx);
            position
        })
        .expect("first slider click");
    cx.run_until_parked();
    cx.update_window(workflow.window, |_, window, cx| {
        window.dispatch_event(
            gpui_kit::MouseDownEvent {
                position,
                button: MouseButton::Left,
                click_count: 2,
                modifiers: window.modifiers(),
                first_mouse: false,
            }
            .to_platform_input(),
            cx,
        );
    })
    .expect("second slider press");
    cx.run_until_parked();
    cx.update_window(workflow.window, |_, window, cx| {
        window.dispatch_event(
            gpui_kit::MouseUpEvent {
                position,
                button: MouseButton::Left,
                click_count: 2,
                modifiers: window.modifiers(),
            }
            .to_platform_input(),
            cx,
        );
        window.render_frame(cx);
    })
    .expect("second slider release resets the control");
    cx.run_until_parked();
}

/// Begins a real slider pointer gesture and leaves its release pending for the test's immediate-flush assertion.
fn press_slider(workflow: &Workflow, id: &'static str, fraction: f32, cx: &mut TestAppContext) -> Point<Pixels> {
    let position = cx
        .update_window(workflow.window, |_, window, cx| {
            window.render_frame(cx);
            let bounds = slider_snapshot(window, id).bounds();
            let position = point(bounds.left() + bounds.size.width * fraction, bounds.center().y);
            window.pointer_down(position, MouseButton::Left, cx);
            position
        })
        .expect("press the production slider");
    cx.run_until_parked();
    position
}

/// Releases an existing gesture through the real window, without advancing the debounce clock.
fn release_slider(workflow: &Workflow, position: Point<Pixels>, cx: &mut TestAppContext) {
    cx.update_window(workflow.window, |_, window, cx| {
        window.pointer_up(position, MouseButton::Left, cx);
    })
    .expect("release the production slider");
    cx.run_until_parked();
}

/// Returns a source-pane point by fraction, without asserting any painted pixels.
fn plot_point(plot: Bounds<Pixels>, fraction: f32) -> Point<Pixels> {
    point(plot.origin.x + plot.size.width * fraction, plot.center().y)
}

/// Reads a canonical command's checkmark from the menus installed by the real root preference handler.
fn menu_checked(cx: &App, expected: &dyn Action) -> bool {
    /// Searches nested native menu records, not a separate presentation model.
    fn checked_in(items: &[OwnedMenuItem], expected: &dyn Action) -> Option<bool> {
        items.iter().find_map(|item| match item {
            OwnedMenuItem::Action { action, checked, .. } if action.partial_eq(expected) => Some(*checked),
            OwnedMenuItem::Submenu(menu) => checked_in(&menu.items, expected),
            _ => None,
        })
    }
    cx.get_menus()
        .expect("production menus installed")
        .iter()
        .find_map(|menu| checked_in(&menu.items, expected))
        .expect("production command in installed menus")
}

/// Checks all context-menu choices against the canonical root setting using native checked properties.
fn assert_preview_menu(window: &mut Window, expected: PreviewViewMode) {
    assert_eq!(window.find("popup-menu").focused(), Some(true));
    let menu = window.within("popup-menu");
    for (index, mode) in PreviewViewMode::ALL.into_iter().enumerate() {
        let item = menu.find(index);
        assert_eq!(item.label(), Some(mode.label()));
        assert_eq!(item.checked(), Some(mode == expected));
    }
}

/// Confirms the two resolved panes match the requested arrangement, without inspecting the scene or raster.
fn assert_preview_arrangement(plots: [Bounds<Pixels>; 2], mode: PreviewViewMode) {
    let [waveform, spectrogram] = plots;
    assert_eq!(waveform.size.width > px(0.0), mode != PreviewViewMode::Spectrogram);
    assert_eq!(spectrogram.size.width > px(0.0), mode != PreviewViewMode::Waveform);
    match mode {
        PreviewViewMode::SideBySide => assert!(spectrogram.left() > waveform.right()),
        PreviewViewMode::Stacked => assert!(spectrogram.top() > waveform.bottom()),
        PreviewViewMode::Waveform | PreviewViewMode::Spectrogram => {}
    }
}

/// Maps a shared manifest role name to the GPUI roles that satisfy it, as `tests/gui_controls.json` lists them.
fn manifest_roles(manifest: &serde_json::Value, role: &str) -> Vec<String> {
    manifest["roles"][role]["gpui"]
        .as_array()
        .unwrap_or_else(|| panic!("manifest role {role} lists GPUI roles"))
        .iter()
        .map(|name| name.as_str().expect("GPUI role name").to_owned())
        .collect()
}

/// Opens the production view at the manifest window size, so the wide layout shows every shared control,
/// and requires each shared control to be registered once with its shared role and accessible label.
/// The C++ `ManifestControlsExistWithRoles` case checks the same manifest, so the two applications cannot drift.
#[gpui_kit::test]
fn integration_manifest_controls_exist_with_roles(cx: &mut TestAppContext) {
    let manifest: serde_json::Value = serde_json::from_str(
        &fs::read_to_string(test_fixtures::gui_controls_manifest()).expect("read the shared GUI control manifest"),
    )
    .expect("parse the shared GUI control manifest");
    let width = manifest["window"]["width"].as_f64().expect("manifest window width");
    let height = manifest["window"]["height"].as_f64().expect("manifest window height");
    let workflow = Workflow::new(cx);
    #[expect(
        clippy::cast_possible_truncation,
        reason = "manifest window sizes are small whole pixel counts"
    )]
    cx.simulate_window_resize(workflow.window, size(px(width as f32), px(height as f32)));
    let controls = manifest["controls"].as_array().expect("manifest controls");
    assert!(controls.len() > 1, "the manifest lists the shared controls");
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        for control in controls {
            let id = control["id"].as_str().expect("control id");
            let role = control["role"].as_str().expect("control role");
            let element_id = gpui_kit::SharedString::from(id.to_owned());
            let roles = manifest_roles(&manifest, role);
            let has_role = |snapshot: &gpui_kit::test::ElementSnapshot| {
                snapshot
                    .role()
                    .is_some_and(|native| roles.contains(&format!("{native:?}")))
            };
            let matches = window.find_all(element_id.clone());
            assert!(matches.len() <= 1, "{id} is registered once: {matches:?}");
            // A composite control, such as a slider track or the table frame,
            // carries its semantics on exactly one descendant inside its shared id scope.
            let snapshot = match matches.into_iter().next() {
                Some(snapshot) if has_role(&snapshot) => snapshot,
                _ => {
                    let scope = window.within(element_id);
                    let semantic: Vec<_> = scope
                        .elements()
                        .into_iter()
                        .filter(|element| has_role(element))
                        .collect();
                    assert_eq!(semantic.len(), 1, "{id} has one {role} element: {semantic:?}");
                    semantic.into_iter().next().expect("one semantic element")
                }
            };
            if let Some(label) = control["label"].as_str() {
                assert_eq!(snapshot.label(), Some(label), "{id} label");
            }
        }
    })
    .expect("query the production window");
}

/// Drops an unsupported file, a nested folder,
/// and then an audio file onto the window through the platform file-drop events,
/// so the window's own drop target handles each drop.
/// The C++ `DropFilesAndFolders` case drops the same inputs in the same order.
#[gpui_kit::test]
async fn integration_drop_files_and_folders(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let first = workflow.copy("sine_stereo_44100_16.wav", "first.wav");
    let nested = workflow.directory.path().join("nested/inner");
    fs::create_dir_all(&nested).expect("nested folder");
    fs::copy(
        test_fixtures::fixture("sine_mono_44100_16.wav"),
        nested.join("second.wav"),
    )
    .expect("nested copy");
    let unsupported = workflow.directory.path().join("notes.txt");
    fs::write(&unsupported, b"not audio").expect("unsupported file");
    let drop = |paths: Vec<PathBuf>, cx: &mut TestAppContext| {
        let position = point(px(400.0), px(200.0));
        cx.update_window(workflow.window, |_, window, cx| {
            for event in [
                gpui_kit::FileDropEvent::Entered {
                    position,
                    paths: gpui_kit::ExternalPaths(paths.into_iter().collect()),
                },
                gpui_kit::FileDropEvent::Pending { position },
                gpui_kit::FileDropEvent::Submit { position },
            ] {
                window.dispatch_event(event.to_platform_input(), cx);
            }
        })
        .expect("drop onto the production window");
        cx.run_until_parked();
    };

    drop(vec![unsupported], cx);
    cx.read(|cx| assert_eq!(workflow.view.read(cx).table.read(cx).delegate().len(), 0));

    // Drop the canonical folder, as the platform reports dropped paths, so listed paths compare exactly.
    let nested_root = workflow
        .directory
        .path()
        .join("nested")
        .canonicalize()
        .expect("canonical nested folder");
    let second = nested_root.join("inner/second.wav");
    drop(vec![nested_root], cx);
    let view = workflow.view.clone();
    cx.wait_for(workflow.window, Duration::from_secs(30), move |_, cx| {
        std::thread::yield_now();
        view.read(cx).table.read(cx).delegate().len() == 1 && !view.read(cx).is_running()
    })
    .await;
    workflow.idle(cx).await;
    cx.read(|cx| {
        let table = workflow.view.read(cx).table.read(cx).delegate();
        assert!(
            table.row_for(&second).is_some(),
            "the nested folder is searched recursively"
        );
    });

    drop(vec![first.clone()], cx);
    workflow.idle(cx).await;
    cx.read(|cx| {
        let table = workflow.view.read(cx).table.read(cx).delegate();
        assert_eq!(table.len(), 2, "a dropped file is added to the listed folder");
        assert!(table.row_for(&first).is_some());
        assert!(table.row_for(&second).is_some());
    });
}

/// Switches the normalization mode, toggles overwrite and normalizing before the plugins through their controls,
/// and confirms that the mode buttons follow the option that enables them.
/// The C++ `NormalizeOptionsFollowControls` case drives the same controls.
#[gpui_kit::test]
fn integration_normalize_options_follow_controls(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    cx.simulate_window_resize(workflow.window, size(px(1920.0), px(1080.0)));
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find(control_ids::PROCESS_NORMALIZE_FIRST).checked(), Some(false));
        window.click(control_ids::PROCESS_NORMALIZE_FIRST, cx);
        assert_eq!(window.find(control_ids::PROCESS_NORMALIZE_FIRST).checked(), Some(true));
        window.click(control_ids::NORMALIZE_LOUDNESS, cx);
        window.click(control_ids::NORMALIZE_OVERWRITE, cx);
        assert_eq!(window.find(control_ids::NORMALIZE_OVERWRITE).checked(), Some(true));
    })
    .expect("drive the normalization controls");
    cx.run_until_parked();
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.normalize_first);
        assert!(view.overwrite_existing);
        assert_eq!(view.settings.normalize_mode, crate::settings::NormalizeMode::Loudness);
    });
    cx.update_window(workflow.window, |_, window, cx| {
        window.click(control_ids::NORMALIZE_PEAK, cx);
        window.click(control_ids::PROCESS_NORMALIZE_FIRST, cx);
        window.render_frame(cx);
        assert_eq!(window.find(control_ids::PROCESS_NORMALIZE_FIRST).checked(), Some(false));
    })
    .expect("drive the normalization controls back");
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(!view.normalize_first);
        assert_eq!(view.settings.normalize_mode, crate::settings::NormalizeMode::Peak);
    });
}

/// Requires Normalize and Process to reject input without a selection,
/// then Normalize to accept it once a file is selected while Process stays disabled without plugins.
/// The C++ `DisabledControlsRejectInput` case checks the same controls.
/// Play is checked here only: this application disables it until a player exists,
/// while the C++ application enables it for a loaded preview without an audio output.
#[gpui_kit::test]
async fn integration_disabled_controls_reject_input(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    cx.simulate_window_resize(workflow.window, size(px(1920.0), px(1080.0)));
    let file = workflow.copy("sine_stereo_44100_16.wav", "file.wav");
    workflow.add(vec![file.clone()], cx);
    workflow.idle(cx).await;
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        for id in [
            control_ids::NORMALIZE_SELECTED,
            control_ids::PROCESS_SELECTED,
            control_ids::PLAY_STOP,
        ] {
            assert_eq!(window.find(id).disabled(), Some(true), "{id} without a selection");
        }
        window.click(control_ids::NORMALIZE_SELECTED, cx);
    })
    .expect("query controls without a selection");
    cx.run_until_parked();
    cx.read(|cx| {
        assert!(
            !workflow.view.read(cx).is_running(),
            "a disabled Normalize starts nothing"
        );
    });
    workflow.select(&file, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        assert_ne!(
            window.find(control_ids::NORMALIZE_SELECTED).disabled(),
            Some(true),
            "Normalize with a selection"
        );
        assert_eq!(
            window.find(control_ids::PROCESS_SELECTED).disabled(),
            Some(true),
            "Process without plugins"
        );
        assert_eq!(
            window.find(control_ids::PLAY_STOP).disabled(),
            Some(true),
            "Play without audio output"
        );
    })
    .expect("query controls with a selection");
}

/// Keeps plugin processing and its optional normalization on the view's isolated disposal policy.
#[gpui_kit::test]
fn integration_plugin_processing_options_retain_isolated_disposal(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    cx.update(|cx| {
        workflow.view.update(cx, |view, cx| {
            view.set_normalize_first(true, cx);
            let options = view.processing_options();
            assert!(Arc::ptr_eq(&options.dispose, &view.dispose));
            let normalization = options.normalize_before_plugins.expect("normalize before plugins");
            assert!(Arc::ptr_eq(&normalization.dispose, &view.dispose));
        });
    });
}

/// Opens the production Audio Settings action without querying physical output devices.
#[gpui_kit::test]
fn integration_audio_settings_opens_without_audio_hardware(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    workflow.focus(cx);
    cx.dispatch_action(workflow.window, OpenAudioSettings);
    cx.run_until_parked();
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.settings_window.is_some());
        assert!(view.player.is_none());
    });
}

/// Delivers the shared primary-A and primary-P key events through the installed bindings to the root handlers.
#[gpui_kit::test]
async fn integration_primary_shortcuts_select_all_and_open_settings(cx: &mut TestAppContext) {
    let primary = if cfg!(target_os = "macos") { "cmd" } else { "ctrl" };
    let workflow = Workflow::new(cx);
    let first = workflow.copy("sine_stereo_44100_16.wav", "first.wav");
    let second = workflow.copy("sine_mono_44100_16.wav", "second.wav");
    workflow.add(vec![first.clone(), second.clone()], cx);
    workflow.idle(cx).await;
    workflow.focus(cx);
    workflow.menu_key(&format!("{primary}-a"), cx);
    cx.read(|cx| {
        let mut selected = workflow.view.read(cx).selected_paths(cx);
        selected.sort();
        let mut expected = vec![first, second];
        expected.sort();
        assert_eq!(selected, expected);
    });
    workflow.focus(cx);
    workflow.menu_key(&format!("{primary}-p"), cx);
    cx.read(|cx| assert!(workflow.view.read(cx).settings_window.is_some()));
}

/// Real key events adjust the semantic gain slider and persist without a pointer release.
#[gpui_kit::test]
async fn integration_keyboard_gain_slider_steps_clamps_resets_and_persists(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "keyboard-gain.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let press = press_slider(&workflow, control_ids::GAIN_TRACK, 0.5, cx);
    release_slider(&workflow, press, cx);
    cx.update_window(workflow.window, |_, window, _| {
        assert_eq!(window.find_by_label(control_ids::GAIN_LABEL).focused(), Some(true));
    })
    .expect("gain semantic node has pointer focus");
    let primary = if cfg!(target_os = "macos") { "cmd" } else { "ctrl" };
    workflow.menu_key(&format!("{primary}-a"), cx);
    workflow.menu_key("right", cx);
    workflow.menu_key("shift-up", cx);
    cx.read(|cx| {
        assert!(same_custom_gain(
            workflow.view.read(cx).selected_record(cx).expect("gain").custom_gain_db,
            Some(1.1)
        ));
    });
    advance_slider_clock(Duration::from_millis(500), cx);
    wait_for_gain(&workflow, &source, Some(1.1), cx).await;
    workflow.menu_key("end", cx);
    workflow.menu_key("right", cx);
    cx.read(|cx| {
        assert!(same_custom_gain(
            workflow
                .view
                .read(cx)
                .selected_record(cx)
                .expect("maximum")
                .custom_gain_db,
            Some(12.0)
        ));
    });
    workflow.menu_key("home", cx);
    workflow.menu_key("left", cx);
    cx.read(|cx| {
        assert!(same_custom_gain(
            workflow
                .view
                .read(cx)
                .selected_record(cx)
                .expect("minimum")
                .custom_gain_db,
            Some(-12.0)
        ));
    });
    workflow.menu_key(&format!("{primary}-0"), cx);
    wait_for_gain(&workflow, &source, None, cx).await;
    workflow.menu_key("tab", cx);
    cx.update_window(workflow.window, |_, window, _| {
        assert_ne!(window.find_by_label(control_ids::GAIN_LABEL).focused(), Some(true));
    })
    .expect("Tab leaves slider");
    workflow.menu_key("shift-tab", cx);
    cx.update_window(workflow.window, |_, window, _| {
        assert_eq!(window.find_by_label(control_ids::GAIN_LABEL).focused(), Some(true));
    })
    .expect("Shift-Tab returns to slider");
    workflow.menu_key(&format!("{primary}-shift-space"), cx);
    cx.update_window(workflow.window, |_, window, cx| {
        let view = workflow.view.read(cx);
        assert!(view.preview_compact);
        assert!(view.focus_handle.contains_focused(window, cx));
    })
    .expect("collapse restores surviving action focus");
    workflow.menu_key(&format!("{primary}-a"), cx);
    cx.read(|cx| assert_eq!(workflow.view.read(cx).selected_paths(cx), vec![source]));
}

/// A thumb press focuses the actual slider node, and disabling gain leaves a valid root command path.
#[gpui_kit::test]
async fn integration_keyboard_thumb_focus_disabled_gain_and_root_shortcuts(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "thumb-focus.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window
            .within(control_ids::GAIN_TRACK)
            .click(("slider-thumb", 0_u32), cx);
        assert_eq!(window.find_by_label(control_ids::GAIN_LABEL).focused(), Some(true));
    })
    .expect("thumb capture focuses semantic slider without losing drag behavior");
    let primary = if cfg!(target_os = "macos") { "cmd" } else { "ctrl" };
    workflow.menu_key(&format!("{primary}-e"), cx);
    cx.read(|cx| assert!(workflow.view.read(cx).chain_window.is_some()));
    cx.update_window(workflow.window, |_, window, cx| {
        window
            .within(control_ids::GAIN_TRACK)
            .click(("slider-thumb", 0_u32), cx);
    })
    .expect("refocus main slider after child window");
    workflow.menu_key("right", cx);
    workflow.menu_key("delete", cx);
    cx.update_window(workflow.window, |_, window, cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.table.read(cx).delegate().len(), 0);
        assert!(view.focus_handle.contains_focused(window, cx));
        assert_eq!(window.find_by_label(control_ids::GAIN_LABEL).disabled(), Some(true));
    })
    .expect("removing selected source disables gain and restores root focus");
    workflow.menu_key(&format!("{primary}-p"), cx);
    cx.read(|cx| assert!(workflow.view.read(cx).settings_window.is_some()));
}

/// The overhanging half of an endpoint thumb focuses its own slider before subsequent keyboard input.
#[gpui_kit::test]
async fn integration_keyboard_endpoint_thumb_press_focuses_its_slider(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "endpoint-focus.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    for (id, label, other, bound, next) in [
        (
            control_ids::ZOOM_TRACK,
            control_ids::ZOOM_LABEL,
            control_ids::GAIN_TRACK,
            "home",
            "right",
        ),
        (
            control_ids::ZOOM_TRACK,
            control_ids::ZOOM_LABEL,
            control_ids::GAIN_TRACK,
            "end",
            "left",
        ),
        (
            control_ids::GAIN_TRACK,
            control_ids::GAIN_LABEL,
            control_ids::ZOOM_TRACK,
            "home",
            "right",
        ),
        (
            control_ids::GAIN_TRACK,
            control_ids::GAIN_LABEL,
            control_ids::ZOOM_TRACK,
            "end",
            "left",
        ),
    ] {
        let press = press_slider(&workflow, id, 0.5, cx);
        release_slider(&workflow, press, cx);
        workflow.menu_key(bound, cx);
        let press = press_slider(&workflow, other, 0.5, cx);
        release_slider(&workflow, press, cx);
        let before = cx.read(|cx| {
            let view = workflow.view.read(cx);
            (
                view.zoom_slider.read(cx).value().start(),
                view.gain_slider.read(cx).value().start(),
            )
        });
        cx.update_window(workflow.window, |_, window, cx| {
            let thumb = window.within(id).find(("slider-thumb", 0_u32)).bounds();
            let slider = window.find_by_label(label).bounds();
            let x = if bound == "home" {
                thumb.left() + thumb.size.width * 0.25
            } else {
                thumb.right() - thumb.size.width * 0.25
            };
            assert!(
                x < slider.left() || x > slider.right(),
                "press must be outside the semantic track"
            );
            let position = point(x, thumb.center().y);
            window.pointer_down(position, MouseButton::Left, cx);
            window.pointer_up(position, MouseButton::Left, cx);
            assert_eq!(window.find_by_label(label).focused(), Some(true));
        })
        .expect("endpoint thumb owns focus even outside the track");
        workflow.menu_key(next, cx);
        cx.read(|cx| {
            let view = workflow.view.read(cx);
            let after = (
                view.zoom_slider.read(cx).value().start(),
                view.gain_slider.read(cx).value().start(),
            );
            if id == control_ids::ZOOM_TRACK {
                assert!((after.0 - before.0).abs() > 0.001);
                assert!((after.1 - before.1).abs() < 0.001);
            } else {
                assert!((after.1 - before.1).abs() > 0.001);
                assert!((after.0 - before.0).abs() < 0.001);
            }
        });
    }
}

/// Zoom and both target modes share a keyboard contract while disabled targets stay out of Tab traversal.
#[gpui_kit::test]
fn integration_keyboard_zoom_and_targets_follow_focus_modes_and_disabled_state(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let press = press_slider(&workflow, control_ids::ZOOM_TRACK, 0.5, cx);
    release_slider(&workflow, press, cx);
    workflow.menu_key("home", cx);
    workflow.menu_key("right", cx);
    workflow.menu_key("pageup", cx);
    cx.read(|cx| {
        let actual = workflow.view.read(cx).zoom_slider.read(cx).value().start();
        assert!((actual - 1.11).abs() < 0.0001, "keyboard zoom was {actual}");
    });
    workflow.menu_key("end", cx);
    cx.read(|cx| assert!((workflow.view.read(cx).zoom_slider.read(cx).value().start() - 20.0).abs() < 0.0001));
    workflow.menu_key("tab", cx);
    cx.update_window(workflow.window, |_, window, _| {
        assert_ne!(window.find_by_label(control_ids::GAIN_LABEL).focused(), Some(true));
        assert_ne!(
            window.find_by_label(control_ids::NORMALIZE_TARGET_LABEL).focused(),
            Some(true)
        );
    })
    .expect("disabled gain and target are not Tab stops");
    for mode in [NormalizeMode::Peak, NormalizeMode::Loudness] {
        let slider = choose_target(&workflow, mode, cx);
        let press = press_slider(&workflow, control_ids::NORMALIZE_TARGET, 0.5, cx);
        release_slider(&workflow, press, cx);
        workflow.menu_key("end", cx);
        workflow.menu_key("down", cx);
        let range = crate::normalization::target_range(mode);
        let expected = range.maximum - range.step;
        cx.read(|cx| assert!((slider.read(cx).value().start() - expected).abs() < 0.0001));
        advance_slider_clock(Duration::from_millis(500), cx);
        assert!((saved_target(&workflow, mode) - f64::from(expected)).abs() < 0.0001);
        cx.update_window(workflow.window, |_, window, _| {
            assert_eq!(
                window.find_by_label(control_ids::NORMALIZE_TARGET_LABEL).focused(),
                Some(true)
            );
        })
        .expect("active target owns semantic focus");
    }
    cx.update_window(workflow.window, |_, window, cx| {
        workflow.view.update(cx, |view, cx| view.set_normalize_first(false, cx));
        window.render_frame(cx);
        assert!(workflow.view.read(cx).focus_handle.contains_focused(window, cx));
    })
    .expect("disabling focused target restores root focus");
    let primary = if cfg!(target_os = "macos") { "cmd" } else { "ctrl" };
    workflow.menu_key(&format!("{primary}-p"), cx);
    cx.read(|cx| assert!(workflow.view.read(cx).settings_window.is_some()));
}

/// A retained pending edit survives a scan result captured before its debounced write.
#[gpui_kit::test]
async fn integration_slider_retained_scan_gain_overrides_earlier_cached_result(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "retained-gain.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let cache = workflow_cache(&workflow, cx);
    let captured = cache.get_analysis(&source).expect("earlier cached result");
    let slider = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    step_slider(&workflow, &slider, 0.1, cx);
    cx.update(|cx| {
        workflow.view.update(cx, |view, cx| {
            // Deliver the production scan-adoption seam deterministically, without racing a cache reader thread.
            view.store_unlisted_gains(std::slice::from_ref(&source), cx);
            view.table.update(cx, |table, cx| {
                table.delegate_mut().set_files(std::slice::from_ref(&source));
                table.delegate_mut().set_record(captured);
                cx.notify();
            });
            assert!(same_custom_gain(
                view.selected_record(cx).expect("retained edit").custom_gain_db,
                Some(0.1)
            ));
            assert_eq!(view.pending_gain_paths.as_slice(), std::slice::from_ref(&source));
            assert!(view.gain_store_task.is_some());
        });
    });
    advance_slider_clock(Duration::from_millis(500), cx);
    wait_for_gain(&workflow, &source, Some(0.1), cx).await;
}

/// Keeps overlapping batch edits on their latest deadline and submits paths dropped from the edited selection.
#[gpui_kit::test]
async fn integration_slider_overlapping_gain_selections_keep_latest_deadline(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let first = workflow.copy("sine_stereo_44100_16.wav", "overlap-first.wav");
    let second = workflow.copy("sine_mono_44100_16.wav", "overlap-second.wav");
    workflow.add(vec![first.clone(), second.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&first, cx);
    let slider = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    let cache = workflow_cache(&workflow, cx);
    step_slider(&workflow, &slider, 0.1, cx);
    advance_slider_clock(Duration::from_millis(400), cx);
    workflow.dispatch(SelectAll, cx);
    step_slider(&workflow, &slider, 0.1, cx);
    advance_slider_clock(Duration::from_millis(100), cx);
    for path in [&first, &second] {
        assert!(
            cache
                .get_analysis(path)
                .expect("unsubmitted batch edit")
                .custom_gain_db
                .is_none()
        );
    }
    cx.read(|cx| assert_eq!(workflow.view.read(cx).pending_gain_paths.len(), 2));
    workflow.select(&second, cx);
    step_slider(&workflow, &slider, -0.1, cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.pending_gain_paths.as_slice(), std::slice::from_ref(&second));
        assert!(view.gain_store_task.is_some());
    });
    advance_slider_clock(Duration::from_millis(500), cx);
    wait_for_gain(&workflow, &first, Some(0.2), cx).await;
    wait_for_gain(&workflow, &second, Some(0.1), cx).await;
}

/// Leaves the results focus scope in both Tab directions without losing selection or root command routing.
#[gpui_kit::test]
async fn integration_results_tab_navigation_leaves_table_and_preserves_root_shortcuts(cx: &mut TestAppContext) {
    let primary = if cfg!(target_os = "macos") { "cmd" } else { "ctrl" };
    let workflow = Workflow::new(cx);
    let first = workflow.copy("sine_stereo_44100_16.wav", "first.wav");
    let second = workflow.copy("sine_mono_44100_16.wav", "second.wav");
    workflow.add(vec![first.clone(), second.clone()], cx);
    workflow.idle(cx).await;
    for key in ["tab", "shift-tab"] {
        workflow.select(&first, cx);
        cx.update_window(workflow.window, |_, window, _| {
            assert_eq!(window.within(control_ids::RESULTS).find("table").focused(), Some(true));
        })
        .expect("pointer selects and focuses the table");
        workflow.menu_key(key, cx);
        cx.update_window(workflow.window, |_, window, cx| {
            let view = workflow.view.read(cx);
            assert!(!view.table.read(cx).focus_handle(cx).contains_focused(window, cx));
            assert!(view.focus_handle.contains_focused(window, cx));
            assert_eq!(view.selected_paths(cx), vec![first.clone()]);
        })
        .expect("Tab leaves the table without changing selected files");
        workflow.menu_key(&format!("{primary}-a"), cx);
        cx.read(|cx| {
            let mut selected = workflow.view.read(cx).selected_paths(cx);
            selected.sort();
            let mut expected = vec![first.clone(), second.clone()];
            expected.sort();
            assert_eq!(selected, expected);
        });
    }
    workflow.menu_key(&format!("{primary}-p"), cx);
    cx.read(|cx| assert!(workflow.view.read(cx).settings_window.is_some()));
}

/// Opens an empty plugin chain through the primary-E binding and reuses its window on repeated requests.
#[gpui_kit::test]
fn integration_primary_shortcut_opens_plugin_chain(cx: &mut TestAppContext) {
    let primary = if cfg!(target_os = "macos") { "cmd" } else { "ctrl" };
    let workflow = Workflow::new(cx);
    workflow.focus(cx);
    workflow.menu_key(&format!("{primary}-e"), cx);
    cx.read(|cx| assert!(workflow.view.read(cx).chain_window.is_some()));
    let window_count = cx.read(|cx| cx.windows().len());
    workflow.focus(cx);
    workflow.menu_key(&format!("{primary}-e"), cx);
    cx.read(|cx| {
        assert!(workflow.view.read(cx).chain_window.is_some());
        assert_eq!(cx.windows().len(), window_count);
    });
}

/// Hides Path once when saved column choices predate the Folder column, then keeps the migrated revision.
#[gpui_kit::test]
fn integration_old_column_choices_hide_path_once(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let settings_path = workflow.paths.data().join(crate::settings::SETTINGS_FILE_NAME);
    fs::create_dir_all(workflow.paths.data()).expect("create isolated data directory");
    fs::write(&settings_path, r#"{"hidden_columns": ["peak_left"]}"#)
        .expect("write settings saved before the Folder column");
    let (_, view) = Workflow::open(Some(AppOptions::in_test_directory(Arc::clone(&workflow.directory))), cx);
    cx.read(|cx| {
        let view = view.read(cx);
        let delegate = view.table.read(cx).delegate();
        assert!(!delegate.is_column_visible(crate::results::ColumnKey::Path));
        assert!(!delegate.is_column_visible(crate::results::ColumnKey::PeakLeft));
        assert!(delegate.is_column_visible(crate::results::ColumnKey::Folder));
        assert_eq!(view.settings.columns_revision, crate::settings::COLUMNS_REVISION);
    });
}

/// Opens copied valid and broken inputs through the file picker and waits for real workers to reach idle.
#[gpui_kit::test]
async fn integration_open_valid_and_broken_files_reaches_idle(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let valid = workflow.copy("sine_stereo_44100_16.wav", "valid.wav");
    let broken = workflow.directory.path().join("broken.wav");
    fs::write(&broken, b"not an audio file").expect("broken input");
    let broken = broken.canonicalize().expect("canonical broken input");
    workflow.add(vec![valid.clone(), broken.clone()], cx);
    workflow.idle(cx).await;
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        let table = view.table.read(cx).delegate();
        assert_eq!(table.len(), 2);
        assert_eq!(table.failed_count(), 1);
        assert_eq!(
            table
                .row_for(&valid)
                .expect("valid row")
                .ready_record()
                .expect("analysis")
                .status,
            AnalysisStatus::Analyzed
        );
        assert!(table.row_for(&broken).expect("failed row").ready_record().is_none());
        assert!(view.run.as_ref().is_some_and(|run| run.completed && run.finished == 2));
        assert!(view.status.contains("failed"));
        assert!(view.player.is_none());
        assert_eq!(view.plugins.chain.len(), 0);
    });
    assert!(workflow.paths.database().is_file());
}

/// Sorts through the table header while retaining the selected file's identity, metadata, and waveform.
#[gpui_kit::test]
async fn integration_selection_sort_retains_identity_metadata_and_waveform(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let first = workflow.copy("sine_stereo_44100_16.wav", "a.wav");
    let selected = workflow.copy("serato_rainbow_stereo_44100_16.aif", "z.aif");
    workflow.add(vec![first, selected.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&selected, cx);
    workflow.preview(&selected, cx).await;
    // Establish descending filename order before reversing it, independent of the default peak ordering.
    cx.update_window(workflow.window, |_, window, cx| {
        window.click(("icon-sort", 0_usize), cx);
    })
    .expect("choose filename ordering");
    cx.run_until_parked();
    assert_selected_row_accessibility(&workflow, &selected, "z.aif", cx);
    let initial_index = cx.read(|cx| workflow.view.read(cx).table.read(cx).delegate().index_of(&selected));
    let tags = cx.read(|cx| Arc::clone(&workflow.view.read(cx).selected_tags.as_ref().expect("selected tags").1));
    assert_eq!(
        tags.serato
            .as_ref()
            .and_then(|serato| serato.overview.as_ref())
            .expect("selected Serato metadata")
            .slices()
            .len(),
        test_fixtures::SERATO_OVERVIEW_SLICES
    );
    cx.update_window(workflow.window, |_, window, cx| {
        window.click(("icon-sort", 0_usize), cx);
    })
    .expect("sort through header");
    cx.run_until_parked();
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        let table = view.table.read(cx);
        assert_eq!(view.selected.as_ref(), Some(&selected));
        assert_ne!(table.delegate().index_of(&selected), initial_index);
        assert_eq!(table.selected_row(), table.delegate().index_of(&selected));
        assert_eq!(view.selected_paths(cx), vec![selected.clone()]);
        assert!(Arc::ptr_eq(
            &view.selected_tags.as_ref().expect("retained metadata").1,
            &tags
        ));
        assert_eq!(view.waveform.read(cx).waveform_source(), Some(selected.as_path()));
    });
    assert_selected_row_accessibility(&workflow, &selected, "z.aif", cx);
}

/// Checks the selected path's semantic row and visible cells against their composed accessible name.
fn assert_selected_row_accessibility(workflow: &Workflow, path: &Path, name: &str, cx: &mut TestAppContext) {
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        let row = window.find(Arc::<Path>::from(path));
        assert_eq!(row.role(), Some(gpui_kit::Role::Row));
        assert_eq!(row.selected(), Some(true));
        let scope = window.within(Arc::<Path>::from(path));
        let cells = scope.find_all_by_role(gpui_kit::Role::Cell);
        assert!(cells.iter().any(|cell| cell.label() == Some(name)));
        assert!(cells.iter().any(|cell| cell.label() == Some("44.1")));
        let label = cells
            .iter()
            .filter_map(|cell| cell.label())
            .filter(|label| !label.is_empty())
            .collect::<Vec<_>>()
            .join(", ");
        assert_eq!(row.label(), Some(label.as_str()));
    })
    .expect("selected row accessibility follows the source identity");
}

/// Reads the retained table's widths without changing automatic fitting, selection, or scrolling.
fn result_column_widths(workflow: &Workflow, cx: &App) -> Vec<Pixels> {
    let table = workflow.view.read(cx).table.read(cx);
    (0..).map_while(|index| table.col_width(index)).collect()
}

/// Checks the native cell wrappers against their headers, including partially clipped columns.
fn assert_result_columns_aligned(workflow: &Workflow, path: &Path, window: &mut Window, cx: &App) {
    let table = workflow.view.read(cx).table.read(cx);
    let row_index = table
        .delegate()
        .index_of(path)
        .expect("display index of the observed row");
    let widths = result_column_widths(workflow, cx);
    let mut aligned = 0;
    for (column_index, width) in widths.into_iter().enumerate() {
        let Some(header) = window.try_find(("col-header", column_index)) else {
            continue;
        };
        if !header.visible() {
            continue;
        }
        let cell = window
            .within(Arc::<Path>::from(path))
            .find(format!("table-cell-{row_index}:{column_index}"));
        assert!(cell.visible(), "a visible header has a visible cell below it");
        assert!((header.bounds().left() - cell.bounds().left()).abs() < px(0.01));
        assert!((header.bounds().right() - cell.bounds().right()).abs() < px(0.01));
        assert!((header.bounds().size.width - width).abs() < px(0.01));
        aligned += 1;
    }
    assert!(aligned > 1, "compare multiple live header and cell edges");
}

/// Returns source identities for real rows currently intersecting the production table viewport.
fn visible_result_paths(workflow: &Workflow, window: &mut Window, cx: &App) -> Vec<PathBuf> {
    let table = workflow.view.read(cx).table.read(cx);
    let rows = window
        .within(control_ids::RESULTS)
        .find_all_by_role(gpui_kit::Role::Row);
    (0..table.delegate().len())
        .filter_map(|index| table.delegate().row(index))
        .filter(|row| {
            let id = gpui_kit::ElementId::from(Arc::<Path>::from(row.path.as_path()));
            rows.iter()
                .any(|snapshot| snapshot.visible() && snapshot.path().last() == Some(&id))
        })
        .map(|row| row.path.clone())
        .collect()
}

/// A manual header drag creates overflow at the minimum width, and real wheel input moves headers and rows together.
#[expect(
    clippy::too_many_lines,
    reason = "One native resize and wheel workflow verifies shared header, cell, and selection invariants"
)]
#[gpui_kit::test]
async fn integration_results_horizontal_wheel_preserves_manual_widths_alignment_and_selection(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let selected = workflow.copy("spectrogram_expected/tone_44100.wav", "selected.wav");
    let other = workflow.copy("spectrogram_expected/tone_44100.wav", "other.wav");
    workflow.add(vec![selected.clone(), other], cx);
    workflow.idle(cx).await;
    cx.simulate_window_resize(workflow.window, size(px(800.0), px(800.0)));
    workflow.select(&selected, cx);
    let before = cx
        .update_window(workflow.window, |_, window, cx| {
            window.render_frame(cx);
            let before = result_column_widths(&workflow, cx);
            let header = window.find(("col-header", 0_usize)).bounds();
            let start = point(header.right() - px(2.0), header.center().y);
            window.pointer_down(start, MouseButton::Left, cx);
            window.pointer_move(start + point(px(8.0), px(0.0)), Some(MouseButton::Left), cx);
            window.pointer_move(start + point(px(220.0), px(0.0)), Some(MouseButton::Left), cx);
            window.pointer_up(start + point(px(220.0), px(0.0)), MouseButton::Left, cx);
            before
        })
        .expect("manually resize the Name column through its native header boundary");
    cx.run_until_parked();
    let (manual, origin, offset) = cx
        .update_window(workflow.window, |_, window, cx| {
            window.render_frame(cx);
            let manual = result_column_widths(&workflow, cx);
            assert_eq!(manual.len(), before.len());
            assert!(
                (*manual.first().expect("Name width") - *before.first().expect("original Name width")).abs()
                    > px(150.0)
            );
            for (actual, original) in manual.iter().zip(&before).skip(1) {
                assert!(
                    (*actual - *original).abs() < px(0.01),
                    "only the manually dragged column changes"
                );
            }
            let viewport = window.within(control_ids::RESULTS).find("table").bounds();
            assert!(manual.iter().copied().sum::<Pixels>() > viewport.size.width + px(150.0));
            assert_result_columns_aligned(&workflow, &selected, window, cx);
            let origin = window.find(("col-header", 0_usize)).bounds().left();
            let offset = workflow
                .view
                .read(cx)
                .table
                .read(cx)
                .horizontal_scroll_handle
                .offset()
                .x;
            window
                .within(control_ids::RESULTS)
                .scroll("table", ScrollDelta::Pixels(point(px(-120.0), px(0.0))), cx);
            (manual, origin, offset)
        })
        .expect("wheel horizontally over the overflowing production table");
    cx.run_until_parked();
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        let current = workflow
            .view
            .read(cx)
            .table
            .read(cx)
            .horizontal_scroll_handle
            .offset()
            .x;
        assert!(
            current < offset - px(50.0),
            "wheel input changes the real horizontal owner"
        );
        assert!(window.find(("col-header", 0_usize)).bounds().left() < origin - px(50.0));
        assert_result_columns_aligned(&workflow, &selected, window, cx);
        window
            .within(control_ids::RESULTS)
            .scroll("table", ScrollDelta::Pixels(point(px(-10_000.0), px(0.0))), cx);
    })
    .expect("headers and cells travel together during horizontal wheel scrolling");
    cx.run_until_parked();
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.find(("col-header", manual.len() - 1)).visible(),
            "the final Status column becomes visible"
        );
        assert_result_columns_aligned(&workflow, &selected, window, cx);
        window
            .within(control_ids::RESULTS)
            .scroll("table", ScrollDelta::Pixels(point(px(10_000.0), px(0.0))), cx);
    })
    .expect("reach the final column and reverse the wheel");
    cx.run_until_parked();
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        assert!((window.find(("col-header", 0_usize)).bounds().left() - origin).abs() < px(0.01));
        for (actual, expected) in result_column_widths(&workflow, cx).iter().zip(&manual) {
            assert!(
                (*actual - *expected).abs() < px(0.01),
                "scrolling never refits the manual widths"
            );
        }
        let view = workflow.view.read(cx);
        assert_eq!(view.selected_paths(cx), vec![selected.clone()]);
        assert_eq!(
            view.table.read(cx).selected_row(),
            view.table.read(cx).delegate().index_of(&selected)
        );
        assert_eq!(
            window.find(Arc::<Path>::from(selected.as_path())).selected(),
            Some(true)
        );
        assert_result_columns_aligned(&workflow, &selected, window, cx);
    })
    .expect("reverse scrolling restores the original alignment without changing selection or widths");
}

/// Wheel scrolling replaces virtualized row identities without selecting them, and arrow navigation reveals its target.
#[expect(
    clippy::too_many_lines,
    reason = "One virtualized table workflow follows row identities through wheel and keyboard input"
)]
#[gpui_kit::test]
async fn integration_results_vertical_wheel_virtualizes_rows_and_keyboard_reveals_selection(cx: &mut TestAppContext) {
    const ROW_COUNT: usize = 96;
    let workflow = Workflow::new(cx);
    let paths: Vec<_> = (0..ROW_COUNT)
        .map(|index| workflow.copy("spectrogram_expected/tone_44100.wav", &format!("row-{index:03}.wav")))
        .collect();
    workflow.add(paths, cx);
    workflow.idle(cx).await;
    cx.simulate_window_resize(workflow.window, size(px(800.0), px(800.0)));
    let initially_visible = cx
        .update_window(workflow.window, |_, window, cx| {
            window.render_frame(cx);
            let visible = visible_result_paths(&workflow, window, cx);
            assert!(visible.len() > 3 && visible.len() < ROW_COUNT / 2);
            let observed = window
                .within(control_ids::RESULTS)
                .find_all_by_role(gpui_kit::Role::Row);
            assert!(observed.len() < ROW_COUNT / 2, "offscreen rows are not all mounted");
            visible
        })
        .expect("read the initial virtualized production row identities");
    let selected = initially_visible.first().expect("initial visible row").clone();
    workflow.select(&selected, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window
            .within(control_ids::RESULTS)
            .scroll("table", ScrollDelta::Pixels(point(px(0.0), px(-960.0))), cx);
    })
    .expect("send a real vertical wheel event to the table");
    cx.run_until_parked();
    let revealed = cx
        .update_window(workflow.window, |_, window, cx| {
            window.render_frame(cx);
            let visible = visible_result_paths(&workflow, window, cx);
            assert!(visible.len() > 3 && visible.len() < ROW_COUNT / 2);
            assert!(
                visible.iter().all(|path| !initially_visible.contains(path)),
                "the wheel reveals different source rows"
            );
            assert!(
                window.try_find(Arc::<Path>::from(selected.as_path())).is_none(),
                "the original row is virtualized away"
            );
            let view = workflow.view.read(cx);
            assert_eq!(view.selected_paths(cx), vec![selected.clone()]);
            assert_eq!(
                view.table.read(cx).selected_row(),
                view.table.read(cx).delegate().index_of(&selected)
            );
            let viewport = window.within(control_ids::RESULTS).find("table").bounds();
            visible
                .into_iter()
                .rev()
                .find(|path| {
                    let row = window.find(Arc::<Path>::from(path.as_path())).bounds();
                    row.bottom() <= viewport.bottom() && row.top() >= viewport.top()
                })
                .expect("last completely visible source row")
        })
        .expect("wheel changes visible identities but preserves the original selected source");
    workflow.select(&revealed, cx);
    let target = cx
        .update_window(workflow.window, |_, window, cx| {
            let table = workflow.view.read(cx).table.read(cx);
            let index = table.delegate().index_of(&revealed).expect("newly selected row index");
            let target = table
                .delegate()
                .row(index + 2)
                .expect("two rows below the visible edge")
                .path
                .clone();
            assert!(
                window
                    .try_find(Arc::<Path>::from(target.as_path()))
                    .is_none_or(|row| !row.visible())
            );
            assert_eq!(workflow.view.read(cx).selected_paths(cx), vec![revealed.clone()]);
            assert_eq!(
                window.find(Arc::<Path>::from(revealed.as_path())).selected(),
                Some(true)
            );
            assert_eq!(window.within(control_ids::RESULTS).find("table").focused(), Some(true));
            target
        })
        .expect("select a row revealed by the wheel and identify the next offscreen keyboard target");
    workflow.menu_key("down", cx);
    workflow.menu_key("down", cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        let view = workflow.view.read(cx);
        assert_eq!(view.selected_paths(cx), vec![target.clone()]);
        assert_eq!(
            view.table.read(cx).selected_row(),
            view.table.read(cx).delegate().index_of(&target)
        );
        let row = window.find(Arc::<Path>::from(target.as_path()));
        assert!(row.visible());
        assert_eq!(row.selected(), Some(true));
        let viewport = window.within(control_ids::RESULTS).find("table").bounds();
        assert!(
            row.bounds().bottom() <= viewport.bottom() + px(0.01),
            "keyboard navigation scrolls the target fully into view"
        );
        assert!(visible_result_paths(&workflow, window, cx).contains(&target));
    })
    .expect("native arrow navigation selects and scrolls the offscreen source into view");
}

/// Normalizes only the selected source copy through its control and follows the committed output everywhere.
#[gpui_kit::test]
async fn integration_normalize_selected_replaces_row_and_preserves_selection(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "normalize.wav");
    let untouched = workflow.copy("sine_stereo_44100_16.wav", "untouched.wav");
    workflow.add(vec![source.clone(), untouched.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.click(control_ids::NORMALIZE_SELECTED, cx);
    })
    .expect("normalize through control handler");
    cx.run_until_parked();
    workflow.idle(cx).await;
    let output = source.with_extension("aif");
    assert!(output.is_file());
    assert!(
        !source.exists(),
        "injected disposer deletes the copied source, never trashes it"
    );
    assert!(untouched.is_file());
    workflow.preview(&output, cx).await;
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        let table = view.table.read(cx).delegate();
        assert_eq!(table.len(), 2);
        assert!(table.row_for(&source).is_none());
        let record = table
            .row_for(&output)
            .expect("output row")
            .ready_record()
            .expect("output analysis");
        assert!((record.overall_peak.abs() - 1.0).abs() < 0.001);
        assert_eq!(view.selected.as_ref(), Some(&output));
        assert_eq!(view.selected_paths(cx), vec![output]);
        assert!(
            view.normalize_run
                .as_ref()
                .is_some_and(|run| run.completed && run.finished == 1)
        );
    });
}

/// Cancels and clears pending work through actions, then rejects stale rows during the next generation.
#[gpui_kit::test]
async fn integration_cancel_and_clear_pending_work_never_resurrects_rows(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "pending.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    workflow.focus(cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.dispatch_action(Box::new(ReanalyzeSelected), cx);
    })
    .expect("dispatch reanalysis without polling worker events");
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.is_analyzing());
        assert_eq!(view.analysis_workers, 1);
    });
    cx.update_window(workflow.window, |_, window, cx| {
        window.dispatch_action(Box::new(CancelRun), cx);
        window.dispatch_action(Box::new(ClearList), cx);
    })
    .expect("cancel and clear before polling worker events");
    workflow.idle(cx).await;
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.table.read(cx).delegate().len(), 0);
        assert!(view.selected.is_none());
        assert!(view.selected_tags.is_none());
        assert!(view.waveform.read(cx).waveform_source().is_none());
        assert_eq!(view.roots, [] as [std::path::PathBuf; 0]);
    });
    // Start a new generation after the cancelled drain and prove only its rows are adopted.
    let replacement = workflow.copy("sine_stereo_44100_16.wav", "replacement.wav");
    workflow.add(vec![replacement.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&replacement, cx);
    workflow.preview(&replacement, cx).await;
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        let table = view.table.read(cx).delegate();
        assert_eq!(table.len(), 1);
        assert!(table.row_for(&source).is_none());
        assert!(table.row_for(&replacement).is_some());
    });
}

/// Dispatches every preview mode through the root while retaining source, metadata, gain, and source-time mapping.
#[expect(
    clippy::too_many_lines,
    reason = "One production workflow compares every pane against the same source, gain, metadata, and viewport"
)]
#[gpui_kit::test]
async fn integration_preview_modes_retain_source_gain_viewport_and_cues(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let fixture = test_fixtures::metadata_fixtures()
        .into_iter()
        .find(|fixture| !fixture.cues.is_empty())
        .expect("shared cue oracle");
    let source = workflow.copy(&format!("metadata/{}", fixture.file), &fixture.file);
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    workflow.preview(&source, cx).await;
    workflow.spectrogram(cx).await;
    let tags = cx.read(|cx| Arc::clone(&workflow.view.read(cx).selected_tags.as_ref().expect("tags").1));
    let cues: Vec<_> = tags.serato.as_ref().expect("Serato tags").cues().collect();
    assert_eq!(cues.len(), fixture.cues.len());
    for (cue, expected) in cues.iter().zip(&fixture.cues) {
        assert_eq!(cue.index, expected.index);
        assert_eq!(cue.position, expected.position_ms);
        assert_eq!(cue.color.to_rgb_u32(), expected.rgb);
    }
    cx.update_window(workflow.window, |_, window, cx| {
        let bounds = window.find(control_ids::GAIN_TRACK).bounds();
        window.click_at(
            control_ids::GAIN_TRACK,
            point(bounds.size.width * 0.75, bounds.size.height * 0.5),
            cx,
        );
    })
    .expect("edit source gain");
    cx.run_until_parked();
    workflow.scroll_preview(point(px(0.0), px(80.0)), Modifiers::none(), cx);
    workflow.scroll_preview(point(px(-40.0), px(0.0)), Modifiers::none(), cx);
    let (gain, zoom) = cx.read(|cx| {
        let view = workflow.view.read(cx);
        (
            view.selected_record(cx)
                .expect("record")
                .custom_gain_db
                .expect("gain edit"),
            view.zoom_slider.read(cx).value().start(),
        )
    });
    assert!(zoom > 1.0);
    let events = WaveformEvents::subscribe(&workflow, cx);
    let mut baseline: Option<f64> = None;
    for mode in PreviewViewMode::ALL {
        workflow.dispatch(SetPreviewViewMode { mode }, cx);
        let plots = workflow.plots(cx);
        assert_preview_arrangement(plots, mode);
        cx.read(|cx| {
            let view = workflow.view.read(cx);
            assert_eq!(view.settings.preview_view_mode, mode);
            assert_eq!(view.menu_state.preview_view_mode, mode);
            assert_eq!(view.waveform.read(cx).waveform_source(), Some(source.as_path()));
            assert!(view.waveform.read(cx).has_spectrogram_raster());
            assert!(Arc::ptr_eq(
                &view.selected_tags.as_ref().expect("retained tags").1,
                &tags
            ));
            assert!(
                (view.selected_record(cx).expect("record").custom_gain_db.expect("gain") - gain).abs() < f32::EPSILON
            );
            assert!((view.zoom_slider.read(cx).value().start() - zoom).abs() < f32::EPSILON);
            for candidate in PreviewViewMode::ALL {
                assert_eq!(
                    menu_checked(cx, &SetPreviewViewMode { mode: candidate }),
                    candidate == mode
                );
            }
        });
        for plot in plots
            .into_iter()
            .filter(|plot| plot.size.width > px(0.0) && plot.size.height > px(0.0))
        {
            let position = plot_point(plot, 0.25);
            cx.update_window(workflow.window, |_, window, cx| {
                window.pointer_down(position, MouseButton::Left, cx);
            })
            .expect("latch pane-local position");
            cx.run_until_parked();
            assert!(events.events.borrow().is_empty());
            let pending = cx.read(|cx| {
                workflow
                    .view
                    .read(cx)
                    .waveform
                    .read(cx)
                    .pending_seek_position()
                    .expect("pending")
            });
            if let Some(expected) = baseline {
                assert!((pending - expected).abs() < SEEK_TOLERANCE_SECONDS);
            } else {
                let duration = cx.read(|cx| {
                    workflow
                        .view
                        .read(cx)
                        .selected_record(cx)
                        .expect("record")
                        .duration_seconds
                });
                assert!(
                    pending > duration * 0.25 / f64::from(zoom),
                    "horizontal pan must be retained"
                );
                baseline = Some(pending);
            }
            cx.update_window(workflow.window, |_, window, cx| {
                window.pointer_up(position, MouseButton::Left, cx);
            })
            .expect("commit pane-local position");
            cx.run_until_parked();
            events.assert_seek(pending);
            events.events.borrow_mut().clear();
        }
    }
}

/// Opens both production preview menus, confirms a keyboard choice, and restores preview focus and root shortcuts.
#[expect(
    clippy::too_many_lines,
    reason = "One production workflow verifies focus restoration and command routing for both preview layouts"
)]
#[gpui_kit::test]
async fn integration_preview_context_menu_checks_modes_and_routes_keyboard_to_root(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "menu.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    workflow.preview(&source, cx).await;
    let events = WaveformEvents::subscribe(&workflow, cx);
    for compact in [false, true] {
        if compact {
            workflow.dispatch(TogglePreviewSection, cx);
        }
        for mode in PreviewViewMode::ALL {
            workflow.dispatch(SetPreviewViewMode { mode }, cx);
            let plot = workflow
                .plots(cx)
                .into_iter()
                .find(|plot| plot.size.width > px(0.0))
                .expect("visible plot");
            cx.update(|cx| {
                workflow.view.update(cx, |view, _| {
                    view.start_after_cues = Some(CueStartIntent::seek(&source));
                });
            });
            let previous_focus = cx
                .update_window(workflow.window, |_, window, cx| {
                    window.pointer_down(plot_point(plot, 0.25), MouseButton::Left, cx);
                    assert!(
                        workflow
                            .view
                            .read(cx)
                            .waveform
                            .read(cx)
                            .pending_seek_position()
                            .is_some()
                    );
                    window.focused(cx).expect("preview pointer focus")
                })
                .expect("establish an uncommitted display target");
            workflow.open_preview_menu(compact, cx);
            cx.update_window(workflow.window, |_, window, cx| {
                window.render_frame(cx);
                assert_preview_menu(window, mode);
                let view = workflow.view.read(cx);
                assert!(!view.focus_handle.is_focused(window));
                assert!(
                    view.waveform.read(cx).pending_seek_position().is_none(),
                    "context menu cancels only pending display intent"
                );
                assert!(
                    view.start_after_cues.is_some(),
                    "right-click must not change transport intent"
                );
            })
            .expect("inspect actual checked menu choices");
            workflow.menu_key("escape", cx);
            cx.wait_for(workflow.window, Duration::from_secs(1), |window, _| {
                window.try_find("popup-menu").is_none()
            })
            .await;
            cx.update_window(workflow.window, |_, window, cx| {
                let root_focus = &workflow.view.read(cx).focus_handle;
                assert_eq!(
                    window.focused(cx).as_ref(),
                    Some(&previous_focus),
                    "Escape restores the actual pre-menu pointer focus"
                );
                assert!(
                    root_focus.contains_focused(window, cx),
                    "restored preview focus retains the root command context"
                );
                assert_eq!(workflow.view.read(cx).settings.preview_view_mode, mode);
                assert!(
                    workflow.view.read(cx).start_after_cues.is_some(),
                    "menu Escape must not dispatch root StopPlayback"
                );
            })
            .expect("Escape preserves preference and restores focus");
            // Once the popup has dismissed,
            // the same physical key must bubble from the restored waveform to the real root StopPlayback handler,
            // without explicitly refocusing the root.
            workflow.menu_key("escape", cx);
            cx.read(|cx| {
                assert!(
                    workflow.view.read(cx).start_after_cues.is_none(),
                    "restored preview focus must deliver the root playback shortcut"
                );
            });
        }
        workflow.open_preview_menu(compact, cx);
        cx.update_window(workflow.window, |_, window, cx| {
            window.within("popup-menu").hover(1_usize, cx);
            assert_eq!(window.within("popup-menu").find(1_usize).selected(), Some(true));
        })
        .expect("select spectral menu choice");
        workflow.menu_key("enter", cx);
        cx.wait_for(workflow.window, Duration::from_secs(1), |window, cx| {
            window.try_find("popup-menu").is_none()
                && workflow.view.read(cx).settings.preview_view_mode == PreviewViewMode::Spectrogram
        })
        .await;
        cx.update_window(workflow.window, |_, window, cx| {
            assert!(workflow.view.read(cx).focus_handle.is_focused(window));
            assert_eq!(
                workflow.view.read(cx).waveform.read(cx).waveform_source(),
                Some(source.as_path())
            );
        })
        .expect("menu command reached real owner");
        workflow.open_preview_menu(compact, cx);
        cx.update_window(workflow.window, |_, window, _| {
            assert_preview_menu(window, PreviewViewMode::Spectrogram);
        })
        .expect("reopened menu reflects committed preference");
        workflow.menu_key("escape", cx);
    }
    assert!(
        events.events.borrow().is_empty(),
        "context gestures never seek, pause, or restart"
    );
}

/// Keeps pending gestures display-only until one release reaches the production root subscriber.
#[gpui_kit::test]
async fn integration_preview_drag_commits_once_on_release_in_each_source_pane(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "gesture.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    workflow.preview(&source, cx).await;
    let events = WaveformEvents::subscribe(&workflow, cx);
    for mode in PreviewViewMode::ALL {
        workflow.dispatch(SetPreviewViewMode { mode }, cx);
        for plot in workflow
            .plots(cx)
            .into_iter()
            .filter(|plot| plot.size.width > px(0.0) && plot.size.height > px(0.0))
        {
            let press = plot_point(plot, 0.25);
            let release = plot_point(plot, 0.75);
            cx.update(|cx| {
                workflow.view.update(cx, |view, _| {
                    view.start_after_cues = Some(CueStartIntent::seek(&source));
                });
            });
            cx.update_window(workflow.window, |_, window, cx| {
                window.pointer_down(press, MouseButton::Left, cx);
            })
            .expect("press source plot");
            cx.run_until_parked();
            cx.read(|cx| {
                let view = workflow.view.read(cx);
                assert!(view.player.is_none());
                assert!(view.start_after_cues.is_some(), "press must not reach transport");
                assert!(
                    (view.waveform.read(cx).pending_seek_position().expect("pending press") - 0.25).abs()
                        < SEEK_TOLERANCE_SECONDS
                );
            });
            assert!(events.events.borrow().is_empty());
            cx.update_window(workflow.window, |_, window, cx| {
                window.pointer_move(release, Some(MouseButton::Left), cx);
            })
            .expect("move display-only target");
            cx.run_until_parked();
            cx.read(|cx| {
                let view = workflow.view.read(cx);
                assert!(view.start_after_cues.is_some(), "drag must not reach transport");
                assert!(
                    (view.waveform.read(cx).pending_seek_position().expect("pending drag") - 0.75).abs()
                        < SEEK_TOLERANCE_SECONDS
                );
            });
            assert!(events.events.borrow().is_empty());
            cx.update_window(workflow.window, |_, window, cx| {
                window.pointer_up(release, MouseButton::Left, cx);
            })
            .expect("release source plot");
            cx.run_until_parked();
            events.assert_seek(0.75);
            cx.read(|cx| {
                let view = workflow.view.read(cx);
                assert!(
                    view.start_after_cues.is_none(),
                    "real root subscriber consumes the explicit seek"
                );
                assert!(view.waveform.read(cx).pending_seek_position().is_none());
                assert_eq!(view.waveform.read(cx).waveform_source(), Some(source.as_path()));
            });
            cx.update_window(workflow.window, |_, window, cx| {
                window.pointer_up(release, MouseButton::Left, cx);
            })
            .expect("duplicate release without a gesture");
            cx.run_until_parked();
            events.assert_seek(0.75);
            events.events.borrow_mut().clear();
        }
    }
}

/// Retains resized settings dimensions after dismissal without persisting them across app sessions.
#[gpui_kit::test]
fn integration_audio_settings_remember_session_size_after_close(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    workflow.dispatch(OpenAudioSettings, cx);
    let settings_window = cx.read(|cx| {
        cx.windows()
            .into_iter()
            .find(|window| *window != workflow.window)
            .expect("settings window")
    });
    let dimensions = size(px(720.0), px(640.0));
    cx.simulate_window_resize(settings_window, dimensions);
    cx.run_until_parked();
    cx.update_window(settings_window, |_, window, _| window.remove_window())
        .expect("closes settings");
    cx.run_until_parked();
    workflow.dispatch(OpenAudioSettings, cx);
    cx.update(|cx| {
        let view = workflow.view.read(cx);
        let handle = cx
            .windows()
            .into_iter()
            .find(|window| *window != workflow.window)
            .expect("reopened settings");
        assert_eq!(view.settings_window_size, Some(dimensions));
        handle
            .update(cx, |_, window, _| assert_eq!(window.bounds().size, dimensions))
            .expect("reads reopened settings dimensions");
    });
}

/// Keeps the open production settings controls, installed menus, and canonical root settings synchronized.
#[gpui_kit::test]
fn integration_preview_actions_sync_open_settings_and_independent_overload_choices(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    workflow.dispatch(OpenAudioSettings, cx);
    let settings_window = cx.read(|cx| {
        cx.windows()
            .into_iter()
            .find(|window| *window != workflow.window)
            .expect("production Audio Settings window")
    });
    cx.simulate_window_resize(settings_window, size(px(800.0), px(1000.0)));
    for mode in PreviewViewMode::ALL {
        workflow.dispatch(SetPreviewViewMode { mode }, cx);
        cx.update_window(settings_window, |_, window, cx| {
            window.render_frame(cx);
            assert_eq!(window.find("preview-view-mode").value(), Some(mode.label()));
        })
        .expect("open selector reflects root action");
    }
    for sample in [false, true] {
        for true_peak in [false, true] {
            for lines in [false, true] {
                for (action, expected) in [
                    (Box::new(ToggleSampleOverloads) as Box<dyn Action>, sample),
                    (Box::new(ToggleTruePeakOverloads) as Box<dyn Action>, true_peak),
                    (Box::new(ToggleOverloadLines) as Box<dyn Action>, lines),
                ] {
                    let current = cx.read(|cx| menu_checked(cx, action.as_ref()));
                    if current != expected {
                        workflow.focus(cx);
                        cx.update_window(workflow.window, |_, window, cx| window.dispatch_action(action, cx))
                            .expect("dispatch independent overload command");
                        cx.run_until_parked();
                    }
                }
                cx.read(|cx| {
                    let view = workflow.view.read(cx);
                    assert_eq!(view.settings.preview_view_mode, PreviewViewMode::Stacked);
                    assert_eq!(view.settings.show_sample_overloads, sample);
                    assert_eq!(view.settings.show_true_peak_overloads, true_peak);
                    assert_eq!(view.settings.show_overload_lines, lines);
                    assert_eq!(view.menu_state.show_sample_overloads, sample);
                    assert_eq!(view.menu_state.show_true_peak_overloads, true_peak);
                    assert_eq!(view.menu_state.show_overload_lines, lines);
                    assert_eq!(menu_checked(cx, &ToggleSampleOverloads), sample);
                    assert_eq!(menu_checked(cx, &ToggleTruePeakOverloads), true_peak);
                    assert_eq!(menu_checked(cx, &ToggleOverloadLines), lines);
                    assert!(view.player.is_none());
                });
                cx.update_window(settings_window, |_, window, cx| {
                    window.render_frame(cx);
                    assert_eq!(window.find("show-sample-overloads").checked(), Some(sample));
                    assert_eq!(window.find("show-true-peak-overloads").checked(), Some(true_peak));
                    assert_eq!(window.find("show-overload-lines").checked(), Some(lines));
                })
                .expect("actual settings checkboxes reflect independent choices");
            }
        }
    }
    // A different settings control uses the production SettingsEvent subscription,
    // and must not replay an old mode or any old overload value.
    cx.update_window(settings_window, |_, window, cx| {
        window.click("serato-waveform-colours", cx);
    })
    .expect("edit another preview preference through its checkbox");
    cx.run_until_parked();
    cx.read(|cx| {
        let settings = &workflow.view.read(cx).settings;
        assert!(!settings.serato_waveform_colours);
        assert_eq!(settings.preview_view_mode, PreviewViewMode::Stacked);
        assert!(settings.show_sample_overloads && settings.show_true_peak_overloads && settings.show_overload_lines);
        assert!(!menu_checked(cx, &ToggleSeratoColours));
    });
}

/// Requires same-pass previews by the root's completed notification, then loads the selected raster from that cache.
#[gpui_kit::test]
async fn integration_fresh_analysis_has_cached_previews_at_completion_and_selected_raster(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("spectrogram_expected/antiphase_48000.wav", "antiphase.wav");
    let completed = Rc::new(Cell::new(false));
    let observed = Rc::clone(&completed);
    let path = source.clone();
    let _subscription = cx.update(|cx| {
        cx.observe(&workflow.view, move |view, cx| {
            let view = view.read(cx);
            if observed.get() || !view.run.as_ref().is_some_and(|run| run.completed) {
                return;
            }
            let cache = view.coordinator.as_ref().expect("coordinator").cache();
            let record = cache.get_analysis(&path).expect("completed measurements");
            let waveform = cache
                .get_waveform(&path)
                .expect("waveform cached before completed notification");
            let spectrogram = cache
                .get_spectrogram(&path)
                .expect("spectrum cached before completed notification");
            assert_eq!(record.sample_rate, 48_000);
            assert_eq!(record.channels, 2);
            assert_eq!(record.length_in_samples, 9600);
            assert_eq!(waveform.sample_rate(), record.sample_rate);
            assert_eq!(waveform.channels(), record.channels);
            assert_eq!(waveform.length_in_frames(), record.length_in_samples);
            assert!(spectrogram.matches_stream(48_000, 9600, 2));
            assert_eq!(spectrogram.column_count(), 10);
            // Independent anti-phase fixture powers must not cancel as summed stereo samples would.
            assert!(spectrogram.intensities().iter().any(|&intensity| intensity > 0));
            observed.set(true);
        })
    });
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    assert!(completed.get(), "observed the production completion notification");
    workflow.select(&source, cx);
    workflow.preview(&source, cx).await;
    workflow.dispatch(
        SetPreviewViewMode {
            mode: PreviewViewMode::Spectrogram,
        },
        cx,
    );
    workflow.spectrogram(cx).await;
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        let record = view.selected_record(cx).expect("selected measured record");
        assert!(!record.from_cache, "first run measured the copied source");
        assert!(record.spectrogram.is_none(), "table rows do not own image payloads");
        assert_eq!(view.waveform.read(cx).waveform_source(), Some(source.as_path()));
        assert!(view.waveform.read(cx).has_spectrogram_raster());
        assert!(view.player.is_none());
    });
}

/// Restores a real slider edit and preference action after replacing the view but retaining its isolated storage.
#[gpui_kit::test]
async fn integration_recreated_view_restores_gain_and_preferences(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "persistent.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        let bounds = window.find(control_ids::GAIN_TRACK).bounds();
        window.click_at(
            control_ids::GAIN_TRACK,
            point(bounds.size.width * 0.75, bounds.size.height * 0.5),
            cx,
        );
    })
    .expect("change gain through slider");
    cx.run_until_parked();
    workflow.dispatch(ToggleLoudnessGraph, cx);
    workflow.dispatch(
        SetPreviewViewMode {
            mode: PreviewViewMode::Stacked,
        },
        cx,
    );
    workflow.dispatch(ToggleSampleOverloads, cx);
    workflow.dispatch(ToggleTruePeakOverloads, cx);
    workflow.dispatch(ToggleOverloadLines, cx);
    workflow.dispatch(ToggleSeratoColours, cx);
    let gain = cx.read(|cx| {
        workflow
            .view
            .read(cx)
            .selected_record(cx)
            .expect("selected record")
            .custom_gain_db
            .expect("slider gain")
    });
    let cache = cx.read(|cx| {
        Arc::clone(
            workflow
                .view
                .read(cx)
                .coordinator
                .as_ref()
                .expect("coordinator")
                .cache(),
        )
    });
    cx.wait_for(workflow.window, Duration::from_secs(30), |_, _| {
        std::thread::yield_now();
        cache
            .get_analysis(&source)
            .and_then(|record| record.custom_gain_db)
            .is_some_and(|stored| (stored - gain).abs() < 0.001)
    })
    .await;
    let persisted = GuiSettings::load_from(&workflow.paths.data().join("gui-settings.json"));
    assert!(persisted.show_loudness_graph);
    assert_eq!(persisted.preview_view_mode, PreviewViewMode::Stacked);
    assert!(!persisted.show_sample_overloads);
    assert!(!persisted.show_true_peak_overloads);
    assert!(!persisted.show_overload_lines);
    assert!(!persisted.serato_waveform_colours);
    cx.update_window(workflow.window, |_, window, _| window.remove_window())
        .expect("close old view");
    let Workflow {
        directory, paths, view, ..
    } = workflow;
    drop(view);
    cx.run_until_parked();
    let services = AppOptions::in_test_directory(Arc::clone(&directory));
    let (window, view) = Workflow::open(Some(services), cx);
    let workflow = Workflow {
        directory,
        paths,
        window,
        view,
    };
    cx.run_until_parked();
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.settings, persisted);
        assert_eq!(view.menu_state.preview_view_mode, persisted.preview_view_mode);
        assert!(menu_checked(
            cx,
            &SetPreviewViewMode {
                mode: PreviewViewMode::Stacked
            }
        ));
        assert!(!menu_checked(cx, &ToggleSampleOverloads));
        assert!(!menu_checked(cx, &ToggleTruePeakOverloads));
        assert!(!menu_checked(cx, &ToggleOverloadLines));
        let record = view.selected_record(cx).expect("reloaded record");
        assert!(record.from_cache);
        assert!((record.custom_gain_db.expect("persisted gain") - gain).abs() < 0.001);
    });
}

/// Persists Change-only gain through the real subscription, restarting its deadline without an initial write.
#[gpui_kit::test]
async fn integration_slider_gain_change_only_restarts_debounce_and_persists(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "change-only.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let slider = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    let cache = workflow_cache(&workflow, cx);
    let counts = Rc::new(Cell::new((0_usize, 0_usize)));
    let recorded = Rc::clone(&counts);
    let _subscription = cx
        .update_window(workflow.window, |_, window, cx| {
            workflow.view.update(cx, |_, cx| {
                cx.subscribe_in(&slider, window, move |_, _, event: &SliderEvent, _, _| {
                    let (changes, releases) = recorded.get();
                    recorded.set(match event {
                        SliderEvent::Change(_) => (changes + 1, releases),
                        SliderEvent::Release(_) => (changes, releases + 1),
                    });
                })
            })
        })
        .expect("observe slider events beside the production subscriber");
    advance_slider_clock(Duration::from_millis(600), cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
        assert!(view.gain_store_task.is_none(), "initial synchronization queues no edit");
    });
    assert!(
        cache
            .get_analysis(&source)
            .expect("cached source")
            .custom_gain_db
            .is_none()
    );
    step_slider(&workflow, &slider, 0.1, cx);
    advance_slider_clock(Duration::from_millis(400), cx);
    step_slider(&workflow, &slider, 0.1, cx);
    advance_slider_clock(Duration::from_millis(100), cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.pending_gain_paths.as_slice(), std::slice::from_ref(&source));
        assert!(
            view.gain_store_task.is_some(),
            "the first deadline cannot flush the newer edit"
        );
        assert!(same_custom_gain(
            view.selected_record(cx).expect("live source").custom_gain_db,
            Some(0.2)
        ));
    });
    assert!(
        cache
            .get_analysis(&source)
            .expect("not yet persisted")
            .custom_gain_db
            .is_none()
    );
    advance_slider_clock(Duration::from_millis(399), cx);
    assert!(
        cache
            .get_analysis(&source)
            .expect("before restarted deadline")
            .custom_gain_db
            .is_none()
    );
    advance_slider_clock(Duration::from_millis(1), cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
        assert!(view.gain_store_task.is_none());
    });
    wait_for_gain(&workflow, &source, Some(0.2), cx).await;
    assert_eq!(counts.get(), (2, 0), "semantic steps emit no pointer Release");
}

/// Checks both Change-only targets before mode switches or quit can conceal a missing debounce save.
#[gpui_kit::test]
fn integration_slider_targets_change_only_restart_debounce_and_persist(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    for mode in [NormalizeMode::Peak, NormalizeMode::Loudness] {
        let slider = choose_target(&workflow, mode, cx);
        let before = saved_target(&workflow, mode);
        let delta = -crate::normalization::target_range(mode).step;
        let path = workflow.paths.data().join(crate::settings::SETTINGS_FILE_NAME);
        let initial_file = fs::read(&path).ok();
        step_slider(&workflow, &slider, 0.0, cx);
        advance_slider_clock(Duration::from_millis(600), cx);
        cx.read(|cx| assert!(workflow.view.read(cx).normalize_target_store_task.is_none()));
        assert_eq!(fs::read(&path).ok(), initial_file);
        step_slider(&workflow, &slider, delta, cx);
        advance_slider_clock(Duration::from_millis(400), cx);
        step_slider(&workflow, &slider, delta, cx);
        advance_slider_clock(Duration::from_millis(100), cx);
        assert!((saved_target(&workflow, mode) - before).abs() < 1e-6);
        cx.read(|cx| assert!(workflow.view.read(cx).normalize_target_store_task.is_some()));
        advance_slider_clock(Duration::from_millis(399), cx);
        assert_eq!(fs::read(&path).ok(), initial_file);
        advance_slider_clock(Duration::from_millis(1), cx);
        assert!((saved_target(&workflow, mode) - f64::mul_add(f64::from(delta), 2.0, before)).abs() < 1e-6);
        cx.read(|cx| {
            let view = workflow.view.read(cx);
            assert!(view.normalize_target_store_task.is_none());
            assert_eq!(
                view.menu_state.normalize_target,
                crate::normalization::describe_target(&view.settings)
            );
            assert_eq!(
                view.settings.normalize_mode, mode,
                "no mode-switch save hides the regression"
            );
        });
    }
}

/// Pointer Release saves gain and either target immediately, cancelling each pending fallback deadline.
#[gpui_kit::test]
async fn integration_slider_pointer_release_flushes_without_waiting_for_debounce(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "pointer-release.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let press = press_slider(&workflow, control_ids::GAIN_TRACK, 0.75, cx);
    let gain = cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.gain_store_task.is_some());
        view.selected_record(cx)
            .expect("source feedback")
            .custom_gain_db
            .expect("pointer gain")
    });
    assert!(
        workflow_cache(&workflow, cx)
            .get_analysis(&source)
            .expect("source")
            .custom_gain_db
            .is_none()
    );
    release_slider(&workflow, press, cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.gain_store_task.is_none());
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
    });
    wait_for_gain(&workflow, &source, Some(gain), cx).await;
    for mode in [NormalizeMode::Peak, NormalizeMode::Loudness] {
        let slider = choose_target(&workflow, mode, cx);
        let before = saved_target(&workflow, mode);
        let press = press_slider(&workflow, control_ids::NORMALIZE_TARGET, 0.25, cx);
        let changed = cx.read(|cx| f64::from(slider.read(cx).value().start()));
        assert!((changed - before).abs() > 0.01);
        assert!((saved_target(&workflow, mode) - before).abs() < 1e-6);
        release_slider(&workflow, press, cx);
        assert!((saved_target(&workflow, mode) - changed).abs() < 1e-5);
        cx.read(|cx| assert!(workflow.view.read(cx).normalize_target_store_task.is_none()));
    }
}

/// Disabled sliders expose disabled native semantics and ignore real pointer presses, drags, and resets.
/// Native accessibility-action rejection cannot be dispatched by this test platform, as `step_slider` documents.
#[gpui_kit::test]
fn integration_slider_disabled_semantics_and_pointer_controls_reject_edits(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    cx.simulate_window_resize(workflow.window, size(px(1920.0), px(1080.0)));
    let path = workflow.paths.data().join(crate::settings::SETTINGS_FILE_NAME);
    let before = fs::read(&path).ok();
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        for (id, label) in [
            (control_ids::GAIN_TRACK, control_ids::GAIN_LABEL),
            (control_ids::NORMALIZE_TARGET, control_ids::NORMALIZE_TARGET_LABEL),
        ] {
            assert_eq!(window.find_by_label(label).disabled(), Some(true));
            let bounds = slider_snapshot(window, id).bounds();
            let start = point(bounds.left() + bounds.size.width * 0.25, bounds.center().y);
            let end = point(bounds.left() + bounds.size.width * 0.75, bounds.center().y);
            window.pointer_down(start, MouseButton::Left, cx);
            window.pointer_move(end, Some(MouseButton::Left), cx);
            window.pointer_up(end, MouseButton::Left, cx);
        }
    })
    .expect("drive disabled production sliders");
    cx.run_until_parked();
    for id in [control_ids::GAIN_TRACK, control_ids::NORMALIZE_TARGET] {
        double_click_slider(&workflow, id, cx);
    }
    advance_slider_clock(Duration::from_millis(600), cx);
    assert_eq!(fs::read(&path).ok(), before);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.gain_slider.read(cx).value().start().abs() < f32::EPSILON);
        assert!(view.settings.normalize_target_peak_dbfs.abs() < f64::EPSILON);
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
        assert!(view.gain_store_task.is_none());
        assert!(view.normalize_target_store_task.is_none());
    });
}

/// Selection changes never transfer a pending edit, while batch edits survive subsequent removal and clearing.
#[gpui_kit::test]
async fn integration_slider_pending_gain_tracks_selection_multiselection_remove_and_clear(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let first = workflow.copy("sine_stereo_44100_16.wav", "first-gain.wav");
    let second = workflow.copy("sine_mono_44100_16.wav", "second-gain.wav");
    workflow.add(vec![first.clone(), second.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&first, cx);
    let slider = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    step_slider(&workflow, &slider, 0.1, cx);
    workflow.select(&second, cx);
    advance_slider_clock(Duration::from_millis(500), cx);
    wait_for_gain(&workflow, &first, Some(0.1), cx).await;
    assert!(
        workflow_cache(&workflow, cx)
            .get_analysis(&second)
            .expect("other source")
            .custom_gain_db
            .is_none()
    );
    workflow.dispatch(SelectAll, cx);
    cx.read(|cx| assert_eq!(workflow.view.read(cx).selected_paths(cx).len(), 2));
    step_slider(&workflow, &slider, 0.2, cx);
    workflow.select(&first, cx);
    workflow.dispatch(RemoveSelected, cx);
    wait_for_gain(&workflow, &first, Some(0.2), cx).await;
    wait_for_gain(&workflow, &second, Some(0.2), cx).await;
    cx.read(|cx| {
        assert_eq!(
            workflow.view.read(cx).selected_paths(cx).as_slice(),
            std::slice::from_ref(&second)
        );
    });
    step_slider(&workflow, &slider, 0.1, cx);
    workflow.dispatch(ClearList, cx);
    wait_for_gain(&workflow, &second, Some(0.3), cx).await;
    advance_slider_clock(Duration::from_millis(600), cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.table.read(cx).delegate().len(), 0);
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
        assert!(view.gain_store_task.is_none());
        assert!(view.selected.is_none());
    });
}

/// Reset clears a pending explicit gain immediately and an obsolete timer cannot restore its old value.
#[gpui_kit::test]
async fn integration_slider_gain_reset_cancels_pending_change_and_persists_clear(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "reset-gain.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let slider = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    step_slider(&workflow, &slider, 0.2, cx);
    advance_slider_clock(Duration::from_millis(500), cx);
    wait_for_gain(&workflow, &source, Some(0.2), cx).await;
    step_slider(&workflow, &slider, 0.1, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.double_click(control_ids::GAIN_TRACK, cx);
    })
    .expect("reset pending gain through the production control");
    cx.run_until_parked();
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.selected_record(cx).expect("reset source").custom_gain_db.is_none());
        assert_eq!(view.pending_gain_paths, [] as [PathBuf; 0]);
        assert!(view.gain_store_task.is_none());
    });
    wait_for_gain(&workflow, &source, None, cx).await;
    advance_slider_clock(Duration::from_millis(600), cx);
    assert!(
        workflow_cache(&workflow, cx)
            .get_analysis(&source)
            .expect("stored clear")
            .custom_gain_db
            .is_none()
    );
}

/// A folder replacement flushes the edited old row before the production scan plan discards it.
#[gpui_kit::test]
async fn integration_slider_pending_gain_survives_replacement_plan(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "replaced-list.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let slider = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    step_slider(&workflow, &slider, 0.1, cx);
    let folder = workflow.directory.path().join("replacement");
    fs::create_dir(&folder).expect("replacement folder");
    fs::copy(test_fixtures::fixture("sine_mono_44100_16.wav"), folder.join("new.wav")).expect("replacement source");
    let position = point(px(400.0), px(200.0));
    cx.update_window(workflow.window, |_, window, cx| {
        for event in [
            gpui_kit::FileDropEvent::Entered {
                position,
                paths: gpui_kit::ExternalPaths(vec![folder].into_iter().collect()),
            },
            gpui_kit::FileDropEvent::Pending { position },
            gpui_kit::FileDropEvent::Submit { position },
        ] {
            window.dispatch_event(event.to_platform_input(), cx);
        }
    })
    .expect("replace roots through the native drop handler");
    cx.run_until_parked();
    workflow.idle(cx).await;
    wait_for_gain(&workflow, &source, Some(0.1), cx).await;
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        let table = view.table.read(cx).delegate();
        assert_eq!(table.len(), 1);
        assert!(table.row_for(&source).is_none());
        assert!(
            table
                .row(0)
                .expect("replacement row")
                .ready_record()
                .expect("new source")
                .custom_gain_db
                .is_none()
        );
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
        assert!(view.gain_store_task.is_none());
    });
}

/// Reset and mode-switch boundaries cancel obsolete target timers and keep each mode's independent target.
#[gpui_kit::test]
fn integration_slider_target_reset_and_mode_switch_flush_pending_changes(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    for mode in [NormalizeMode::Peak, NormalizeMode::Loudness] {
        let slider = choose_target(&workflow, mode, cx);
        let delta = -crate::normalization::target_range(mode).step;
        step_slider(&workflow, &slider, delta, cx);
        double_click_slider(&workflow, control_ids::NORMALIZE_TARGET, cx);
        let default = f64::from(crate::normalization::target_range(mode).default);
        assert!(
            (saved_target(&workflow, mode) - default).abs() < 1e-6,
            "{mode:?} reset saved {} instead of {default}",
            saved_target(&workflow, mode)
        );
        cx.read(|cx| assert!(workflow.view.read(cx).normalize_target_store_task.is_none()));
        step_slider(&workflow, &slider, delta, cx);
        let other = match mode {
            NormalizeMode::Peak => NormalizeMode::Loudness,
            NormalizeMode::Loudness => NormalizeMode::Peak,
        };
        choose_target(&workflow, other, cx);
        assert!((saved_target(&workflow, mode) - (default + f64::from(delta))).abs() < 1e-6);
        cx.read(|cx| assert!(workflow.view.read(cx).normalize_target_store_task.is_none()));
        advance_slider_clock(Duration::from_millis(600), cx);
        assert!((saved_target(&workflow, mode) - (default + f64::from(delta))).abs() < 1e-6);
    }
}

/// Graceful app shutdown invokes the real quit subscriptions and drains pending gain and both target values.
#[gpui_kit::test]
async fn integration_slider_quit_flushes_pending_gain_and_target(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "quit-gain.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let gain = cx.read(|cx| workflow.view.read(cx).gain_slider.clone());
    step_slider(&workflow, &gain, 0.1, cx);
    let peak = choose_target(&workflow, NormalizeMode::Peak, cx);
    step_slider(&workflow, &peak, -0.1, cx);
    let loudness = choose_target(&workflow, NormalizeMode::Loudness, cx);
    step_slider(&workflow, &loudness, -0.5, cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.gain_store_task.is_some());
        assert!(view.normalize_target_store_task.is_some());
    });
    let cache = workflow_cache(&workflow, cx);
    cx.quit();
    assert!(same_custom_gain(
        cache.get_analysis(&source).expect("quit drained writer").custom_gain_db,
        Some(0.1)
    ));
    assert!((saved_target(&workflow, NormalizeMode::Peak) + 0.1).abs() < 1e-6);
    assert!((saved_target(&workflow, NormalizeMode::Loudness) + 14.5).abs() < 1e-6);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert_eq!(view.pending_gain_paths, [] as [std::path::PathBuf; 0]);
        assert!(view.gain_store_task.is_none());
        assert!(view.normalize_target_store_task.is_none());
    });
}

/// Changes the custom gain with the secondary key and the wheel over the waveform,
/// one slider step per event, one step per ten events with Shift also held, and not at all without the modifier.
#[gpui_kit::test]
async fn integration_wheel_gain_steps_coarse_and_fine(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "wheel-gain.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    workflow.preview(&source, cx).await;
    let secondary = cx.read(Modifiers::secondary_key);
    let secondary_shift = Modifiers {
        shift: true,
        ..secondary
    };
    let gain = |cx: &mut TestAppContext| cx.read(|cx| workflow.view.read(cx).gain_slider.read(cx).value().start());
    let notch = point(px(0.0), px(20.0));
    for _ in 0..3 {
        workflow.scroll_preview(notch, secondary, cx);
    }
    assert!((gain(cx) - 0.3).abs() < 1e-4);
    for _ in 0..9 {
        workflow.scroll_preview(-notch, secondary_shift, cx);
    }
    assert!((gain(cx) - 0.3).abs() < 1e-4);
    // macOS delivers a vertical mouse wheel as a horizontal one while Shift is held.
    workflow.scroll_preview(point(px(-20.0), px(0.0)), secondary_shift, cx);
    assert!((gain(cx) - 0.2).abs() < 1e-4);
    // Reversing the wheel starts the count of fine events again.
    for _ in 0..5 {
        workflow.scroll_preview(notch, secondary_shift, cx);
    }
    for _ in 0..9 {
        workflow.scroll_preview(-notch, secondary_shift, cx);
    }
    assert!((gain(cx) - 0.2).abs() < 1e-4);
    workflow.scroll_preview(-notch, secondary_shift, cx);
    assert!((gain(cx) - 0.1).abs() < 1e-4);
    let zoom = cx.read(|cx| workflow.view.read(cx).zoom_slider.read(cx).value().start());
    workflow.scroll_preview(notch, Modifiers::none(), cx);
    assert!((gain(cx) - 0.1).abs() < 1e-4);
    assert!(cx.read(|cx| workflow.view.read(cx).zoom_slider.read(cx).value().start()) > zoom);
    let stored = cx.read(|cx| {
        workflow
            .view
            .read(cx)
            .selected_record(cx)
            .expect("record")
            .custom_gain_db
            .expect("wheel gain edit")
    });
    assert!((stored - 0.1).abs() < 1e-4);
}

/// Sends real native divider drags and proves that only release persists an independent preference.
#[gpui_kit::test]
fn integration_panel_width_drags_commit_independently_without_height_refit(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let path = workflow.paths.data().join("gui-settings.json");
    for (id, controls) in [
        (control_ids::CONTROLS_WIDTH_DIVIDER, true),
        (control_ids::INFORMATION_WIDTH_DIVIDER, false),
    ] {
        let before = cx.read(|cx| workflow.view.read(cx).settings.clone());
        let previous_file = fs::read(&path).ok();
        let height = cx.read(|cx| workflow.view.read(cx).preview_height);
        let start = cx
            .update_window(workflow.window, |_, window, cx| {
                window.render_frame(cx);
                let first = window.find(control_ids::CONTROLS_WIDTH_DIVIDER).bounds().center().x;
                let second = window.find(control_ids::INFORMATION_WIDTH_DIVIDER).bounds().center().x;
                let start = window.find(id).bounds().center();
                window.pointer_down(start, MouseButton::Left, cx);
                window.pointer_move(start + point(px(6.0), px(0.0)), Some(MouseButton::Left), cx);
                window.pointer_move(start + point(px(16.0), px(0.0)), Some(MouseButton::Left), cx);
                let changed_first = window.find(control_ids::CONTROLS_WIDTH_DIVIDER).bounds().center().x;
                let changed_second = window.find(control_ids::INFORMATION_WIDTH_DIVIDER).bounds().center().x;
                if controls {
                    assert!((f32::from(changed_first - first) - 16.0).abs() < 0.01);
                    assert!((f32::from(changed_second - changed_first - (second - first))).abs() < 0.01);
                } else {
                    assert!((f32::from(changed_first - first)).abs() < 0.01);
                    assert!((f32::from(changed_second - second) - 16.0).abs() < 0.01);
                }
                start
            })
            .expect("drag native preview divider");
        cx.read(|cx| {
            let view = workflow.view.read(cx);
            assert_eq!(view.settings, before, "drag feedback is not a committed preference");
            assert!(view.panel_width_drag.is_some());
            assert!((view.preview_height - height).abs() < f32::EPSILON);
        });
        assert_eq!(fs::read(&path).ok(), previous_file, "movement never writes settings");
        cx.update_window(workflow.window, |_, window, cx| {
            window.pointer_up(start + point(px(16.0), px(0.0)), MouseButton::Left, cx);
        })
        .expect("release native preview divider");
        cx.run_until_parked();
        let committed = cx.read(|cx| workflow.view.read(cx).settings.clone());
        if controls {
            assert!(committed.controls_width_scale > before.controls_width_scale);
            assert!((committed.information_width_scale - before.information_width_scale).abs() < f32::EPSILON);
        } else {
            assert!(committed.information_width_scale > before.information_width_scale);
            assert!((committed.controls_width_scale - before.controls_width_scale).abs() < f32::EPSILON);
        }
        assert_eq!(GuiSettings::load_from(&path), committed);
        cx.read(|cx| assert!((workflow.view.read(cx).preview_height - height).abs() < f32::EPSILON));
    }
}

/// Keeps keyboard adjustment, individual reset, collapse, and recreation on the same production preference path.
#[gpui_kit::test]
fn integration_panel_width_keyboard_reset_collapse_and_recreation(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let path = workflow.paths.data().join("gui-settings.json");
    cx.update_window(workflow.window, |_, window, cx| {
        window.click(control_ids::CONTROLS_WIDTH_DIVIDER, cx);
        window.press("right", cx);
        window.press("right", cx);
        let controls = window.find(control_ids::CONTROLS_WIDTH_DIVIDER).bounds().center().x;
        window.press("shift-right", cx);
        let wider = window.find(control_ids::CONTROLS_WIDTH_DIVIDER).bounds().center().x;
        assert!((f32::from(wider - controls) - 16.0).abs() < 0.01);
        window.press("shift-left", cx);
        let restored = window.find(control_ids::CONTROLS_WIDTH_DIVIDER).bounds().center().x;
        assert!(f32::from(restored - controls).abs() < 0.01);
        window.click(control_ids::INFORMATION_WIDTH_DIVIDER, cx);
        window.press("right", cx);
        let information = window.find(control_ids::INFORMATION_WIDTH_DIVIDER).bounds().center().x;
        window.press("shift-right", cx);
        let wider = window.find(control_ids::INFORMATION_WIDTH_DIVIDER).bounds().center().x;
        assert!((f32::from(wider - information) - 16.0).abs() < 0.01);
        assert!(
            (f32::from(window.find(control_ids::CONTROLS_WIDTH_DIVIDER).bounds().center().x - controls)).abs() < 0.01
        );
        window.press("shift-left", cx);
        let restored = window.find(control_ids::INFORMATION_WIDTH_DIVIDER).bounds().center().x;
        assert!(f32::from(restored - information).abs() < 0.01);
    })
    .expect("adjust separately focused native dividers");
    cx.run_until_parked();
    let changed = cx.read(|cx| workflow.view.read(cx).settings.clone());
    assert!(changed.controls_width_scale > 0.95);
    assert!(changed.information_width_scale > 0.90);
    assert_eq!(GuiSettings::load_from(&path), changed);
    workflow.dispatch(TogglePreviewSection, cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find(control_ids::CONTROLS_WIDTH_DIVIDER).is_none());
        assert!(window.try_find(control_ids::INFORMATION_WIDTH_DIVIDER).is_none());
    })
    .expect("compact preview contains no width handles");
    workflow.dispatch(TogglePreviewSection, cx);
    cx.read(|cx| assert_eq!(workflow.view.read(cx).settings, changed));
    cx.update_window(workflow.window, |_, window, cx| {
        window.double_click(control_ids::CONTROLS_WIDTH_DIVIDER, cx);
    })
    .expect("reset only controls by double click");
    let reset = cx.read(|cx| workflow.view.read(cx).settings.clone());
    assert!((reset.controls_width_scale - 0.95).abs() < f32::EPSILON);
    assert!((reset.information_width_scale - changed.information_width_scale).abs() < f32::EPSILON);
    assert_eq!(GuiSettings::load_from(&path), reset);
    cx.update_window(workflow.window, |_, window, _| window.remove_window())
        .expect("close original view");
    let Workflow {
        directory, paths, view, ..
    } = workflow;
    drop(view);
    cx.run_until_parked();
    let (window, view) = Workflow::open(Some(AppOptions::in_test_directory(Arc::clone(&directory))), cx);
    cx.read(|cx| assert_eq!(view.read(cx).settings, reset));
    cx.update_window(window, |_, window, cx| {
        window.click(control_ids::INFORMATION_WIDTH_DIVIDER, cx);
        window.press("home", cx);
    })
    .expect("reset restored information width from keyboard");
    cx.run_until_parked();
    let persisted = GuiSettings::load_from(&paths.data().join("gui-settings.json"));
    assert!((persisted.controls_width_scale - 0.95).abs() < f32::EPSILON);
    assert!((persisted.information_width_scale - 0.90).abs() < f32::EPSILON);
}

/// Window constraints change only resolved geometry and cannot replace a stored relative width.
#[gpui_kit::test]
fn integration_panel_width_preferences_survive_temporary_window_constraints(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    cx.update_window(workflow.window, |_, window, cx| {
        window.click(control_ids::CONTROLS_WIDTH_DIVIDER, cx);
        window.press("right", cx);
        window.click(control_ids::INFORMATION_WIDTH_DIVIDER, cx);
        window.press("left", cx);
    })
    .expect("establish explicit width preferences");
    cx.run_until_parked();
    let saved = cx.read(|cx| workflow.view.read(cx).settings.clone());
    let path = workflow.paths.data().join("gui-settings.json");
    let file = fs::read(&path).expect("saved settings");
    for width in [300.0, 800.0, 1600.0, 2200.0, 1600.0] {
        cx.simulate_window_resize(workflow.window, size(px(width), px(1000.0)));
        cx.update_window(workflow.window, |_, window, cx| window.render_frame(cx))
            .expect("render constrained view");
        cx.read(|cx| assert_eq!(workflow.view.read(cx).settings, saved));
        assert_eq!(fs::read(&path).expect("saved settings remain"), file);
    }
}

/// The menu action persists one canonical preference while keeping the source record and custom gain intact.
#[gpui_kit::test]
async fn integration_gain_estimate_menu_toggles_and_persists_without_changing_source(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "metric-menu.wav");
    workflow.add(vec![source.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    let original = cx.update(|cx| {
        workflow.view.update(cx, |view, cx| {
            view.change_selected_gain(2.0, cx);
            view.selected_record(cx).expect("selected source").clone()
        })
    });
    for enabled in [false, true] {
        workflow.dispatch(ToggleGainAdjustedMetrics, cx);
        cx.read(|cx| {
            let view = workflow.view.read(cx);
            assert_eq!(view.settings.show_gain_adjusted_metrics, enabled);
            assert_eq!(view.menu_state.show_gain_adjusted_metrics, enabled);
            let record = view.selected_record(cx).expect("unchanged source");
            assert!((record.integrated_lufs - original.integrated_lufs).abs() < f64::EPSILON);
            assert_eq!(record.custom_gain_db, original.custom_gain_db);
            let options = cx
                .get_menus()
                .expect("menus installed")
                .into_iter()
                .find(|menu| menu.name == "Options")
                .expect("Options menu");
            assert!(options.items.iter().any(|item| matches!(item,
                OwnedMenuItem::Action { name, action, checked, disabled, .. }
                if name == "Show custom gain estimates" && action.as_any().is::<ToggleGainAdjustedMetrics>()
                    && *checked == enabled && !disabled
            )));
        });
        let saved = GuiSettings::load_from(&workflow.paths.data().join("gui-settings.json"));
        assert_eq!(saved.show_gain_adjusted_metrics, enabled);
    }
}

/// Routes menu preferences through the real root and persists column visibility and playback choices.
#[gpui_kit::test]
fn integration_playback_and_folder_menu_preferences(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    cx.read(|cx| {
        let delegate = workflow.view.read(cx).table.read(cx).delegate();
        assert!(delegate.is_column_visible(crate::results::ColumnKey::Folder));
        assert!(!delegate.is_column_visible(crate::results::ColumnKey::Path));
        for column in [
            crate::results::ColumnKey::Plr,
            crate::results::ColumnKey::Psr,
            crate::results::ColumnKey::Crest,
            crate::results::ColumnKey::Dc,
            crate::results::ColumnKey::Clips,
            crate::results::ColumnKey::Correlation,
        ] {
            assert!(!delegate.is_column_visible(column));
        }
    });
    workflow.dispatch(ToggleContinuePlayback, cx);
    workflow.dispatch(TogglePlayFromFirstCuePoint, cx);
    workflow.dispatch(ToggleColumn { id: "path".into() }, cx);
    workflow.dispatch(ToggleColumn { id: "clips".into() }, cx);
    cx.read(|cx| {
        let view = workflow.view.read(cx);
        assert!(view.settings.continue_playback);
        assert!(view.settings.play_from_first_cue_point);
        assert!(
            view.table
                .read(cx)
                .delegate()
                .is_column_visible(crate::results::ColumnKey::Clips)
        );
        assert!(menu_checked(cx, &ToggleContinuePlayback));
        assert!(menu_checked(cx, &TogglePlayFromFirstCuePoint));
        assert!(
            view.table
                .read(cx)
                .delegate()
                .is_column_visible(crate::results::ColumnKey::Path)
        );
    });
    let saved = GuiSettings::load_from(&workflow.paths.data().join("gui-settings.json"));
    assert!(saved.continue_playback && saved.play_from_first_cue_point);
    assert!(
        !saved
            .hidden_columns
            .expect("explicit columns")
            .iter()
            .any(|id| id == "path")
    );
}

/// Exercises transient prediction publication through the production view without any plugin or output file.
#[gpui_kit::test]
async fn integration_gained_loudness_rejects_stale_gain_selection_and_toggle(cx: &mut TestAppContext) {
    let workflow = Workflow::new(cx);
    let source = workflow.copy("sine_stereo_44100_16.wav", "prediction.wav");
    let other = workflow.copy("silence_stereo_44100_16.wav", "other.wav");
    workflow.add(vec![source.clone(), other.clone()], cx);
    workflow.idle(cx).await;
    workflow.select(&source, cx);
    workflow.preview(&source, cx).await;
    cx.update(|cx| {
        workflow.view.update(cx, |view, cx| {
            view.change_selected_gain(12.0, cx);
            view.update_gain_prediction(cx);
            assert!(view.gained_loudness(cx).is_none());
        });
    });
    let view = workflow.view.clone();
    cx.wait_for(workflow.window, Duration::from_secs(15), move |_, cx| {
        std::thread::yield_now();
        view.read(cx).gained_loudness(cx).is_some()
    })
    .await;
    cx.update(|cx| {
        workflow.view.update(cx, |view, cx| {
            let record = view.selected_record(cx).expect("source");
            let predicted = view.gained_loudness(cx).expect("prediction");
            assert!(record.integrated_lufs + 12.0 - predicted.integrated() > 3.0);
            assert!((record.custom_gain_db.expect("gain") - 12.0).abs() < f32::EPSILON);
            let original_lufs = record.integrated_lufs;
            view.change_selected_gain(6.0, cx);
            assert!(view.gained_loudness(cx).is_none(), "old gain is hidden immediately");
            view.update_gain_prediction(cx);
            let mut settings = view.settings.clone();
            settings.show_gain_adjusted_metrics = false;
            view.apply_settings(settings, cx);
            assert!(view.gained_loudness(cx).is_none());
            assert!(
                (view.selected_record(cx).expect("unchanged source").integrated_lufs - original_lufs).abs()
                    < f64::EPSILON
            );
            let mut settings = view.settings.clone();
            settings.show_gain_adjusted_metrics = true;
            view.apply_settings(settings, cx);
            view.select_path(&other, false, cx);
            assert!(
                view.gained_loudness(cx).is_none(),
                "other file cannot borrow prediction"
            );
            view.clear_selection(cx);
            assert!(view.gained_loudness(cx).is_none());
        });
    });
}
