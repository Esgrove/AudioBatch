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
Escape stops playback and returns to the start.

Folder is visible beside Name, while Path is hidden by default and remains available under Options > Columns.
Folder shows the parent directory without the file name.
On macOS, user-home prefixes become `~/`, while `/Users/Shared` and non-home folders stay absolute.
Windows keeps the full parent path. Sorting uses the full parent path in natural order, then the file name.
Saved column preferences migrate once to hide Path. Showing it again remains a persistent user choice.

## Source statistics

PLR is whole-file true peak in dBTP minus integrated LUFS.
PSR is whole-file true peak in dBTP minus maximum short-term LUFS, using the existing three-second window
and zero-filled startup. It is not the maximum of a time-local peak-to-short-term curve.
Crest factor is sample peak divided by unweighted RMS across all channel samples, in dB.
RMS includes DC, without loudness gating or mono weighting.
DC offsets are signed channel arithmetic means in linear full-scale units.
The compact DC column shows the largest absolute channel mean as percent full scale.

Clips counts samples with magnitude at least 1.0, including exact positive and negative full scale.
The total sums channel samples, not affected frames or contiguous events, and its tooltip gives channel counts.
Full-scale samples are not proof of analog clipping or an audible defect.
The existing waveform sample-over flags remain strictly above 1.0 and describe a different threshold.
Correlation is the whole-file centered stereo Pearson coefficient, clamped to [-1, +1].
Mono reports +1, including silence. A silent or constant stereo channel has zero variance and no defined coefficient.
Multichannel correlation is absent. This describes linear co-variation, not stereo width or sound quality.

These source measurements share the decode pass and bounded per-channel online moments.
They remain independent of custom-gain and limiter predictions, with Source lines in the inspector
and six optional table columns hidden by default, including for saved preferences.
Rendered records describe actual quantized normalization or plugin output.
Normalization meters while writing, while plugin processing analyzes its committed output.
Undefined scalar values show a dash, SQL NULL, JSON null, or an empty CSV field.
JSON and CSV add `plrDb`, `psrDb`, `crestFactorDb`, `dcOffsets`, `clipCounts`, `clipsTotal`, and `stereoCorrelation`.
Arrays use JSON syntax inside CSV fields. Ratios have two decimal places of precision, DC and correlation six,
and counts remain integers.

Shared measurement contract 15 requires finite decoded source PCM
and refills older rows while retaining same-source gains and compatible previews.
The shared `dc_offsets` blob contains one little-endian IEEE binary64 per channel,
and `clip_counts` one little-endian unsigned 64-bit count per channel.
`uv run --script ../fixtures/metrics_expected.py --check` checks the independent NumPy oracle used by both test suites.

## Audio formats

Discovery accepts WAV and BWF, AIFF and AIFC, FLAC, MP3, AAC and M4A, Ogg Vorbis, and CAF files.
Decoding uses symphonia on every platform, including AAC and lossless ALAC in M4A containers.
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

In the October 30, 2026 measurements on an M3 Max,
the release CLI analyzed the 29-file benchmark corpus in 0.297 s with one worker,
compared with 1.618 s for C++, about 5.44 times faster.
Peak and loudness normalization took 0.885 s and 0.928 s, about 3.71 and 3.52 times faster than C++.
See [Analysis performance](../docs/performance.md) for the full results, baseline comparisons, and validation limits.

## Build

Run the commands from this directory.

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
The installer packages the GUI only on macOS and Windows.
On other platforms, use Cargo to build and install the GUI binary directly.
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

```shell
cargo build --release
```

### Local macOS signing

`build.sh` and `install.sh` sign `AudioBatchRs.app` only after copying the executable, icons, licenses, and other resources.
Set `CODESIGN_IDENTITY` in their environment to select an installed identity explicitly,
for example `CODESIGN_IDENTITY="Apple Development: Your Name (TEAMID)" ./build.sh --debug`.
An unset or empty value uses ad-hoc signing (`-`), without searching or changing the keychain.
An invalid explicit identity fails packaging, rather than silently falling back.
Both applications use the same helper in the root `common.sh`
and the shared [`macos.entitlements`](../assets/macos.entitlements).

The signature enables hardened runtime with `--options runtime`, not an entitlement.
Only library validation is disabled, allowing external unsigned or differently signed plugins to load.
There is no audio-input or microphone entitlement, microphone usage description, or sandbox.
Signing validates both plists and verifies the final bundle. Failures stop packaging and remain visible.
Do not modify a signed bundle without signing it again, including when adding icons or license files.
These signatures are for local use only, without timestamping, notarization, distribution, or CI signing setup.
Ad-hoc signatures do not establish a trusted developer identity.
Direct Cargo builds, the CLI, and Windows packaging are unchanged.

```shell
codesign --verify --deep --strict --verbose=2 AudioBatchRs.app
codesign --display --entitlements - --xml AudioBatchRs.app
plutil -lint ../assets/macos.entitlements
uv run --script ../tests/test_mac_signing.py
```

On macOS, prefix the test command with `AUDIOBATCH_TEST_NATIVE_SIGNING=1` to inspect real ad-hoc Rust packages
and disposable Ninja and Xcode bundles, including resource tampering checks.

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
Ctrl-C during analysis, normalization, or plugin processing cancels the run with exit code 130.
The first interrupt drains work and cleans unfinished outputs. A second exits immediately.
The shared executable contracts check cancellation, normalization targets, output conflicts, gain refusal,
plugin listing, shell completions, cache reuse, and in-place AIFF output with isolated storage.
The `list` subcommand lists scanned VST3 effects, and `completion` generates shell completions.

`--normalize` writes analyzed files as a sibling `<name>.aif` at the source sample rate,
brought to a sample peak of 0 dBFS, or to the `--peak` or `--lufs` target.
A loudness target is limited by a -0.3 dBTP true peak ceiling, and a gain above 40 dB is refused.
The output keeps the source bit depth when AIFF supports it, so 32-bit float files become 24-bit.
Lossy MP3, AAC, and Ogg Vorbis files store no bit depth and become 16-bit,
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

`--preset` / `-p` is required. Names resolve in the GUI's `chain-presets` folder.
Only native Rust JSON presets are accepted, not the C++ application's XML presets.
CLI loading rejects the whole preset if any slot description or nonempty base64 state is damaged,
leaving sources and existing outputs unchanged rather than recovering a partial chain.
Without input paths the current directory is processed.
`--recurse`, `--refresh`, `--jobs`, `--sort`, `--reverse`, and `--output table|csv|json` match analysis.
`--normalize` / `-n` optionally applies peak or LUFS normalization before the first enabled plugin,
defaulting to a 0 dBFS peak target. `--peak` and `--lufs` require `--normalize` and cannot be combined.
Persisted custom gain takes precedence, and a refused normalization gain uses unity with a notice, as in the GUI.
There is no post-chain normalization, limiter, or dither.

The same processing coordinator as the GUI writes 16-bit PCM sibling `<name>.aif` files,
compensates latency, drains bounded tails, folds stereo plugin output back to mono for mono inputs,
and preserves metadata and album art without the obsolete Serato Overview.
Finite-output checks, exact PCM writing, quantized-output measurements, source identity checks,
temporary publication, disposal, and cache updates remain in the shared services.
`--force` and `--delete` work without normalization in this subcommand.
They allow destination replacement and permanent disposal rather than trash, respectively.
In-place `.aif` processing retains its path. Results report committed output paths and measurements.
Ctrl-C cleans unfinished temporary files, retains and reports committed outputs, and exits 130.
The main thread pumps native events and dispatches all plugin lifecycle work while workers render.
Only scanning is crash-isolated. A plugin crash or hang during processing can end or block the CLI.

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

The spectrogram uses a Mel frequency axis from zero to the source Nyquist frequency and a fixed source-power palette.
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

The analysis cache lives in the `AudioBatch-rs` folder of the user data directory,
apart from the cache of the C++ application.

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
Windows x86-64 uses snapshots in `crates/core/tests/golden/windows-x86_64/` for outputs affected by platform math,
and falls back to the shared snapshots for all other outputs.
Generated signal identities and source fixture checksums are shared across platforms.
Only regenerate snapshots after validating an intentional numerical change:

```shell
RUSTFLAGS="" UPDATE_GOLDEN=1 cargo test -p audiobatch-core --lib golden_tests
```

On Windows this writes only differing overrides, preserves the shared snapshots,
and removes an override when its output becomes identical to the shared snapshot.
Both debug and release tests must pass without `UPDATE_GOLDEN`.

The plugin tests also load `AudioBatch Test MS`, the JUCE VST3 in `../test-plugin/` that the C++ tests use,
and check its output against the expected files in `../fixtures/plugin_expected/`.
Build it once with CMake and the JUCE submodule:

```shell
git submodule update --init ../cpp/JUCE
../test-plugin/build.sh
```

The tests find the bundle in `../test-plugin/build`, or at the path in `AUDIOBATCH_TEST_VST3`.
Without it they are skipped, except when the `CI` variable is set, where they fail.

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
The same can be run with cargo directly:

```shell
# Run tests with coverage (text output)
cargo llvm-cov nextest --workspace

# Generate and open the HTML report in the browser
cargo llvm-cov nextest --workspace --html --open
```

The coverage job in CI runs on macOS, adds the summary to the workflow run,
and uploads the HTML and LCOV reports as the `rust-coverage` artifact.

## Miri

[Miri](https://github.com/rust-lang/miri) interprets the tests and reports undefined behaviour,
such as invalid pointer use or aliasing violations in unsafe code.
`miri.sh` runs it with the Stacked Borrows model on the crates where that matters most:
the unsafe COM code of `vst3-host` and the test plugin, the Serato tag parsers,
and the readers of the waveform, overload, and spectrogram blobs in the cache.

```shell
# Install the nightly toolchain with Miri, then run the tests under it
./miri.sh --install
./miri.sh
```

The selected tests currently contain 189 cases, of which 173 run and 16 are explicitly ignored under Miri.
`vst3-host` runs 68 of 76 tests, Serato runs 83 of 87, the core blob selection runs 22 of 26,
and the test plugin has no standalone unit tests but supplies the in-process COM implementations exercised by the host.
The 16 ignores are eight host tests needing native bundles or file-system access under Miri's default isolation,
four slow Serato overview and colouring tests, and four slow waveform and overload builder tests.
Another 452 core tests are excluded by the script's filter, not Miri ignores.
Nextest reports filtered tests as skipped on each invocation, so its per-command skipped counts must not be summed.
Miri supports some file-system access when isolation is disabled, but this script keeps isolation enabled.
Native library loading, native editor windows, SQLite, physical devices, and third-party plugin code are not covered.

The covered unsafe host paths include COM host objects, parameter queues, memory streams,
instance lifecycle and state, processing buffers, and UI-bound ownership with in-process fake plugins.
A clean run is bounded evidence for these tests and Miri's model, not proof that all plugin hosting is free of UB.
The script runs 138 cases with leak and thread-exit checking enabled.
Only the 35 cases starting the process-lifetime test UI thread get `-Zmiri-ignore-leaks`,
which disables allocation leak detection and outstanding-thread checks but leaves pointer and aliasing checks enabled.
The exception covers the instance tests, four processor tests, three dispatcher tests, and the UiBound test.
The processor's buffer-binding-only test and other host-object tests retain leak checking.
Caller-supplied `MIRIFLAGS` apply to all invocations, so additional user flags can weaken those defaults.
The `miri` job in CI runs the same script on macOS.

## Fuzzing

`fuzz/` holds [cargo-fuzz](https://github.com/rust-fuzz/cargo-fuzz) targets for libFuzzer.
It is its own workspace, excluded from the main one, because the targets build only on nightly.

- `serato_tags` runs the Serato tag parsers and the Overview cache round trip.
  Its input layout is that of the C++ `SeratoTagFuzzer`, so it also replays that corpus.
- `cli_arguments` parses each line of the input as one argument, like the C++ `CliArgumentsFuzzer`,
  and replays that corpus too.
- `cache_blobs` reads the input as a waveform, overload, spectrogram, and Serato Overview blob,
  and requires every accepted blob to be written back unchanged.
- `decode_audio` writes the input to a file with an extension the first byte selects, and decodes it.
  File payloads over 256 KiB are skipped.
  After opening, it admits at most eight channels and 192 kHz and requests at most 256 chunks
  or 65,536 frames per pass, with at most one packet of frame-bound overshoot, before trying one seek and another pass.
  Ordinary open, decode, and seek errors are accepted outcomes, not fuzz failures.

The seed corpus of each target is in `fuzz/corpus/<target>/`.
`fuzz.sh` takes the same options as `cpp/fuzz.sh`.
New inputs go to `fuzz/scratch/` and crashing inputs to `fuzz/artifacts/`, so the committed corpus never changes.
Replay uses `-runs=0` and fuzzing puts the scratch directory first, the only corpus directory libFuzzer writes.

A small compressed file can declare huge dimensions or packets, and the decoder allocates during opening
and within individual chunk calls, before the harness can inspect the results.
Both replay and fuzzing therefore use a five-second per-input timeout,
a 128 MiB single-allocation reporting threshold, and a 512 MiB sampled RSS threshold.
These are libFuzzer watchdogs, not a hard OS memory sandbox or an exact maximum allocation footprint.
Timeout or memory-limit violations remain findings with saved inputs, rather than silently discarded decoder errors.
The cache blob readers independently validate dimensions and payload lengths before allocating.
Serato output scales with the bounded payload, and CLI input is separately limited to 4096 bytes.

### Exact malformed WAV and CAF arithmetic regressions

Symphonia 0.6.1 has two independently reproduced checked arithmetic panics during decoder opening:

- `symphonia-format-riff` computes PCM block alignment in `u16` at `src/wave/chunks.rs:100`
  before validating the channel count.
  The `wav-block-align-overflow` input reproduces that multiplication overflow.
- `symphonia-format-caf` computes `bytes_per_packet * 8` in `u32` at `src/demuxer.rs:389`.
  The `caf-bits-per-coded-sample-overflow` input declares 3,758,096,130 bytes per packet
  and reproduces the multiplication overflow before a decoder can reject the inconsistent PCM parameters.

Overflow checks stay enabled for all fuzz dependencies.
Only those two byte-exact committed inputs, including their extension selector,
call the actual `analyze_file_catching_panics` worker entry point instead of the raw decoder.
The hook permits only the corresponding dependency location and multiplication-overflow payload inside that call.
Each replay requires exactly one verified panic and a failed record with `AnalysisError::Panicked`.
Mutations, unexpected locations, unrelated panics,
and these same panics outside the production call still reach libFuzzer's aborting hook.
A dependency update or a regression that stops reproducing fails closed and requires reviewing the allowance.

Run `cargo test --manifest-path fuzz/Cargo.toml --test decoder_regressions` to check both caught failures,
both raw decoder aborts, mutation rejection, wrong-location rejection inside production,
unrelated panic propagation, and ordinary decoder errors in isolated subprocesses.
CI runs this with overflow checking before corpus replay.
These third-party arithmetic defects remain unfixed.
The verified analysis entry point fails the affected file without unwinding out of its worker.
The normalization and processing coordinators also have catch-unwind boundaries,
but these tests do not establish their behavior or the safety of plugin destructors during unwinding.
Direct decoder callers, including preview decoding, do not acquire panic protection from this harness.
A checked-arithmetic preview decoder panic can terminate its decode thread.
Catch-unwind cannot contain aborts, allocation exhaustion, or hangs.
Unchecked release arithmetic is not evidence of safety.
Fuzz mutations may rediscover the same unfixed upstream defects and must remain visible findings,
not be silently treated as ordinary decoder errors or evidence of a clean fuzz campaign.
`fuzz/tests/wav-overflow-unallowlisted` retains one such fuzz-discovered WAV variant outside the replay corpus.
The subprocess regression requires that variant to remain unallowlisted and abort through the original panic hook.
The captured `fuzz/tests/wav-overflow-unallowlisted` variant is intentionally outside the replay corpus,
so replay checks the two exact production-boundary regressions while the subprocess suite requires this variant to abort.
The decode campaign remains blocked by that upstream WAV overflow, even when committed corpus replay passes.
After building, reproduce the unallowlisted finding from `rust/` with
`fuzz/target/$(rustc +nightly -vV | awk '/^host:/ { print $2 }')/release/decode_audio -runs=1 -timeout=5 -malloc_limit_mb=128 -rss_limit_mb=512 fuzz/tests/wav-overflow-unallowlisted`.
This command must exit unsuccessfully until the upstream defect is fixed or a separately reviewed production fix lands.

```shell
# Build the targets, replay the corpus, and fuzz each target for 60 seconds
./fuzz.sh

# Only replay the committed corpus, or only fuzz, for a chosen time
./fuzz.sh --replay
./fuzz.sh --fuzz --seconds 300

# Build without running, or run what was built before
./fuzz.sh --build-only
./fuzz.sh --no-build --replay
```

Add an input that found a bug to the corpus of its target, so the replay keeps checking it.
The `fuzz` job in CI replays the corpus, fuzzes each target for 60 seconds,
and uploads crashing inputs as the `rust-fuzz-artifacts` artifact.

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
