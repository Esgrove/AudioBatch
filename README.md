# AudioBatch

GUI and CLI tools (C++23 and JUCE) for analyzing audio files and preparing batch work.

Current implementation focus:

- Analyze audio files and calculate decoded sample peak values.
- Calculate true peak and integrated loudness during analysis.
- Cache analysis results in SQLite so unchanged files are not re-read every run.
- Show sortable peak, true peak, and loudness data in the GUI.
- Show per-file processing state directly in the results list while background work is running.
- Normalize selected files to `0 dBFS` peak from the GUI or the CLI.
- Convert any normalized output to AIFF and carry over the source tags,
  including embedded album art and custom tags, by writing them back as ID3v2.4.
- Batch process selected files through a chain of VST3 or Audio Unit plugins from the GUI.
- Run the same analysis and normalization from a console executable.

Analysis scope currently assumes mono or stereo source files, including MP3 joint stereo.
Multichannel files are not a target for this tool.

## Dependencies

- [JUCE](https://juce.com/)
- CMake
- SQLite 3
- [libebur128](https://github.com/jiixyj/libebur128)
- [TagLib](https://taglib.org/) for cross-format metadata handling.
- [{fmt}](https://github.com/fmtlib/fmt) for string formatting.

`SQLite3` is resolved through CMake.
If a system `SQLite3` package is available it will be used,
otherwise CMake falls back to fetching the SQLite amalgamation.

`libebur128` is fetched through CMake for loudness and true peak analysis.

`TagLib` is fetched through CMake.
It is used to read every tag and embedded picture from the source file,
and to write the resulting metadata back as ID3v2.4 on the AIFF output.

`{fmt}` is fetched through CMake.
It powers all display and log text formatting through a `utils::format` helper
that works directly with JUCE string and file types.

## Build

For routine development checks,
use the CMake presets to reuse the same build directories as the editor or IDE.

Windows debug with the Visual Studio generator:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
```

Windows debug with Ninja and MSVC, which also produces `compile_commands.json`:

```powershell
cmake --preset windows-ninja-debug
cmake --build --preset windows-ninja-debug
```

macOS debug:

```shell
cmake --preset macos-debug
cmake --build --preset macos-debug
```

`build.sh` is intended for release builds and requires Bash (use Git Bash on Windows):

```shell
./build.sh -b Release
```

This produces two binaries:

- `AudioBatchApp.exe` / `AudioBatch.app` for the GUI app.
- `audiobatch` for CLI binary.

## Development

Formatting and linting run through scripts in the repository root:

- `./format.sh` formats C++ with clang-format, `CMakeLists.txt` with cmake-format, and shell scripts with shfmt.
  cmake-format runs through `uvx` when it is not installed.
  `./format.sh --check` only verifies formatting, which is what CI runs.
- `./lint.sh` runs clang-tidy on `src/` using the debug build's `compile_commands.json`.
  `./lint.sh --check` exits with code 1 on any warning.
- `ctest --test-dir cmake-build-mac-debug --output-on-failure` runs the unit and integration tests
  after a debug build.
  They use the audio fixtures in `tests/fixtures`, which `tests/fixtures/generate.sh` regenerates with ffmpeg.
- `.pre-commit-config.yaml` runs the basic file checks, clang-format, and cmake-format through [prek](https://prek.j178.dev).

The project sources compile with warnings as errors and a stricter warning set than the JUCE defaults.
CMake 4.2 or newer is required.
Linux is not supported yet.

## GUI

The GUI analyzes the selected root folders automatically in the background and stores results in a sortable table.

- `File > Choose Folder...` replaces the current root folders with the chosen folder.
- `File > Add Folder...` appends another root folder and analyzes it without clearing the current results.
  A folder that is already inside a root is skipped,
  and existing roots inside a newly added parent folder are merged into it.
- Dropping folders onto the window replaces the root folders.
  Hold `Shift` while dropping to append them instead.
- `Rescan` and `F5` re-analyze every root folder.
- The folder chooser starts in the last root folder,
  otherwise in a `Dropbox` folder when there is one, then the system Music folder.
- Folders are scanned in the background, so the window stays responsive with large libraries.
  Hidden files and macOS `._` metadata files are skipped.

Starting the GUI executable with `-V` or `--version` prints the version line and exits without opening a window,
the same as the CLI:

```text
AudioBatch 0.9.0.3 2026-10-11 a11667c main JUCE v9.0.3
```

Starting the GUI with `-H` or `--headless` runs the app with its main window hidden.
This is meant for automated startup checks, since the window cannot be shown again from inside the app.

The results list supports per-file actions from the context menu, including:

- reveal file in Finder or Explorer
- open the parent folder
- re-analyze selected files
- move files to the system trash
- remove files from the list without deleting them
- normalize selected files to `0 dBFS` peak
- process selected files through the current plugin chain

Keyboard shortcuts in the results list:

- `Delete` or `Backspace` removes the selected rows from the list.
- `Ctrl-Backspace` moves the selected files to the system trash.
- `Cmd-R` or `Ctrl-R` reveals the selected file in Finder or Explorer.
- `F5` refreshes the analysis for all root folders.
- `Space` starts or stops the background analysis.

Normalization works for every readable input format,
including AIFF, WAV, FLAC, MP3, and Ogg Vorbis,
because the output is always a sibling `.aif` file written through the JUCE AIFF writer.

The status column also shows per-file activity while analysis or normalization is running.

Columns:

- file name
- full path
- file type
- bitrate
- sample rate
- peak L (hidden by default)
- peak R (hidden by default)
- peak max
- true peak
- max short
- integrated loudness
- custom gain (hidden by default)
- status

Columns can be shown or hidden from the table header context menu.

Default sorting is by overall peak ascending, so the quietest files appear first.

## Plugin processing

AudioBatch can run selected files through a chain of VST3 or Audio Unit effect plugins
and write the processed audio to disk.

- Use the plugin menu in the GUI to scan for installed plugins and build a chain.
  Plugins are loaded in a separate worker process while scanning,
  so a plugin that crashes during the scan does not take down the app.
  A crashing plugin is blacklisted in the plugin manager, and the scan continues with the next one.
  Adding a plugin opens its editor window automatically so it can be configured.
  Multiple plugin editor windows can be open at the same time.
  Plugin state is captured when an editor window is closed, and again when processing starts,
  and the whole chain is persisted between runs.
- The chain editor window lists the plugins in processing order.
  Individual plugins can be enabled or disabled without removing them from the chain,
  reordered by dragging a row or with the arrow buttons, and removed entirely.
  Removing a plugin closes its editor window.
- Plugins run in offline (non-realtime) rendering mode,
  the same mode DAWs use for bouncing,
  so plugins that offer higher-quality processing for offline rendering use it.
  VST3 plugins receive this through the processing setup,
  and Audio Units through the offline render property.
- Files are processed through the enabled plugins in chain order in a single pass.
  The processing tail is the sum of each plugin's reported tail,
  so reverbs and delays ring out fully through the rest of the chain.
- Plugin latency is compensated.
  The latency each plugin reports, for example from lookahead or linear phase processing,
  is summed over the chain and removed from the start of the output,
  so the processed audio stays aligned with the original and its end is not cut off.
- Mono files fed to a stereo-only plugin send the same signal to both plugin inputs,
  and the output is written back as mono from the average of both channels.
- The chain output is rendered unquantized first.
  The final file keeps the source bit depth, up to 24 bits, so 32-bit float sources become 24-bit.
  If the processed audio would go over 0 dBFS, the whole file is turned down just enough to avoid clipping,
  and the status column shows how much, for example `Reduced 1.3 dB to avoid clipping`.
  16-bit output is dithered.
  Silence left over from the tail allowance is trimmed, but the output is never shorter than the input.
- A file that changed on disk since it was analyzed is analyzed again before its gain is computed.
  Normalization does the same.
- Processed output is always written as `<name>.aif` next to the input file,
  with the same overwrite and collision rules as normalization.
  If the input has a different extension, the original is moved to the system trash once the new file has been written.
- Source metadata is copied onto the processed AIFF through TagLib,
  so tags and embedded artwork survive the processing step.
  If the metadata cannot be written, the file fails and the original is kept.
- Optional pre-plugin normalization scales each file by `1 / overall peak` before sending it through the chain,
  which keeps the input level consistent across the batch.
  A per-file custom input gain can also be set from the results list.

Normalization always writes an AIFF file next to the source.

- `<name>.aif` sources are normalized in place.
- `<name>.aiff` and `<name>.aifc` sources are rewritten as `<name>.aif`,
  and the original file is moved to the system trash once the write succeeds.
- Any other readable format, including MP3, FLAC, WAV, and Ogg,
  is converted to `<name>.aif` and the original is moved to the system trash.
- The output keeps the source sample rate.

Existing files are never overwritten by default:

- If `<name>.aif` already exists and is not the source itself, the file fails with an error in the status column.
  Enable `Force overwrite` in the GUI, or pass `--force` to the CLI, to replace it.
  The replaced file is moved to the system trash.
- If several selected files would write the same `<name>.aif`, for example `song.mp3` and `song.flac`,
  only one of them is processed and the others fail.
  A source that is already `<name>.aif` always wins.
- Cancelling a run stops before any file on disk is changed.

Metadata is read from the source through TagLib and written back to the AIFF output as ID3v2.4.
This includes standard text tags, custom tags such as ID3v2 `TXXX` frames and Vorbis comment keys,
and embedded pictures such as front cover art.
FLAC Vorbis comments and pictures are translated to the equivalent ID3v2.4 frames on output.

## CLI

The console executable supports both short and long option forms.

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
  -j, --jobs <count>      Override worker count
  -s, --sort <mode>       Sort by peak, lufs, name, or path
      --reverse           Sort in descending order
  -o, --output <format>   Print results as table (default), csv, or json
  -l, --list-plugins      List the available audio plugins and exit
  -g, --group-by <mode>   Group the plugin list by name (alphabetical), manufacturer, type, or format
  -R, --rescan            Scan for new or changed plugins before listing them

Short options cannot be combined, so pass -r -f instead of -rf.
```

`--help` and `--version` only print to the console.
They do not start logging or write to the log file.

Short options must be passed separately.
A combined argument such as `-rf` is rejected,
and `--list-plugins` cannot be combined with the analysis options.

Options that take a value accept both `--jobs 4` and `--jobs=4`.
`--sort lufs` can also be written as `--sort loudness`.
Without any paths, the current directory is analyzed.
`--jobs` sets the number of parallel analysis workers.
Normalization in the CLI processes one file at a time.

Exit codes:

- `0`: every file was analyzed (and normalized, with `-n`) successfully.
- `1`: invalid arguments, or the output or plugin list could not be written.
- `2`: at least one file failed analysis or normalization.

### Plugin listing

`--list-plugins` prints the VST3 and Audio Unit plugins known to AudioBatch.
It reads the plugin list shared with the GUI from `KnownPlugins.xml` next to the GUI settings file,
so listing is instant and loads no plugins.
CLAP plugins are not listed, because JUCE cannot host them.

- `--group-by name` (default) prints one alphabetical table.
- `--group-by manufacturer`, `type`, or `format` prints one section per group.
  The type is the processing type reported by the plugin, such as Dynamics, EQ, Reverb, or Instrument.
- `-R` / `--rescan` first scans the plugin folders for new or changed plugins
  and removes plugins that no longer exist.
  It uses the same folders as the last scan in the GUI plugin manager, or the default plugin folders.
  Each plugin is loaded in a separate worker process,
  so a plugin that crashes is blacklisted and the scan continues.
  The updated list is saved to `KnownPlugins.xml`,
  which the GUI reloads when the plugin manager opens.
  The GUI settings file is never written by the CLI.
  If the list cannot be saved, the command reports an error and exits with code 1.
  Only one plugin scan runs at a time.
  A rescan refuses to start while the GUI plugin manager is scanning,
  and the GUI skips its scan with a message while a rescan is running.

```shell
audiobatch --list-plugins
audiobatch -l -g manufacturer
audiobatch -l -R --group-by type
```

Example output:

```text
NAME                     MANUFACTURER     TYPE      FORMAT  VERSION
AnSpec                   Voxengo          Analyzer  VST3    1.2
APU Loudness Compressor  APU Software     Dynamics  VST3    5.0.2
```

```text
Analyzer (19)
  AnSpec                   Voxengo          VST3  1.2
  Correlometer             Voxengo          VST3  1.1
```

Examples:

```shell
audiobatch "music/song.wav"
audiobatch -r -s name "music/library"
audiobatch --recurse --sort peak "music/library"
```

Default CLI output format:

```text
dBFS  dBTP  LUFS-I  TRACK
<peak>  <true peak>  <integrated loudness>  <audio filename>
```

Example output:

```text
  -0.31    -0.17     -8.48  audiofile.wav
```

Normalize mode re-analyzes each output file after rewriting it
and reports the resulting output paths using the same format:

```text
   dBFS     dBTP    LUFS-I  TRACK
  -0.00    -0.00    -10.42  C:\path\to\normalized-output.aif
```

### Output formats

`--output csv` and `--output json` print one entry per file for spreadsheets and scripts.
They include the path, file properties, peak in dBFS, true peak in dBTP, integrated and maximum short-term loudness,
the status, and the error for files that failed.
Silence has no level, which JSON writes as `null` and CSV as an empty field.
Log messages always go to stderr, so stdout only holds the results:

```shell
audiobatch -r -o json "music/library" > levels.json
audiobatch -s lufs --reverse -o csv "music/library" > levels.csv
```

## Cache

Analysis results are cached in a SQLite database located under the
user application data directory in an `AudioBatch/analysis.db` file.

The cache is invalidated when any of these change:

- file path
- file size
- file modification time
- internal analysis schema version

Failed analyses are cached too, so a file that cannot be decoded is not retried on every run.
A custom gain only applies to the version of the file it was set for,
so it is cleared when the file changes on disk.
On GUI startup, rows for files that no longer exist are removed in the background.
Use `--refresh` in the CLI, or re-analyze from the GUI, to retry it.

## TODO

- User config file.
