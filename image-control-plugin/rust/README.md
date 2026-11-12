# ImageControl RS

ImageControl RS is an independent Rust multiband stereo-imaging plugin with a native Iced/wgpu editor.
It provides VST3 and CLAP on macOS and Windows, and an experimental self-contained AUv2 on macOS.
It shares the repository's Cargo workspace, but does not use AudioBatch's application services or GUI.
It installs alongside the C++ ImageControl plugin rather than replacing it.
C++ is the feature, close user-experience, and audio reference, not an exact internal implementation blueprint.
Compatibility with older RS state or normalized automation is not required.

The implementation targets macOS and Windows.
Historical local verification covers native macOS processing, editor, GPU tests, and Windows MSVC cross-checks.
That evidence predates the current cleanup. New verification is in progress, with no new measurements asserted here.
Native Windows execution and graphics remain unverified.
The C++ plugin remains the supported implementation.

## Features

- Four complementary linear-phase FIR bands with independent Mid, Side, Width, Recover, Pan, and Solo controls.
- Output trim, latency-aligned Delta, latency-preserving bypass, and a final Mono fold, with 20 ms control ramps.
- Bit-exact delayed input at settled neutral settings, with warm convolution history for processing changes.
- Background crossover-kernel design for realtime use and synchronous deterministic design for offline rendering.
- Six-tier spatial spectral analysis with frequency reassignment, stereo position, and phase coherence.
- GPU particles or solid density contours, persistent energy, five-level bloom, and history-preserving resize,
  with Logarithmic/Mel frequency axes, frequency/phase coloring, and adjustable stereo-position curves.
- Consistent Iced controls, embedded font weights, compact numeric editing, and modal display settings.
  Plot gestures move crossovers or whole bands, adjust source gains, and support reset and keyboard shortcuts.
- Local Undo/Redo, independently editable A/B sounds with copy, and a per-user Rust JSON preset library.
- An embedded resize grip with host-approved logical window sizes retained across close/reopen and saved projects.
- Pre/Post analysis, persistence and contrast, correlation trends and fading range fields,
  hide/stop analysis controls, frame-rate diagnostics, and shortcut help.
- Thirty-one stable automatable sound parameters and independent versioned state for sound, comparisons,
  preset origins, complete editor layout, display preferences, and window geometry.

At 48 kHz, processing latency is 4352 samples and the conservative tail beyond compensation is 4096 samples.
Long bass-analysis windows affect display responsiveness, not the reported audio latency.
The spectrogram uses relative adaptive brightness and is not an absolute level meter.

Recover ranges from 0–200%, matching C++.
It adds a finite quadrature-rotated contribution from removed Side amplitude when Width is below one.
At 200%, the recovered contribution is twice its 100% amplitude, not twice the overall output level.
It is not automatic loudness compensation or a power-preserving width law.

Preset files are independent RS JSON, not C++ XML imports or migrated C++ projects.
The Presets callout supports load, Save/Save As, rename, delete, reconnect, and explicit absolute-path import/export.
Filesystem work runs asynchronously and is joined on native editor close.
Stale selections are rejected rather than silently overwriting another instance's changes.

Crossovers use a true logarithmic `f64` mapping, with useful plain-unit snapping for stepped controls,
not JUCE float emulation. Parameter IDs remain stable, but older RS normalized automation is not promised compatibility.
The current implementation is the first supported iteration.
Parameter/project-state and preset schemas both start at version 1.
Prototype data has no compatibility guarantee and is not migrated.
Before using it with existing data, move aside prototype preset files.
Their library is `~/Library/Application Support/esgrove/ImageControl RS/Presets` on macOS
or `%APPDATA%\esgrove\ImageControl RS\Presets` on Windows.
Recreate Rust plugin instances and host/chain presets saved by prototype builds.
Some prototypes also used version 1, so a matching version number alone does not establish compatibility.
Leave the independent C++ ImageControl library unchanged.
The detached codec uses `Snapshot`, `AnalysisTap`, and `DisplayPreferences` directly, without duplicate state mirrors.

## Build and test

Run scripts from this directory.
Use the Rust toolchain declared by the [root workspace](../../Cargo.toml), a native platform build toolchain,
and a GPU supported by wgpu's Metal or Direct3D backend.
The Rust plugin does not require JUCE to compile.
The macOS AU build additionally requires CMake and an Apple C++/Objective-C++ toolchain.
It fetches immutable upstream clap-wrapper, CLAP SDK, and AudioUnitSDK revisions into the selected build tree.
The component statically links the Rust CLAP implementation and does not search for an installed CLAP.
Build-tree validation can build the repository-pinned pluginval when it is not on `PATH`.

```shell
# Release VST3 and CLAP, plus AUv2 on macOS, without installation.
./build.sh

# Debug build, complete scoped tests, and build-tree pluginval validation.
./build.sh --debug --test --validate

# Release build and tests.
./build.sh --test

# Full native contracts, including packaged editor open/close/reopen.
./test.sh

# Static checks and additional verification.
./format.sh --check
./lint.sh --check
./test.sh --miri
./test.sh --licenses
./rtsan.sh
./rtsan.sh --format clap
# Verify known AU dependency reports as BLOCKED, not clean callback evidence.
./rtsan.sh --format au --diagnostic
```

The default artifacts are `rust/target/bundled/ImageControl RS.vst3`, `ImageControl RS.clap`,
and on macOS `ImageControl RS.component`, relative to the repository root, not this directory's ignored `target/`.
The Windows CLAP artifact is a single file. The macOS CLAP and AU artifacts are bundles.
`CARGO_TARGET_DIR` overrides the artifact location, resolved from the invocation directory before scripts change directories.
Explicit external target roots are allowed, with canonical path checks rejecting symlink escapes.
Debug and Release packaging use the same bundle path, so the last build selects its contents.
`./test.sh` requires an existing build-tree bundle.
Every script provides `-h` and `--help`.

The packaged realtime checks require macOS, nightly Rust, uv, jq, and a matching Homebrew LLVM runtime.
They verify allocation and mutex detector controls separately from required-clean callback cases.
Passing these exercised scenarios does not establish audio deadlines or arbitrary host compatibility.

## Cross-implementation tests and benchmarks

The packaged parity host compares both actual VST3s with deterministic signals and independent neutral,
gain, pan, mono, latency, and tail expectations.
It uses plain-unit parameter mappings, not shared normalized IDs or project-state migration.

```shell
# Build C++ once with tests, then its independent packaged-plugin comparison host.
../build.sh --test
cmake --build ../cmake-build-release --config Release --target ImageControlParity
./build.sh
./parity.sh --cpp "$PWD/../cmake-build-release/ImageControl_artefacts/Release/VST3/ImageControl.vst3" \
    --executable "$PWD/../cmake-build-release/ImageControlParity_artefacts/Release/ImageControlParity"

# Preallocated production DSP/analysis benchmarks, with preparation measured separately.
cargo bench -p image-control-dsp --bench dsp -- --quick

# Production GPU spectrogram: packing, submission, and completed-work Criterion groups.
cargo bench -p image-control-plugin-rs --features render-benchmark --bench spectrogram

# Complete-process hyperfine runs with separate warmed-callback diagnostics.
./benchmark.sh --case recover --frames 960000 --runs 5 --warmup 1

# Broadband audio with independently moving, beat-enveloped frequency bands.
./benchmark.sh --signal moving-noise --frames 384000 --runs 5

# Export eight seconds of production-analyzed particles for either native renderer.
cargo run --release -p image-control-plugin-rs --features render-benchmark \
  --example audio_render_workload -- --output "$PWD/target/benchmark/moving-noise.icrb"
IMAGECONTROL_RENDER_INPUT="$PWD/target/benchmark/moving-noise.icrb" \
  cargo bench -p image-control-plugin-rs --features render-benchmark --bench spectrogram -- audio_input --quick
```

The broadband source uses seeded noise, six bass-balanced bands, independent rhythmic envelopes,
and frequency-dependent equal-power panning that evolves over time.
The DSP Criterion target includes separate `imager_warm_moving_noise` and `analyzer_warm_moving_noise` groups,
streaming successive chunks of an eight-second precomputed source outside input-generation timing.
The visible `editor_benchmark` also accepts `--signal moving-noise` and records its eight-second PCM-loop hash.
For paired GPU measurements, pass the export
as `--input` to `../benchmarks/render_benchmark.py` with explicit `--cpp` and/or `--rust` binaries
and a new `--artifacts` directory.
The export uses production FFT analysis and particle projection after one second of warmup,
at 48 kHz stereo and 30 render frames per second.
It retains every generated particle, padding shorter frames with zero-energy records for the shared fixed-stride format.
The sidecar records active counts, audio identity, and workload hashes.
Criterion audio groups include the input SHA-256.
Tone and static-cloud controls remain separate.
This synthetic music-like source is not a recorded-music corpus.

Use the `.exe` parity host on Windows.
`parity.sh` rejects incomplete setup rather than skipping a missing counterpart.
The Rust integration test is optional without counterpart selection, and CI supplies both build-tree bundles.
Peak residual must stay below `1e-5` full scale and RMS residual below `1e-6`, with exact neutral checks retained.
Reports live under the shared target's `image-control-parity/` directory.
Benchmark artifacts include exact binary hashes, compiler/source identity, workload, checksums, and machine metadata.
Native processing reports separate module loading, instance preparation, and warmed callbacks.
Preparation covers instance creation through activation, not exclusive kernel design or complete-process wall time.
Run timings with other builds idle. There is no implied speedup without a matched baseline.
The `spectrogram` Criterion target uses the native GPU without a host window.
Cargo's bench profile inherits Release with full optimization, LTO, and one codegen unit.
The repository enables `target-cpu=native`.
Measured entry points reject development, non-opt-level-3, or non-native artifacts.
Its completed-work group includes bounded batch completion, not physical display FPS or exclusive GPU timestamps.
Filter it with `-- 'completed_gpu_work/1280x720/particles/4096$' --quick` for a short single-case run.
See [the evaluation](docs/rust-evaluation.md#cross-implementation-and-performance-evidence) for measured scope
and limitations.

For isolated application CLI integration, capture host-native presets from explicit build-tree artifacts:

```shell
cargo run --release -p audiobatch-plugins --example image_control_presets -- \
  --bundle "$PWD/../../rust/target/bundled/ImageControl RS.clap" \
  --artifacts "$PWD/target/native-host-presets"
```

The artifacts directory must be new and absolute.
The helper saves neutral and -6 dB output-trim presets through AudioBatch-rs's production host and preset store.
It also accepts the C++ or RS VST3, without installed discovery or application settings changes.
C++ AudioBatch requires its own native XML presets, captured with `ImageControlPresetWriter --bundle`.
`--neutral` saves initialized defaults, while the default captures its mapped comparison controls.
Use separate absolute `AUDIOBATCH_DATA_ROOT` directories and temporary input copies for `process --preset` runs.
The dated Windows results and remaining GUI qualification gates are recorded in the evaluation.

## Install

Close hosts using ImageControl RS before replacing its installed bundle.

```shell
./install.sh
./install.sh --no-build --validate
```

The default per-user VST3 destinations are `~/Library/Audio/Plug-Ins/VST3` on macOS
and `%LOCALAPPDATA%/Programs/Common/VST3` on Windows.
CLAP uses the sibling `CLAP` folder. The macOS Audio Unit uses `~/Library/Audio/Plug-Ins/Components`.
Use `--destination <directory>`, `--clap-destination <directory>`, and `--au-destination <directory>`
to select format-specific folders, and `--validation-level <5-10>` for installed validation.
Rescan the host and select **ImageControl RS**.
The installer never replaces the C++ ImageControl bundles.
Installed AU validation registers and checks the distinct `aufx / Egir / Esgr` component with strict auval.
Build-tree tests load the exact artifacts without installation or registrar changes.
Local macOS signing is ad-hoc, not Developer ID signing or notarization.

## Scope and limitations

- The VST3 retains its own class identity and `com.esgrove.imagecontrol-rs-own` bundle identity.
  CLAP uses descriptor ID `com.esgrove.imagecontrol-rs-own`, while the AU has a separate component identity.
  Formats share the Rust state codec, not JUCE ImageControl project compatibility or the removed Nice experiment.
- AUv2 is experimental because of known limitations in the upstream clap-wrapper and Apple AudioUnitSDK dependencies,
  not the Rust imaging DSP. These affect offline-mode forwarding, state lifecycle/error reporting, and contention.
  AU realtime diagnostics also detect first-use allocation and reset-entry locking.
  See [AU dependency limitations](ARCHITECTURE.md#au-dependency-limitations) for ownership, practical effects,
  and the evidence boundaries. Clean VST3 or direct CLAP checks do not qualify the AU wrapper.
- Only matching mono-to-mono and stereo-to-stereo main buses and 32-bit floating-point processing are supported.
  Automation is sampled per host block, not sample-accurate within a block.
- Audio preparation supports actual rates from 1 through 2,822,400 Hz.
  Spatial analysis supports 8 through 768 kHz, reporting unavailable outside that range without disabling audio.
- Presets store sound, excluding Solo, Delta, Bypass, editor preferences, and local history.
  Project preset origins retain identity and a sound baseline, not permission to overwrite a library revision.
  After project recall, reconnect explicitly before changing the corresponding library entry.
- Windows CLAP exposes the custom Iced editor with synchronous native retirement during host callbacks.
  The published Iced bridge repair defers nested runtime polling without losing queued events.
  Bounded native callback probes pass, but repeated unobstructed Debug/Release, broader input/DPI,
  and actual application GUI workflows remain qualification gates, as recorded in the evaluation.
- Full Iced device-loss recovery, native accessibility qualification, safe arbitrary module unloading,
  and complete upstream widget-cache reclamation are not established.
  Invalid off-thread or reentrant final release retains UI resources as leak containment, not unload qualification.

See [ARCHITECTURE.md](ARCHITECTURE.md) for ownership, DSP, analysis, rendering, state, and verification contracts.
The bounded `editor_compare` example hosts explicit C++ and Rust build-tree bundles with the same stereo comb.
Use it for manual layout and spectrum comparison without audio hardware.
[The evaluation](docs/rust-evaluation.md) records dependency research, historical experiments, and production gates.
[Plugin agent instructions](../AGENTS.md) and [repository instructions](../../AGENTS.md) govern development.
