# AudioBatch

GUI and CLI tool (C++23 and JUCE) for analyzing and batch processing audio files.

- Measure sample peak, true peak and integrated loudness (EBU R128).
- Compare files in a sortable results table with live processing status.
- Normalize to a target peak or loudness.
- Batch process through VST3 or Audio Unit effect chains in the GUI.
- Write AIFF output while preserving tags and embedded artwork.
- Cache analysis so unchanged files do not need to be read again.

Supports mono and stereo audio, including WAV, AIFF, FLAC, MP3 and Ogg Vorbis.
Multichannel audio and Linux are not currently supported.

## GUI

Choose a folder or drop folders onto the window to start background analysis.
Use `File > Add Folder...` or hold `Shift` while dropping to add folders without clearing the results.
`Rescan` or `F5` refreshes all selected folders.
Quit with `Ctrl+Q` on Windows or `Cmd+Q` on macOS.

Sort results by level, loudness or file details, and show or hide columns from the table header menu.
The results context menu lets you normalize, process, re-analyze, reveal or remove selected files.
Removing a row does not delete the file. Moving a file to the trash is a separate action.

### Waveform Preview

View and play tracks with a zoomable waveform.
The resizable preview workspace has separate columns for controls, selected-file information, and the waveform.
Sample peak, true peak, integrated loudness, and maximum short-term loudness appear in dedicated metric cards.

Serato DJ metadata adds optional waveform colours and coloured cue markers,
and the info panel shows BPM and musical key when available.
Files without Serato tag data show a default blue waveform.

Use the small outlined arrow in the controls' top-left corner, or the collapse/expand command in the Options menu.
The shortcut is `Ctrl+Shift+Space` on Windows and `Cmd+Shift+Space` on macOS.
The compact row keeps Play/Stop, Process, all four metrics, and a smaller waveform visible.
Other controls are hidden but retain their settings.

### Normalization

Select files, choose a mode and target, then normalize from the results context menu.

- **Peak:** -12 to 0 dBFS, default 0 dBFS.
- **Loudness:** -24 to -6 LUFS, default -14 LUFS, with a -0.3 dBTP ceiling to avoid clipping.
  Files may remain below the target when the ceiling limits the gain.

### Plugin Processing

Scan for installed plugins from the plugin menu, then build and configure an effect chain.
Plugins can be reordered, enabled or disabled, and the chain is saved between sessions.
Scanning isolates plugin crashes so they do not take down the app.

Processing uses offline rendering, compensates plugin latency and preserves reverb and delay tails.
Output is reduced in level when needed to avoid clipping.
You can normalize before the chain or set a custom input gain for each file.

### Output And Originals

Normalization and plugin processing always write `<name>.aif` next to the source,
keeping its sample rate, tags and artwork.

**These operations replace the original audio.**
Other formats are converted to AIFF and the source is moved to the system trash after a successful write.
Existing `.aif` sources are rewritten in place, with the original moved to the trash first.

- An already existing output file is protected unless `Force overwrite` (GUI) or `--force` (CLI) is enabled.
  Replaced files go to the trash.
- If selected files share an output name, only one is processed. An existing `.aif` source takes priority.
- If metadata cannot be preserved, the file fails and the original is kept.
- On Windows, operations that require trashing a file fail on drives without a recycle bin, such as network shares.

## CLI

Output of `audiobatch --help`:

```text
audiobatch <version> <date> <commit>
Usage: audiobatch [options] <paths...>

Options:
  -h, --help              Print usage and exit
  -V, --version           Print version and exit
  -r, --recurse           Recurse into directories
  -f, --refresh           Ignore cached analysis and re-analyze files
  -n, --normalize         Normalize analyzed files, then report the re-analyzed output peaks
  -F, --force             Let --normalize replace an existing output file
      --peak <dBFS>       Normalize to this sample peak, -12 to 0 (default 0)
      --lufs <LUFS>       Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling
  -j, --jobs <count>      Override worker count
  -s, --sort <mode>       Sort by peak, lufs, name, or path
      --reverse           Sort in descending order
  -o, --output <format>   Print results as table (default), csv, or json
  -l, --list-plugins      List the available audio plugins and exit
  -g, --group-by <mode>   Group the plugin list by name (alphabetical), manufacturer, type, or format
  -R, --rescan            Scan for new or changed plugins before listing them

Short options cannot be combined, so pass -r -f instead of -rf.
```

Without a path, AudioBatch analyzes the current directory.

Analyze a file or scan a library recursively:

```shell
audiobatch "music/song.wav"
audiobatch -r --sort peak "music/library"
```

Normalize to a sample peak or integrated loudness:

```shell
audiobatch -n --peak -1 "music/song.wav"
audiobatch -r -n --lufs -14 "music/album"
```

Normalization follows the same targets and output rules as the GUI.
`-n` without a target defaults to 0 dBFS sample peak.
Results report the levels and paths of the normalized output.

Export analysis as JSON or CSV for scripts and spreadsheets:

```shell
audiobatch -r -o json "music/library" > levels.json
audiobatch -r -s lufs --reverse -o csv "music/library" > levels.csv
```

Default output is a table of peak, true peak, loudness and track name.
Log messages go to stderr, keeping stdout available for results.
Use `--refresh` to bypass cached analysis, including previously failed files.
Pass short options separately, such as `-r -f`, not `-rf`.

List plugins known to the GUI, or rescan and group them by type:

```shell
audiobatch --list-plugins
audiobatch -l -R --group-by type
```

Plugin listing supports grouping by `name`, `manufacturer`, `type` or `format`.
Plugin processing itself is GUI-only. CLAP plugins are not supported.

Exit codes: `0` for success, `1` for invalid arguments or output errors,
and `2` when any file fails analysis or normalization.

## Build

Built with C++23 and JUCE. Requires CMake 4.2 or newer and a supported platform toolchain.
JUCE is included as a submodule.
CMake fetches the other dependencies as needed: SQLite 3, libebur128, TagLib, {fmt} and GoogleTest (tests only).

Run these commands in a terminal on macOS or Git Bash on Windows.

macOS:

```shell
cmake --preset macos-debug
cmake --build --preset macos-debug
```

Windows (Git Bash, with the Visual Studio toolchain):

```shell
cmake --preset windows-debug
cmake --build --preset windows-debug
```

For Ninja with MSVC and a compilation database, use `windows-ninja-debug` instead.

The build produces `AudioBatch.app` / `AudioBatchApp.exe` (GUI) and `audiobatch` (CLI).

For release builds, use the build script:

```shell
./build.sh
```

On macOS, pass `--xcode` to generate and build with Xcode instead of Ninja.
To generate a project and open it in the IDE without building, use `--open`:

```shell
./build.sh --open -b Debug
```

This selects Xcode on macOS and Visual Studio on Windows.
Add `--reuse` to open an existing project without regenerating it.
`--open` cannot be combined with `--ninja`.
With `--open --test`, tests are included in the project but are not built or run.

To install the release build, run:

```shell
./install.sh
```

On Windows, run this from Git Bash.
The GUI app is installed under `%LOCALAPPDATA%\Programs\AudioBatch`,
and a Start Menu shortcut is created for Start search.
The CLI is installed to `~/.local/bin`.
Use `--app-only` or `--cli-only` to install just one of the two.
Pass `--reuse` to skip CMake generation and reuse the existing release build directory.

## Development

- `./format.sh` formats C++, CMake and shell scripts. `--check` verifies formatting without changes.
- `./lint.sh` runs clang-tidy. `--check` fails on warnings.

Build and run tests with the script (Release by default):

```shell
./build.sh --test
./build.sh --test -b Debug
```

## TODO

- User config file
- Save and load plugin chains
