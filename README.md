# AudioBatch

Tools for analyzing and batch processing audio files.
They measure sample peak, true peak, and integrated loudness (EBU R128),
cache the results in SQLite, and list them in a GUI and a CLI.

The repository holds two versions of the application side by side:

- [`cpp/`](cpp/README.md): the C++23 and JUCE application, the `AudioBatch` GUI and the `audiobatch` CLI.
  It is the reference implementation and the full-featured version,
  with normalization, AIFF conversion, and VST3 / Audio Unit plugin processing.
- [`rust/`](rust/README.md): the Rust port, the `AudioBatchRs` GUI built on gpui-kit and the `audiobatch-rs` CLI.
  It covers analysis, the results table, the waveform overview, playback, normalization,
  and VST3 plugin processing on macOS and Windows.

Both versions run side by side.
They keep separate analysis caches, and their CLIs print the same table, CSV, and JSON output.

## Layout

```text
AudioBatch/
├── assets/      icons and fonts for both applications
├── fixtures/    committed audio fixtures shared by both test suites, and generate.sh
│   ├── normalization/    a Git LFS collection for comparing the normalized output of both CLIs
│   └── plugin_expected/  what the test plugin must output, computed by plugin_expected.py
├── cpp/         the C++ project with its CMake build and the JUCE submodule
├── rust/        the Rust Cargo workspace
└── test-plugin/ a JUCE VST3 with gain and mid/side width that both test suites load
```

Build and test each version from its own directory, as described in its README.

## Scripts

The scripts in the repository root run the scripts of the same name in both projects:

```shell
# Build the release binaries of both versions
./build.sh

# Install both CLIs, GUI apps, and shell completions
./install.sh

# Format both projects, the root shell scripts, and the Python scripts, add --check to only verify them
./format.sh
```

Name a project first to run only its script, such as `./build.sh rust` or `./install.sh cpp --app-only`.
The options are passed on to the project scripts,
so running both projects accepts only the options both scripts have, such as `--cli-only` for `install.sh`.
`./build.sh cpp --help` shows the options of the C++ build script.

## Differences between the versions

Both CLIs take the same options, print the same output, and write the same normalized samples for lossless sources,
which `rust/scripts/parity.sh` checks.
The differences below are in the GUIs and in plugin hosting.

### Features only in the C++ version

- Audio Unit plugins on macOS. The Rust version hosts VST3 plugins only, on macOS and Windows.
- A context menu in the results table for normalizing, processing, re-analyzing, revealing, and removing files.
- A collapsible controls row, and the number keys 1 to 8 for jumping to Serato hot cues.
- Opening the audio device only while playing.
  The Rust version opens the device when the window opens and keeps it open.

### Features only in the Rust version

- Processing all analyzed files through the plugin chain from the Plugins menu, after a confirmation.
- Cancelling a plugin processing run, from the Cancel button that replaces Process or from the Plugins menu.
  A normalization run has no cancel action in either version.
  Clearing the list stops it in the Rust version, while the C++ version refuses to clear the list during a run.

### Plugin hosting

- **Data files.** The Rust version keeps its plugin list and chain in its own `AudioBatch-rs` folder.
  The chain state that JUCE saves is specific to JUCE, and its plugin ids are hashes, so the two cannot share them.
- **Scan lock.** When a CLI rescan holds the scan lock, both GUIs skip their scan and show one message.
- **Missing plugins.** Both CLIs and both GUIs remove the plugins and blacklist entries whose files are gone
  after every scan that runs to its end,
  but only inside scanned folders that exist, so plugins on an unmounted drive stay listed.
  A cancelled scan removes nothing.
- **`VST3_PATH`** is split with the platform path separator in both versions, which keeps Windows drive letters intact.
- **Refused pre-normalization gain.** When Normalize is on and a file cannot reach the target,
  because it is silent or would need more than 40 dB of gain,
  both versions process it at unity gain, but the Rust result has a notice that names the reason.
- **Processing failures.** Both versions fail the file when a VST3 plugin reports a processing error,
  and turn the preview off with the plugin's name and error.
- **Mono-only plugins in the preview.** The C++ preview prepares the chain for the loaded file's channel count,
  so a plugin that only supports mono turns the preview off for every stereo file.
  The Rust preview runs in stereo, and prepares a plugin that refuses stereo for mono,
  folding the signal into it and copying its output to both channels.
  Offline processing refuses a stereo file through a mono-only plugin in both versions.
- **Custom gain in the preview.** The C++ preview applies a file's custom gain only when plugin preview is on.
  The Rust preview applies it in both cases, and also draws the waveform with it.

## Known issues in the C++ version

- **A refused pre-normalization gain is silent.**
  `getInputGain` in `PluginProcessing.cpp` falls back to unity gain when `getNormalizationGain` refuses a file,
  and the result does not say so.
  Its documentation mentions files without a measurable level, but not the 40 dB gain limit.
- **Plugin processing cannot be cancelled from the GUI.**
  Only analysis has a cancel action.
  A processing run started on a large selection runs to the end unless the app quits.

## Future work

- [ ] Show the embedded album art of the selected file in the info panel, in both the C++ and the Rust application.
