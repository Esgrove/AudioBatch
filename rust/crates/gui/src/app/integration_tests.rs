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

use audiobatch_core::AnalysisStatus;
use audiobatch_core::storage::StoragePaths;
use gpui_kit::test::{TestAppContextExt as _, TestEventExt as _, TestWindowExt as _};
use gpui_kit::{
    Action, App, AppContext as _, Bounds, Entity, InputEvent as _, MouseButton, OwnedMenuItem, Pixels, Point,
    ScrollDelta, Subscription, TestAppContext, Window, WindowBounds, WindowOptions, point, px, size,
};
use tempfile::TempDir;

use super::cue_start::CueStartIntent;
use super::{AppOptions, AppView, LaunchOptions};
use crate::menus::{
    AddFiles, CancelRun, ClearList, OpenAudioSettings, ReanalyzeSelected, SetPreviewViewMode, ToggleLoudnessGraph,
    ToggleOverloadLines, TogglePreviewSection, ToggleSampleOverloads, ToggleSeratoColours, ToggleTruePeakOverloads,
};
use crate::settings::{GuiSettings, PreviewViewMode};
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

    /// Sends a native wheel event over a real pane, using layout only to target the existing renderer.
    fn scroll_preview(&self, delta: Point<Pixels>, cx: &mut TestAppContext) {
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
                    modifiers: window.modifiers(),
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
        window.click("normalize-selected", cx);
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
        let bounds = window.find("gain-track").bounds();
        window.click_at(
            "gain-track",
            point(bounds.size.width * 0.75, bounds.size.height * 0.5),
            cx,
        );
    })
    .expect("edit source gain");
    cx.run_until_parked();
    workflow.scroll_preview(point(px(0.0), px(80.0)), cx);
    workflow.scroll_preview(point(px(-40.0), px(0.0)), cx);
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
            assert_eq!(record.length_in_samples, 9_600);
            assert_eq!(waveform.sample_rate(), record.sample_rate);
            assert_eq!(waveform.channels(), record.channels);
            assert_eq!(waveform.length_in_frames(), record.length_in_samples);
            assert!(spectrogram.matches_stream(48_000, 9_600, 2));
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
        let bounds = window.find("gain-track").bounds();
        window.click_at(
            "gain-track",
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
