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
  The Rust version has the same commands in its File and Plugins menus.

### Features only in the Rust version

- File > Add Files… adds individual files to the list.
- The CLI also installs completions for elvish.

### Audio formats

- The C++ version reads WAV, AIFF, FLAC, Ogg Vorbis, and MP3 with JUCE on every platform,
  and on macOS also what Core Audio reads, such as AAC, Apple Lossless, and CAF.
- The Rust version reads with symphonia on every platform,
  so it also reads AAC files on Windows.
  It reads WAV and BWF, AIFF and AIFC, FLAC, Ogg Vorbis, MP3, AAC and Apple Lossless in M4A, and CAF.
- Both write the normalized and processed files as AIFF.
  For lossless sources both write the same samples,
  and for lossy sources the two decoders can differ by a step or two of the output.

### Runs

- Both versions normalize or process every analyzed file from the menus after a confirmation.
- Both versions cancel an analysis, normalization, or processing run
  from the Cancel button that replaces Process during a run, or from File > Cancel Run (Command or Control and period).
  Files a cancelled run has not committed keep their rows, and files it already committed show their new output.
- Clear All Files stops any run in both versions.

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
  both versions process it at unity gain, with a notice that names the reason.
- **Processing failures.** Both versions fail the file when a VST3 plugin reports a processing error,
  and turn the preview off with the plugin's name and error.
- **Preview channels.** Both previews run the plugin chain in stereo for every file,
  and prepare a plugin that refuses stereo for mono, folding the signal into it and copying its output to both channels.
  The output of a mono file is folded back to mono, so it sounds like the processed file.
  Offline processing refuses a stereo file through a mono-only plugin in both versions.
- **Custom gain in the preview.** Both previews apply a file's custom gain only in front of the plugin chain,
  so a preview without plugins plays the file unchanged, and both draw the waveform with it.

## Future work

- [ ] Show the embedded album art of the selected file in the info panel, in both the C++ and the Rust application.
