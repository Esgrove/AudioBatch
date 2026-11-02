//! Nonvisual integration workflows using the production view, table, controls, workers, and SQLite cache.
//! All writable paths are temporary, playback is disabled, and converted sources use a deleting disposer.

#![expect(
    clippy::future_not_send,
    reason = "GPUI test contexts remain on their foreground thread"
)]

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use audiobatch_core::AnalysisStatus;
use audiobatch_core::storage::StoragePaths;
use gpui_kit::test::{TestAppContextExt as _, TestWindowExt as _};
use gpui_kit::{AppContext as _, Bounds, Entity, Point, TestAppContext, WindowBounds, WindowOptions, point, px, size};
use tempfile::TempDir;

use super::{AppOptions, AppView, LaunchOptions};
use crate::menus::{AddFiles, CancelRun, ClearList, OpenAudioSettings, ReanalyzeSelected, ToggleLoudnessGraph};
use crate::settings::GuiSettings;
use crate::test_helpers::plugin_dispatcher;

/// Owns isolated application storage and source copies for a workflow.
struct Workflow {
    directory: Arc<TempDir>,
    paths: StoragePaths,
    window: gpui_kit::AnyWindowHandle,
    view: Entity<AppView>,
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
    workflow.focus(cx);
    cx.dispatch_action(workflow.window, ToggleLoudnessGraph);
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
    assert!(GuiSettings::load_from(&workflow.paths.data().join("gui-settings.json")).show_loudness_graph);
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
        assert!(view.settings.show_loudness_graph);
        let record = view.selected_record(cx).expect("reloaded record");
        assert!(record.from_cache);
        assert!((record.custom_gain_db.expect("persisted gain") - gain).abs() < 0.001);
    });
}
