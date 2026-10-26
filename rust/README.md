# AudioBatchRs

Rust port of AudioBatch, built next to the C++ application in `../cpp`.
It analyzes audio files for sample peak, true peak, and integrated loudness (EBU R128),
caches the results in SQLite, and lists them in a CLI and a GUI with a waveform overview and playback.
The CLI also normalizes files to a sample peak or loudness target, written as AIFF with their tags carried over.

The C++ application remains the reference implementation and the full-featured version.
Plugin processing is only available there, and the GUI cannot normalize files yet.

## Crates

- `crates/core`: decoding with symphonia, analysis with ebur128,
  the waveform overview, tags with lofty, the SQLite cache, and normalization to AIFF
- `crates/playback`: preview playback with cpal, rtrb, and rubato
- `crates/serato`: parsers for the Serato GEOB tags, extracted from track-rename
- `crates/cli`: the `audiobatch-rs` command line tool
- `crates/gui`: the AudioBatchRs app, built on gpui-kit
- `crates/test-fixtures`: locates the shared test fixtures in `../fixtures`

## Build

Run the commands from this directory.

```shell
./build.sh
./install.sh
```

`build.sh` builds the release binaries and moves `audiobatch-rs` and `AudioBatchRs` into this directory.
`install.sh` installs both with `cargo install` into the Cargo bin directory,
and then installs the shell completions for the CLI.
The GUI is installed as a plain binary that runs from the terminal, not as an app bundle.
Both scripts take `--cli-only` to skip the slower GUI build.

`.cargo/config.toml` compiles for the native CPU, so binaries built here may not run on older machines.
CI overrides it with an empty `RUSTFLAGS`.

```shell
cargo build --release
```

## Shell Completions

`audiobatch-rs` generates shell completions with the `completion` subcommand.
`install.sh` runs `completions.sh`, which detects the platform and installs completions for its shells:

- **Windows**: Bash and PowerShell
- **macOS**: Zsh
- **Linux**: Zsh and Bash

```shell
# Install the completions for the shells of this platform
./completions.sh

# Print the completion script to stdout
audiobatch-rs completion zsh

# Install the completion script to the shell's completion directory
audiobatch-rs completion zsh --install --verbose
```

Zsh completions go to `~/.zsh/completions`, or into a custom plugin when oh-my-zsh is installed.
Bash completions go to `~/.bash_completion.d`.
`completions.sh` prints what to add to the shell config to load them.

## CLI

`audiobatch-rs` takes the analysis and normalization options of the C++ `audiobatch` CLI
and prints the same table, CSV, and JSON output.
It exits with code 2 when a file fails and with code 1 for invalid arguments.
The `completion` subcommand is the only addition that the C++ CLI does not have.

`--normalize` writes every analyzed file as a sibling `<name>.aif` at the source sample rate,
brought to a sample peak of 0 dBFS, or to the `--peak` or `--lufs` target.
A loudness target is limited by a -0.3 dBTP true peak ceiling, and a gain above 40 dB is refused.
The output keeps the source bit depth when AIFF supports it, so 32-bit float files become 24-bit,
and the tags, album art, and Serato data are copied over without the Serato overview.
The converted original goes to the system trash, and an `.aif` file rewritten in place leaves a copy there.
An existing output file is only replaced with `--force`,
and when two selected files would write the same output, only one of them is normalized.
The output of lossless sources has the same samples as the C++ application writes, without dither.

```console
Analyze audio files for sample peak, true peak, and integrated loudness (EBU R128)

Usage: audiobatch-rs [OPTIONS] [PATHS]... [COMMAND]

Commands:
  completion  Generate shell completion script
  help        Print this message or the help of the given subcommand(s)

Arguments:
  [PATHS]...  Files and directories to analyze, the current directory when none are given

Options:
  -r, --recurse          Recurse into directories
  -f, --refresh          Ignore cached analysis and re-analyze files
  -n, --normalize        Normalize analyzed files, then report the re-analyzed output peaks
  -F, --force            Let --normalize replace an existing output file
      --peak <DBFS>      Normalize to this sample peak, -12 to 0 (default 0)
      --lufs <LUFS>      Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling
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
- The Input gain slider sets a per-file gain for the selected file, shown in the Gain column.
  It applies to the preview and the waveform, and is saved in the analysis cache until the file changes.
  File > Clear Per-file Gain removes it.
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
    ./target/release/AudioBatchRs --autoplay ../fixtures
```

## Test

```shell
cargo nextest run
```

`scripts/parity.sh` runs the Rust and the C++ CLI on the same files and compares their JSON output.
Build the C++ CLI first with the CMake debug preset or `build.sh` in `../cpp`.
With `--normalize`, each CLI normalizes its own copy of the files in a temporary folder,
and the re-analyzed outputs are compared.
The originals are left alone, but the replaced copies go to the system trash.

```shell
./scripts/parity.sh ../fixtures
./scripts/parity.sh --normalize ../fixtures
```

`cargo run -p audiobatch-playback --example play -- <file> [seconds] [sample rate] [buffer size]`
plays a file through the default output device, optionally at a chosen sample rate and buffer size,
and `cargo run -p audiobatch-core --example read_tags -- <files...>` prints tags and Serato data.

## Code Coverage

Code coverage is generated using [cargo-llvm-cov](https://github.com/taiki-e/cargo-llvm-cov)
with [cargo-nextest](https://nexte.st/).
`coverage.sh` runs the tests of the whole workspace with coverage and prints a per-file summary.

```shell
# Print the coverage summary
./coverage.sh

# Also write the HTML report and open it in the browser
./coverage.sh --open

# Also write an LCOV report for editors and coverage services
./coverage.sh --lcov

# Remove the previous coverage data first
./coverage.sh --clean
```

The HTML report is generated in `target/llvm-cov/html/` and the LCOV report in `target/llvm-cov/lcov.info`.
The same can be run with cargo directly:

```shell
# Run tests with coverage (text output)
cargo llvm-cov nextest --workspace

# Generate and open the HTML report in the browser
cargo llvm-cov nextest --workspace --html --open
```

The coverage job in CI runs on macOS, adds the summary to the workflow run,
and uploads the HTML and LCOV reports as the `rust-coverage` artifact.

## Required Tools

```shell
cargo install --locked cargo-nextest
cargo install --locked cargo-llvm-cov
rustup component add llvm-tools-preview
```
