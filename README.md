# AudioBatch

GUI and CLI tools (C++23 and JUCE) for analyzing audio files and preparing batch work.

Current implementation focus:

- Analyze audio files and calculate decoded sample peak values.
- Calculate true peak and integrated loudness during analysis.
- Cache analysis results in SQLite so unchanged files are not re-read every run.
- Show sortable peak, true peak, and loudness data in the GUI.
- Show per-file processing state directly in the results list while background work is running.
- Normalize selected files to `0 dBFS` peak from the GUI or the CLI.
- Convert any normalized output to AIFF and preserve all source metadata,
  including embedded album art and custom tags, by writing them back as ID3v2.4.
- Run the same analysis and normalization from a console executable.

Analysis scope currently assumes mono or stereo source files, including MP3 joint stereo.
Multichannel files are not a target for this tool.

![Screenshot](./screenshot_mac.png)

## Dependencies

- [JUCE](https://juce.com/)
- CMake
- SQLite 3
- [libebur128](https://github.com/jiixyj/libebur128)
- [TagLib](https://taglib.org/) for cross-format metadata handling.

`SQLite3` is resolved through CMake.
If a system `SQLite3` package is available it will be used,
otherwise CMake falls back to fetching the SQLite amalgamation.

`libebur128` is fetched through CMake for loudness and true peak analysis.

`TagLib` is fetched through CMake.
It is used to read every tag and embedded picture from the source file,
and to write the resulting metadata back as ID3v2.4 on the AIFF output.

MP3 normalization requires a working `lame` encoder executable to be installed and available to the app.

Install `lame` with your platform package manager:

Windows:

```powershell
scoop install main/lame
```

macOS:

```shell
brew install lame
```

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

## GUI

The GUI analyzes the selected root folder automatically in the background and stores results in a sortable table.

The results list supports per-file actions from the context menu, including:

- reveal file in Finder or Explorer
- open the parent folder
- re-analyze selected files
- move files to the system trash
- remove files from the list without deleting them
- normalize selected files to `0 dBFS` peak

Keyboard shortcuts in the results list:

- `Delete` or `Backspace` removes the selected rows from the list.
- `Ctrl-Backspace` moves the selected files to the system trash.
- `Cmd-R` or `Ctrl-R` re-analyzes the selected files.
- `F5` refreshes the analysis for the current root folder.
- `Space` starts or stops the background analysis.

MP3 files can only be normalized when `lame` is installed.
AIFF, WAV, FLAC, and other readable formats are supported as input regardless,
because normalization always writes a sibling `.aif` file using the JUCE AIFF writer.

The status column also shows per-file activity while analysis or normalization is running.

Visible columns:

- file name
- full path
- file type
- bitrate
- peak L
- peak R
- peak max
- true peak
- max short
- integrated loudness
- status

Default sorting is by overall peak ascending, so the quietest files appear first.

## Normalization

Normalization always writes an AIFF file next to the source.

- `<name>.aif` sources are normalized in place.
- `<name>.aiff` and `<name>.aifc` sources are rewritten as `<name>.aif`,
  and the original file is moved to the system trash once the write succeeds.
- Any other readable format, including MP3, FLAC, WAV, and Ogg,
  is converted to `<name>.aif` and the original is moved to the system trash.

Metadata is read from the source through TagLib and written back to the AIFF output as ID3v2.4.
This includes standard text tags, custom tags such as ID3v2 `TXXX` frames and Vorbis comment keys,
and embedded pictures such as front cover art.
FLAC Vorbis comments and pictures are translated to the equivalent ID3v2.4 frames on output.

## CLI

The console executable supports both short and long option forms.

```text
audiobatch [options] <paths...>

  -h, --help
  -V, --version
  -r, --recurse
  -f, --refresh
  -n, --normalize
  -j, --jobs <count>
  -s, --sort <peak|name|path>
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

## Cache

Analysis results are cached in a SQLite database
located under the user application data directory in an `AudioBatch/analysis.db` file.

The cache is invalidated when any of these change:

- file path
- file size
- file modification time
- internal analysis schema version

## TODO

- User config file.
- VST3 and AU batch processing.
- "Add folder" menu option to append more roots to the current list.
- Optional report export, for example CSV or JSON.
