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
└── test-plugin/ a JUCE VST3 with gain, mid/side width, and latency that both test suites load
```

The fixtures and the test plugin are described in [`fixtures/README.md`](fixtures/README.md)
and [`test-plugin/README.md`](test-plugin/README.md).

Build and test each version from its own directory, as described in its README.
Each version explains how its parts work together in its `ARCHITECTURE.md`,
[`cpp/ARCHITECTURE.md`](cpp/ARCHITECTURE.md) and [`rust/ARCHITECTURE.md`](rust/ARCHITECTURE.md).

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
Both build and install scripts accept `-c` or `--clean` to rebuild from scratch.
This deletes the selected C++ CMake build directory and its local compilation cache before rebuilding.
The Rust scripts accept the option without cleaning Cargo artifacts.
Use `-l` or `--cli-only` to build or install only the CLIs.
For example, `./install.sh --clean` rebuilds C++ from scratch and installs both versions.

## Differences between the versions

Both CLIs take the same options, print the same output, and write the same normalized samples for lossless sources,
which `rust/scripts/parity.sh` checks.
The main differences are format support, plugin hosting, and cancellation.

### Audio formats

- The C++ version reads WAV, AIFF, FLAC, Ogg Vorbis, and MP3 with JUCE on every platform,
  and on macOS also what Core Audio reads, such as AAC, Apple Lossless, and CAF.
- The Rust version reads WAV and BWF, AIFF and AIFC, FLAC, Ogg Vorbis, MP3,
  AAC and Apple Lossless in M4A, and CAF with symphonia on every platform, including AAC on Windows.
- Both write the normalized and processed files as AIFF.
  For lossless sources both write the same samples,
  and for lossy sources the two decoders can differ by a step or two of the output.

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

- [ ] Show the embedded album art of the selected file in the info panel, in both the C++ and the Rust application.
