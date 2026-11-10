# ImageControl

A multiband mid/side stereo imaging effect built with JUCE.
Available as a VST3 for macOS and Windows, and an Audio Unit for macOS.
It lives in the AudioBatch repository and is primarily used in AudioBatch's plugin chain,
but works independently in compatible plugin hosts.
See [ARCHITECTURE.md](ARCHITECTURE.md) for code organization, DSP, rendering, and threading details.

## Key features

- Four frequency bands: Low, Low Mid, High Mid, and High, with three adjustable crossovers.
- Independent Mid and Side source trims, Width, and Recover per band.
  Width 0 removes Side, 1 leaves it unchanged, and 2 doubles it after the source trim.
  Recover defaults to Off and adds a quadrature-rotated share of removed Side to Mid only below Width 1.
- Per-band Pan, applied after Width/Recover, so even a narrowed band can be placed to one side.
- Combinable band solos, output gain, Delta monitoring, click-free bypass, and automatable Mono output.
- A spatial spectrogram for inspecting the input or processed output, with optional per-band correlation meters.
- Plugin-local undo/redo, persistent A/B comparisons, and a portable user preset library.

Crossover frequencies stay ordered and at least a third of an octave apart.
Hosts can save and automate the processing parameters in real-time and offline renders.
Width and Recover have independent host parameters, separate from Mid and Side gains.
Recover sits beneath Width, with 0% shown as Off.
Side Off prevents recovery, while Mid Off leaves recovered content audible.
Width remains editable with Mid Off, and Link affects only gain edits.
Older states retain their existing gains with Width 1 and Recover Off, without a gain-derived migration.
See [Parameters.h](src/Parameters.h) for parameter ids, ranges, and defaults.

## Editing and presets

The header's curved-arrow Undo and Redo buttons track local parameter edits,
including linked controls and spectrogram gestures.
Its chevron shows or hides the A/B and preset row, and that layout choice is saved with the host project.
The history survives closing the editor but is not saved with a project, and a host state restore clears it.
Incoming host automation and display-setting edits do not create undo entries.
See [Keyboard shortcuts](#keyboard-shortcuts) for editing, navigation, and modifier gestures.
Hover either analysis section to reveal its collapse chevron, or focus it with the keyboard.
Hidden sections retain labelled restore handles, with the same visibility choices available in Settings.

A and B hold independently editable sound settings, with a copy action for the other slot.
Both slots and the selected slot are saved with the host project.
A/B and presets recall crossovers, Mid, Side, Width, Recover, Pan, output gain, and Mono.
Solo, Delta, Bypass, Link, and display preferences stay unchanged.
Switching A/B, copying a slot, and loading a preset each form one undo step.

The preset menu saves, updates, renames, deletes, imports, and exports ImageControl sound presets.
Modified status marks sound settings that differ from the selected preset baseline.
When the preset row is collapsed, its name and a modified asterisk appear in the existing title area.
Both A/B slots retain their selected preset names and baselines with the host project, even without the library files.
Copy changes only the other slot's sound, not its preset identity.
After project restore, explicitly reconnect the library revision before updating, renaming, deleting, or exporting it.
Reconnection does not load sound or replace the stored baseline.
Updates ask for confirmation, and exports use the saved library revision rather than current edits.
Imports add to the library without changing the current sound.
The library is shared by plugin instances and formats, separately from host presets and AudioBatch chain presets.
It lives in `~/Library/Application Support/esgrove/ImageControl/Presets` on macOS
and `%APPDATA%\\esgrove\\ImageControl\\Presets` on Windows.
Projects save their sound independently of these files.

## Keyboard shortcuts

Shortcuts apply while the plugin editor has keyboard focus and the host forwards the keys.
Text fields keep their own editing shortcuts instead of triggering plugin-wide commands.

### Editing and monitoring

| Action | macOS | Windows |
| --- | --- | --- |
| Undo local processing edit | Cmd+Z | Ctrl+Z |
| Redo local processing edit | Cmd+Shift+Z or Cmd+Y | Ctrl+Shift+Z or Ctrl+Y |
| Toggle audio Bypass | Cmd+B | Ctrl+B |
| Toggle Delta monitoring | Cmd+D | Ctrl+D |
| Toggle Solo for the band under the pointer | S | S |

### Navigation and display

| Action | Shortcut |
| --- | --- |
| Focus next / previous control | Tab / Shift+Tab |
| Open Display settings | Cmd+comma (macOS) / Ctrl+comma (Windows) |
| Open controls help | F1 |
| Dismiss Display settings or cancel a preset dialog | Escape |
| Resize with the divider focused | Up / Down, 8 pixels per press |
| Resize the divider finely | Shift+Up / Shift+Down, 1 pixel per press |

### Sliders and value fields

| Action | Key or gesture |
| --- | --- |
| Increase / decrease a focused slider | Right or Up / Left or Down |
| Adjust a focused slider finely | Shift+arrow key, at least one legal value step |
| Fine pointer adjustment | Shift+drag or Shift+scroll |
| Enter an exact value | Click the value field, type a value, then press Return / Enter |
| Reset a slider | Double-click its track or thumb |

### Spectrogram gestures

Start band gestures inside a band region, away from crossover lines.
Mid gain edits follow the band's Link setting, while Side gain edits leave Mid unchanged.

| Action | macOS | Windows |
| --- | --- | --- |
| Move a crossover or band | Drag sideways | Drag sideways |
| Reset a crossover | Double-click its line | Double-click its line |
| Change band Mid gain | Cmd+drag vertically or Cmd+scroll | Ctrl+drag vertically or Ctrl+scroll |
| Change band Side gain | Option+drag vertically or scroll | Alt+drag vertically or scroll |
| Adjust band gain finely | Add Shift to a gain drag or scroll gesture | Add Shift to a gain drag or scroll gesture |

## Processing and transparency

Linear-phase crossovers split the signal into complementary bands that sum back to the delayed input.
At neutral settings the output is the input delayed by the plugin latency, bit for bit.
Gain, Width, Recover, pan, solo, Delta, Mono, and bypass changes are smoothed to avoid clicks.
Crossover filters are redesigned off the audio thread and crossfaded into place.

Delta monitors only what the processing changes, with the dry reference aligned to the output.
It is silent at neutral settings and respects the selected solo bands.
Bypass restores the delayed input with processing disabled, ignoring solos and Delta, without changing latency.
Mono folds the full processed output equally into both channels after Delta or bypass, with a click-free transition.
It is saved and automatable, applies to offline renders, and is reflected in the Post spectrogram.
Pre analysis and per-band correlation meters remain independent of the final Mono fold.

The FIR filters can ring beyond the latency-compensated input endpoint when bands have different settings.
The plugin reports this finite tail to hosts so they can render it rather than truncate it.
Recover uses finite quadrature FIRs without adding a second cascaded filter delay.
It can retain the audibility of Side-derived detail, such as some reverb tails, when narrowing.
Recover follows the amount of Side amplitude removed by Width, not a constant-power or automatic gain-compensation law.
It does not turn on when pressing global Mono, and it cannot restore material already removed by the Side source trim.
Arbitrary phase relationships can cause cancellation or higher peaks, and swapping L/R need not give the same result.
Its quadrature response is zero at DC and Nyquist.
[Filter tests](tests/CrossoverTests.cpp) bound the sampled fullband gain error across 20 Hz to 20 kHz at supported rates.

The linear-phase filters introduce substantial latency: about 90 ms at 48 kHz.
The plugin reports this to the host for compensation,
making it better suited to mixing and offline processing than latency-sensitive live monitoring.
Exact filter lengths and convolution details are in [CrossoverDesign.h](src/dsp/CrossoverDesign.h)
and [PartitionedConvolver.h](src/dsp/PartitionedConvolver.h).
[ImagerTests.cpp](tests/ImagerTests.cpp), [RecoveryTests.cpp](tests/RecoveryTests.cpp),
and [ProcessorTests.cpp](tests/ProcessorTests.cpp) cover transparency and processing behavior.
The [recovery DSP section](ARCHITECTURE.md#finite-quadrature-recovery) explains the filter design and its limitations.

## Spatial spectrogram

Frequency runs from bass to treble left to right,
and stereo position from full left at the top through the centre to full right at the bottom.
Position follows the level difference between channels, not phase or stereo width:
equal-level anti-phase content sits in the centre just like mono content.
Frequency colours identify spectral regions.
Phase colours distinguish in-phase and anti-phase content.

Brightness represents the analyzed source power from both channels, not the power at a screen position.
Frequency reassignment sharpens tonal traces without replacing their original bin power.
Spectral tilt and automatic scaling make quiet and loud material readable,
so the display is a relative view rather than an absolute level meter.
Pre shows the input and Post the processed output, including the difference signal when Delta is active.

The analyzer and rendering run separately from audio processing.
The display needs OpenGL 3.3.
Audio processing and controls remain available without it.
See [SpatialAnalyzer.h](src/analysis/SpatialAnalyzer.h)
and [AnalyzerTests.cpp](tests/AnalyzerTests.cpp) for the analysis contract,
and [test-signals/generate.py](test-signals/generate.py) for reference signals.

The optional correlation row has one horizontal meter per band, from -1 for anti-phase to +1 for in-phase.
It measures each processed band after its source trims, Width/Recover, and Pan,
before Delta and Mono, independently of Pre/Post.
Silence or insufficient energy in either channel gives an unavailable reading.
Readings also clear when processing stops, the meters are hidden, or no new callbacks arrive.
The coloured marker shows the live reading, a yellow highlight shows its smoothed trend,
and a soft trail fills the recently observed range, fading its older portions within the same persistence window.
The number and accessible value follow the smoothed trend by default.
Turn off `Smooth correlation numbers` in Display settings for live numbers without resetting meter history.
`Persistence` controls both trend smoothing and the range duration, as well as the spectrogram fade.
`Show band correlation` in Display settings controls the row independently of spectrogram visibility.

## Build and install

Requires the JUCE submodule in `../cpp/JUCE`, CMake 4.2 or newer, and a C++23 compiler.
macOS builds require the Xcode toolchain and use Ninja when available.
The minimum deployment target is macOS 15.
Windows builds use Visual Studio by default, with Ninja available through `--ninja`.
Initialize JUCE from the repository root with `git submodule update --init cpp/JUCE`.
Run the following commands from `image-control-plugin/`.

```shell
./build.sh
./install.sh
```

The build writes Release bundles under `cmake-build-release/` and prints the VST3 path.
`--print` prints that path without building, `--debug` selects Debug, and `--reuse` skips configuration.
Windows Ninja builds use `cmake-build-ninja-release/` instead.
See `./build.sh --help` and `./install.sh --help` for other options.

The installer builds and copies the VST3 to `~/Library/Audio/Plug-Ins/VST3` on macOS
or `C:\Program Files\Common Files\VST3` on Windows, requesting elevation if needed.
On macOS it also installs the Audio Unit to `~/Library/Audio/Plug-Ins/Components` and refreshes its registry.
`./build.sh --validate` validates the built VST3 with pluginval, without installing the plugin.
`./install.sh --validate` validates the installed VST3 and, on macOS, the registered Audio Unit with auval.
Both use pluginval from `PATH` when available.
Otherwise they fetch and build the pinned version in the repository's `.deps/` directory and reuse it on later runs.
This needs Git, the build toolchain, and network access for missing dependencies,
with no global installation or PATH setup.
Setup or validation failures fail the command instead of skipping VST3 validation.
Both accept `--validation-level 5` through `--validation-level 10`, defaulting to 5.
Specifying a level also enables validation, for example `./build.sh --validation-level 10`.
The level controls pluginval strictness. Audio Unit validation always uses auval's strict mode.

### Tests and lint

```shell
./build.sh --test
./lint.sh --check
```

Tests use GoogleTest, fetched by CMake.
Git and network access are needed for missing dependencies.
`--reuse --test` requires a workspace already configured with `IMAGE_CONTROL_BUILD_TESTS=ON`.
Lint requires clang-tidy and the shared C++ configuration.
It prepares a Debug build and compile database.
On Windows, enable Developer Mode and `git config core.symlinks true` before checkout
so the configuration symlinks are real links, not text placeholders.
`./format.sh` formats the plugin's C++, CMake, shell, Python, and prose files, and `./format.sh --check` verifies them.
On macOS, `./sanitize.sh` runs the plugin tests with AddressSanitizer and UndefinedBehaviorSanitizer,
`./sanitize.sh --thread` uses ThreadSanitizer, and `./coverage.sh --lcov` reports plugin source coverage.
`./sanitize.sh --realtime` uses Homebrew LLVM to check cold real-time processing and the build-tree VST3 on macOS.
Detector controls and clean direct-processing cases are separate from verified `BLOCKED` JUCE host/wrapper audits.
Diagnostics are retained under `cmake-build-mac-rtsan/realtime-artifacts/`. No plugin is installed.
Offline kernel design is intentionally outside the checked boundary,
and passing checks do not establish audio deadlines.
Each sanitizer uses a separate build directory and does not build the AudioBatch applications.
Lint also checks RTSan-only sources when their instrumented compilation database exists,
and otherwise reports that those probes need `./sanitize.sh --realtime` first.
CI runs Debug and Release tests on both platforms, pinned VST3 validation, and macOS Audio Unit validation.

### Optional pluginval target

For reproducible VST3 validation, enable the pinned pluginval build and explicitly run its target:

```shell
cmake -S . -B cmake-build-release -DAUDIOBATCH_PLUGINVAL=ON
cmake --build cmake-build-release --config Release --target ImageControl_pluginval
```

This needs Git, the build toolchain, and network access for missing dependencies.
The target always uses the pinned version and validates at strictness level 5.
The scripts share its pinned build helper when pluginval is missing from `PATH`.
Use the matching build directory if using a Windows Ninja workspace.

### Benchmarks and reference signals

The benchmark measures full-callback throughput and timing distributions across sample rates and block sizes,
including recovery off, selected/all bands, crossover changes, and optional metering/analysis load.
It also measures isolated imager processing and Width/Recover setters, plus separate analyzer and particle workloads.
CPU-side overlay probes include hovered response curves, and focused callback rows cover Solo/Delta and small blocks.
These measurements exclude GPU drawing.
After configuring tests, build the benchmark explicitly.
`--test` does not build it:

```shell
cmake --build cmake-build-release --config Release --target ImageControlBenchmark
```

Run `ImageControlBenchmark` from that build directory (`Release/ImageControlBenchmark.exe` on Windows).
Use a Release build for measurements. `--quick` shortens the full matrix, and `--help` lists workload controls.
`IMAGE_CONTROL_HARDENING=OFF` permits a separate hardening comparison.

On macOS, [compare_formats.py](benchmarks/compare_formats.py) compares VST3
and Audio Unit rendering speed and PCM output.
It requires both installed formats, a built AudioBatch CLI, uv, ffmpeg, and generated benchmark inputs.
From `image-control-plugin/`:

```shell
uv run --script ../fixtures/benchmark/generate.py
uv run --script benchmarks/compare_formats.py --cli /absolute/path/to/audiobatch
```

Generate reference signals with `uv run --script test-signals/generate.py`.
Output goes to the ignored `test-signals/output/` directory.
Use `--group listening` for manual Width/Recover comparisons.
It selects synthetic decays, pure-Side signals, adversarial phase pairs, and hard-panned transients/bass.
Use `--output /tmp/image-control-signals` to select scratch output.
These inputs are not a record of listening evaluations.

## Future work

- Add a Metal backend for `SpectrogramRenderer` on macOS.
  [JUCE Metal rendering on macOS](https://github.com/UVI-Dev/JUCE/tree/metal-renderer)
