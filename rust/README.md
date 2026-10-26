# AudioBatchRs

Rust port of AudioBatch, built next to the C++ application in the repository root.
It analyzes audio files for sample peak, true peak, and integrated loudness (EBU R128),
caches the results in SQLite, and lists them in a CLI and a GUI with a waveform overview and playback.

The C++ application remains the reference implementation and the full-featured version.
Normalization and plugin processing are only available there.

## Crates

- `crates/core`: decoding with symphonia, analysis with ebur128,
  the waveform overview, tags with lofty, and the SQLite cache
- `crates/playback`: preview playback with cpal, rtrb, and rubato
- `crates/serato`: parsers for the Serato GEOB tags, extracted from track-rename
- `crates/cli`: the `audiobatch-rs` command line tool
- `crates/gui`: the AudioBatchRs app, built on gpui-kit
- `crates/test-fixtures`: locates the shared test fixtures in `../tests/fixtures`

## Build

Run the commands from this directory.

```shell
cargo build --release
```

## CLI

`audiobatch-rs` takes the analysis options of the C++ `audiobatch` CLI
and prints the same table, CSV, and JSON output.
It exits with code 2 when a file fails and with code 1 for invalid arguments.

```console
Analyze audio files for sample peak, true peak, and integrated loudness (EBU R128)

Usage: audiobatch-rs [OPTIONS] [PATHS]...

Arguments:
  [PATHS]...  Files and directories to analyze, the current directory when none are given

Options:
  -r, --recurse          Recurse into directories
  -f, --refresh          Ignore cached analysis and re-analyze files
  -j, --jobs <COUNT>     Override worker count
  -s, --sort <MODE>      Sort by peak, lufs, name, or path [default: peak]
      --reverse          Sort in descending order
  -o, --output <FORMAT>  Print results as table, csv, or json [default: table] [possible values: table, csv, json]
  -h, --help             Print help (see more with '--help')
  -V, --version          Print version
```

## GUI

```shell
cargo run --release --bin AudioBatchRs -- [--autoplay] [paths...]
```

The window follows the layout and colors of the C++ application:
the results table with the same columns in the same order,
and below it the controls, the information panel, and the waveform.
A status bar along the bottom shows the listed folders, the analysis status and progress,
and the output device with its sample rate and buffer size in samples.

- Drop folders to replace the list, or hold Shift to add them. Dropped files are added.
  File > Choose Folder, Add Folder, and Add Files do the same through a dialog.
- Select a row to see its levels, details, tags, waveform, and Serato cue points.
  Double-click a row, press space, or use Play to play, and press escape to stop and return to the start.
- Drag a column header to reorder the columns, and hover it to see what the column shows and in which unit.
- Click or drag on the waveform to seek.
  The wheel zooms around the playhead, and Command or Control with the wheel changes the input gain.
  Double-click the Zoom or Input gain slider to reset it to 100% or 0 dB.
- File > Rescan (F5) picks up new and changed files,
  and the File menu also re-analyzes, removes, trashes, or reveals the selected file.
- Options > Show Loudness Graph, or the same option in the settings window,
  draws the short-term loudness over the waveform, and Options > Output Device switches the audio device.
- The Audio Settings button, or Options > Audio Settings… (Command or Control and comma),
  opens the settings window like the C++ application.
  It chooses the output device, sample rate, and buffer size,
  continuing playback when changing files, starting at the first cue point, and the loudness graph with its range.
  The settings are saved in `gui-settings.json` next to the analysis cache.
- Hovering the controls, the information panel, and the status bar shows tooltips like the C++ application.
- `--autoplay` selects and plays the first file that analyzed successfully once the startup analysis completes.

The analysis cache lives in the `AudioBatch-rs` folder of the user data directory,
apart from the cache of the C++ application.

### Stress test

`--demo-rows <count>` fills the table with synthetic rows and updates 64 random rows thirty times per second,
with a generated waveform and a looping playhead.
Setting `AUDIOBATCH_GUI_DEBUG=1` prints the waveform frame rate, run summaries, and player events to stderr.

```shell
AUDIOBATCH_GUI_DEBUG=1 cargo run --release --bin AudioBatchRs -- --demo-rows 10000
```

### Window snapshots

The `snapshot` feature saves a PNG image of the window and quits,
which shows the layout without needing screen recording permission.
`AUDIOBATCH_GUI_SNAPSHOT_DELAY` sets the delay in seconds, five by default,
and `AUDIOBATCH_GUI_SNAPSHOT_ACTIONS` lists actions to dispatch first.

```shell
cargo build --release -p audiobatch-gui --features snapshot
AUDIOBATCH_GUI_SNAPSHOT=/tmp/gui.png AUDIOBATCH_GUI_SNAPSHOT_ACTIONS=audiobatch::ToggleLoudnessGraph \
    ./target/release/AudioBatchRs --autoplay ../tests/fixtures
```

## Test

```shell
cargo nextest run
```

`scripts/parity.sh` runs the Rust and the C++ CLI on the same files and compares their JSON output.
Build the C++ CLI first with the CMake debug preset or `build.sh` in the repository root.

```shell
./scripts/parity.sh ../tests/fixtures
```

`cargo run -p audiobatch-playback --example play -- <file> [seconds] [sample rate] [buffer size]`
plays a file through the default output device, optionally at a chosen sample rate and buffer size,
and `cargo run -p audiobatch-core --example read_tags -- <files...>` prints tags and Serato data.
