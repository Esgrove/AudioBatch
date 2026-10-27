# AudioBatchRs

The Rust version of AudioBatch, developed next to the C++ application in `../cpp`.
It analyzes audio files for sample peak, true peak, and integrated loudness (EBU R128),
caches the results in SQLite, and lists them in a CLI and a GUI with a waveform overview and playback.
Both also normalize files to a sample peak or loudness target, written as AIFF with their tags carried over.
The GUI hosts VST3 effects on macOS and Windows:
it processes files through a plugin chain, shows the plugins' editors, and can play the preview through the chain.
The CLI lists the plugins a scan found.
The CLI takes the same arguments and prints the same output as the C++ CLI, so the two can be compared directly.

## Crates

- `crates/core`: decoding with symphonia, analysis with ebur128,
  the waveform overview, tags with lofty, the SQLite cache, and normalization to AIFF
- `crates/playback`: preview playback with cpal, rtrb, and rubato, with an optional insert such as a plugin chain
- `crates/vst3-host`: VST3 hosting on the `vst3` bindings: modules, instances, processing, and editor windows
- `crates/plugins`: plugin scanning in a worker process, the plugin chain, batch processing, and the preview chain
- `crates/test-plugin`: fake VST3 plugins for the tests
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
On macOS, the GUI is packaged as `AudioBatchRs.app` with the bundle id `com.esgrove.audiobatch-rs`.
`install.sh` installs the CLI with `cargo install` into the Cargo bin directory,
and then installs the shell completions for it.
On macOS, it installs the app bundle to `/Applications`, and elsewhere the GUI binary next to the CLI.
Both scripts take `--cli-only` to skip the slower GUI build.
`format.sh` formats the Rust code with rustfmt, the shell scripts with shfmt, and the prose with slb,
and `format.sh --check` only verifies them.

The app uses the same icons as the C++ application, from the root `assets` directory.
The app bundle carries the layered Icon Composer icon, compiled for macOS 15 and later with `actool` from Xcode 26,
and `icon_mac.icns` as the fallback icon file.
On Windows, `crates/gui/build.rs` embeds `icon_win.ico` and the version information into the executable.
The `.ico` file is generated from `icon_win.png` with ImageMagick:

```shell
magick ../assets/icon_win.png -define icon:auto-resize=256,128,96,64,48,40,32,24,20,16 ../assets/icon_win.ico
```

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
The `list` and `completion` subcommands work the same as in the C++ CLI,
except that only `audiobatch-rs` also generates elvish completions.

`--normalize` writes every analyzed file as a sibling `<name>.aif` at the source sample rate,
brought to a sample peak of 0 dBFS, or to the `--peak` or `--lufs` target.
A loudness target is limited by a -0.3 dBTP true peak ceiling, and a gain above 40 dB is refused.
The output keeps the source bit depth when AIFF supports it, so 32-bit float files become 24-bit.
Lossy MP3, AAC, and Ogg Vorbis files store no bit depth and become 16-bit,
while ALAC files keep the depth stored in the file.
The tags, album art, and Serato data are copied over without the Serato overview.
The converted original goes to the system trash, and an `.aif` file rewritten in place leaves a copy there.
An existing output file is only replaced with `--force`,
and when two selected files would write the same output, only one of them is normalized.
The output of lossless sources has the same samples as the C++ application writes, without dither.

```console
Analyze audio files for sample peak, true peak, and integrated loudness (EBU R128)

Usage: audiobatch-rs [OPTIONS] [PATHS]...
       audiobatch-rs <COMMAND>

Commands:
  list        List the available VST3 plugins
  completion  Generate shell completion script
  help        Print this message or the help of the given subcommand(s)

Arguments:
  [PATHS]...  Files and directories to analyze, the current directory when none are given

Options:
  -r, --recurse          Recurse into directories
  -f, --refresh          Ignore cached analysis and re-analyze files
  -n, --normalize        Normalize analyzed files, then report the re-analyzed output peaks
  -F, --force            Let normalize replace an existing output file
      --peak <DBFS>      Normalize to this sample peak, -12 to 0 (default 0)
      --lufs <LUFS>      Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling
  -j, --jobs <COUNT>     Override worker count
  -s, --sort <MODE>      Sort by peak, lufs, name, or path [default: peak]
  -S, --reverse          Sort in descending order
  -o, --output <FORMAT>  Print results as table, csv, or json [default: table] [possible values: table, csv, json]
  -h, --help             Print help (see more with '--help')
  -V, --version          Print version
```

### Plugin Listing

`audiobatch-rs list` prints the known VST3 plugins,
and `--rescan` first scans the plugin folders for new or changed ones.
The C++ CLI has the same subcommand with the same options.
Each plugin is loaded in a separate worker process, so a plugin that crashes is blacklisted instead of ending the scan,
and one that takes longer than three minutes is skipped.
Bundles that are unchanged since the last scan are not loaded again.
The list is saved to `plugins.json` in the data folder and shared with the GUI.

```console
List the available VST3 plugins

Usage: audiobatch-rs list [OPTIONS]

Options:
  -R, --rescan           Scan for new or changed plugins before listing them
  -g, --group-by <MODE>  Arrange the plugins in one table, or in sections by manufacturer, type, or format [default: name] [possible values: name, manufacturer, type, format]
  -h, --help             Print help (see more with '--help')
```

## GUI

```shell
cargo run --release --bin AudioBatchRs -- [--autoplay] [paths...]
```

The window shows the results table,
and below it the controls, the information panel, and the waveform.
A status bar along the bottom shows the listed folders, the analysis or normalization status and progress,
and the output device with its sample rate and buffer size in samples.

- Drop folders to replace the list, or hold Shift to add them. Dropped files are added.
  File > Choose Folder, Add Folder, and Add Files do the same through a dialog.
- Select a row to see its levels, details, tags, waveform, and Serato cue points.
  A file with a Serato Overview tag gets a waveform coloured from it,
  while the shape still comes from the decoded audio, and other files are drawn in blue.
  Serato data is read from MP3, AIFF, WAV, and FLAC files.
  Double-click a row, press space, or use Play to play, and press escape to stop and return to the start.
- Drag a column header to reorder the columns, and hover it to see what the column shows and in which unit.
- Click or drag on the waveform to seek.
  The wheel zooms around the playhead, and Command or Control with the wheel changes the input gain.
  Double-click the Zoom or Input gain slider to reset it to 100% or 0 dB.
- The Input gain slider sets a per-file gain for the selected file, shown in the Gain column.
  It applies to the preview and the waveform, and is saved in the analysis cache until the file changes.
  File > Clear Per-file Gain removes it.
- The Normalize group chooses a sample peak target from -12 to 0 dBFS,
  or a loudness target from -24 to -6 LUFS limited to a -0.3 dBTP true peak.
  The Normalize button and File > Normalize Selected normalize the selected file,
  and File > Normalize All Files normalizes every analyzed file after a confirmation.
  Each file is written as a sibling `.aif` that replaces its row, and a converted source is moved to the system trash.
  Overwrite lets the output replace an existing file of the same name, which is moved to the trash too.
  The Status column shows the progress of each file, and a failure keeps the row with the reason.
  The list cannot change while files are being normalized, and normalization waits for analysis to finish.
- File > Rescan (F5) picks up new and changed files,
  and the File menu also re-analyzes, removes, trashes, or reveals the selected file.
- Options > Show Loudness Graph, or the same option in the settings window,
  draws the short-term loudness over the waveform, and Options > Output Device switches the audio device.
- The Audio Settings button, or Options > Audio Settings… (Command or Control and comma),
  opens the settings window.
  It chooses the output device, sample rate, and buffer size, previewing through the plugin chain,
  continuing playback when changing files, starting at the first cue point, and the loudness graph with its range.
  The settings are saved in `gui-settings.json` next to the analysis cache.
- Hovering the controls, the information panel, and the status bar shows tooltips.
- `--autoplay` selects and plays the first file that analyzed successfully once the startup analysis completes.

The analysis cache lives in the `AudioBatch-rs` folder of the user data directory,
apart from the cache of the C++ application.
It also keeps the Serato Overview of each file, so the waveform is coloured before the tags are read again.

### Plugins

The GUI hosts VST3 audio effects on macOS and Windows.
Instruments are listed by scans but cannot be added to the chain.

- Plugins > Plugin Manager… scans the plugin folders, which are the platform's standard VST3 folders
  after the folders in `VST3_PATH`, unless custom folders are set there.
  Each plugin is loaded in a worker process, so a plugin that crashes is blacklisted instead of ending the scan,
  and one that takes longer than three minutes is skipped.
  The manager lists the known effects and the blacklisted bundles, which can be removed from the blacklist.
- Plugins > Add Plugin, the Plugin Manager, or the Plugin Chain window adds an effect at the end of the chain
  and opens its editor in a window of its own.
  Plugins > Edit Chain… opens the Plugin Chain window, which enables, reorders, removes,
  and opens the editor of each plugin, and shows the latency of each plugin and of the whole chain.
  The chain and the plugins' settings are saved after every change, when an editor closes, and on quit.
- The Presets row of the Plugin Chain window saves the chain under a name,
  with the plugins, their order, which ones are enabled, and their settings,
  and lists the saved presets to load or delete.
  Plugins > Chain Presets loads a preset from the menu too, with the loaded one checked.
  Each preset is a JSON file in the `chain-presets` folder of the data directory.
  Changing the chain afterwards keeps the preset file as it is, until it is saved again.
- Process, or Plugins > Process Selected With Plugin Chain, renders the selected file through the enabled plugins,
  and Plugins > Process All Files With Plugin Chain… renders every analyzed file after a confirmation.
  Before the first plugin each file gets its custom gain,
  or with the Normalize option next to Process, its normalization gain.
  Each file starts from the plugins' saved settings, and the output is a 16-bit AIFF `<name>.aif` next to the source,
  with the tags and Serato data carried over, like normalization writes.
  The latency of the chain is removed, and the tails of the plugins are rendered, up to 20 seconds per plugin,
  but the output is never shorter than the source and trailing audio below -96 dBFS is cut off.
  Samples are truncated to 16 bits without dither or gain changes, since dither and level are up to the plugins,
  and samples beyond full scale saturate, which the result reports.
  A run can be cancelled, which leaves the files in progress with their originals.
- Options > Preview Plugin Chain plays the preview through the enabled plugins,
  after the custom gain, at the output device's rate.
  With Options > Compensate Preview Plugin Latency, each start and seek first fills the chain's latency in silence,
  so what is heard lines up with the waveform.
  At the end of a file the plugins' tails keep playing, up to 20 seconds each, while the playhead waits at the end.
  A plugin that cannot be prepared or fails to process turns the preview off with its name and error,
  and output that is not finite stops playback.

Scans write `plugins.json`, the list of known plugins that the CLI shares,
and the chain is saved in `plugin-chain.json`, both in the `AudioBatch-rs` folder.
A plugin that crashes while processing, previewing, or showing its editor ends the app,
because only scans run plugins in a separate process.

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

The plugin tests also load `AudioBatch Test MS`, the JUCE VST3 in `../test-plugin/` that the C++ tests use,
and check its output against the expected files in `../fixtures/plugin_expected/`.
Build it once with CMake and the JUCE submodule:

```shell
git submodule update --init ../cpp/JUCE
../test-plugin/build.sh
```

The tests find the bundle in `../test-plugin/build`, or at the path in `AUDIOBATCH_TEST_VST3`.
Without it they are skipped, except when the `CI` variable is set, where they fail.

`scripts/parity.sh` runs the Rust and the C++ CLI on the same files,
and `scripts/compare_levels.py` compares their JSON output.
Build the C++ CLI first with the CMake debug preset or `build.sh` in `../cpp`.
With `--normalize`, each CLI normalizes its own copy of the files in a temporary folder,
and the re-analyzed outputs are compared.
Every output must reach the target given with `--peak` or `--lufs`.
`scripts/compare_outputs.py` then compares the output files themselves:
the file names, the AIFF format, the samples within -72 dBFS, and the ID3v2 frames.
Both scripts run through [uv](https://docs.astral.sh/uv/), which installs the Python packages they need.
The originals are left alone, but the replaced copies go to the system trash.

`../fixtures/normalization` is the collection made for this,
with every supported format, sample rate, and bit depth, a wide range of tags, and edge cases.
It is stored with Git LFS, so run `git lfs install` and `git lfs pull` first.

```shell
./scripts/parity.sh ../fixtures
./scripts/parity.sh --normalize ../fixtures/normalization
./scripts/parity.sh --normalize --lufs -14 ../fixtures/normalization
```

Lossless sources give byte for byte identical outputs.
Lossy sources differ by about one least significant bit, and AAC more,
because the C++ application decodes AAC through the platform decoder.

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
