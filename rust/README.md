# AudioBatchRs

The Rust version of AudioBatch, developed next to the C++ application in `../cpp`.
It analyzes audio files for sample peak, true peak, and integrated loudness (EBU R128),
caches the results in SQLite, and lists them in a CLI and a GUI with a waveform overview and playback.
Both also normalize files to a sample peak or loudness target, written as AIFF with their tags carried over.
The GUI hosts VST3 effects on macOS and Windows:
it processes files through a plugin chain, shows the plugins' editors, and can play the preview through the chain.
The CLI lists the plugins a scan found.
The CLI takes the same arguments and prints the same output as the C++ CLI, so the two can be compared directly.

## GUI commands

The File menu adds files or folders, selects all files, normalizes the selection, opens parent folders,
and clears per-file gain. The Plugins menu processes the selection through the live chain.
Options offers Output Device and Columns submenus, Continue Playback When Changing Files, and Play from First Cue Point.
Chain Presets includes Show Presets Folder.
Audio Settings uses Cmd/Ctrl+P only, Add Folder uses Cmd/Ctrl+Shift+O, and Select All Files uses Cmd/Ctrl+A.
Cmd/Ctrl+E opens the plugin chain editor.
Escape stops playback and returns to the start.

The Folder column, shown beside Name, holds the parent directory.
On macOS a path under a user home starts with `~/`, and other paths stay absolute.
Path is hidden by default and available under Options > Columns.
Sorting by Folder uses the full parent path in natural order, then the file name.

## Source statistics

The inspector's Source lines and six optional table columns, hidden by default, show:

- PLR: whole-file true peak in dBTP minus integrated loudness.
- PSR: whole-file true peak in dBTP minus maximum short-term loudness.
- Crest factor: sample peak over unweighted RMS of all channel samples, including DC, in dB.
- DC: the signed mean of each channel. The column shows the largest absolute mean as percent full scale.
- Clips: samples with magnitude at least 1.0, summed over channels, with per-channel counts in the tooltip.
  Full-scale samples do not prove audible clipping.
- Correlation: the centered Pearson coefficient of the two stereo channels.
  Mono is +1, and a silent or constant stereo channel or more than two channels has no value.

The values describe the source, never custom gain or limiter estimates.
Normalized and processed results describe the quantized output.
Missing values show a dash in the table, `null` in JSON, and an empty field in CSV.
JSON and CSV name them `plrDb`, `psrDb`, `crestFactorDb`, `dcOffsets`, `clipCounts`, `clipsTotal`,
and `stereoCorrelation`, with the channel arrays as JSON arrays in CSV too.

## Audio formats

Discovery accepts WAV and BWF, AIFF and AIFC, FLAC, MP3, and AAC and M4A files.
Decoding uses Symphonia, including lossless ALAC in M4A containers, except for the macOS AAC case below.
On macOS, single-track mono or stereo AAC-LC MP4 files decode with Core Audio,
so perceptual noise substitution matches the C++ application.
`scripts/test_aac_reconstruction.py --cpp-cli <cli> --rust-cli <cli>` compares both applications on AAC.
It needs both release CLIs on macOS, so it runs by hand rather than in CI.
Opus is not supported.
Analysis and normalization support multichannel audio, while playback and plugin processing accept only mono and stereo.

## Crates

- `crates/core`: decoding with symphonia, custom BS.1770 loudness and SIMD true peak with fearless_simd,
  the waveform overview, tags with lofty, the SQLite cache with rusqlite, and normalization to AIFF.
  Ebur128 is a development-only reference for differential tests and benchmarks.
- `crates/playback`: preview playback with cpal, rtrb, and rubato, with an optional insert such as a plugin chain
- `crates/vst3-host`: VST3 hosting on the `vst3` bindings: modules, instances, processing, and editor windows
- `crates/plugins`: plugin scanning in a worker process, the plugin chain, batch processing, and the preview chain
- `crates/test-plugin`: fake VST3 plugins for the tests
- `crates/serato`: parsers for the Serato GEOB tags, extracted from track-rename
- `crates/cli`: the `audiobatch-rs` command line tool
- `crates/gui`: the AudioBatchRs app, built on gpui-kit
- `crates/build-info`: compile-time version and build metadata for the CLI and GUI
- `crates/test-fixtures`: locates the shared test fixtures in `../fixtures`

[`ARCHITECTURE.md`](ARCHITECTURE.md) explains how the crates work together,
with the threads, the analysis and processing pipelines, the preview, and VST3 hosting.

Analysis uses custom K weighting and block-energy gating, with SIMD true peak.
Loudness stays within 0.001 LU of ebur128 and matches displayed integrated
and maximum short-term values to two decimals.
Normalization measures the exact quantized output while writing, without a second full decode.

See [Analysis performance](../docs/performance.md) for the November 2, 2026 macOS measurements
and November 1, 2026 Windows measurements, benchmark settings, and validation limits.

## Build

Run the commands from this directory.
The required Rust toolchain is declared by `rust-version` in [`Cargo.toml`](Cargo.toml).

```shell
./build.sh
./install.sh
```

`build.sh` builds the release binaries by default and moves `audiobatch-rs` and `AudioBatchRs` into this directory.
Use `./build.sh --debug` or `./build.sh -d` to build and move the debug binaries instead.
On macOS, the GUI is packaged as `AudioBatchRs.app` with the bundle id `com.esgrove.audiobatch-rs`.
`install.sh` installs the CLI with `cargo install` into the Cargo bin directory,
and then installs the shell completions for it.
On macOS, it installs the app bundle to `/Applications`.
On Windows, it installs the GUI to `%LOCALAPPDATA%/Programs/AudioBatchRs` with a Start Menu shortcut.
Both scripts take `-l` or `--cli-only` to skip the slower GUI build.
They also accept `-c` or `--clean` for the repository root scripts,
but leave Cargo artifacts untouched because the option only cleans the C++ CMake build directory.

K-weighting coefficient calculations, the filter recurrence,
and true-peak interpolation coefficient calculation include adaptations from ebur128 0.1.11.
Its complete copyright and MIT license notice is in [`crates/core/licenses/ebur128.txt`](crates/core/licenses/ebur128.txt).
`build.sh` copies it into `audiobatch-rs-licenses/` beside the loose binaries,
and `install.sh` copies it into the same directory beside the installed CLI.
macOS app bundles include it in `Contents/Resources/licenses/`, and Windows GUI installations in `licenses/`.
Include this notice when distributing binaries built or installed directly with Cargo too.

Windows GUI builds require the Visual Studio C++ Build Tools, a Windows SDK, and LLVM with `libclang.dll`.
If LLVM is not found automatically, set `LIBCLANG_PATH` to its `bin` directory.
CPAL's ASIO dependency downloads the SDK on the first build and caches it in the temporary directory.
No CMake step is needed for the Rust application.
For an existing or offline SDK, set `CPAL_ASIO_DIR` to its directory containing `common/` and `host/`.

`format.sh` formats the Rust code with rustfmt, the shell scripts with shfmt when installed,
and the prose with slb when installed,
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
The meter still requires AVX2 on x86 or NEON on Apple Silicon, so clearing native tuning is not an older-CPU mode.

```shell
cargo build --release
```

### Local macOS signing

`build.sh` and `install.sh` sign the finished `AudioBatchRs.app` with `sign_mac_app_bundle` from the root `common.sh`,
shared with the C++ build.
`CODESIGN_IDENTITY` selects an installed identity, and an unset or empty value signs ad hoc.
An invalid identity fails the build.
The signature uses the hardened runtime,
and the shared [`macos.entitlements`](../assets/macos.entitlements) only disables library validation,
so third-party plugins load. The signature is for local use, without notarization.

```shell
codesign --verify --deep --strict --verbose=2 AudioBatchRs.app
uv run --script ../tests/test_mac_signing.py
```

`AUDIOBATCH_TEST_NATIVE_SIGNING=1` makes the test also sign and inspect real bundles on macOS.

## Shell Completions

`audiobatch-rs` generates shell completions with the `completion` subcommand.
`install.sh` runs `completions.sh`, which detects the platform and installs completions for its shells:

- **Windows**: Bash and PowerShell
- **macOS**: Zsh

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
Ctrl-C cancels the run, removes unfinished temporary output, reports committed files, and exits with code 130.
A second Ctrl-C exits immediately.
The `list` subcommand lists scanned VST3 effects, and `completion` generates shell completions.

`--normalize` writes analyzed files as a sibling `<name>.aif` at the source sample rate,
brought to a sample peak of 0 dBFS, or to the `--peak` or `--lufs` target.
A loudness target is limited by a -0.3 dBTP true peak ceiling, and a gain above 40 dB is refused.
The output keeps the source bit depth when AIFF supports it, so 32-bit float files become 24-bit.
Lossy MP3 and AAC files store no bit depth and become 16-bit,
while ALAC files keep the depth stored in the file.
The tags, album art, and Serato data are copied over without the Serato overview.
The converted original goes to the system trash, and an `.aif` file rewritten in place leaves a copy there.
With `-D` or `--delete`, replaced originals are permanently deleted instead.
`AUDIOBATCH_DELETE=true` (or `1`) enables the same option, and `false` (or `0`) disables it.
In the default analysis command the option requires `--normalize`.
An existing output file is only replaced with `--force`,
and when two selected files would write the same output, only one of them is normalized.
Lossless output is sample-exact with C++ in peak mode and differs by at most one PCM step in loudness mode.
Neither mode applies dither.
An in-place `.aif` already at its target is left unchanged after cancellation and source-identity checks.

```console
Analyze audio files for sample peak, true peak, and integrated loudness (EBU R128)

Usage: audiobatch-rs [OPTIONS] [PATHS]...
       audiobatch-rs <COMMAND>

Commands:
  process     Render audio through a saved plugin chain as 16-bit AIFF
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
  -D, --delete           Permanently delete replaced originals instead of moving them to the trash [env: AUDIOBATCH_DELETE=]
      --peak <DBFS>      Normalize to this sample peak, -12 to 0 (default 0)
      --lufs <LUFS>      Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling
  -j, --jobs <COUNT>     Override worker count
  -s, --sort <MODE>      Sort by peak, lufs, name, or path [default: peak]
  -S, --reverse          Sort in descending order
  -o, --output <FORMAT>  Print results as table, csv, or json [default: table] [possible values: table, csv, json]
  -h, --help             Print help (see more with '--help')
  -V, --version          Print version
```

Results go to stdout, and messages and progress go to stderr.

### Log Files

The CLI writes `audiobatch-rs.log` and the GUI `AudioBatchRs.log`,
in `~/Library/Logs/AudioBatch-rs` on macOS and in the `logs` folder of the `AudioBatch-rs` data directory elsewhere.
Messages logged while a file is analyzed, normalized, or processed name the file,
and a panic is logged with its backtrace.
The CLI prints each message once to stderr, and the GUI prints everything it logs to stderr as well.

### Plugin processing

Save a native chain preset in the GUI, then process by name or by explicit JSON preset path:

```shell
audiobatch-rs process --preset "Master" --recurse --jobs 2 --output json "music/album"
audiobatch-rs process -p "/path/to/Master.json" -n --lufs -16 --force --delete "music/song.wav"
```

`--preset` / `-p` is required.
A value that is a file, contains a path separator, or ends in `.json` is a preset file,
and any other value is a preset name in the GUI's `chain-presets` folder.
Only the Rust JSON presets are read, not the C++ XML presets.
A preset with any damaged slot is rejected as a whole, before any file is touched.
`--recurse`, `--refresh`, `--jobs`, `--sort`, `--reverse`, and `--output` work as in analysis.
`--normalize` applies a peak or loudness gain before the first enabled plugin, by default a 0 dBFS peak.
A saved custom gain takes precedence, as in the GUI.
`--force` replaces existing outputs, and `--delete` deletes replaced originals instead of moving them to the trash.

The output is a 16-bit sibling `<name>.aif` written by the same processing coordinator as the GUI,
with latency compensation, effect tails, and the source tags and album art.
Ctrl-C keeps committed outputs, removes unfinished ones, and exits with code 130.
A plugin crash or hang can end or block the CLI, since only scanning runs in a separate process.

### Plugin Listing

`audiobatch-rs list` prints the known VST3 plugins,
and `--rescan` first scans the plugin folders for new or changed ones.
The C++ CLI has the same subcommand with the same options.
Bundles with `moduleinfo.json` are described without loading their code.
Other bundles are loaded in a worker process shared by the scan.
A crash blacklists the bundle and restarts the worker instead of ending the scan,
and a bundle that takes longer than three minutes is skipped without being blacklisted.
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

Open or drop files and folders to start background analysis.
Compare levels and file properties in a sortable results table with configurable columns and per-file progress.
Rescanning analyzes only new and changed files, while re-analysis ignores cached results.
Normalization, processing, and file actions work on the selection, with confirmed batch commands for all analyzed files.
Right-click a row for a menu of these actions on the selection.
Drag selected rows out to other applications, such as Finder, Explorer, or a DAW, to give them copies of the files.

### Spectrogram benchmarks

Run `cargo bench -p audiobatch-core --bench spectrogram` from this directory.
The Criterion suite separates FFT setup, streaming pushes, finalization, bounded long-file merging,
serialization, selected-cache access, and combined artifact-only backfill.
Use `-- --save-baseline <name>` and `-- --baseline <name>` for repeatable local comparisons.
The existing `analysis` benchmark measures full-meter and decoded-file impact independently.

### Waveform and playback

The preview shows the selected file's levels, tags, embedded album artwork, waveform, and Serato cue points.
Two aligned five-column property rows sit below the level cards, above the genre, wrapped album, and wrapped comment.
Drag the vertical dividers to adjust the controls and information widths within their limits.
Double-click a divider to restore its default width.
Album artwork can be hidden, and PNG and JPEG covers are decoded into bounded thumbnails in the background.
It supports mono and stereo playback, preview zoom, release-time seeking, and playback from Serato hot cues.
Choose Waveform, Spectrogram, Both: side by side, or Both: stacked from the preview context menu,
Options > Preview view, or Audio Settings. Both panes show the same time interval and linked playheads and cues.
Serato Overview tags provide optional waveform colours.
The short-term loudness graph overlays the waveform when present, or a full spectrogram otherwise.

The spectrogram caches Mel bands up to source Nyquist, while the displayed frequency axis extends to at least 20 kHz.
It uses a fixed source-power palette.
A gentle S-shaped display curve darkens quiet content without changing cached numerical intensities.
Preview gain, plugins, and output-device resampling do not alter the source spectrogram.
Frequency cutoffs can reveal limited bandwidth, but do not prove a file's lossless encoding history.

Fresh analysis builds both previews during the loudness decode pass and persists them
before delivering completed results.
The selected waveform fills progressively from a bounded, replaceable snapshot.
Background decoding is reserved for missing or outdated artifacts, with separate completed/total file status.

Playback continues while a plot is pressed or dragged, without releasing the audio device.
The white cursor follows actual playback, while a dimmer grey cursor shows the pending release target.
Release performs one seek and preserves running playback, or starts a stopped source from that position.
The status bar shows elapsed and total source time, with hours on both clocks for sources at least one hour long.

Waveform magnitude references use dBFS values without a unit caption, with grid lines while loudness is disabled.
Yellow sample-overload caps and orange true-peak caps appear only at the waveform's outer edges.
Options > Overload indicators and Audio Settings independently toggle both kinds and their subdued vertical lines.
These source warnings remain independent of preview gain and plugins.
Command/Ctrl+O chooses a folder and Command/Ctrl+P opens the compact, scrollable Audio Settings window.
Multichannel playback is not supported.

Per-file input gain scales the waveform and feeds plugin preview and processing.
Dry playback stays unchanged.
The gain is cached until the source file changes.
Optional gain-adjusted loudness and limiter estimates are predictions, not source measurements or live plugin metering.
See [Architecture](ARCHITECTURE.md) for their scope and limitations.

Audio Settings chooses the output device, sample rate, buffer size, and preview preferences,
including plugin preview, latency compensation, continued playback when changing files, and starting at the first cue.
Windows playback requires an installed ASIO driver, with no WASAPI fallback.
Other platforms use their default audio host.
Settings persist between sessions.

### Normalization and file safety

Normalize selected files or every analyzed file to a sample peak from -12 to 0 dBFS,
or loudness from -24 to -6 LUFS with a -0.3 dBTP ceiling.
The ceiling can leave a file below its loudness target.
Output is a sibling `.aif` with the source metadata, and replaced originals go to the system trash.
Existing output files are protected unless Overwrite is enabled.
Failures keep the original row and report the reason.

Analysis, normalization, and processing runs can be cancelled.
Uncommitted files keep their originals and rows, while completed files show their new output.
Removing a row does not delete its file, and moving files to the trash is a separate action.

Both applications share `~/Library/AudioBatch/analysis.db` on macOS
or `%APPDATA%/AudioBatch/analysis.db` on Windows.
Rust settings, logs, plugin data, and presets remain private to `AudioBatch-rs`.
`AUDIOBATCH_DATA_ROOT` selects an absolute root for isolated application storage, not file replacements or trash.
See [Isolated application storage](../AGENTS.md#isolated-application-storage) for the storage contract.

### Plugins

The GUI hosts VST3 audio effects on macOS and Windows.
Instruments are listed by scans but cannot be added to the chain.

- Plugins > Plugin Manager… scans the plugin folders, which are the platform's standard VST3 folders
  after the folders in `VST3_PATH`, unless custom folders are set there.
  Bundles are described from `moduleinfo.json` when available, otherwise loaded in a worker shared by the scan.
  A crash blacklists the bundle and restarts the worker instead of ending the scan,
  and a bundle that takes longer than three minutes is skipped without being blacklisted.
  The manager lists the known effects and the blacklisted bundles, which can be removed from the blacklist.
- Plugins > Add Plugin, the Plugin Manager, or the Plugin Chain window adds an effect at the end of the chain
  and opens its editor in a window of its own.
  Plugins > Edit Chain… opens the Plugin Chain window, which enables, reorders, removes,
  and opens the editor of each plugin, and shows the latency of each plugin and of the whole chain.
  The chain and the plugins' settings are saved after every change, when an editor closes, and on quit.
  Like the C++ version, the saved chain's plugins load only when first needed:
  when the preview plays through the chain, when the Plugin Chain window opens, or when an editor opens.
- The Presets row of the Plugin Chain window saves the chain under a name,
  with the plugins, their order, which ones are enabled, and their settings,
  and lists the saved presets, which load as soon as one is picked, like in the C++ version, or can be deleted.
  Plugins > Chain Presets loads a preset from the menu too, with the loaded one checked.
  Each preset is a JSON file in the `chain-presets` folder of the data directory.
  Changing the chain or a plugin's settings afterwards keeps the preset file as it is, until it is saved again.
- Process, or Plugins > Process Selected With Plugin Chain, renders the selected file through the enabled plugins,
  and Plugins > Process All Files With Plugin Chain… renders every analyzed file after a confirmation.
  Before the first plugin each file gets its custom gain,
  or with the Normalize option next to Process, its normalization gain.
  Each file starts from the plugins' saved settings, and the output is a 16-bit AIFF `<name>.aif` next to the source,
  with the tags and Serato data carried over, like normalization writes.
  The latency of the chain is removed, and the tails of the plugins are rendered, up to 20 seconds per plugin,
  but the output is never shorter than the source and trailing audio below -96 dBFS is cut off.
  Samples are floored to 16 bits without dither or gain changes, since dither and level are up to the plugins,
  and samples beyond full scale saturate, which the result reports.
- Options > Preview Plugin Chain plays the preview through the enabled plugins,
  after the custom gain, at the output device's rate.
  The preview runs in stereo, and a plugin that only supports mono gets both channels folded into one,
  with its output copied to both.
  The output of a mono file is folded back to mono, so it sounds like the processed file.
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
Setting `AUDIOBATCH_GUI_DEBUG=1` adds the waveform frame rate and player events to the log.

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
AUDIOBATCH_GUI_SNAPSHOT=target/gui.png AUDIOBATCH_GUI_SNAPSHOT_ACTIONS=audiobatch::ToggleLoudnessGraph \
    ./target/release/AudioBatchRs --autoplay ../fixtures
```

## Test

```shell
# The same test selections and integration diagnostics as CI
./test.sh --unit
./test.sh --gui-integration
./test.sh --optimized
./test.sh --cli-integration
./test.sh --tooling

# Run every workspace test together
cargo nextest run
```

`test.sh` defaults to the workspace unit and service tier, excluding the nonvisual GUI integration workflows.
The GUI tier saves `target/integration-artifacts/gui.log` and propagates failures through its logging pipeline.
The optimized tier runs release core tests, adding the CLI normalization subset on Windows.
The CLI tier builds the debug executable and runs the shared contracts through uv with isolated application storage.
Its diagnostics go to `target/integration-artifacts/cli/`.
Use `--artifacts <directory>` with either integration tier to choose a different diagnostic directory.
`--tooling` runs the parity-oracle and benchmark-tooling regression suites.
Set `RUSTFLAGS=""` to reproduce CI's portable CPU configuration.

The core golden tests compare exact floating point bits, histories, waveforms, and normalized samples.
Windows x86-64 uses snapshots in `crates/core/tests/golden/windows-x86_64/` for outputs affected by platform math.
The two native AAC fixture cases on macOS use `crates/core/tests/golden/macos-native-aac/`.
Both fall back to the shared snapshots for outputs without an override.
Generated signal identities and source fixture checksums are shared across platforms.
Only regenerate snapshots after validating an intentional numerical change:

```shell
RUSTFLAGS="" UPDATE_GOLDEN=1 cargo test -p audiobatch-core --lib golden_tests
```

For these platform cases this writes only differing overrides, preserves the shared snapshots,
and removes an override when its output becomes identical to the shared snapshot.
Both debug and release tests must pass without `UPDATE_GOLDEN`.

The plugin tests also load `AudioBatch Test MS`, the JUCE VST3 in `../test-plugin/` that the C++ tests use,
and check its output against the expected files in `../fixtures/plugin_expected/`.
Build it once with CMake and the JUCE submodule:

```shell
git submodule update --init ../cpp/JUCE
../test-plugin/build.sh
```

The tests find the bundle in `../test-plugin/cmake-build-release`, or at the path in `AUDIOBATCH_TEST_VST3`.
Without it they are skipped, except when the `CI` variable is set, where they fail.
Normalization golden corpus tests skip missing Git LFS audio locally,
unless `AUDIOBATCH_REQUIRE_NORMALIZATION_FIXTURES=1` requires the complete corpus.
Cross-language cache checks need `AUDIOBATCH_CPP_TESTS` to name the built C++ test executable, otherwise they skip.
See [Testing](AGENTS.md#testing) for the prerequisite and coverage details.

`scripts/parity.sh` runs both CLIs on the same files, compares their JSON measurements, and reports their wall times.
Build a C++ release CLI first, then pass `--release` for meaningful timing comparisons.
With `--normalize`, each CLI works on its own temporary copies and permanently deletes replaced copies with `--delete`.
Original fixtures remain untouched.
The checks compare reported levels, normalization targets, output names, AIFF properties, samples, and ID3 metadata.
Lossless samples must match exactly in peak mode and within one PCM step in loudness mode.
Lossy output samples use a -72 dBFS difference tolerance for decoder differences.
The Python helpers run through [uv](https://docs.astral.sh/uv/), which installs their packages.

`../fixtures/normalization` is the collection made for this,
with every supported format, sample rate, and bit depth, a wide range of tags, and edge cases.
It is stored with Git LFS, so run `git lfs install` and `git lfs pull` first.

```shell
./scripts/parity.sh --release ../fixtures
./scripts/parity.sh --release --normalize ../fixtures/normalization
./scripts/parity.sh --release --normalize --lufs -14 ../fixtures/normalization
```

For repeated timing comparisons with isolated inputs, warmups, and JSON and Markdown exports:

```shell
./scripts/cli_benchmark.sh --corpus ../fixtures/normalization
```

See [Analysis performance](../docs/performance.md) for measured results and benchmark settings.

For offline VST3 processing without decoding or file writes in the timed region:

```shell
./scripts/vst3_benchmark.sh
./scripts/vst3_benchmark.sh --iterations 10 --repeat 20 --no-build
```

The runner builds the Release C++ test executable and the Rust `vst3_benchmark` example,
and passes the same real AudioBatch Test MS bundle to both.
The six cases cover mono/stereo, 44.1/48/96 kHz, one/four/eight plugins, and latency.
Plugin loading, state restoration, decoding, allocation, and sample exports are outside timing.
Both host and chain modes include input/output transfers.
Chain mode also uses the production channel adapter and finite checks, but neither mode writes audio while timing.
Every output must match independent binary32 arithmetic and final 16-bit PCM exactly.
Reports and planar sample exports are retained in `target/vst3-benchmark/`.
Missing plugins and output differences fail the command. Timing differences are informational.

`cargo run -p audiobatch-playback --example play -- <file> [seconds] [sample rate] [buffer size]`
plays a file through the default output device, optionally at a chosen sample rate and buffer size,
and `cargo run -p audiobatch-core --example read_tags -- <files...>` prints tags and Serato data.

## Code Coverage

Code coverage is generated using [cargo-llvm-cov](https://github.com/taiki-e/cargo-llvm-cov)
with [cargo-nextest](https://nexte.st/).
`coverage.sh` runs the tests of the whole workspace with coverage and prints a per-file summary.
It also builds and runs the executable CLI contracts with the same instrumentation.

```shell
# Print the coverage summary
./coverage.sh

# Also write the HTML report and open it in the browser
./coverage.sh --open

# Also write an LCOV report for editors and coverage services
./coverage.sh --lcov

# Remove the previous coverage data first
./coverage.sh --clean

# Write LCOV, HTML, and the summary again without rebuilding or running tests
./coverage.sh --report-only
```

The HTML report is generated in `target/llvm-cov/html/` and the LCOV report in `target/llvm-cov/lcov.info`.
Cargo can cover the Rust workspace tests directly, but these commands do not run the executable Python CLI contracts:

```shell
# Run tests with coverage (text output)
cargo llvm-cov nextest --workspace

# Generate and open the HTML report in the browser
cargo llvm-cov nextest --workspace --html --open
```

The coverage job in CI runs on macOS, adds the summary to the workflow run,
and uploads the HTML and LCOV reports as the `rust-coverage` artifact.

## Miri

[Miri](https://github.com/rust-lang/miri) interprets tests and reports undefined behaviour in unsafe code.
`miri.sh` runs it with Stacked Borrows on the unsafe COM code of `vst3-host` and the test plugin,
the Serato tag parsers, and the waveform, overload, and spectrogram blob readers of the cache.
It does not exercise the native Core Audio AAC bridge, which needs native validation.

```shell
# Install the nightly toolchain with Miri, then run the tests under it
./miri.sh --install
./miri.sh
```

Tests that need native bundles, the file system, SQLite, or audio devices, or that take minutes under Miri,
are marked with `#[cfg_attr(miri, ignore = "...")]`.
The tests that start the process-lifetime test UI thread run with `-Zmiri-ignore-leaks`,
which keeps the pointer and aliasing checks.
The `miri` job in CI runs the same script on macOS.

## RealtimeSanitizer

`./rtsan.sh` checks hardware-free playback and preview processing on macOS with nightly Rust and matching Homebrew LLVM.
It uses native compiler annotations, a startup-linked runtime,
and isolated `target/rtsan/` build and diagnostic storage.
The ordinary stable toolchain and Windows builds do not enable this instrumentation.
`./rtsan.sh --skip-native` requires clean first-party audio data callbacks, including admission and guard destruction.
Use `./rtsan.sh --diagnostic` to verify native-plugin and owned stream-error disposal blockers alongside clean cases.
Native callback captures are pinned until backend disposal, and renderer resources retire on the owning thread.
A `BLOCKED` result is not a clean real-time callback.
Unexpected findings, missing bundles, and detector failures fail the run.
The runner records exact toolchain versions and requires compatible Rust/Clang runtime versions.
Build the native fixture with `../test-plugin/build.sh`, or provide its build-tree path with `AUDIOBATCH_TEST_VST3`.
No installed plugins or physical audio device are used.
RTSan checks exercised unsafe calls, not audio deadlines, arbitrary plugin safety, or every spin loop.

## Fuzzing

`fuzz/` holds [cargo-fuzz](https://github.com/rust-fuzz/cargo-fuzz) targets for libFuzzer,
in its own workspace because the targets build only on nightly.

- `serato_tags` runs the Serato tag parsers and the Overview cache round trip.
  It shares its input layout with the C++ `SeratoTagFuzzer` and replays that corpus too.
- `cli_arguments` parses each input line as one argument, like the C++ `CliArgumentsFuzzer`, and replays its corpus.
- `cache_blobs` reads the input as each cache blob type and requires accepted blobs to round trip unchanged.
- `decode_audio` writes the input to a file whose extension the first byte selects, then decodes and seeks it,
  with bounded file size, channels, sample rate, and decoded frames.
  Ordinary decode errors are accepted outcomes.

The seeds of each target are in `fuzz/corpus/<target>/`.
`fuzz.sh` takes the options of `cpp/fuzz.sh` plus `--targets all|parsers|decoder`.
New inputs go to `fuzz/scratch/` and crashing inputs to `fuzz/artifacts/`, so the committed corpus never changes.
Every run uses a five-second timeout, a 128 MiB single-allocation limit, and a 512 MiB RSS limit,
because a small file can make a decoder allocate a lot,
and a reduced ASan quarantine keeps the sanitizer's own memory under that limit.

```shell
# Build the targets, replay the corpus, and fuzz each target for 60 seconds
./fuzz.sh

# Only replay the corpus, or fuzz only the parser and cache targets
./fuzz.sh --replay
./fuzz.sh --fuzz --targets parsers --seconds 300
```

Add an input that found a bug to the corpus of its target, so the replay keeps checking it.

### Known Symphonia findings

Symphonia 0.6.1 panics on checked arithmetic overflow while opening some malformed WAV files,
and no newer release fixes it.
The committed input `wav-block-align-overflow` replays through the production `analyze_file_catching_panics` entry point,
which must turn exactly that panic into a failed record.
Any other panic, including mutations of those inputs, stays a fuzz finding.
`fuzz/tests/decoder_regressions.rs` checks these rules in subprocesses:

```shell
cargo test --manifest-path fuzz/Cargo.toml --test decoder_regressions
```

Decoder mutation fuzzing keeps finding variants of the same overflows,
so CI runs it in a separate non-blocking job that uploads its log and inputs as `rust-decoder-fuzz-diagnostics`.
The required `fuzz` job runs the regression tests, replays every corpus, and fuzzes the parser targets,
uploading crashing inputs as `rust-fuzz-artifacts`.
Preview decoding calls the decoder directly, so one of these files can end its decode thread.

## Required Tools

```shell
cargo install --locked cargo-nextest
cargo install --locked cargo-llvm-cov
cargo install --locked cargo-fuzz
cargo install --locked samply
rustup component add llvm-tools-preview
rustup toolchain install nightly --component miri,rust-src
```

[samply](https://github.com/mstange/samply) records sampling profiles of release or profiling builds.
It is a development tool, not a runtime dependency.
See [Analysis performance](../docs/performance.md#reproducing-the-measurements) for profiling commands.
