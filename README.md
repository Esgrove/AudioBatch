# AudioBatch

Tools for analyzing and batch processing audio files.
They measure sample peak, true peak, and integrated loudness (EBU R128),
cache the results in SQLite, and list them in a GUI and a CLI.
The GUIs provide waveform previews, embedded artwork, and playback through live plugin chains.
The project targets macOS and Windows.

The repository holds two versions of the application side by side:

- [`cpp/`](cpp/README.md): the C++23 and JUCE application, the `AudioBatch` GUI and the `audiobatch` CLI.
  It is the reference implementation and the full-featured version,
  with normalization, AIFF conversion, and VST3 / Audio Unit plugin processing.
- [`rust/`](rust/README.md): the Rust port, the `AudioBatchRs` GUI built on gpui-kit and the `audiobatch-rs` CLI.
  It covers analysis, the results table, the waveform overview, playback, normalization,
  and VST3 plugin processing on macOS and Windows.

Both versions run side by side.
They share the `AudioBatch/analysis.db` analysis database,
while settings, logs, plugin lists, chains, and presets stay separate.
Their CLIs print the same table, CSV, and JSON output.

## Layout

```text
AudioBatch/
├── assets/      icons and fonts for both applications
├── fixtures/    committed audio fixtures shared by both test suites, and generate.sh
│   ├── normalization/    a Git LFS collection for comparing the normalized output of both CLIs
│   └── plugin_expected/  what the test plugin must output, computed by plugin_expected.py
├── cpp/         the C++ project with its CMake build and the JUCE submodule
├── rust/        the Rust Cargo workspace
└── test-plugin/ a JUCE VST3 with gain, mid/side width, and latency that both test suites load
```

The fixtures and the test plugin are described in [`fixtures/README.md`](fixtures/README.md)
and [`test-plugin/README.md`](test-plugin/README.md).

Shared measurements, profiling commands, and plugin host comparisons are in [Performance](docs/performance.md).

Build and test each version from its own directory, as described in its README.
Each version explains how its parts work together in its `ARCHITECTURE.md`,
[`cpp/ARCHITECTURE.md`](cpp/ARCHITECTURE.md) and [`rust/ARCHITECTURE.md`](rust/ARCHITECTURE.md).

## Scripts

The scripts in the repository root run the scripts of the same name in both projects:

```shell
# Build the release binaries of both versions
./build.sh

# Build the debug binaries of both versions
./build.sh --debug

# Install both CLIs, GUI apps, and shell completions
./install.sh

# Format both projects, the root shell scripts, and the Python scripts, add --check to only verify them
./format.sh
```

Name a project first to run only its script, such as `./build.sh rust` or `./install.sh cpp --app-only`.
The options are passed on to the project scripts,
so running both projects accepts only the options both scripts have, such as `--cli-only` for `install.sh`.
`./build.sh cpp --help` shows the options of the C++ build script.
Both build and install scripts accept `-c` or `--clean`.
For C++, this deletes the selected CMake build directory and its local compilation cache before rebuilding.
The Rust scripts accept the option without cleaning Cargo artifacts.
Use `-l` or `--cli-only` to build or install only the CLIs.
The build scripts accept `-d` or `--debug` to select debug builds, and keep release as the default.
The C++ script also retains `-b` or `--build-type` for selecting any build configuration.
For example, `./install.sh --clean` rebuilds C++ from scratch and installs both versions.

## Differences between the versions

Both CLIs take the same analysis and normalization options and print the same output.
Lossless normalization is sample-exact in peak mode and differs by at most one PCM step in loudness mode,
which `rust/scripts/parity.sh` checks.
The main differences are format support, plugin hosting, and cancellation.

### Audio formats

- The C++ version reads WAV, AIFF, FLAC, Ogg Vorbis, Opus, and MP3 with JUCE on every platform,
  and on macOS also what Core Audio reads, such as AAC, Apple Lossless, and CAF.
- The Rust version reads WAV and BWF, AIFF and AIFC, FLAC, Ogg Vorbis, MP3,
  AAC and Apple Lossless in M4A, and CAF with symphonia on every platform, including AAC on Windows.
- Both write the normalized and processed files as AIFF.
  Lossy sources can produce different samples because the decoders differ.

### Plugin hosting

- Both versions host VST3 effects on macOS and Windows. C++ also hosts Audio Units on macOS
  and provides a generic parameter editor for plugins without their own editor.
- Both support saved chains, presets, live preview, and offline processing with latency compensation and effect tails.
  Their plugin lists, chains, and presets are stored separately and cannot be shared.
- Plugin scanning runs in a separate process to isolate crashes.
  Processing, preview, and plugin editors run inside the application, so a plugin crash can end the app.
- Rust detects and reports plugins that refuse saved settings. JUCE does not expose that failure to C++.

### Cancellation and file handling

Both GUIs cancel analysis, normalization, and plugin processing without replacing uncommitted originals.
Completed files keep their new output.
The Rust CLI also handles Ctrl-C during normalization, cleans up temporary output,
reports completed files, and exits with code 130.
A second Ctrl-C ends it immediately.

Both versions write output next to the source and move replaced originals to the system trash by default.
CLI normalization can permanently delete them with `--delete`.
Existing output files are protected unless overwrite is enabled.

## Future work

### Phase 4: Out-of-process plugin processing and preview

Only plugin scanning is isolated today.
In both applications, a plugin crash during batch processing, live preview, or editing can end the app.
A plugin hang can block processing, playback, or the interface without necessarily terminating the app.
This phase would move plugin execution and editors out of the main process.
The isolation described here is future work, not an implemented feature.

#### Batch processing

Each file would render in a worker process relaunched from the same executable, as the scan worker does today.
The app would send the source path and a snapshot of the chain's order, enabled flags, and saved plugin state.
The worker would load the chain and render only the temporary audio output.
The app would retain ownership of source-identity checks, output validation and publication,
metadata preservation, original-file disposal, and updates to the shared analysis database and its preview caches.
Workers would not publish output, replace or delete originals, or write the cache independently.

A worker crash or timeout would fail only the affected file, identify the plugin involved,
and leave the original intact without publishing partial output.
Temporary output would be cleaned up, completed outputs would remain committed,
and the app would remain responsive and continue with the other files.
Cancellation and overwrite protection would preserve the existing output-publication safety rules.
Successful renders would preserve the same metadata and Serato handling as in-process processing.

#### Preview and editors

The live chain would run in a worker process fed through shared-memory audio ring buffers.
Latency compensation, pre-roll, effect-tail draining, fades, and channel handling would retain their current behavior.
The audio callback would remain allocation-free, lock-free, and nonblocking.
It would never wait for worker IPC, process supervision, state synchronization, or a plugin editor.
The chain window would report the additional round-trip latency of the worker connection.

If the worker crashed or stopped responding, the app would disable plugin preview and show a message
while dry playback continued, rather than stopping playback or ending the app.
Plugin editor windows would live in the worker process too, with the plugin's required event loop and lifecycle thread.
Parameter changes and saved plugin state would synchronize back to the app,
so chain settings, presets, subsequent previews, and offline renders used the current edits.
Editor failure or worker recovery would not silently discard the app's last synchronized chain state.

#### Shared behavior and tests

Both versions would implement the same observable failure, recovery, output-publication, and preview behavior.
Shared tests would use deliberately crashing and hanging fake plugins, not installed third-party plugins.
They would verify per-file crash and timeout handling, intact originals, removal of partial temporary outputs,
continued batch work, metadata preservation, and main-process cache ownership.
Preview tests would cover dry-playback continuity, latency reporting, pre-roll, tails, fades,
callback constraints, and editor parameter and saved-state synchronization.
