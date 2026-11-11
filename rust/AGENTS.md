# AGENTS.md

This file governs everything under `rust/`, the Rust version of AudioBatch.
`../cpp/AGENTS.md` describes the C++ application in `../cpp/`, the reference implementation of AudioBatch.
Its build, format, lint, and CMake instructions do not apply here.
Its prose style, naming, and documentation rules do.
When the two files disagree about code under `rust/`, this file wins.

## Working Next To The C++ Project

- Follow the repository-wide [behavioral parity rules](../AGENTS.md#behavioral-parity).
  Differences in results for the same input are bugs unless a narrow, technically justified exception is documented.
- Read the C++ sources freely to understand how a feature behaves.
  The C++ application is a reference,
  not an oracle that overrides the file format or independently verified expectations.
  Question anything that looks wrong before carrying it over, and fix a confirmed bug in both applications.
  `../cpp/src/analysis/AudioAnalysis.cpp`, `../cpp/src/analysis/AnalysisCache*.cpp`, and `../cpp/src/cli/`
  are the closest counterparts of `crates/core` and `crates/cli`, and `../cpp/src/plugins/`
  and `../cpp/src/gui/LiveChain*.cpp` of `crates/plugins` and the GUI's `plugins/`.
- The tests read the shared fixtures in place from the root `fixtures` directory through the `test-fixtures` crate.
  Do not copy fixtures into `rust/`.
- The GUI embeds the Roboto fonts from the root `assets` directory, the same files the C++ application bundles,
  and uses the same app icons from there.
  Do not copy them into `rust/` either.
- Stage with explicit paths, for example `git add rust`.
  Avoid `git add -A`, `git add .`, and `git commit -a`, so unrelated work in progress stays out of Rust commits.

**NEVER run destructive git commands**, including `git checkout -- <file>`, `git restore`, `git reset --hard`,
`git clean`, and `git stash drop`.
They can destroy uncommitted work.
Ask the user to do it manually if something needs undoing.

## What It Does

The Rust version covers the key features of the C++ application:

- Analyze audio files for decoded sample peak, true peak, and integrated loudness (EBU R128).
- Cache analysis results in SQLite so unchanged files are not decoded again.
- List the files with their stats in the CLI and the GUI.
- Draw a waveform overview of the selected file in the GUI,
  coloured from the Serato Overview tag when the file has one.
  Show separate source sample-over and true-peak-over markers, independent of preview gain and plugins.
- Show the selected file's embedded PNG or JPEG album artwork in the GUI,
  read independently of the tags and decoded into a bounded thumbnail.
- Cache the Serato Overview of each file in a `serato_tag_cache` table with the same schema as the C++ cache.
- Play the selected file in the GUI.
- Keep a per-file custom gain in the GUI and the cache, applied to the waveform and to the plugin chain preview.
- Accept files and folders dropped onto the GUI.
- Drag the selected rows out of the GUI to other applications as file copies, on macOS and Windows.
- Offer a right-click menu on the rows of the GUI, with the items of the C++ menu except Normalization Format Support.
- Normalize files from the CLI and the GUI to a sample peak or loudness target,
  written as AIFF with tags and Serato data.
  Lossless sources produce identical samples in peak mode and differ by at most one PCM step in loudness mode.
- Scan for VST3 plugins in a worker process and list them in the CLI and the GUI.
- Process files in the GUI or the CLI's `process --preset` subcommand through a chain of VST3 effects,
  written as 16-bit AIFF with tags and Serato data.
- Show plugin editors in native windows, and play the preview through the plugin chain.

Plugin hosting covers VST3 on macOS and Windows.
Audio Unit plugins are only hosted by the C++ application.

## Workspace Layout

[`ARCHITECTURE.md`](ARCHITECTURE.md) explains how the parts work together, with charts.
This section lists the files.

The repository-root [`Cargo.toml`](../Cargo.toml) defines the shared virtual workspace,
and its `rust-version` declares the required Rust toolchain.
Shared package metadata, dependency versions, and lints are defined once there,
and the main workspace crates inherit them with `.workspace = true`.
AudioBatch dependency versions remain in its `[workspace.dependencies]` table.
ImageControl reuses shared entries but declares plugin-only dependencies in its own crate manifests.
The application and primary ImageControl crates share the root lockfile and `rust/target` artifact directory.
ImageControl's own VST3/Iced adapter lives in `image-control-plugin/rust/crates/plugin` and uses no GPUI patches.
The excluded `fuzz/` workspace declares its own dependencies.
Write ordinary versions as full `major.minor.patch` versions, so compatible updates are picked up.
Possible breaking changes are not a reason to add an exact pin.
Preserve the existing exact pins for `gpui-pre` and `gpui-pre-platform` snapshot support,
and the coordinated `vst3` SDK pin described below.

- `crates/core` (`audiobatch-core`): the shared library used by the CLI and the GUI.
  - `analysis.rs` decodes a file and measures peaks and loudness, and builds the waveform overview in the same pass.
    `meter.rs` groups the decoded chunks into the blocks that feed the measurements.
    `signal_metrics.rs` uses bounded channel moments for source-only PLR, PSR, unweighted crest factor,
    signed DC means, full-scale sample counts, and centered stereo correlation, separate from gain predictions.
    `loudness/k_weighting.rs` holds the combined fourth order K-weighting filter,
    `loudness/gating.rs` the weighted block-energy windows and integrated gates,
    and `loudness/true_peak.rs` the SIMD interpolator.
    Ebur128 is a development-only differential reference, not a production dependency.
  - `benches/` measures analysis, decoding, normalization, waveform construction,
    cache reads, and sorting with Criterion.
    `benches/spectrogram.rs` isolates spectral setup, streaming, finalization, merging, and codec costs.
    The longer inputs come from `../fixtures/benchmark/generate.py` and are generated locally, not committed.
  - `decode.rs` wraps Symphonia: probing, the decode loop, and the read failure rules.
    Isolated packet failures become silence with a notice, and three counted consecutive failures fail the pass.
    Early EOF can instead produce a successful measurement of available audio with a truncation notice.
    On macOS, `decode/native_aac.rs` uses Core Audio for single-track mono/stereo AAC-LC MP4,
    reconciling perceptual noise substitution while keeping Symphonia for every other codec and platform.
    It trims the gapless padding the way the decoders of the C++ application do, keeping the source timeline aligned,
    reading the MP4 edit list in `decode/gapless.rs`, which symphonia parses but never applies,
    and leaving out the MP3 synthesis delay when no LAME header states the encoder delay.
  - `loudness/known_answers.rs` checks the loudness against known answers from the EBU Tech 3341 and 3342 signals,
    independently of any reference implementation.
    `golden_tests.rs` holds the golden snapshot tests of the analysis and normalization output.
  - `record.rs` holds `AnalysisRecord`, its status, and the short-term loudness history.
    `overload.rs` builds and validates bounded per-channel source overload flags and their independent cache blob.
  - `waveform.rs` holds the min/max peak overview and its resolution levels.
  - `spectrogram.rs` builds the optional mono/stereo mel spectrogram from measurement blocks with `realfft`,
    and holds `SpectrogramData` with its little-endian `SPEC` codec shared with the C++ application.
    Cached bands extend to source Nyquist, while the GUI frequency axis extends to at least 20 kHz.
  - `gain_loudness.rs` remeasures gain-and-hard-clip loudness and applies the empirical integrated limiter estimate.
    Its results are presentation predictions, never source metrics, cache authority, or live plugin measurements.
  - `discovery.rs` expands input paths into the sorted list of supported audio files.
    `file_selection.rs` chooses the paths of a drag out and groups selected files by folder for revealing them.
  - `cache.rs` is the SQLite result cache, with its Serato tag table in `cache/serato.rs`,
    the Rust waveform table in `cache/waveform_cache.rs`,
    and the shared `spectrogram_cache` table in `cache/spectrogram.rs`,
    while the `cache/test_*.rs` files hold its tests by area,
    `coordinator.rs` runs analysis in parallel with cancellation and progress events,
    and `normalize_coordinator.rs` does the same for normalization, keeping the cache in step with the outputs.
  - `format.rs` formats levels, file properties, and Folder-column home shortening for display.
    `sorting.rs` sorts records.
  - `tags.rs` reads display tags and Serato data with lofty, and tells what the Serato Overview read found.
    Serato data comes from the `GEOB` frames of MP3, AIFF, and WAV files, and the `SERATO_*` comments of FLAC files.
  - `normalize.rs` computes the normalization gain and writes the normalized file.
    `aiff.rs` is its AIFF writer, `metadata.rs` copies the tags onto the output,
    and `output_file.rs` commits the output and disposes of the replaced files,
    with `output_file/windows_recycle_bin.rs` telling whether a folder's files go to the Windows recycle bin.
  - `operation.rs` holds the result of an operation that rewrites one file, and the cache update after it.
  - `storage.rs` resolves shared database and private application paths into injectable `StoragePaths`.
  - `log_file.rs` sets up the `tracing` log file and the stderr output of both front ends,
    and `atomic_write.rs` replaces small data files, such as the settings, atomically.
  - `test_helpers.rs` holds the fixture builders that the unit tests of several modules share.
- `crates/playback` (`audiobatch-playback`): the preview engine.
  A decode thread feeds an `rtrb` ring buffer that the `cpal` output callback reads,
  with `rubato` resampling when the device rate differs from the file rate.
  The callback can run the output through a `PreviewInsert`.
  `output/insert.rs` drives it with the pre-roll of its latency, fades, and the drain of its tail at the end.
  It has no GUI or plugin dependency, only the trait.
  `lib.rs` holds the `Player`, `device.rs` the output device selection and stream configuration,
  `engine.rs` the decode thread, `pipeline.rs` the conversion of a file to stereo frames at the device rate,
  `output/mod.rs` the shared state and the callback renderer, and `preview_insert.rs` the `PreviewInsert` trait.
  `callback_access.rs` gives the callback exclusive renderer access with one atomic admission attempt,
  so the callback never touches a mutex or wakes a waiter, and control threads close admission before waiting.
  `stream_errors.rs` carries stream errors out of the callback through a bounded channel.
  `callback_lifecycle_tests.rs` and `callback_transport_tests.rs` hold the hardware-free callback contracts,
  and `realtime_tests.rs` the RealtimeSanitizer controls, which only the nightly `rtsan.sh` configuration enables.
  The ignored, hardware-free callback benchmark is in `callback_benchmark.rs`,
  with its CPU accounting in `callback_benchmark_cpu.rs` and its native insert in `callback_benchmark_native.rs`.
- `crates/vst3-host` (`vst3-host`): VST3 hosting on top of the raw `vst3` bindings.
  `module.rs` loads bundles, `factory.rs` lists their classes,
  `instance.rs` holds the plugin lifecycle and the host side parameter changes of `set_parameter`,
  `processor.rs` runs audio processing on preallocated buffers, and `host_objects/` implements the host's COM objects.
  `thread.rs` holds the UI thread token and the dispatcher, and `ui_bound.rs` a value only the UI thread may use.
  The `editor` feature adds the native editor windows in `editor/`, with an AppKit and a Win32 backend,
  in `editor/macos.rs` and `editor/windows.rs`, with `editor/unsupported.rs` for the other platforms,
  and `editor/plug_frame.rs` passes the size requests of a view to its window.
  `host_objects/` holds the host application, the attribute lists and messages, the component handler,
  the parameter changes, and the memory stream, one file each.
  `discovery.rs` finds bundles on disk, `module_info.rs` reads their `moduleinfo.json` without loading them,
  `platform_events.rs` runs the platform event loop in a process without a GUI framework,
  `strings.rs` converts the fixed-size string fields of the VST3 structs, and `enums.rs` the enum constants.
  It holds the unsafe code of VST3 hosting.
- `crates/plugins` (`audiobatch-plugins`): the plugin features, in safe code only.
  `scanner.rs` scans out of process through `scan_worker.rs`, `library.rs` keeps the known plugins,
  `chain.rs` the saved chain, `presets.rs` the named chain presets,
  `processing.rs` renders one file through a chain,
  `processing_coordinator.rs` runs batches on a worker pool with `min(files, workers)` reusable chains,
  and `preview_chain.rs` runs the live chain in the preview.
  `chain_signal.rs` holds the channel handling that processing and the preview share,
  `processing_chain.rs` the instances one processing worker renders with,
  and `scan_protocol.rs` the messages between a scan and its worker process.
  `tests/realtime.rs` holds the RealtimeSanitizer contracts of the complete preview chain.
  The `vst3_benchmark` example measures native VST3 processing for `scripts/vst3_benchmark.sh`,
  and the `limiter_probe` example collects the gain sweeps that `../fixtures/limiter_model.py` fits.
- `crates/test-plugin` (`audiobatch-test-plugin`): dev-only fake VST3 plugins for the tests.
  `factory()` serves them inside the test process, and the `module` example builds them into a library
  that `bundle.rs` wraps in a `.vst3` bundle, found under `target/<profile>/examples`.
  `factory.rs`, `controller.rs`, and `processor.rs` implement the COM interfaces of the fake plugins.
- `crates/serato` (`serato-tags`): parsers for the Serato GEOB tags, extracted from the track-rename project.
  They take raw frame bytes and do no terminal printing, so any tag library and front end can use them.
  `overview.rs` parses the Overview tag strictly and defines its cache representation,
  and `colouring.rs` decodes the palette indices and computes the column colours of the waveform.
  `markers.rs` parses Markers2 with the cue points, loops, and track color, `beatgrid.rs` the BeatGrid,
  `autotags.rs` the Autotags with the BPM and gain, and `native.rs` the Serato comments of FLAC and Ogg files.
- `crates/cli` (`audiobatch-cli`): the `audiobatch-rs` binary.
  Its options, JSON field names, and exit codes match the C++ `audiobatch` CLI,
  so `scripts/parity.sh` can compare the two directly.
  With `--normalize`, `scripts/compare_outputs.py` also compares the files both CLIs wrote,
  sample by sample and ID3v2 frame by frame, so a change to decoding or tag copying must keep them in agreement.
  The `list` and `completion` subcommands match the C++ CLI too.
  `completion.rs` generates the scripts with clap, where the C++ CLI builds them from its own option tables,
  and both install them to the same per-user directories.
  Only the Rust CLI also supports elvish.
  `args.rs` defines the arguments, `run.rs` the analysis and normalization workflow,
  `process.rs` the `process --preset` workflow with native JSON presets and main-thread event pumping,
  `output.rs` the table, CSV, and JSON output, `plugin_list.rs` the `list` subcommand,
  and `messages.rs` the coloured log messages, which the log file also receives.
- `crates/gui` (`audiobatch-gui`): the `AudioBatchRs` GUI binary, built on gpui-kit.
  It is the only crate that depends on gpui-kit.
  `app/mod.rs` holds the main view state, `app/startup.rs` what it opens at startup,
  `app/analysis.rs` the analysis runs, `app/selection.rs` the selection, `app/gain.rs` the per-file gain,
  `app/actions.rs` the menu and transport actions, `app/normalize.rs` normalization, and `app/render.rs` the layout.
  `app/preferences.rs` holds the Audio Settings window and the preview options,
  and `app/tags.rs` the tags of the selected file with its Serato cue points and Overview.
  `app/enrichment.rs` owns post-load waveform, spectrogram, and Serato filling with one active cancellable reader.
  `app/gain_loudness.rs` debounces selected-file gain predictions and shares decoder admission with enrichment,
  and `app/gain_writer.rs` serializes coalesced explicit gain edits without blocking the GUI.
  `app/panel_widths.rs` owns persisted panel-width scales and fits them with `resolve_widths`.
  `app/cue_start.rs` retains a play or seek intent until matching cue metadata and source duration are ready.
  `app/artwork.rs` reads the album artwork of the selected file, which `artwork.rs` decodes into a bounded thumbnail.
  `plugins/` holds the live chain in `live_chain.rs`,
  the Plugin Chain and Plugin Manager windows in `chain_window.rs` and `manager_window.rs`,
  the preview insert in `preview.rs`, and their texts in `labels.rs`,
  and `app/plugins.rs`, `app/process.rs`, and `app/preview.rs` connect them to the main view.
  The views outside `app/` are `results.rs` for the table, `waveform.rs` for the four preview modes,
  with `waveform/` holding their geometry, rendering, axes, loudness graph, overloads, and spectrogram raster,
  `controls.rs` and `info_panel.rs` for the controls column and the information panel,
  and `settings_window.rs` for the Audio Settings window.
  `controls/slider.rs` is the focusable slider with shared pointer, keyboard, and accessibility value changes.
  `control_ids.rs` holds the element ids of the controls shared with the C++ application.
  `menus.rs` defines the actions, the shortcuts, and the menu bar, `theme.rs` the palette and the fonts,
  `settings.rs` the saved preferences, `normalization.rs` the normalization target and options,
  `operation_run.rs` the progress of a normalization or processing run,
  `playback_status.rs` the status bar's playback clock,
  and `files.rs` revealing files and opening their folders.
  `file_drag.rs` drags the selected rows out to other applications,
  and `file_drag/windows.rs` is its OLE drag source on Windows, where gpui has no outbound file drag.
  `single_instance.rs` hands the paths of a second launch to the running app,
  `demo.rs` holds the data of the `--demo-rows` stress test, and `snapshot.rs` the `snapshot` feature.
  `build_info.rs` holds the version and build metadata that the About dialog, `--version`, and the log show.
  Its `build.rs` embeds the app icon from the root `assets` directory into the Windows executable.
  On macOS, `build.sh` and `install.sh` package it as `AudioBatchRs.app` through `create_mac_app_bundle` in `common.sh`.
- `crates/build-info` (`build-info`): the build script helper of the CLI and the GUI.
  Its `emit` records the version, build time, git branch and commit, and compiler version as environment variables.
  The binaries read them at compile time, and `--version` and the About dialog show them.
- `fuzz` (`audiobatch-fuzz`): the cargo-fuzz targets, a separate workspace excluded from this one,
  because they build only on nightly.
  `fuzz_targets/` holds one file per target, and `corpus/<target>/` its committed seeds.
- `crates/test-fixtures`: a dev-only helper that locates the shared fixtures and lists their expected properties.
  It also locates the JUCE test VST3, lists the outputs that plugin must produce,
  and holds `null_test` in `null_test.rs`, which compares two decoded signals in 16-bit steps
  and finds the delay between them when they do not cancel.

Keep the core and playback crates free of GUI dependencies,
so the GUI framework stays replaceable without touching them.
Keep plugin code out of them too, so the preview engine knows plugins only through `PreviewInsert`.

## Analysis And Normalization Contracts

- Loudness may differ from the ebur128 reference by at most 0.001 LU for integrated loudness,
  maximum short-term loudness, and every short-term history point.
  Integrated and maximum short-term values must also display the same two decimal places.
  Do not require reference bit equality for loudness fields or serialized history.
  True peak must match the ebur128 reference bit for bit, including x86 arithmetic underflow.
- Goldens are our own regression snapshots, not immutable reference output.
  A validated numerical change may regenerate the affected snapshots.
  Keep differential, displayed-value, chunk-invariance, normalization, and parity checks as independent validation.
  Windows x86-64 selects exact-bit overrides in `crates/core/tests/golden/windows-x86_64/`.
  The two native AAC fixture cases on macOS select `crates/core/tests/golden/macos-native-aac/`.
  Both fall back to shared snapshots for outputs without an override.
  For these platform cases, `UPDATE_GOLDEN=1` writes only differing overrides,
  removes redundant overrides, and never rewrites the shared snapshots.
  Generated input identities and fixture checksums remain shared across platforms.
- K weighting uses a combined fourth order `f64` filter with fixed state for 64 channels,
  ebur128's default channel map, and dual-mono weighting for mono sources.
  Gating sums a fixed ring of 30 weighted 100 ms energies directly to avoid running-sum drift.
  Only absolute-gate-admitted integrated energies are appended to a `Vec`, reserved from the expected length.
- A final partial short-term window after three seconds must expire only the outgoing prefix.
  One weighted squared energy per frame is retained in a shared three-second ring for that boundary,
  not filtered per-channel audio.
  This bounded memory compromise is separate from complete updates, which sum only the 30 block energies.
- Meter errors occur only at construction for streams outside 1 through 64 channels or 16 through 2822400 Hz.
  Extrema and plane references use fixed arrays.
  Waveform building reuses extrema when a block fits in the remaining bucket, otherwise it rescans.
  Preserve the existing minimum waveform bucket size of 64 frames, not 100 ms.
- Normalization measures exact decoded `f32` values from the writer's 16- or 24-bit quantizer during rendering.
  Keep header and frame-count validation, without a full output re-decode.
  Fuse one `f32` gain multiplication into the writer, without a scaled copy or per-chunk plane-reference `Vec`.
  An in-place file already at target returns the current record after cancellation and source-identity checks.
- Shared measurement contract version 16 is coordinated with the C++ cache.
  A measurement-affecting change in either implementation requires a coordinated invalidation.
  Preserve same-identity custom gains, including explicit clears, and compatible previews when refilling measurements.
  Keep ebur128 as a development dependency only.

## VST3 Hosting

- **The UI thread rule.** VST3 requires loading modules, creating, setting up, activating, and destroying instances,
  their state, and their editors on the thread that runs the event loop.
  Only `process` and `setProcessing` may run elsewhere.
  Methods that need the UI thread take a `&UiThread`, a token that only exists there and is not `Send`.
  Other threads reach the UI thread through `UiDispatcher`, and the GUI drains its `UiJobs` on the foreground executor.
  A worker that waits on the UI thread passes its cancellation check, so it never blocks a UI thread waiting on it.
- **Unsafe code.** The workspace denies `unsafe_code`,
  and a module that needs it opts in with `#![expect(unsafe_code, reason = "...")]`, kept as narrow as possible.
  Mostly this is in `vst3-host` and the test plugin,
  plus the Windows file drag in `crates/gui/src/file_drag/windows.rs`,
  the Core Audio AAC bridge in `crates/core/src/decode/native_aac.rs`,
  the callback admission in `crates/playback/src/callback_access.rs`,
  and the test-only RealtimeSanitizer and CPU accounting code in `crates/playback/src/realtime_tests.rs`,
  `crates/playback/src/callback_benchmark_cpu.rs`, and `crates/plugins/tests/realtime.rs`.
  Every unsafe block has a `// SAFETY:` comment, which the `undocumented_unsafe_blocks` lint enforces.
  Keep COM pointers inside `vst3-host`, so the crates above it stay safe.
  Run `./miri.sh` after changing unsafe code in its covered modules,
  and keep new tests runnable under Miri where possible.
  The script does not exercise the native Core Audio AAC bridge, which needs native validation.
- **The SDK version.** The `vst3` crate is pinned to `=0.3.0`, whose bindings are generated from VST SDK 3.8.0,
  the SDK that JUCE bundles in the C++ application.
  Keep both applications on the same SDK version, and upgrade them together.
  The bindings are pre-generated, so building needs no SDK or extra toolchain.
- **Crashes.** Only scanning is isolated in a worker process.
  A plugin that crashes while processing, previewing, or showing its editor takes the GUI down.
- **Data files** in the `AudioBatch-rs` data directory:
  `plugins.json` holds the known plugins, the blacklist, and the custom scan folders,
  `plugin-chain.json` the chain with its base64 plugin state,
  `chain-presets/` one `<name>.json` file per chain preset in the same form,
  `plugin-scan.lock` keeps two scans from running at once,
  and `plugin-scan-worker.log` collects what plugins print while a worker loads them.
  None of them is shared with the C++ application, whose chain state and plugin ids are specific to JUCE.
- **Logging** goes through the `tracing` macros, never `eprintln!` in library code.
  `audiobatch_core::log_file::install` writes the log file and prints to stderr,
  and forwards records of crates that use the `log` facade.
  The CLI logs the messages it prints itself with the `FILE_ONLY_TARGET` target, so they are not printed twice.
  `analyze_file`, `normalize_file`, and `process_file` open a span with the file path,
  so messages from worker threads name their file.
  Never hold a stderr lock across a run, since worker threads log to stderr, and never log from the audio callback.
- **Tests** use the fake plugins of the test plugin crate and the JUCE `AudioBatch Test MS` VST3 in `../test-plugin/`,
  never installed plugins.
  `crates/plugins/tests/juce_test_plugin.rs` saves its settings in a preset, loads it back, processes a fixture,
  and requires the output to cancel against the expected file that `../fixtures/plugin_expected.py` computes.
  These tests skip when the bundle has not been built with `../test-plugin/build.sh`, and fail in CI.
  To try real plugins by hand, run `cargo run -p vst3-host --example inspect`
  or `cargo run -p vst3-host --features editor --example editor -- <bundle>`.
- **Windows.** CI builds and tests on Windows, including the Win32 editor backend.
  Locally, `cargo clippy -p vst3-host --features editor --target x86_64-pc-windows-msvc --all-targets -- -D warnings`
  checks the backend from macOS, after `rustup target add x86_64-pc-windows-msvc`.

## Running Both Versions Side By Side

- The Rust CLI is `audiobatch-rs`, and the Rust app is `AudioBatchRs` with the bundle id `com.esgrove.audiobatch-rs`.
  The C++ versions are `audiobatch` and `AudioBatch` with `com.esgrove.audiobatch`.
- Both applications share `AudioBatch/analysis.db`, with a common versioned `file_analysis` measurement contract.
  The database is `~/Library/AudioBatch/analysis.db` on macOS, not in Application Support,
  and `%APPDATA%/AudioBatch/analysis.db` on Windows.
  Settings, logs, plugin data, and presets remain in their separate application directories.
  `AUDIOBATCH_DATA_ROOT` selects an absolute root with shared `AudioBatch/analysis.db`
  and Rust-private `AudioBatch-rs/`, including its `logs/` subdirectory.
  Invalid explicit roots must never fall back to production storage.
  In-process tests inject `StoragePaths` rather than changing process-wide environment variables.
- Measurement provenance names the application that measured the audio, not the last writer of gain or preview data.
  Successful measurements are reusable in either direction, while decoder failures are producer-specific.
  Rust waveforms use `rust_waveform_cache`, leaving JUCE waveform columns untouched for compatible unchanged sources.
  Both applications share `spectrogram_cache` and its `SPEC` codec.
  Grids may differ by one intensity step in at most one cell per thousand, because the FFT libraries differ.
  `crates/core/tests/shared_cache.rs` measures that documented bound
  when `AUDIOBATCH_CPP_TESTS` names the C++ test executable.
  Missing or corrupt waveforms, spectrograms, and Serato cache payloads are artifact misses, not measurement misses.
- The GUI fills missing native waveforms, spectrograms, and Serato outcomes after initial loading.
  A bounded background queue does it, decoding once for every missing audio artifact.
  Keep selected-file work within the same admission limit,
  retain stable negative tag outcomes, and reject stale identities.
  Do not persist inherited gain values as new user edits, hydrate every waveform in measurement reads,
  or run SQLite work on the audio callback.
- Foreign measurements are reporting results of their originating decoder and meter.
  Remeasure natively before normalization decisions, including refusal and no-op checks,
  unless operation compatibility is independently established.
  Explicit custom-gain or unity plugin processing does not require foreign-origin remeasurement.

## Build And Test Commands

Run the application scripts from `rust/`. Cargo commands also work from the repository root.
Use explicit application packages for scoped Cargo work, because root `--workspace` includes primary ImageControl crates.
After making code changes, always run:

```shell
cargo clippy --fix --allow-dirty --all-targets
cargo fmt
./test.sh --unit
./test.sh --gui-integration
./test.sh --optimized
./test.sh --cli-integration
./test.sh --tooling
```

`test.sh` defaults to `--unit`, excluding GUI integration workflows.
The GUI and CLI integration tiers retain diagnostics under `target/integration-artifacts/`,
and accept `--artifacts <directory>`.
The optimized tier runs release core tests and adds the CLI normalization subset on Windows.
The CLI tier builds the executable and runs the shared Python contracts through uv,
and the tooling tier runs the parity and benchmark regression suites through uv.
Raw `cargo nextest run` includes the GUI integration workflows but not either Python suite.
`cargo test` also runs the Rust workspace tests.
`./lint.sh` runs the same clippy command with `-D warnings`, also on the fuzz crate,
and `./lint.sh --check` reports the warnings without applying fixes.
CI runs `cargo clippy --all-targets -- -D warnings` and the tests on macOS and Windows,
and `cargo fmt --check` and a coverage run on macOS.

Other commands:

```shell
# Build the release binaries into rust/, or install them and the CLI shell completions.
# On macOS, the GUI becomes AudioBatchRs.app, and install.sh puts it in /Applications.
./build.sh
./install.sh

# Format the Rust code, the shell scripts, and the prose, add --check to only verify them
./format.sh

# Cover workspace tests and executable CLI contracts, add --open for the HTML report
./coverage.sh

# Cover only Rust workspace tests, without the Python CLI contracts
cargo llvm-cov nextest --workspace

# Run the unsafe VST3 code, the Serato parsers, the callback admission, and the cache blob readers under Miri on nightly
./miri.sh

# Build the cargo-fuzz targets, replay their corpus, and fuzz each for 60 seconds, add --replay to only replay
./fuzz.sh
# Select the Serato, CLI, and cache targets, or only the decoder, and set the fuzzing time
./fuzz.sh --targets parsers --seconds 60
./fuzz.sh --targets decoder

# Check the playback and preview callbacks with RealtimeSanitizer on macOS, with nightly Rust and Homebrew LLVM
./rtsan.sh --skip-native --artifacts target/rtsan/artifacts/first-party
./rtsan.sh --diagnostic

# Compare native VST3 processing in both hosts with the real test plugin
./scripts/vst3_benchmark.sh --iterations 30 --repeat 100

# Check the macOS AAC reconstruction of both CLIs against FFmpeg and afconvert
uv run --script scripts/test_aac_reconstruction.py --cpp-cli <absolute-cpp-cli> --rust-cli <absolute-rust-cli>

# Generate the long benchmark inputs, which the file benchmarks skip when absent
uv run ../fixtures/benchmark/generate.py

# Save a Criterion reference and compare a later measurement without replacing it
cargo bench -p audiobatch-core --bench analysis -- --save-baseline before
cargo bench -p audiobatch-core --bench analysis -- --baseline before

# Run just the in-memory measurement benchmarks
cargo bench -p audiobatch-core --bench analysis -- meter/

# Build and capture a sampling profile without opening a browser
cargo build --profile profiling --bin audiobatch-rs
samply record --save-only -o target/profile.json.gz -- ./target/profiling/audiobatch-rs --refresh --jobs 1 ../fixtures/benchmark/output/music_stereo_44100_16.wav

# Save the CLI reference before an optimization, then compare without updating the reference
./scripts/snapshot.sh save target/snapshot-before
./scripts/snapshot.sh compare target/snapshot-before

# Run the CLI
cargo run --bin audiobatch-rs -- [args]

# Run the GUI
cargo run --bin AudioBatchRs

# Compare the Rust and C++ CLI output on a folder, or their normalized output of copies of it
./scripts/parity.sh <path>
./scripts/parity.sh --normalize [--peak <dBFS> | --lufs <LUFS>] ../fixtures/normalization

# Compare release CLI wall times, with isolated inputs and configuration, requires hyperfine and uv
./scripts/cli_benchmark.sh
./scripts/cli_benchmark.sh --corpus ../fixtures/normalization --jobs 2 --runs 3 --warmup 1

# Save an image of the GUI window and quit, for checking the layout without screen recording permission
cargo build --release -p audiobatch-gui --features snapshot
AUDIOBATCH_GUI_SNAPSHOT=/tmp/gui.png ./target/release/AudioBatchRs --autoplay ../fixtures
```

`scripts/snapshot.sh` checks exact CLI output stability, not isolated integration behavior.
It uses normal application storage unless `AUDIOBATCH_DATA_ROOT` is supplied,
and moves replaced normalization copies to the system trash.
It does not use `--delete`, so an isolated data root does not eliminate trash side effects.

The CLI benchmark defaults to `--corpus auto`, combining `../fixtures/normalization` with `../fixtures/benchmark/output`
when the generated audio is present.
An explicit `--corpus <directory>` uses only that directory.
Measured runs alternate C++ and Rust, with warmup once per command,
and fresh copies and correctness checks outside timing.
Raw hyperfine exports, combined run statistics, Markdown, and machine/build metadata stay in `target/cli-benchmark/`.
[`../docs/performance.md`](../docs/performance.md) records the measured comparisons and how to reproduce them,
including the VST3 and playback callback comparisons.

The `snapshot` feature also reads `AUDIOBATCH_GUI_SNAPSHOT_DELAY` in seconds,
`AUDIOBATCH_GUI_SNAPSHOT_SIZE` as a window size such as `1000x800`,
and `AUDIOBATCH_GUI_SNAPSHOT_ACTIONS` with action names such as `audiobatch::ToggleLoudnessGraph`
that are dispatched before the image is taken.
The GUI shares its palette, fonts, and icons with the C++ application,
so both versions look alike, and `../cpp/src/gui/` is worth comparing when changing the layout.

Dependencies are compiled with optimizations even in debug builds, because decoding is too slow otherwise.
The root `.cargo/config.toml` compiles for the native CPU, and CI clears that tuning with an empty `RUSTFLAGS`.
The production meter still requires AVX2 on x86 or NEON on Apple Silicon.
Clearing native tuning does not provide an older-CPU compatibility mode.
The shell scripts in `rust/` source `common.sh`, which names the CLI and GUI binaries and the macOS app bundle,
and sources `../common.sh` for the platform detection and print helpers shared with `../cpp/`.
Keep its binary names in step with the `[[bin]]` names in the crate manifests.

## Code Organization

Order the items of a source file like this:

1. Constants and statics.
2. Enums, public before private.
3. Structs, public before private.
4. Implementations, in the order the types are defined.
   A type's trait implementations directly follow its inherent `impl` block.
5. Functions, public before private.
6. Root-level test modules.

Within an `impl` block:

- Constructors come first.
  `new` comes first when there is one,
  then the other associated functions that return `Self`, `Result<Self>`, or `Arc<Self>`, such as `from_*` and `open_*`.
- After the constructors, public methods come before private ones.
  Within each visibility, methods that take `self` come before associated functions.

Module boundaries:

- About 1000 lines of non-test code is the signal to look for separate concerns in a file, not a hard limit.
  A long test section or one tightly coupled implementation does not need a split.
  Give each new file its own `//!` documentation naming its concern.
- Put a helper that is not specific to one front end in `crates/core`, never in the CLI or the GUI crate.
- Keep an item's visibility as narrow as possible.
  In the library crates, prefer `pub(crate)` or `pub(super)` over `pub`.
  The binary crates use `pub` for items shared between their modules,
  because the nursery lint `redundant_pub_crate` rejects `pub(crate)` there.
- All `use` imports go at the top of the file, never inside functions.
  Imports used only on some platforms go there too, behind the same `#[cfg]` as the code using them.

## Code Style and Conventions

- Rust 2024 edition, with clippy pedantic and nursery lints denied.
- Group decimal integer parts with underscores only when they have five or more digits.
  Write `4000` and `4000.0`, but `40_000` and `40_000.0`, grouping larger integer parts in threes from the right.
  Fractional groups remain allowed independently, such as `1681.974_450_955_533`.
  Preserve radix literals, fractional precision, identifiers, and serialized or user-facing formats.
- Prefer named constants over unexplained numeric literals when values represent domain concepts, protocol fields,
  policy limits, or reused values. Keep self-evident arithmetic and conventional values inline.
- Do not use plain `unwrap`. Use proper error handling, or `.expect()` in constants and tests.
- Use domain-specific errors derived with `thiserror` in library and internal crates.
  Preserve concrete source errors and structured context, and match variants instead of parsing messages.
  Keep appropriate standard error types, such as `std::io::Error`, when a custom type adds no useful distinction.
- Use `anyhow` with `Result<T>` in the CLI and GUI for application context and reporting.
  Convert errors to strings only at presentation, persistence, or protocol boundaries.
  Keep existing user-facing messages and use `tracing` for logging.
  Audio callback errors must remain allocation-free.
- Use `clap` with derive macros. Every CLI argument has both a short and a long option,
  except where the C++ CLI has only a long one, because both CLIs take the same arguments.
- Use the `colored` crate for terminal colors.
- After changing CLI arguments, update the usage in `README.md` from the short help,
  `cargo run --bin audiobatch-rs -- -h`.
- Use descriptive names. No single character variables, except `x` and `y` for plot and screen coordinates.
- Prefer full names over abbreviations, for example `directories` instead of `dirs`.
  Established domain terms such as `lufs`, `db`, and `id` are fine, and so are the common `min` and `max`,
  and so are the names the frameworks use throughout, such as gpui's `cx` and `window` and clap's `args`.
- Give every struct, enum, and function a docstring that explains what it does and why,
  including threading constraints and non-obvious behaviour.
  Methods implementing a trait need none, since the trait documents them.
- Silence a lint with `#[expect(lint, reason = "...")]` rather than `#[allow]`,
  so the attribute goes away when it is no longer needed.
- Every Rust source file starts with module-level `//!` documentation describing its purpose.
- Avoid trailing comments. Put comments on their own line above the code.
- Compare floating point values for equality with a tolerance, not with `==`.
- The audio callback in `crates/playback` must not allocate, lock, or block,
  and neither may the `PreviewInsert` it runs, such as `PreviewChain`.
  Anything the callback replaces goes back to the UI thread to be dropped there.

### Return values

These guidelines also apply to test helpers, benchmarks, examples, and fuzz targets.

- Prefer a small domain-specific struct with named fields when returning several values with distinct meanings.
  Do not use raw tuples as result records, especially tuples with three or more elements.
  For example, return `ParameterRange { minimum, maximum, step }` instead of `(f64, f64, f64)`.
- Use named records for ambiguous two-value results too, such as a value together with a warning or status.
  A tuple type alias does not provide named fields and is not a substitute for a struct.
- Keep tuples where their positional meaning is conventional and clear, such as coordinates, homogeneous bounds,
  key-value iterator items, or channel endpoints, and where an external trait requires a tuple return type.
  Preserve external interfaces and serialized formats when improving internal return types.

### Comments and documentation formatting

Use semantic line breaks for comments, docstrings, and documentation.
Start a new line at sentence boundaries or natural clause boundaries.
Target about 120 characters per line, but prefer a clean sentence break over a strict width.
Avoid dashes and semicolons in prose. Use commas and periods to separate ideas.
Quote literal user input values, for example "y" and "n".
Do not wrap prose by splitting a sentence at an arbitrary point in the middle.
Run `slb` in `rust/` to check the prose, and `slb --fix` to reformat it.

Comments, docstrings, and documentation describe the current behaviour and why it is so.
They contain no development history and no notes about porting,
such as "ported from `PluginProcessing.cpp`", "follows the C++ panel", or "like the C++ application".
Mention the C++ application only when it matters for the implementation:

- A requirement shared with it, stated with its reason.
  Examples are the CLI options, messages, and exit codes that `scripts/parity.sh` compares,
  the normalized output that `scripts/compare_outputs.py` compares, the shared fixtures and assets,
  and the VST SDK version kept in step with JUCE's.
- A deliberate difference, with its reason.

## Testing

- Keep unit tests lightweight and focused, targeting under one second per case.
  Put executable CLI contracts and driven production-view workflows in the separately identified integration tier.
  Integration tests may take longer, but must have bounded waits and process timeouts.
  Use isolated application storage and copied fixtures,
  with no system trash, physical audio devices, or installed plugins.
  Do not add screenshot comparisons, pixel assertions, or desktop automation outside the test process.
  GPUI integration tests use the production view and real event handlers, not a parallel test-only model.
  They prove application composition and event delivery, not packaged-app or native OS behavior.
- Drive GUI workflows through gpui-kit's `TestWindowExt`, `TestEventExt`, and `TestQueryExt`,
  with clicks, keys, drags, wheel events, and platform file-drop events on the real window.
  Find shared controls by the ids in `crates/gui/src/control_ids.rs`,
  which match `../tests/gui_controls.json` and the C++ component ids.
  Use those constants in production code and tests, never repeated string literals,
  and keep the shared accessible labels there too.
  The GPUI test platform renders no real window, so these tests cannot be watched on screen.
  The C++ tier's `../cpp/test.sh --visible` shows the same shared workflows step by step.
  Register a custom element for queries with `.test_support()` under `cfg(test)`,
  and give shared controls the accessible label and role the manifest lists.
  Dispatch root actions directly only for menu bar commands, which have no element in the window.
  Write each shared workflow with the same name and steps as its C++ counterpart,
  and document a remaining difference between the applications beside both tests.
- Run the driven production-view tier with `cargo nextest run -p audiobatch-gui -E 'test(integration_tests::)'`.
  The default workspace run includes these cases too.
  Build `audiobatch-rs`
  and run the executable contracts with `uv run --script ../tests/cli_integration.py --cli target/debug/audiobatch-rs`.
  Both tiers preserve production workers and use isolated storage.
  Under `cfg(test)`, the normal GUI constructor owns unique temporary storage,
  disables playback, and deletes source copies.
  Test-only helpers and storage ownership remain under `cfg(test)`, without changing process-wide environment variables.
  The CLI subprocess suite supplies its storage override automatically because it runs the ordinary production executable.
- Put public-library integration contracts in each package's `tests/` directory, such as `crates/core/tests/storage.rs`.
  Cargo does not automatically discover `tests/` beneath this virtual workspace root.
  Integration targets link the ordinary library, so its `cfg(test)` helpers are unavailable there.
  Keep private-state GUI workflows in their `cfg(test)` module instead of exposing application internals for tests.
- Test the core and the CLI against the shared fixtures through `test-fixtures`.
  Tests that modify files work on copies in a temporary directory.
  Real JUCE plugin tests skip locally without the bundle and fail when it is missing under `CI`.
  Normalization golden corpus tests skip absent or unmaterialized Git LFS audio locally,
  unless `AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1` requires the complete corpus.
  Cross-language shared-cache checks need `AUDIOBATCH_CPP_TESTS` to name the built C++ test executable,
  and otherwise skip that coverage.
- **NEVER use nested modules inside test modules.**
  All test modules are separate root-level `#[cfg(test)]` modules, never wrapped in a single parent `mod tests`.
- Group related tests into modules of about three to five tests or more, named by area, such as `test_peaks`.
  Avoid one-test modules by merging them into a related module,
  but a file with only a few tests can keep them in a single module.
  Split a module that grows past a few hundred lines by area.
- `miri.sh` runs the tests of `vst3-host`, `audiobatch-test-plugin`, `serato-tags`,
  the callback admission, lifecycle, and transport tests of `audiobatch-playback`,
  and the waveform, overload, and spectrogram blob readers of the core under Miri with Stacked Borrows.
  In those crates, mark a test that needs the file system, a plugin bundle, an audio device, or SQLite,
  or that takes minutes under Miri, with `#[cfg_attr(miri, ignore = "reason")]`.
  Fix undefined behaviour that Miri finds instead of ignoring the test.
- `rtsan.sh` builds and runs the RealtimeSanitizer contracts on macOS with an installed nightly Rust
  and the matching Homebrew LLVM runtime, each scenario in its own process.
  `--skip-native` requires clean first-party callbacks, including admission and guard destruction,
  without the native VST3 cases.
  `--diagnostic` also verifies the known native plugin and stream-error disposal blockers as `BLOCKED` audits,
  which are not clean results.
  The build and diagnostics stay in `target/rtsan/`, and `--artifacts <directory>` selects where reports go.
  Run it after changing callback, renderer, or preview insert code.
  A missing or mismatched runtime blocks the check, and the check proves no audio deadlines.
- The callback benchmark is an ignored libtest case, `callback_benchmark::production_callback`.
  `../tests/callback_benchmark.py` compares paired baseline and candidate runs,
  following [`../docs/performance.md`](../docs/performance.md).
- The cargo-fuzz targets in `fuzz/` cover the Serato tag parsers, the CLI arguments, the cache blobs, and decoding.
  `serato_tags` and `cli_arguments` share their input layout with the C++ harnesses in `../cpp/tests/fuzz/`,
  and `fuzz.sh` replays the C++ corpus in place, so keep the layouts in step.
  Add each input that found a bug to `fuzz/corpus/<target>/`.
  Keep overflow checking enabled, and do not suppress unexpected panics.
  Only the committed Symphonia WAV overflow input, described in the README, may panic,
  and `cargo test --manifest-path fuzz/Cargo.toml --test decoder_regressions` checks that allowance.
  The targets use only public library interfaces, so keep any item made public for them as narrow as possible.
  Run `cargo clippy --manifest-path fuzz/Cargo.toml --all-targets` for them,
  since the workspace clippy run does not include the excluded crate, and `format.sh` formats it.
- Put test helpers used by several modules in a `#[cfg(test)]` module declared from the crate root,
  such as `crates/core/src/test_helpers.rs`.
  Helpers that only locate fixtures or list their properties belong in `test-fixtures`,
  which cannot call into the core crate.
