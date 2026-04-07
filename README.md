# AudioBatch

GUI and CLI tools (C++23 and JUCE) for analyzing audio files and preparing batch work.

Current implementation focus:

- Analyze audio files and calculate decoded sample peak values.
- Cache analysis results in SQLite so unchanged files are not re-read every run.
- Show sortable peak data in the GUI.
- Run the same analysis from a console executable.

![Screenshot](./screenshot_mac.png)

## Dependencies

- [JUCE](https://juce.com/)
- CMake
- SQLite 3

`SQLite3` is resolved through CMake. If a system `SQLite3` package is available it will be used, otherwise CMake falls back to fetching the SQLite amalgamation.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

This produces two binaries:

- `AudioBatchApp` for the GUI.
- `audiobatch` for terminal analysis.

## GUI

The GUI now analyzes the selected root folder automatically in the background and stores results in a sortable table.

Visible columns:

- file name
- full path
- peak L
- peak R
- peak max
- status

Default sorting is by overall peak ascending, so the quietest files appear first.

## CLI

The console executable supports both short and long option forms.

```text
audiobatch [options] <paths...>

  -h, --help
  -V, --version
  -c, --cli
  -H, --headless
  -r, --recurse
  -f, --refresh
  -j, --jobs <count>
  -s, --sort <peak|name|path>
```

Examples:

```powershell
audiobatch "music/song.wav"
audiobatch -r -s name "music/library"
audiobatch --recurse --sort peak "music/library"
```

Default CLI output format:

```text
<peak>  <audio filename>
```

Example output:

```text
 -20.48 dBFS  cassette_recorder.wav
```

## Cache

Analysis results are cached in a SQLite database located under the user application data directory in an `AudioBatch/analysis.db` file.

The cache is invalidated when any of these change:

- file path
- file size
- file modification time
- internal analysis schema version

## Current Scope

This milestone covers decoded sample-peak analysis only.

Not implemented yet:

- loudness analysis
- true peak analysis
- VST batch processing
- drag and drop export workflows
