# ImageControl

A multiband mid/side stereo imaging effect built with JUCE.
Available as a VST3 for macOS and Windows, and an Audio Unit for macOS.
It lives in the AudioBatch repository and is primarily used in AudioBatch's plugin chain,
but works independently in compatible plugin hosts.
See [ARCHITECTURE.md](ARCHITECTURE.md) for code organization, DSP, rendering, and threading details.

## Key features

- Four frequency bands: Low, Low Mid, High Mid, and High, with three adjustable crossovers.
- Independent Mid and Side gains per band, with Width for narrowing or widening the stereo image.
  Width 0 makes a band mono, 1 preserves its image, and 2 doubles Side relative to Mid.
  Width adjusts Side gain rather than adding a separate automation parameter.
- Per-band Pan, applied after the mid/side gains, so even a narrowed band can be placed to one side.
- Combinable band solos, output gain, Delta monitoring, click-free bypass, and automatable Mono output.
- A spatial spectrogram for inspecting the input or processed output, with optional per-band correlation meters.

Crossover frequencies stay ordered and at least a third of an octave apart.
Hosts can save and automate the processing parameters.
See [Parameters.h](src/Parameters.h) for parameter ids, ranges, and defaults.

## Processing and transparency

Linear-phase crossovers split the signal into complementary bands that sum back to the delayed input.
At neutral settings the output is the input delayed by the plugin latency, bit for bit.
Gain, pan, solo, Delta, Mono, and bypass changes are smoothed to avoid clicks.
Crossover filters are redesigned off the audio thread and crossfaded into place.

Delta monitors only what the processing changes, with the dry reference aligned to the output.
It is silent at neutral settings and respects the selected solo bands.
Bypass restores the delayed input with processing disabled, ignoring solos and Delta, without changing latency.
Mono folds the full processed output equally into both channels after Delta or bypass, with a click-free transition.
It is saved and automatable, applies to offline renders, and is reflected in the Post spectrogram.
Pre analysis and per-band correlation meters remain independent of the final Mono fold.

The FIR filters can ring beyond the latency-compensated input endpoint when bands have different settings.
The plugin reports this finite tail to hosts so they can render it rather than truncate it.

The linear-phase filters introduce substantial latency: about 90 ms at 48 kHz.
The plugin reports this to the host for compensation,
making it better suited to mixing and offline processing than latency-sensitive live monitoring.
Exact filter lengths and convolution details are in [CrossoverDesign.h](src/dsp/CrossoverDesign.h)
and [PartitionedConvolver.h](src/dsp/PartitionedConvolver.h).
[ImagerTests.cpp](tests/ImagerTests.cpp) and [ProcessorTests.cpp](tests/ProcessorTests.cpp) cover transparency
and processing behavior.

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
It measures each processed band after its Mid/Side gains and Pan,
before Delta and Mono, independently of Pre/Post.
Silence or insufficient energy in either channel gives an unavailable reading.
Readings also clear when processing stops, the meters are hidden, or no new callbacks arrive.
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
`./install.sh --validate` uses pluginval if available on the path and, on macOS, validates the Audio Unit with auval.

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
`../cpp/format.sh` formats this plugin along with the shared C++ sources.
On macOS, `./sanitize.sh` runs the plugin tests with AddressSanitizer and UndefinedBehaviorSanitizer,
`./sanitize.sh --thread` uses ThreadSanitizer, and `./coverage.sh --lcov` reports plugin source coverage.
Each uses a separate build directory and does not build the AudioBatch applications.
CI runs Debug and Release tests on both platforms, pinned VST3 validation, and macOS Audio Unit validation.

### Optional pluginval target

For reproducible VST3 validation, enable the pinned pluginval build and explicitly run its target:

```shell
cmake -S . -B cmake-build-release -DAUDIOBATCH_PLUGINVAL=ON
cmake --build cmake-build-release --config Release --target ImageControl_pluginval
```

This needs Git, the build toolchain, and network access for missing dependencies.
The target validates at strictness level 5 and is separate from the installer's path-based pluginval check.
Use the matching build directory if using a Windows Ninja workspace.

### Benchmarks and reference signals

The benchmark measures full-callback throughput and timing distributions across sample rates and block sizes,
including crossover changes and optional metering/analysis load, plus CPU-side spectrogram work, not GPU drawing.
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

Generate signals for checking the spectrogram by ear and eye with `uv run --script test-signals/generate.py`.
Output goes to the ignored `test-signals/output/` directory.

## Future work

- Plugin-local undo/redo for parameter edits.
- A/B snapshots for comparing two processed settings.
- Plugin-owned preset management, separate from existing host state and preset support.
- Add a Metal backend for `SpectrogramRenderer` on macOS.
  [JUCE Metal rendering on macOS](https://github.com/UVI-Dev/JUCE/tree/metal-renderer)
