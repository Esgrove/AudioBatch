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

## Differences between the versions

Both CLIs take the same options, print the same output, and write the same normalized samples for lossless sources,
which `rust/scripts/parity.sh` checks.
The differences below are in the GUIs and in plugin hosting.

### Features only in the C++ version

- Audio Unit plugins on macOS. The Rust version hosts VST3 plugins only, on macOS and Windows.
  Its `audiobatch list` also lists instruments, which the Rust CLI leaves out.
- A context menu in the results table for normalizing, processing, re-analyzing, revealing, and removing files.
  The Rust version has the same commands in its File and Plugins menus,
  except Open Parent Folder and Normalization Format Support.
- Help > Normalization Format Support… lists the formats this platform can normalize.
- A generic parameter editor for plugins without an editor of their own.
- Dragging rows out of the results table into other applications.
- Choosing the audio driver and the active output channels in Audio Settings.
  The Rust version chooses the output device, its sample rate, and the buffer size.
- `--headless` starts the GUI app without showing its window.
- The plugin manager is the stock JUCE plugin list, while the Rust version has its own plugin manager window.
- The analysis cache keeps every Serato tag the preview reads,
  while the Rust cache keeps only the Serato Overview and reads the cue points from the file again.

### Features only in the Rust version

- File > Add Files… adds individual files to the list.
- The CLI also installs completions for elvish.
- The CLI handles Ctrl-C: the file being normalized keeps its original and leaves no temporary file,
  the files already committed are still reported, and the CLI exits with code 130.
  A second Ctrl-C ends it at once.
- Shortcuts for opening a folder (Command or Control and O), adding one (the same with Shift),
  stopping playback (Escape), and Audio Settings (Command or Control and comma).

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
  The C++ version keeps the scanned folders, so Rescan brings the rows back,
  and Rescan also brings back rows that were removed.
  The Rust version forgets the folders with Clear All Files and keeps removed files out of later rescans.
- Dropping files and folders together analyzes only the files in the C++ version,
  and makes all of them the new roots in the Rust version.

### Menus and controls

- The C++ version puts the plugin items in the Options menu, with Scan for Plugins….
  The Rust version has a Plugins menu with Plugin Manager… and Process Selected With Plugin Chain.
- Only the Rust File menu has Normalize Selected, Clear Per-file Gain, and Select All Files,
  and only its Options menu has Output Device and Columns.
  Only the C++ Options menu has Continue Playback When Changing Files and Play from First Cue Point,
  which the Rust version has in Audio Settings.
- The C++ version disables the menu items that cannot run, such as Cancel Run when idle or Reveal without a selection.
  The Rust version keeps them enabled.
- The Plugin chain button opens a menu in the C++ version and the chain window in the Rust version.
- The Process button needs an enabled plugin and any file in the C++ version,
  and a selected, analyzed file in the Rust version.
- Preview errors show in a dialog in the C++ version and in the status bar in the Rust version.

### Plugin hosting

- **Data files.** The Rust version keeps its plugin list and chain in its own `AudioBatch-rs` folder.
  The chain state that JUCE saves is specific to JUCE, and its plugin ids are hashes, so the two cannot share them.
- **Scan lock.** When a CLI rescan holds the scan lock, both GUIs skip their scan and say so,
  the C++ version in a dialog and the Rust version in the status bar.
- **Missing plugins.** Both CLIs and both GUIs remove the plugins and blacklist entries whose files are gone
  after every scan that runs to its end,
  but only inside scanned folders that exist, so plugins on an unmounted drive stay listed.
  A cancelled scan removes nothing.
- **`VST3_PATH`** is split with the platform path separator in both versions, which keeps Windows drive letters intact.
- **Refused pre-normalization gain.** When Normalize is on and a file cannot reach the target,
  because it is silent or would need more than 40 dB of gain,
  both versions process it at unity gain, with a notice that names the reason.
- **Processing failures.** Both versions fail the file when a VST3 plugin reports a processing error.
  In the preview, both stop playback and name the plugin and its error.
  The Rust version also turns plugin preview off, while the C++ version keeps it on for the next play.
- **Refused plugin settings.** Only the Rust version can tell when a VST3 plugin refuses its saved settings,
  since JUCE does not report it.
  Its live chain then keeps the saved settings instead of the plugin's defaults until the editor opens,
  and a processed file gets a notice that names the plugin.
- **Preview channels.** Both previews run the plugin chain in stereo for every file,
  and prepare a plugin that refuses stereo for mono, folding the signal into it and copying its output to both channels.
  The output of a mono file is folded back to mono, so it sounds like the processed file.
  Offline processing refuses a stereo file through a mono-only plugin in both versions.
- **Custom gain in the preview.** Both previews apply a file's custom gain only in front of the plugin chain,
  and both draw the waveform with it.
  With plugin preview off, both play the file unchanged.
  With it on, the C++ version applies the gain even when the chain is empty or every plugin is bypassed,
  while the Rust version plays the file unchanged until a plugin is enabled.

## Future work

- [ ] Show the embedded album art of the selected file in the info panel, in both the C++ and the Rust application.
